#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/im2col.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/vision_layers.hpp"
#include "dnn.h"

namespace caffe {
template <typename Dtype>
DnnConvolutionLayer<Dtype>::DnnConvolutionLayer(const LayerParameter& param)
      : ConvolutionLayer<Dtype>(param),
        fwd_bottom_data   (new MklDnnMemoryDescriptor<Dtype, false> ()),
        fwd_top_data      (new MklDnnMemoryDescriptor<Dtype, false> ()),
        fwd_filter_data   (new MklDnnMemoryDescriptor<Dtype, false> ()),
        fwd_bias_data     (new MklDnnMemoryDescriptor<Dtype, false> ()),
        bwdd_top_diff     (new MklDnnMemoryDescriptor<Dtype, true> ()),
        bwdd_bottom_diff  (new MklDnnMemoryDescriptor<Dtype, true> ()),
        bwdd_filter_data  (new MklDnnMemoryDescriptor<Dtype, false> ()),
        bwdf_top_diff     (new MklDnnMemoryDescriptor<Dtype, true> ()),
        bwdf_filter_diff  (new MklDnnMemoryDescriptor<Dtype, true> ()),
        bwdf_bottom_data  (new MklDnnMemoryDescriptor<Dtype, false> ()),
        bwdb_top_diff     (new MklDnnMemoryDescriptor<Dtype, true> ()),
        bwdb_bias_diff    (new MklDnnMemoryDescriptor<Dtype, true> ()),
        convolutionBwdBias(new MklDnnMemoryDescriptor<Dtype, false> ()) {}

template <typename Dtype>
void DnnConvolutionLayer<Dtype>::compute_output_shape() {
  this->height_out_ = (this->height_ + 2 * this->pad_h_ - this->kernel_h_)
      / this->stride_h_ + 1;
  this->width_out_ = (this->width_ + 2 * this->pad_w_ - this->kernel_w_)
      / this->stride_w_ + 1;
}

template <typename Dtype>
DnnConvolutionLayer<Dtype>::~DnnConvolutionLayer()
{
    dnnDelete<Dtype>(convolutionFwd);
    dnnDelete<Dtype>(convolutionBwdData);
    dnnDelete<Dtype>(convolutionBwdFilter);
    dnnDelete<Dtype>(convolutionBwdBias);
}

template <typename Dtype>
void DnnConvolutionLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  ConvolutionLayer<Dtype>::LayerSetUp(bottom, top);
  this->width_ = bottom[0]->width(); this->height_ = bottom[0]->height(); this->num_ = bottom[0]->num();
  compute_output_shape();
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;
  size_t kw, kh; /* filter */
  size_t dimension = 4;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_;

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_;

  kw = this->kernel_w_;
  kh = this->kernel_h_;

  size_t bdata_sizes[4] = {iw, ih, ic, n};
  size_t bdata_strides[4] = {1, iw, iw*ih, iw*ih*ic};

  size_t fdata_sizes[4] = {kw, kh, ic/g, oc};
  size_t fdata_strides[4]  = {1, kw, kw*kh, kw*kh*ic/g};

  size_t bias_sizes[1] = {oc};
  size_t bias_strides[1] = {1};

  size_t tdata_sizes[4] = {ow, oh, oc, n};
  size_t tdata_strides[4]  = {1, ow, ow*oh, ow*oh*oc};

  size_t convolutionStrides[2] = {this->stride_w_, this->stride_h_};
  int    inputOffset[2] = {-this->pad_w_, -this->pad_h_};

  if (g > 1)
  {
    status = dnnGroupsConvolutionCreateForwardBias<Dtype>(
      &convolutionFwd,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else
  {
    status = dnnConvolutionCreateForwardBias<Dtype>(
      &convolutionFwd,
      dnnAlgorithmConvolutionDirect,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }
  CHECK(status == 0) << "Failed dnnCreateConvolution<Dtype>(dnnForward) with status " << status << "\n"  ;

  status = dnnLayoutCreateFromPrimitive<Dtype>(&fwd_bottom_data->layout_int, convolutionFwd, dnnResourceSrc);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(&fwd_bottom_data->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&fwd_top_data->layout_int, convolutionFwd, dnnResourceDst);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(&fwd_top_data->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&fwd_filter_data->layout_int, convolutionFwd, dnnResourceFilter);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(&fwd_filter_data->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&fwd_bias_data->layout_int, convolutionFwd, dnnResourceBias);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(&fwd_bias_data->layout_int, ...) with status " << status << "\n";

  status = dnnLayoutCreate<Dtype>(&fwd_bottom_data->layout_usr, dimension, bdata_sizes, bdata_strides);
  CHECK(status == 0) << "Failed creation of l_fwd_bottom_data_usr layout with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&fwd_top_data->layout_usr   , dimension, tdata_sizes, tdata_strides);
  CHECK(status == 0) << "Failed creation of l_fwd_top_data_usr layout with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&fwd_filter_data->layout_usr, dimension, fdata_sizes, fdata_strides);
  CHECK(status == 0) << "Failed creation of l_fwd_filter_data_usr layout with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&fwd_bias_data->layout_usr  ,         1, bias_sizes , bias_strides );
  CHECK(status == 0) << "Failed creation of l_fwd_bias_data_usr layout with status " << status << "\n";

  fwd_bottom_data->create_conversions();
  fwd_top_data   ->create_conversions();
  fwd_filter_data->create_conversions();
  fwd_bias_data  ->create_conversions();

/*
 * Backward by data layer setup
 */
  if (g > 1)
  {
    status = dnnGroupsConvolutionCreateBackwardData<Dtype>(
      &convolutionBwdData,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else
  {
    status = dnnConvolutionCreateBackwardData<Dtype>(
      &convolutionBwdData,
      dnnAlgorithmConvolutionDirect,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }
  CHECK(status == 0) << "Failed dnnCreateConvolution<Dtype>(dnnBackwardData) with status " << status << "\n";

  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdd_bottom_diff->layout_int, convolutionBwdData, dnnResourceDiffSrc);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdd_bottom_diff->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdd_top_diff->layout_int   , convolutionBwdData, dnnResourceDiffDst);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdd_top_diff->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdd_filter_data->layout_int, convolutionBwdData, dnnResourceFilter);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdd_filter_data->layout_int, ...) with status " << status << "\n";

  status = dnnLayoutCreate<Dtype>(&bwdd_bottom_diff->layout_usr, dimension, bdata_sizes, bdata_strides);
  CHECK(status == 0) << "Failed creation of bwdd_bottom_diff->layout_usr with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&bwdd_top_diff->layout_usr   , dimension, tdata_sizes, tdata_strides);
  CHECK(status == 0) << "Failed creation of bwdd_top_diff->layout_usr with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&bwdd_filter_data->layout_usr, dimension, fdata_sizes, fdata_strides);
  CHECK(status == 0) << "Failed creation of bwdd_filter_data->layout_usr with status " << status << "\n";

  bwdd_bottom_diff->create_conversions();
  bwdd_top_diff->create_conversions();
  bwdd_filter_data->create_conversions();

/*
 * Backward by filter layer setup
 */

  if (g > 1)
  {
    status = dnnGroupsConvolutionCreateBackwardFilter<Dtype>(
      &convolutionBwdFilter,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else
  {
    status = dnnConvolutionCreateBackwardFilter<Dtype>(
      &convolutionBwdFilter,
      dnnAlgorithmConvolutionDirect,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }
  CHECK(status == 0) << "Failed dnnCreateConvolution<Dtype>(dnnBackwardFilter) with status " << status << "\n";

  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdf_bottom_data->layout_int, convolutionBwdFilter, dnnResourceSrc);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdf_bottom_data->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdf_top_diff->layout_int   , convolutionBwdFilter, dnnResourceDiffDst);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdf_top_diff->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdf_filter_diff->layout_int, convolutionBwdFilter, dnnResourceDiffFilter);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdf_filter_diff->layout_int, ...) with status " << status << "\n";

  status = dnnLayoutCreate<Dtype>(&bwdf_bottom_data->layout_usr, dimension, bdata_sizes, bdata_strides);
  CHECK(status == 0) << "Failed creation of bwdf_bottom_data->layout_usr with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&bwdf_top_diff->layout_usr   , dimension, tdata_sizes, tdata_strides);
  CHECK(status == 0) << "Failed creation of bwdf_top_diff->layout_usr with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&bwdf_filter_diff->layout_usr, dimension, fdata_sizes, fdata_strides);
  CHECK(status == 0) << "Failed creation of bwdf_filter_diff->layout_usr with status " << status << "\n";

  bwdf_bottom_data->create_conversions();
  bwdf_top_diff->create_conversions();
  bwdf_filter_diff->create_conversions();

/*
 * Backward by bias layer setup
 */
  if (g > 1)
  {
    status = dnnGroupsConvolutionCreateBackwardBias<Dtype>(
      &convolutionBwdBias,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      tdata_sizes);
  } else
  {
    status = dnnConvolutionCreateBackwardBias<Dtype>(
      &convolutionBwdBias,
      dnnAlgorithmConvolutionDirect,
      dimension,
      tdata_sizes);
  }
  CHECK(status == 0) << "Failed dnnCreateConvolution<Dtype>(dnnBackwardBias) with status " << status << "\n"  ;

  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdb_top_diff->layout_int , convolutionBwdBias, dnnResourceDiffDst);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdb_top_diff->layout_int, ...) with status " << status << "\n";
  status = dnnLayoutCreateFromPrimitive<Dtype>(&bwdb_bias_diff->layout_int, convolutionBwdBias, dnnResourceDiffBias);
  CHECK(status == 0) << "Failed dnnLayoutCreateFromPrimitive<Dtype>(bwdb_bias_diff->layout_int, ...) with status " << status << "\n";

  status = dnnLayoutCreate<Dtype>(&bwdb_top_diff->layout_usr , dimension, tdata_sizes, tdata_strides);
  CHECK(status == 0) << "Failed creation of bwdb_top_diff->layout_usr with status " << status << "\n";
  status = dnnLayoutCreate<Dtype>(&bwdb_bias_diff->layout_usr,         1, bias_sizes , bias_strides );
  CHECK(status == 0) << "Failed creation of bwdb_bias_diff->layout_usr with status " << status << "\n";

  bwdb_top_diff->create_conversions();
  bwdb_bias_diff->create_conversions();

  // Names are for debugging purposes only. TODO: Consider removing this.
  fwd_bottom_data    ->name = "fwd_bottom_data   @ " + this->layer_param_.name();
  fwd_top_data       ->name = "fwd_top_data      @ " + this->layer_param_.name();
  fwd_filter_data    ->name = "fwd_filter_data   @ " + this->layer_param_.name();
  fwd_bias_data      ->name = "fwd_bias_data     @ " + this->layer_param_.name();
  bwdd_top_diff      ->name = "bwdd_top_diff     @ " + this->layer_param_.name();
  bwdd_bottom_diff   ->name = "bwdd_bottom_diff  @ " + this->layer_param_.name();
  bwdd_filter_data   ->name = "bwdd_filter_data  @ " + this->layer_param_.name();
  bwdf_top_diff      ->name = "bwdf_top_diff     @ " + this->layer_param_.name();
  bwdf_bottom_data   ->name = "bwdf_bottom_data  @ " + this->layer_param_.name();
  bwdf_filter_diff   ->name = "bwdf_filter_diff  @ " + this->layer_param_.name();
  bwdb_top_diff      ->name = "bwdb_top_diff     @ " + this->layer_param_.name();
  bwdb_bias_diff     ->name = "bwdb_bias_diff    @ " + this->layer_param_.name();
}

template <typename Dtype, bool is_diff>
void MklDnnMemoryDescriptor<Dtype, is_diff>::convert_from_prv(void* prv_ptr, void* cpu_ptr)
{
  CHECK(prv_ptr);
  CHECK(cpu_ptr);

  int status;
  void *convert_resources[dnnResourceNumber];

  DLOG(INFO) << "convert priv =>      from "  << this->name;

  convert_resources[dnnResourceFrom] = (void *)prv_ptr;
  convert_resources[dnnResourceTo]   = (void *)cpu_ptr;
  status = dnnExecute<Dtype>(this->convert_from_int, convert_resources);
  CHECK(status == 0) << "[8] | Conversion from prv failed with status " << status;
}

template <typename Dtype, bool is_diff>
Dtype* MklDnnMemoryDescriptor<Dtype, is_diff>::get_converted_prv(Blob<Dtype>* blob, bool test_prv_layout)
{
  if (this->convert_to_int)
  {
    int status;
    void *convert_resources[dnnResourceNumber];
    const Dtype* prv_ptr = is_diff ?  blob->prv_diff() : blob->prv_data();
    if(prv_ptr == NULL)
    {
      DLOG(INFO) << "convert      => priv for  " << this->name;

      convert_resources[dnnResourceFrom] = is_diff ? (void *) blob->cpu_diff() : (void *) blob->cpu_data();
      convert_resources[dnnResourceTo]   = (void *)this->internal_ptr;

      status = dnnExecute<Dtype>(this->convert_to_int, convert_resources);
      CHECK(status == 0) << "Conversion failed with status " << status;

      if(is_diff)
      {
        blob->set_prv_diff(this->internal_ptr, true);
        blob->set_prv_descriptor_diff(get_shared());
      }
      else
      {
        blob->set_prv_data(this->internal_ptr, true);
        blob->set_prv_descriptor_data(get_shared());
      }

      return this->internal_ptr;
    }
    else if (test_prv_layout)
    {
      // This section helps if padding needs to be added (or removed...)
      // TODO: consider removing when no longer needed.

      shared_ptr<MklDnnMemoryDescriptor<Dtype, is_diff> > current_descr =
        is_diff ?  boost::static_pointer_cast<MklDnnMemoryDescriptor<Dtype, is_diff> > (blob->get_prv_descriptor_diff())
        : boost::static_pointer_cast<MklDnnMemoryDescriptor<Dtype, is_diff> > (blob->get_prv_descriptor_data());

      if(!dnnLayoutCompare<Dtype>(current_descr->layout_int , this->layout_int))
      {
        DLOG(INFO) << "convert priv => priv from " << current_descr->name << " to " << this->name;

        dnnPrimitive_t convert_padding;
        status = dnnConversionCreate<Dtype>(&convert_padding, current_descr->layout_int , this->layout_int);
        //CHECK(status == 0) << "Failed creation convert_padding with status " << status << "\n";
        if(status != 0)
        { // TODO: Very weird that we end up here for conv1. No idea why....
          DLOG(INFO) << "!!!! Failed creation convert_padding with status " << status << "\n";
          convert_resources[dnnResourceFrom] = is_diff ? (void *) blob->cpu_diff() : (void *) blob->cpu_data();
          convert_resources[dnnResourceTo]   = (void *)this->internal_ptr;

          status = dnnExecute<Dtype>(this->convert_to_int, convert_resources);
          CHECK(status == 0) << "Conversion failed with status " << status;

        } else {
          convert_resources[dnnResourceFrom] = is_diff ? (void *) blob->prv_diff() : (void *) blob->prv_data();
          convert_resources[dnnResourceTo]   = (void *) this->internal_ptr;
          status = dnnExecute<Dtype>(convert_padding, convert_resources);
          CHECK(status == 0) << "Conversion failed with status " << status;
          dnnDelete<Dtype>(convert_padding);
        }

        if(is_diff)
        {
          blob->set_prv_diff(this->internal_ptr, true);
          blob->set_prv_descriptor_diff(get_shared());
        }
        else
        {
          blob->set_prv_data(this->internal_ptr, true);
          blob->set_prv_descriptor_data(get_shared());
        }

        return this->internal_ptr;
      }
      else if(current_descr.get() != this) {
        DLOG(INFO) << "layout OK                 " << current_descr->name << " == " << this->name;
      }
    }

    return (Dtype*) prv_ptr;
  }

  return (is_diff ? (Dtype*) blob->cpu_diff() : (Dtype*) blob->cpu_data());
}


template <typename Dtype>
void DnnConvolutionLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top)
{
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n) << "Inclompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Inclompatible shape of bottom with layer";


  void *res_convolutionFwd[dnnResourceNumber];
  res_convolutionFwd[dnnResourceSrc]    = fwd_bottom_data->get_converted_prv(bottom[0], true);
  res_convolutionFwd[dnnResourceFilter] = fwd_filter_data->get_converted_prv(this->blobs_[0].get(), true);
  res_convolutionFwd[dnnResourceBias]   = fwd_bias_data  ->get_converted_prv(this->blobs_[1].get(), false);

  if (fwd_top_data->convert_from_int)
  {
    top[0]->set_prv_data(fwd_top_data->internal_ptr, false);
    top[0]->set_prv_descriptor_data(fwd_top_data);
    res_convolutionFwd[dnnResourceDst] = (void *)fwd_top_data->internal_ptr;
  }
  else
    res_convolutionFwd[dnnResourceDst] = top[0]->mutable_cpu_data();

  status = dnnExecute<Dtype>( convolutionFwd, res_convolutionFwd);
  CHECK(status == 0) << "[7] | Forward conv failed with status " << status;

}

template <typename Dtype>
void DnnConvolutionLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom)
{
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n) << "Incompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Incompatible shape of bottom with layer";

  if (propagate_down[0])
  {
    void *res_convolutionBwdData[dnnResourceNumber];

    res_convolutionBwdData[dnnResourceDiffDst] = bwdd_top_diff->get_converted_prv(top[0], true);
    res_convolutionBwdData[dnnResourceFilter]  = bwdd_filter_data->get_converted_prv(this->blobs_[0].get(), true);

    if (bwdd_bottom_diff->convert_from_int)
    {
      bottom[0]->set_prv_diff(bwdd_bottom_diff->internal_ptr, false);
      bottom[0]->set_prv_descriptor_diff(bwdd_bottom_diff);
      res_convolutionBwdData[dnnResourceDiffSrc] = (void *)bwdd_bottom_diff->internal_ptr;
    }
    else
      res_convolutionBwdData[dnnResourceDiffSrc] = bottom[0]->mutable_cpu_diff();

    status = dnnExecute<Dtype>( convolutionBwdData, res_convolutionBwdData);
    CHECK(status == 0) << "[6] | Backward Data conv failed with status " << status;

  }

  if (this->param_propagate_down(0))
  {
    void *res_convolutionBwdFilter[dnnResourceNumber];

    res_convolutionBwdFilter[dnnResourceDiffDst] = bwdf_top_diff->get_converted_prv(top[0], true);
    res_convolutionBwdFilter[dnnResourceSrc] = bwdf_bottom_data->get_converted_prv(bottom[0], true);

    if (bwdf_filter_diff->convert_from_int)
    {
      this->blobs_[0]->set_prv_diff(bwdf_filter_diff->internal_ptr, false);
      this->blobs_[0]->set_prv_descriptor_diff(bwdf_filter_diff);

      res_convolutionBwdFilter[dnnResourceDiffFilter] = (void*) bwdf_filter_diff->internal_ptr;

    }
    else
      res_convolutionBwdFilter[dnnResourceDiffFilter] = this->blobs_[0]->mutable_cpu_diff();

    status = dnnExecute<Dtype>( convolutionBwdFilter, res_convolutionBwdFilter);
    CHECK(status == 0) << "[6] | Backward Filter conv failed with status " << status;

  }

  if (this->param_propagate_down(1))
  {
    void *res_convolutionBwdBias[dnnResourceNumber];

    res_convolutionBwdBias[dnnResourceDiffDst] = bwdb_top_diff->get_converted_prv(top[0], true);

    if (bwdb_bias_diff->convert_from_int)
    {
      this->blobs_[1]->set_prv_diff(bwdb_bias_diff->internal_ptr, false);
      this->blobs_[1]->set_prv_descriptor_diff(bwdb_bias_diff);
      res_convolutionBwdBias[dnnResourceDiffBias] = bwdb_bias_diff->internal_ptr;
    }
    else
      res_convolutionBwdBias[dnnResourceDiffBias] = (void *)this->blobs_[1]->mutable_cpu_diff();;

    status = dnnExecute<Dtype>( convolutionBwdBias, res_convolutionBwdBias);
    CHECK(status == 0) << "[4] | Backward Bias conv failed with status " << status;

  }
}

#ifdef CPU_ONLY
STUB_GPU(DnnConvolutionLayer);
#endif

INSTANTIATE_CLASS(DnnConvolutionLayer);
REGISTER_LAYER_CLASS(DnnConvolution);
}  // namespace caffe
