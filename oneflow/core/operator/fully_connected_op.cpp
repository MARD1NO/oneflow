#include "oneflow/core/operator/fully_connected_op.h"
#include "oneflow/core/common/balanced_splitter.h"

namespace oneflow {

void FullyConnectedOp::InitFromOpConf() {
  CHECK(op_conf().has_fully_connected_conf());

  EnrollInputBn("in");
  EnrollOutputBn("out");
  EnrollModelBn("weight");

  if (op_conf().fully_connected_conf().use_bias()) {
    EnrollModelBn("bias");
    EnrollModelTmpBn("bias_multiplier");
  }
}

const PbMessage& FullyConnectedOp::GetSpecialConf() const {
  return op_conf().fully_connected_conf();
}

void FullyConnectedOp::InferBlobDescs(
    std::function<BlobDesc*(const std::string)> GetBlobDesc4BnInOp,
    const ParallelContext* parallel_ctx) const {
  // useful vars
  const FullyConnectedOpConf& conf = op_conf().fully_connected_conf();
  const BlobDesc* in_blob_desc = GetBlobDesc4BnInOp("in");
  CHECK_EQ(in_blob_desc->data_type(), JobDesc::Singleton()->DefaultDataType());
  int32_t units = conf.units();
  if (parallel_ctx->policy() == kModelParallel) {
    BalancedSplitter splitter(units, parallel_ctx->parallel_num());
    units = splitter.At(parallel_ctx->parallel_id()).size();
  }
  // out
  BlobDesc* out_blob_desc = GetBlobDesc4BnInOp("out");
  *out_blob_desc = *in_blob_desc;
  out_blob_desc->mut_shape() = Shape({in_blob_desc->shape().At(0), units});

  // weight
  GetBlobDesc4BnInOp("weight")->mut_shape() =
      Shape({units, in_blob_desc->shape().Count(1)});

  if (op_conf().fully_connected_conf().use_bias()) {
    // bias
    GetBlobDesc4BnInOp("bias")->mut_shape() = Shape({1, units});

    // bias_multiplier
    GetBlobDesc4BnInOp("bias_multiplier")->mut_shape() =
        Shape({in_blob_desc->shape().At(0), 1});
  }
}

REGISTER_OP(OperatorConf::kFullyConnectedConf, FullyConnectedOp);

}  // namespace oneflow
