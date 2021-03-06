#include <map>
#include "caffe/proto/caffe.pb.h"

#include "caffe/common.hpp"
#include "caffe/blob.hpp"
#include "caffe/data_transformer.hpp"
#include "caffe/internal_thread.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/parallel.hpp"

namespace caffe {

template<typename Ftype, typename Btype>
size_t BasePrefetchingDataLayer<Ftype, Btype>::threads(const LayerParameter& param) {
  // Check user's override in prototxt file
  size_t threads = param.data_param().threads();
  if (!auto_mode(param) && threads == 0U) {
    threads = 1U;  // input error fix
  }
  // 1 thread for test net
  return (auto_mode(param) || param.phase() == TEST || threads == 0U) ? 1U : threads;
}

template<typename Ftype, typename Btype>
size_t BasePrefetchingDataLayer<Ftype, Btype>::parser_threads(const LayerParameter& param) {
  // Check user's override in prototxt file
  size_t parser_threads = param.data_param().parser_threads();
  if (!auto_mode(param) && parser_threads == 0U) {
    parser_threads = 1U;  // input error fix
  }
  // 1 thread for test net
  return (auto_mode(param) || param.phase() == TEST || parser_threads == 0U) ? 1U : parser_threads;
}

template<typename Ftype, typename Btype>
bool BasePrefetchingDataLayer<Ftype, Btype>::auto_mode(const LayerParameter& param) {
  // Both should be set to positive for manual mode
  const DataParameter& dparam = param.data_param();
  bool auto_mode = !dparam.has_threads() && !dparam.has_parser_threads();
  return auto_mode;
}

template<typename Ftype, typename Btype>
BaseDataLayer<Ftype, Btype>::BaseDataLayer(const LayerParameter& param, size_t transf_num)
    : Layer<Ftype, Btype>(param), transform_param_(param.transform_param()) {}

template<typename Ftype, typename Btype>
void BaseDataLayer<Ftype, Btype>::LayerSetUp(const vector<Blob*>& bottom,
    const vector<Blob*>& top) {
  output_labels_ = top.size() != 1;
  // Subclasses should setup the size of bottom and top
  DataLayerSetUp(bottom, top);
}

template<typename Ftype, typename Btype>
BasePrefetchingDataLayer<Ftype, Btype>::BasePrefetchingDataLayer(const LayerParameter& param)
    : BaseDataLayer<Ftype, Btype>(param, threads(param)),
      InternalThread(Caffe::current_device(), this->solver_rank_, threads(param), false),
      auto_mode_(Caffe::mode() == Caffe::GPU && this->phase_ == TRAIN && auto_mode(param)),
      parsers_num_(parser_threads(param)),
      transf_num_(threads(param)),
      queues_num_(transf_num_ * parsers_num_),
      batch_transformer_(make_shared<BatchTransformer<Ftype, Btype>>(Caffe::current_device(),
          this->solver_rank_, queues_num_, param.transform_param(), is_gpu_transform())) {
  CHECK_EQ(transf_num_, threads_num());
  batch_size_ = param.data_param().batch_size();
  // We begin with minimum required
  ResizeQueues();
}

template<typename Ftype, typename Btype>
BasePrefetchingDataLayer<Ftype, Btype>::~BasePrefetchingDataLayer() {
  batch_transformer_->StopInternalThread();
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::LayerSetUp(const vector<Blob*>& bottom,
    const vector<Blob*>& top) {
  this->rank_ = this->solver_rank_;
  bottom_init_ = bottom;
  top_init_ = top;
  BaseDataLayer<Ftype, Btype>::LayerSetUp(bottom, top);

  for (int i = 0; i < transf_num_; ++i) {
    data_transformers_.emplace_back(
        make_shared<DataTransformer>(this->transform_param_, this->phase_));
  }
  const Solver* psolver = this->parent_solver();
  const uint64_t random_seed = (psolver == nullptr ||
      static_cast<uint64_t>(psolver->param().random_seed()) == Caffe::SEED_NOT_SET) ?
          Caffe::next_seed() : static_cast<uint64_t>(psolver->param().random_seed());
  StartInternalThread(true, random_seed);
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::InternalThreadEntry() {
  InternalThreadEntryN(0U);
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::InternalThreadEntryN(size_t thread_id) {
  static thread_local bool iter0 = this->phase_ == TRAIN;
  if (iter0 && this->net_inititialized_flag_ != nullptr) {
    this->net_inititialized_flag_->wait();
  } else {  // nothing to wait -> initialize and start pumping
    InitializePrefetch();
    start_reading();
    iter0 = false;
  }
  try {
    while (!must_stop(thread_id)) {
      const size_t qid = this->queue_id(thread_id);
      shared_ptr<Batch> batch = batch_transformer_->prefetched_pop_free(qid);
      CHECK_EQ((size_t) -1L, batch->id());
      load_batch(batch.get(), thread_id, qid);
      if (must_stop(thread_id)) {
        break;
      }
      batch_transformer_->prefetched_push_full(qid, batch);
      if (iter0) {
        if (this->net_iteration0_flag_ != nullptr) {
          this->net_iteration0_flag_->wait();
        }
        if (this->net_inititialized_flag_ != nullptr) {
          this->net_inititialized_flag_ = nullptr;  // no wait on the second round
          InitializePrefetch();
          start_reading();
        }
        if (this->auto_mode()) {
          break;
        }  // manual otherwise, thus keep rolling
        iter0 = false;
      }
    }
  } catch (boost::thread_interrupted&) {
  }
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::ResizeQueues() {
  size_t size = batch_ids_.size();
  if (transf_num_ > size) {
    batch_ids_.resize(transf_num_);
    for (size_t i = size; i < transf_num_; ++i) {
      batch_ids_[i] = i;
    }
  }
  size = this->data_transformers_.size();
  if (transf_num_ > size) {
    for (size_t i = size; i < transf_num_; ++i) {
      this->data_transformers_.emplace_back(
          make_shared<DataTransformer>(this->transform_param_, this->phase_));
    }
  }
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::InitializePrefetch() {
  ResizeQueues();
  this->DataLayerSetUp(bottom_init_, top_init_);
}

template<typename Ftype, typename Btype>
void BasePrefetchingDataLayer<Ftype, Btype>::Forward_cpu(const vector<Blob*>& bottom,
    const vector<Blob*>& top) {
  // Note: this function runs in one thread per object and one object per one Solver thread
  shared_ptr<Batch> batch = this->batch_transformer_->processed_pop();
  top[0]->Swap(*batch->data_);
  if (this->output_labels_) {
    top[1]->Swap(*batch->label_);
  }
  this->batch_transformer_->processed_push(batch);
}

#ifdef CPU_ONLY
STUB_GPU_FORWARD(BasePrefetchingDataLayer, Forward);
#endif

INSTANTIATE_CLASS_FB(BaseDataLayer);
INSTANTIATE_CLASS_FB(BasePrefetchingDataLayer);

}  // namespace caffe
