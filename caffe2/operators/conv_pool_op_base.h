#ifndef CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_
#define CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_

#include "caffe2/core/context.h"
#include "caffe2/core/operator.h"
#include "caffe2/proto/caffe2_legacy.pb.h"
#include "caffe2/utils/math.h"
#include "caffe2/core/logging.h"

// This macro is here just to allow us to experiment with padding values that
// determines, when we have an odd number of pads, which side gets the one
// additional pad value, the head side, or the tail side. Setting it to false
// will enable the TensorFlow behavior, and setting it to true will enable
// a behavior more consistent with Caffe and CuDNN.
// This only affects the case when you set legacy pad to VALID or SAME. The
// behavior inherits from the early designs of Google's CNN implementation,
// where padding values are implicitly calculated instead of explicitly
// specified. This is still the case with TensorFlow. Many frameworks have
// followed a slightly different approach of explicitly giving padding values,
// in which case the value of this constant value does not matter.
const bool CAFFE2_PAD_HEAD_MORE = false;

namespace caffe2 {

template <class Context>
class ConvPoolOpBase : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  ConvPoolOpBase(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        pad_(OperatorBase::GetSingleArgument<int>("pad", 0)),
        pad_t_(OperatorBase::GetSingleArgument<int>("pad_t", pad_)),
        pad_l_(OperatorBase::GetSingleArgument<int>("pad_l", pad_)),
        pad_b_(OperatorBase::GetSingleArgument<int>("pad_b", pad_)),
        pad_r_(OperatorBase::GetSingleArgument<int>("pad_r", pad_)),
        legacy_pad_(
            static_cast<LegacyPadding>(OperatorBase::GetSingleArgument<int>(
                "legacy_pad",
                LegacyPadding::NOTSET))),
        kernel_h_(OperatorBase::GetSingleArgument<int>(
            "kernel_h",
            OperatorBase::GetSingleArgument<int>("kernel", 0))),
        kernel_w_(OperatorBase::GetSingleArgument<int>(
            "kernel_w",
            OperatorBase::GetSingleArgument<int>("kernel", 0))),
        stride_h_(OperatorBase::GetSingleArgument<int>(
            "stride_h",
            OperatorBase::GetSingleArgument<int>("stride", 1))),
        stride_w_(OperatorBase::GetSingleArgument<int>(
            "stride_w",
            OperatorBase::GetSingleArgument<int>("stride", 1))),
        order_(StringToStorageOrder(
            OperatorBase::GetSingleArgument<string>("order", "NCHW"))) {
    CHECK_GT(kernel_h_, 0);
    CHECK_GT(kernel_w_, 0);
    // For the padding, they should either be the legacy padding strategy
    // (VALID or SAME), or an explicit, non-negative value.
    if (legacy_pad_ == LegacyPadding::VALID ||
        legacy_pad_ == LegacyPadding::SAME) {
      CHECK(!OperatorBase::HasArgument("pad") &&
            !OperatorBase::HasArgument("pad_t") &&
            !OperatorBase::HasArgument("pad_l") &&
            !OperatorBase::HasArgument("pad_b") &&
            !OperatorBase::HasArgument("pad_r"))
          << "If you use legacy padding VALID or SAME, you should not specify "
             "any specific padding values.";
    }

    CHECK_LE(stride_h_, kernel_h_);
    CHECK_LE(stride_w_, kernel_h_);
    CHECK_GE(pad_, 0);
    CHECK_GE(pad_t_, 0);
    CHECK_GE(pad_l_, 0);
    CHECK_GE(pad_b_, 0);
    CHECK_GE(pad_r_, 0);
    CHECK_GT(stride_h_, 0);
    CHECK_GT(stride_w_, 0);
  }

  // Sets the output size. The output channel is manually provided since
  // it may not be identical to the input channels.
  // This function can be used in the forward functions to obtain the output
  // sizes.
  void SetOutputSize(const Tensor<Context>& input,
                     Tensor<Context>* output,
                     int output_channel) {
    DCHECK_EQ(input.ndim(), 4);
    DCHECK_GT(input.size(), 0);
    int N = input.dim32(0);
    bool channel_first = false;  // initialized to suppress compiler warning.
    int H = 0, W = 0;  // initialized to suppress compiler warning.
    switch (order_) {
    case StorageOrder::NHWC:
      channel_first = false;
      H = input.dim32(1);
      W = input.dim32(2);
      break;
    case StorageOrder::NCHW:
      // Old Caffe order.
      channel_first = true;
      H = input.dim32(2);
      W = input.dim32(3);
      break;
    default:
      LOG(FATAL) << "Unknown Storage order: " << order_;
    }
    CHECK_GE(H, kernel_h_);
    CHECK_GE(W, kernel_w_);
    int output_height = 0, output_width = 0;
    ComputeSizeAndPad(H, stride_h_, kernel_h_,
                      &pad_t_, &pad_b_, &output_height);
    ComputeSizeAndPad(W, stride_w_, kernel_w_,
                      &pad_l_, &pad_r_, &output_width);
    if (channel_first) {
      output->Resize(N, output_channel, output_height, output_width);
    } else {
      output->Resize(N, output_height, output_width, output_channel);
    }
    //VLOG(2) << "In: N " << N << " C " << C << " H " << H << " W " << W;
    //VLOG(2) << "Out: C " << output_channel << " H " << output_height
    //        << " W " << output_width;
  }

  // ComputePads could be used in backward functions to figure out the padding
  // values for the given input.
  void ComputePads(const int height, const int width) {
    if (legacy_pad_ != LegacyPadding::NOTSET) {
      int output_unused;
      ComputeSizeAndPad(height, stride_h_, kernel_h_,
                        &pad_t_, &pad_b_, &output_unused);
      ComputeSizeAndPad(width, stride_w_, kernel_w_,
                        &pad_l_, &pad_r_, &output_unused);
    }
  }

  bool RunOnDevice() override {
    switch (order_) {
    case StorageOrder::NHWC:
      //VLOG(2) << "Running NHWC";
      return RunOnDeviceWithOrderNHWC();
    case StorageOrder::NCHW:
      //VLOG(2) << "Running NCHW";
      return RunOnDeviceWithOrderNCHW();
    default:
      LOG(FATAL) << "Unknown storage order: " << order_;
    }
    // To suppress old compiler warnings
    return true;
  }

  // The actual function that does the computation, if the different
  // storage order leads to different implementations.
  virtual bool RunOnDeviceWithOrderNHWC() {
    CAFFE_NOT_IMPLEMENTED;
    return false;
  }
  virtual bool RunOnDeviceWithOrderNCHW() {
    CAFFE_NOT_IMPLEMENTED;
    return false;
  }

  virtual ~ConvPoolOpBase() {}

 private:
  // I put this private section before protected because these variables are
  // going to be initialized before pad_t_ et al. However, a derivative class
  // should never use these values. They should refer to pad_t et al. for the
  // exact padding values. This isolates out the padding scheme that are growing
  // unfortunately complex due to implementational differences from different
  // frameworks.
  int pad_;

 protected:
  int pad_t_;
  int pad_l_;
  int pad_b_;
  int pad_r_;
  LegacyPadding legacy_pad_;
  int kernel_h_;
  int kernel_w_;
  int stride_h_;
  int stride_w_;
  StorageOrder order_;

  inline void ComputeSizeAndPad(
      const int in_size, const int stride, const int kernel,
      int* pad_head, int* pad_tail, int* out_size) {
    switch (legacy_pad_) {
      case LegacyPadding::NOTSET:
        // We will just use the direct padding head and tail values, but we
        // will verify that they are non-negative.
        CHECK_GE(*pad_head, 0);
        CHECK_GE(*pad_tail, 0);
        *out_size = static_cast<int>(
            static_cast<float>(in_size + *pad_head + *pad_tail - kernel) / stride
            + 1);
        break;
      case LegacyPadding::VALID:
        *pad_head = 0;
        *pad_tail = 0;
        *out_size = (in_size - kernel) / stride + 1;
        break;
      case LegacyPadding::SAME:
      {
        int legacy_target_size = (in_size + stride - 1) / stride;
        int pad_needed = (legacy_target_size - 1) * stride + kernel - in_size;
        if (CAFFE2_PAD_HEAD_MORE) {
          *pad_head = (pad_needed + 1) / 2;
        } else {
          *pad_head = pad_needed / 2;
        }
        *pad_tail = pad_needed - *pad_head;
        *out_size = (in_size + pad_needed - kernel) / stride + 1;
        break;
      }
      case LegacyPadding::CAFFE_LEGACY_POOLING:
        // This is in order to adapt Caffe's pooling padding case. In this case,
        // we will only use pad_head and will compute pad_tail to match the
        // old caffe pooling strategy. Also see caffe2_legacy.proto for more
        // details.
        CHECK_GE(*pad_head, 0);
        // Here, notice that caffe casts UP while caffe2 casts DOWN for the
        // output size computation.
        *out_size = std::ceil(
            static_cast<float>(in_size + *pad_head * 2 - kernel) / stride + 1);
        // If we have padding, caffe also ensures that the last pooling starts
        // strictly inside the image (instead of at the padding); otherwise clip
        // the last.
        if (*pad_head > 0 && (*out_size - 1) * stride >= in_size + *pad_head) {
          --*out_size;
        }
        // Now, compare the output size with the standard Caffe2 output size.
        // The
        // caffe2 standard output size should always be no larger than the
        // output
        // size of caffe.
        int standard_out_size = static_cast<int>(
            static_cast<float>(in_size + *pad_head * 2 - kernel) / stride + 1);
        CHECK_GE(*out_size, standard_out_size)
            << "This should never happen. If this happens, double check the logic "
            << "above.";
        if (*out_size > standard_out_size) {
          LOG(WARNING)
              << "You are hitting a case where Caffe's legacy padding calculation "
                 "is hit. This leads to inefficient and sometimes incorrect "
                 "results. We are keeping this behavior for backward compatibility"
                 ", but you are strongly recommended to move away from it. The "
                 "operator that generates this warning is: "
              << ProtoDebugString(def());
        }
        *pad_tail = *pad_head + stride * (*out_size - standard_out_size);
        break;
    }
  }

 private:
};

#define USE_CONV_POOL_BASE_FUNCTIONS                                           \
  USE_OPERATOR_CONTEXT_FUNCTIONS;                                              \
  using ConvPoolOpBase<Context>::pad_t_;                                       \
  using ConvPoolOpBase<Context>::pad_l_;                                       \
  using ConvPoolOpBase<Context>::pad_b_;                                       \
  using ConvPoolOpBase<Context>::pad_r_;                                       \
  using ConvPoolOpBase<Context>::kernel_h_;                                    \
  using ConvPoolOpBase<Context>::kernel_w_;                                    \
  using ConvPoolOpBase<Context>::stride_h_;                                    \
  using ConvPoolOpBase<Context>::stride_w_;                                    \
  using ConvPoolOpBase<Context>::order_

}  // namespace caffe2

#endif  // CAFFE2_OPERATORS_CONV_POOL_OP_BASE_H_
