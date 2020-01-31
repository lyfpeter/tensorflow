/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");

You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <algorithm>
#include <vector>

#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/nn_ops.h"
#include "tensorflow/cc/ops/nn_ops_internal.h"
#include "tensorflow/cc/ops/standard_ops.h"

#include "tensorflow/examples/cc/gan/dcgan/nn_ops_rkz.h"
#include "tensorflow/examples/cc/gan/dcgan/util.h"

namespace tensorflow {
namespace ops {

TFVariable::TFVariable(const ::tensorflow::Scope& scope,
                       PartialTensorShape shape, DataType dtype,
                       const Variable::Attrs& attrs, bool trainable) {
  this->output = Variable(scope, shape, dtype, attrs);

  if (trainable) scope.AddTrainableVariable(this->output, shape);
}

TFVariable::TFVariable(const ::tensorflow::Scope& scope,
                       PartialTensorShape shape, DataType dtype, bool trainable)
    : TFVariable(scope, shape, dtype, Variable::Attrs(), trainable) {}

TFAssign::TFAssign(const ::tensorflow::Scope& scope, ::tensorflow::Input ref,
                   ::tensorflow::Input value, const Assign::Attrs& attrs) {
  this->output = Assign(scope, ref, value, attrs);

  scope.AddAssignOp(this->output);
}

TFAssign::TFAssign(const ::tensorflow::Scope& scope, ::tensorflow::Input ref,
                   ::tensorflow::Input value)
    : TFAssign(scope, ref, value, Assign::Attrs()) {}

Moments::Moments(const ::tensorflow::Scope& scope, const ::tensorflow::Input& x,
                 const std::initializer_list<int>& axes, bool keep_dims) {
  auto m = ReduceMean(scope, x, Input(axes), ReduceMean::KeepDims(true));

  auto sg = StopGradient(scope, m);
  auto sd = SquaredDifference(scope, x, sg);
  auto v = ReduceMean(scope, sd, Input(axes), ReduceMean::KeepDims(true));

  if (keep_dims) {
    this->mean = m;
    this->variance = v;
  } else {
    this->mean = Squeeze(scope, m, Squeeze::Axis(axes));
    this->variance = Squeeze(scope, v, Squeeze::Axis(axes));
  }
}

// tf.nn.batch_normalization
// def batch_normalization(x,
//                         mean,
//                         variance,
//                         offset,
//                         scale,
//                         variance_epsilon,
//                         name=None):
//   with ops.name_scope(name, "batchnorm", [x, mean, variance, scale, offset]):
//     inv = math_ops.rsqrt(variance + variance_epsilon)
//     if scale is not None:
//       inv *= scale
//     # Note: tensorflow/contrib/quantize/python/fold_batch_norms.py depends on
//     # the precise order of ops that are generated by the expression below.
//     return x * math_ops.cast(inv, x.dtype) + math_ops.cast(
//         offset - mean * inv if offset is not None else -mean * inv, x.dtype)
BatchNormalization::BatchNormalization(
    const ::tensorflow::Scope& scope, const ::tensorflow::Input& x,
    const ::tensorflow::Input& mean, const ::tensorflow::Input& variance,
    const ::tensorflow::Input& offset, const ::tensorflow::Input& scale,
    const ::tensorflow::Input& variance_epsilon) {
  auto inv = Multiply(
      scope, Rsqrt(scope, Add(scope, variance, variance_epsilon)), scale);
  LOG(INFO) << "Node building status: " << scope.status();

  auto tmp1 = Multiply(scope, x, Cast(scope, inv, DT_FLOAT));
  LOG(INFO) << "Node building status: " << scope.status();

  auto tmp2 = Multiply(scope, mean, inv);
  LOG(INFO) << "Node building status: " << scope.status();

  this->output =
      Add(scope, tmp1, Cast(scope, Sub(scope, offset, tmp2), DT_FLOAT));
}

Dropout::Dropout(const ::tensorflow::Scope& scope, const ::tensorflow::Input x,
                 const int rate) {
  float keep_prob = 1 - rate;
  auto random_value5 = RandomUniform(scope, Shape(scope, x), DT_FLOAT);
  LOG(INFO) << "Node building status: " << scope.status();

  auto random_tensor =
      Add(scope, random_value5, Const<float>(scope, {keep_prob}));
  LOG(INFO) << "Node building status: " << scope.status();

  auto binary_tensor = Floor(scope, random_tensor);
  LOG(INFO) << "Node building status: " << scope.status();

  this->output = Multiply(
      scope, Div(scope, x, Const<float>(scope, {keep_prob})), binary_tensor);
}

// python code:
//     # The logistic loss formula from above is
//     #   x - x * z + log(1 + exp(-x))
//     # For x < 0, a more numerically stable formula is
//     #   -x * z + log(1 + exp(x))
//     # Note that these two expressions can be combined into the following:
//     #   max(x, 0) - x * z + log(1 + exp(-abs(x)))
//     # To allow computing gradients at zero, we define custom versions of max
//     and # abs functions. zeros = array_ops.zeros_like(logits,
//     dtype=logits.dtype) cond = (logits >= zeros) relu_logits =
//     array_ops.where(cond, logits, zeros) neg_abs_logits =
//     array_ops.where(cond, -logits, logits) return math_ops.add(
//         relu_logits - logits * labels,
//         math_ops.log1p(math_ops.exp(neg_abs_logits)),
//         name=name)
SigmoidCrossEntropyWithLogits::SigmoidCrossEntropyWithLogits(
    const ::tensorflow::Scope& scope, const ::tensorflow::Input labels,
    const ::tensorflow::Input logits) {
  auto zeros = ZerosLike(scope, logits);
  auto cond = GreaterEqual(scope, logits, zeros);
  auto relu_logits = SelectV2(scope, cond, logits, zeros);
  auto neg_abs_logits = SelectV2(scope, cond, Negate(scope, logits), logits);

  this->output =
      Add(scope, Sub(scope, relu_logits, Multiply(scope, logits, labels)),
          Log1p(scope, Exp(scope, neg_abs_logits)));
}

// Only DT_FLOAT and 2D/4D shape is supported for now
GlorotUniform::GlorotUniform(const ::tensorflow::Scope& scope,
                             const std::initializer_list<int64>& shape) {
  // RandomUniform
  auto random_value =
      RandomUniform(scope, Const(scope, Input::Initializer(shape)), DT_FLOAT);
  LOG(INFO) << "Node building status: " << scope.status();

  std::vector<int64> shape_vec(shape);

  // For 2D
  float fan_in = shape_vec[0];
  float fan_out = shape_vec[1];

  // For 4D
  if (shape_vec.size() == 4) {
    float receptive_field_size = 1.0f * shape_vec[0] * shape_vec[1];
    fan_in = receptive_field_size * shape_vec[2];
    fan_out = receptive_field_size * shape_vec[3];
  }

  // Python code:
  //   scale /= max(1., (fan_in + fan_out) / 2.)
  //   limit = math.sqrt(3.0 * scale) => minval is -limit, maxval is limit
  //   result = math_ops.add(rnd * (maxval - minval), minval, name=name)
  float scale = 1.0f / std::max(1.0f, (fan_in + fan_out) / 2.0f);
  float limit = std::sqrt(3.0f * scale);
  float maxval = limit;
  float minval = -limit;
  auto result =
      Add(scope,
          Multiply(scope, random_value, Const<float>(scope, (maxval - minval))),
          Const<float>(scope, minval));
  LOG(INFO) << "Node building status: " << scope.status();

  this->output = result;
}

// Conv2DTranspose
Conv2DTranspose::Conv2DTranspose(const ::tensorflow::Scope& scope,
                                 const ::tensorflow::Input& input_sizes,
                                 const ::tensorflow::Input& filter,
                                 const ::tensorflow::Input& out_backprop,
                                 const gtl::ArraySlice<int>& strides,
                                 const StringPiece padding) {
  // Conv2DBackpropInput
  this->output = Conv2DBackpropInput(scope, input_sizes, filter, out_backprop,
                                     strides, padding);
}

TFBatchNormalization::TFBatchNormalization(const ::tensorflow::Scope& scope,
                                           const PartialTensorShape& shape) {
  // moving mean and variance
  this->moving_mean = Variable(scope, shape, DT_FLOAT);
  TFAssign(scope, this->moving_mean, ZerosLike(scope, this->moving_mean));

  this->moving_variance = Variable(scope, shape, DT_FLOAT);
  TFAssign(scope, this->moving_variance,
           ZerosLike(scope, this->moving_variance));

  // gamma
  this->gamma = TFVariable(scope.WithOpName("gamma"), shape, DT_FLOAT, true);
  TFAssign(scope, this->gamma, OnesLike(scope, this->gamma));
  LOG(INFO) << "Node building status: " << scope.status();

  // beta
  this->beta = TFVariable(scope.WithOpName("beta"), shape, DT_FLOAT, true);
  TFAssign(scope, this->beta, ZerosLike(scope, this->beta));
  LOG(INFO) << "Node building status: " << scope.status();
}

Output TFBatchNormalization::Build(const ::tensorflow::Scope& scope,
                                   const ::tensorflow::Input& x,
                                   const std::initializer_list<int>& axes,
                                   const ::tensorflow::Input& variance_epsilon,
                                   bool training) {
  Output mean;
  Output variance;

  if (training) {
    // mean and variance
    auto moments = Moments(scope, x, axes, false);
    mean = moments.mean;
    variance = moments.variance;

    // update ops
    auto decay = Const<float>(scope, 1.0f - MOMENTUM, {});
    auto update_delta1 =
        Multiply(scope, Sub(scope, this->moving_mean, mean), decay);
    auto update_moving_mean = AssignSub(scope.WithOpName("update_moving_mean"),
                                        this->moving_mean, update_delta1);
    scope.AddUpdateOp(update_moving_mean);
    LOG(INFO) << "Node building status: " << scope.status();

    auto update_delta2 =
        Multiply(scope, Sub(scope, this->moving_variance, variance), decay);
    auto update_moving_variance =
        AssignSub(scope.WithOpName("update_moving_variance"),
                  this->moving_variance, update_delta2);
    scope.AddUpdateOp(update_moving_variance);
    LOG(INFO) << "Node building status: " << scope.status();
  } else {
    mean = this->moving_mean;
    variance = this->moving_variance;
  }

  // output
  return BatchNormalization(scope, x, mean, variance, this->beta, this->gamma,
                            variance_epsilon);
}

TFFusedBatchNorm::TFFusedBatchNorm(const ::tensorflow::Scope& scope,
                                   const PartialTensorShape& shape) {
  // moving mean and variance
  this->moving_mean = Variable(scope, shape, DT_FLOAT);
  TFAssign(scope, this->moving_mean, ZerosLike(scope, this->moving_mean));

  this->moving_variance = Variable(scope, shape, DT_FLOAT);
  TFAssign(scope, this->moving_variance,
           ZerosLike(scope, this->moving_variance));

  // gamma
  this->gamma =
      TFVariable(scope.WithOpName("fused_gamma"), shape, DT_FLOAT, true);
  TFAssign(scope, this->gamma, OnesLike(scope, this->gamma));
  LOG(INFO) << "Node building status: " << scope.status();

  // beta
  this->beta =
      TFVariable(scope.WithOpName("fused_beta"), shape, DT_FLOAT, true);
  TFAssign(scope, this->beta, ZerosLike(scope, this->beta));
  LOG(INFO) << "Node building status: " << scope.status();
}

Output TFFusedBatchNorm::Build(const ::tensorflow::Scope& scope,
                               const ::tensorflow::Input& x,
                               const float variance_epsilon, bool training) {
  if (training) {
    // mean and variance
    auto mean = Const<float>(scope, {});
    auto variance = Const<float>(scope, {});

    auto fused_batch_norm =
        FusedBatchNorm(scope, x, this->gamma, this->beta, mean, variance,
                       FusedBatchNorm::Epsilon(variance_epsilon));

    // update ops
    auto decay = Const<float>(scope, 1.0f - MOMENTUM, {});
    auto update_delta1 = Multiply(
        scope, Sub(scope, this->moving_mean, fused_batch_norm.batch_mean),
        decay);
    auto update_moving_mean =
        AssignSub(scope.WithOpName("fused_update_moving_mean"),
                  this->moving_mean, update_delta1);
    scope.AddUpdateOp(update_moving_mean);
    LOG(INFO) << "Node building status: " << scope.status();

    auto update_delta2 = Multiply(
        scope,
        Sub(scope, this->moving_variance, fused_batch_norm.batch_variance),
        decay);
    auto update_moving_variance =
        AssignSub(scope.WithOpName("fused_update_moving_variance"),
                  this->moving_variance, update_delta2);
    scope.AddUpdateOp(update_moving_variance);
    LOG(INFO) << "Node building status: " << scope.status();

    return fused_batch_norm.y;
  } else {
    auto mean = this->moving_mean;
    auto variance = this->moving_variance;

    auto fused_batch_norm = FusedBatchNorm(
        scope, x, this->gamma, this->beta, mean, variance,
        FusedBatchNorm::Epsilon(variance_epsilon).IsTraining(false));

    return fused_batch_norm.y;
  }
}

// Generator constructor to set variables and assigns
Generator::Generator(const ::tensorflow::Scope& scope) {
  this->w1 = TFVariable(scope.WithOpName("weight"), {NOISE_DIM, UNITS},
                        DT_FLOAT, true);
  LOG(INFO) << "Node building status: " << scope.status();

  auto rate = Const(scope, {0.01f});
  auto random_value = RandomNormal(scope, {NOISE_DIM, UNITS}, DT_FLOAT);
  TFAssign(scope, w1, Multiply(scope, random_value, rate));

  // filter, aka kernel
  this->filter =
      TFVariable(scope.WithOpName("filter"), {5, 5, 128, 256}, DT_FLOAT, true);
  auto random_value1 = GlorotUniform(scope, {5, 5, 128, 256});
  TFAssign(scope, filter, random_value1);

  // filter, aka kernel
  this->filter2 =
      TFVariable(scope.WithOpName("filter2"), {5, 5, 64, 128}, DT_FLOAT, true);
  auto random_value2 = GlorotUniform(scope, {5, 5, 64, 128});
  TFAssign(scope, filter2, random_value2);

  // filter, aka kernel
  this->filter3 = TFVariable(scope.WithOpName("filter3"),
                             {5, 5, NUM_CHANNELS, 64}, DT_FLOAT, true);
  auto random_value3 = GlorotUniform(scope, {5, 5, NUM_CHANNELS, 64});
  TFAssign(scope, filter3, random_value3);

  this->batchnorm_op = TFBatchNormalization(scope, {UNITS});
  this->batchnorm1_op = TFFusedBatchNorm(scope, {128});
  this->batchnorm2_op = TFFusedBatchNorm(scope, {64});
}

// Build model
Output Generator::Build(const ::tensorflow::Scope& scope, const int batch_size,
                        bool training) {
  // random noise input
  auto noise = RandomNormal(scope, {batch_size, NOISE_DIM}, DT_FLOAT);
  LOG(INFO) << "Node building status: " << scope.status();

  // dense 1
  auto dense = MatMul(scope, noise, this->w1);
  LOG(INFO) << "Node building status: " << scope.status();

  // BatchNormalization
  auto variance_epsilon = Const<float>(scope, {0.001f});
  auto batchnorm =
      this->batchnorm_op.Build(scope, dense, {0}, variance_epsilon, training);
  LOG(INFO) << "Node building status: " << scope.status();

  // LeakyReLU
  auto leakyrelu =
      internal::LeakyRelu(scope, batchnorm, internal::LeakyRelu::Alpha(0.3f));
  LOG(INFO) << "Node building status: " << scope.status();

  // Reshape
  auto reshape1 = Reshape(scope, leakyrelu, {batch_size, 7, 7, 256});
  LOG(INFO) << "Node building status: " << scope.status();

  // Conv2DTranspose 1
  auto input_sizes = Const<int>(scope, {batch_size, 7, 7, 128});

  // out_backprop, aka input. here it's reshape1
  auto deconv1 = Conv2DTranspose(scope, input_sizes, this->filter, reshape1,
                                 {1, 1, 1, 1}, "SAME");
  LOG(INFO) << "Node building status: " << scope.status();

  // BatchNormalization 1, use FusedBatchNorm
  auto batchnorm1 = this->batchnorm1_op.Build(scope, deconv1, 0.001f, training);
  LOG(INFO) << "Node building status: " << scope.status();

  // LeakyReLU 1
  auto leakyrelu1 =
      internal::LeakyRelu(scope, batchnorm1, internal::LeakyRelu::Alpha(0.3f));
  LOG(INFO) << "Node building status: " << scope.status();

  // Conv2DTranspose 2
  auto input_sizes2 = Const(scope, {batch_size, 14, 14, 64});

  auto deconv2 = Conv2DTranspose(scope, input_sizes2, this->filter2, leakyrelu1,
                                 {1, 2, 2, 1}, "SAME");
  LOG(INFO) << "Node building status: " << scope.status();

  // BatchNormalization 2, use FusedBatchNorm
  auto offset2 = BroadcastTo(scope, 0.0f, {64});
  auto scale2 = BroadcastTo(scope, 1.0f, {64});
  auto batchnorm2 = this->batchnorm2_op.Build(scope, deconv2, 0.001f, training);
  LOG(INFO) << "Node building status: " << scope.status();

  // LeakyReLU 2
  auto leakyrelu2 =
      internal::LeakyRelu(scope, batchnorm2, internal::LeakyRelu::Alpha(0.3f));
  LOG(INFO) << "Node building status: " << scope.status();

  // Conv2DTranspose 3
  auto input_sizes3 = Const(scope, {batch_size, 28, 28, NUM_CHANNELS});

  auto output =
      Conv2DTranspose(scope.WithOpName("generator"), input_sizes3,
                      this->filter3, leakyrelu2, {1, 2, 2, 1}, "SAME");
  LOG(INFO) << "Node building status: " << scope.status();

  return output;
}

// Discriminator constructor to set variables and assigns
Discriminator::Discriminator(const ::tensorflow::Scope& scope) {
  this->conv1_weights = TFVariable(scope.WithOpName("conv1_weights"),
                                   {5, 5, NUM_CHANNELS, 64}, DT_FLOAT, true);
  auto random_value = GlorotUniform(scope, {5, 5, NUM_CHANNELS, 64});
  TFAssign(scope, conv1_weights, random_value);

  this->conv1_biases =
      TFVariable(scope.WithOpName("conv1_biases"), {64}, DT_FLOAT, true);
  Tensor b_zero_tensor(DT_FLOAT, TensorShape({64}));
  b_zero_tensor.vec<float>().setZero();
  TFAssign(scope, conv1_biases, b_zero_tensor);

  this->conv2_weights = TFVariable(scope.WithOpName("conv2_weights"),
                                   {5, 5, 64, 128}, DT_FLOAT, true);
  auto random_value2 = GlorotUniform(scope, {5, 5, 64, 128});
  TFAssign(scope, conv2_weights, random_value2);

  this->conv2_biases =
      TFVariable(scope.WithOpName("conv2_biases"), {128}, DT_FLOAT, true);
  TFAssign(scope, conv2_biases, Const<float>(scope, 0.0f, TensorShape({128})));

  int s1 = IMAGE_SIZE;
  s1 = s1 / 4;
  s1 = std::pow(s1, 2) * 128;

  this->fc1_weights =
      TFVariable(scope.WithOpName("fc1_weights"), {s1, 1}, DT_FLOAT, true);
  auto random_value3 = GlorotUniform(scope, {s1, 1});
  TFAssign(scope, fc1_weights, random_value3);

  this->fc1_biases =
      TFVariable(scope.WithOpName("fc1_biases"), {1}, DT_FLOAT, true);
  TFAssign(scope, fc1_biases, Const<float>(scope, 0.0f, TensorShape({1})));
}

// Build model
Output Discriminator::Build(const ::tensorflow::Scope& scope,
                            const ::tensorflow::Input& inputs,
                            const int batch_size) {
  // Convnet Model begin
  auto conv2d_1 = Conv2D(scope, inputs, this->conv1_weights,
                         gtl::ArraySlice<int>{1, 2, 2, 1}, "SAME");
  LOG(INFO) << "Node building status: " << scope.status();

  auto relu_1 =
      internal::LeakyRelu(scope, BiasAdd(scope, conv2d_1, this->conv1_biases),
                          internal::LeakyRelu::Alpha(0.3f));
  LOG(INFO) << "Node building status: " << scope.status();

  auto dropout_1 = Dropout(scope, relu_1, 0.3f);
  LOG(INFO) << "Node building status: " << scope.status();

  auto conv2d_2 = Conv2D(scope, dropout_1, this->conv2_weights,
                         gtl::ArraySlice<int>{1, 2, 2, 1}, "SAME");
  LOG(INFO) << "Node building status: " << scope.status();

  auto relu_2 =
      internal::LeakyRelu(scope, BiasAdd(scope, conv2d_2, this->conv2_biases),
                          internal::LeakyRelu::Alpha(0.3f));
  LOG(INFO) << "Node building status: " << scope.status();

  auto dropout_2 = Dropout(scope, relu_2, 0.3f);
  LOG(INFO) << "Node building status: " << scope.status();

  int s1 = IMAGE_SIZE;
  s1 = s1 / 4;
  s1 = std::pow(s1, 2) * 128;

  // reshape
  auto reshape1 = Reshape(scope, dropout_2, {batch_size, s1});
  LOG(INFO) << "Node building status: " << scope.status();

  // model output
  auto output =
      BiasAdd(scope.WithOpName("discriminator"),
              MatMul(scope, reshape1, this->fc1_weights), this->fc1_biases);
  // Convnet Model ends

  return output;
}

}  // namespace ops
}  // namespace tensorflow
