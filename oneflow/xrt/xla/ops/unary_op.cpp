#include "oneflow/xrt/xla/ops/op_context.h"
#include "oneflow/xrt/xla/ops/op_kernel.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"

#include "oneflow/xrt/xla/ops/unary_op.h"

namespace oneflow {
namespace xrt {
namespace mola {

template <typename UnaryOp>
class ApplyUnaryOp : public OpKernel {
 public:
  void Compile(OpKernelContext *ctx) override {
    ctx->SetOutput("out", UnaryOp()(ctx->Input("in")));
  }
};

REGISTER_XLA_OP_KERNEL(Sigmoid, ApplyUnaryOp<op::Logistic>).Finalize();
REGISTER_XLA_OP_KERNEL(Tanh, ApplyUnaryOp<op::Tanh>).Finalize();

struct Gelu {
  xla::XlaOp operator()(const xla::XlaOp &x) {
    xla::XlaOp dot_5 = xla::ScalarLike(x, 0.5f);
    xla::XlaOp inv_sqrt2 = xla::ScalarLike(x, std::sqrt(0.5f));
    xla::XlaOp one = xla::ScalarLike(x, 1.f);
    // cdf = erf(sqrt(0.5) * x)
    xla::XlaOp cdf = xla::Erf(xla::Mul(inv_sqrt2, x));
    // return 0.5 * x * (1.0 + cdf)
    return xla::Mul(xla::Mul(dot_5, x), xla::Add(one, cdf));
  }
};

REGISTER_XLA_OP_KERNEL(Gelu, ApplyUnaryOp<Gelu>).Finalize();

struct Identity {
  xla::XlaOp operator()(const xla::XlaOp &x) { return x; }
};

// REGISTER_XLA_OP_KERNEL(Identity, ApplyUnaryOp<Identity>).Finalize();

}  // namespace mola
}  // namespace xrt
}  // namespace oneflow
