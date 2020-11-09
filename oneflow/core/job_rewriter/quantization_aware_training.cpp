/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "oneflow/core/common/util.h"
#ifdef WITH_CUDA

#include "oneflow/core/job_rewriter/auto_mixed_precision_lists.h"

#include <algorithm>

#include "oneflow/core/device/cuda_util.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/job_rewriter/op_graph_pass.h"
#include "oneflow/core/job/job_desc.h"

namespace oneflow {

typedef HashSet<std::string> QATList;

const QATList& Int8List() {
  static QATList white_list = {"matmul", "batch_matmul", "conv2d", "avg_pool_2d", "max_pool_2d"};
  return white_list;
}

const QATList& ProduceFloat32List() {
  static QATList black_list = {};
  return black_list;
}

const QATList& TransparentList() {
  static QATList gray_list = {"add_n",
                              "bias_add",
                              "multiply",
                              "sigmoid",
                              "tanh",
                              "sqrt",
                              "scalar_mul",
                              "scalar_add",
                              "broadcast_add",
                              "broadcast_sub",
                              "broadcast_mul",
                              "broadcast_div",
                              "layer_norm",
                              "dropout",
                              "softmax",
                              "gelu",
                              "normalization",
                              "normalization_add_relu",
                              "gather",
                              "reshape",
                              "relu",
                              "transpose",
                              "random_mask_like",
                              "concat",
                              "pad",
                              "same_padding"};
  return gray_list;
}

namespace {

#define INSERT_CHECK(expr) CHECK(expr.second)

template<typename MapT, typename KeyT>
bool IsKeyFound(const MapT& m, const KeyT& k) {
  return m.find(k) != m.end();
}

bool IsNodeInList(const QATList& amp_list, OpNode* node) {
  if (node->op().op_conf().has_user_conf() == false) { return false; }
  const std::string op_type = node->op().op_conf().user_conf().op_type_name();
  return IsKeyFound(amp_list, op_type);
}

template<typename ContainerT, typename ElemT>
std::string Container2Str(const ContainerT& container,
                          std::function<std::string(const ElemT&)> elem2str) {
  std::string ret;
  bool is_first = true;
  for (const ElemT& elem : container) {
    if (is_first) {
      is_first = false;
    } else {
      ret += ",\n";
    }
    ret += elem2str(elem);
  }
  return ret;
}

void VerifyQATList(const QATList& amp_list) {
  for (const auto& op_type : amp_list) {
    CHECK(user_op::UserOpRegistryMgr::Get().GetOpRegistryResult(op_type) != nullptr)
        << "Cannot find " << op_type << " of QuantAwareTraining list in OpRegistry.";
  }
}

std::string ReplaceSlashToDash4Lbn(std::string lbn) {
  std::replace(lbn.begin(), lbn.end(), '/', '-');
  return lbn;
}

void DfsTopoGraphTraversal(const OpGraph& graph, bool reversed,
                           std::function<bool(OpNode*)> IsCurNodeStartNode,
                           std::function<bool(OpNode*)> IsCurNodeSatisfied,
                           std::function<bool(OpNode*)> IsFatherNodeSatisfied,
                           std::function<void(OpNode*)> NodeHandler) {
  auto start_nodes = reversed ? graph.sink_nodes() : graph.source_nodes();
  std::function<void(OpNode*, std::function<void(OpNode*)>)> NodeOnInEdge =
      reversed ? &OpNode::ForEachNodeOnOutEdge : &OpNode::ForEachNodeOnInEdge;
  std::function<void(OpNode*, std::function<void(OpNode*)>)> NodeOnOutEdge =
      reversed ? &OpNode::ForEachNodeOnInEdge : &OpNode::ForEachNodeOnOutEdge;
  graph.DfsTopoForEachNode(start_nodes, NodeOnInEdge, NodeOnOutEdge, [&](OpNode* node) {
    if (IsCurNodeStartNode(node)) {
      NodeHandler(node);
      return;
    }
    if (IsCurNodeSatisfied(node)) {
      bool is_one_father_of_node_satisfied = false;
      NodeOnInEdge(node, [&](OpNode* father_node) {
        if (is_one_father_of_node_satisfied) { return; }
        if (IsFatherNodeSatisfied(father_node)) { is_one_father_of_node_satisfied = true; }
      });
      if (is_one_father_of_node_satisfied) { NodeHandler(node); }
    }
  });
}

class QuantAwareTraining final : public OpGraphPass {
 public:
  OF_DISALLOW_COPY_AND_MOVE(QuantAwareTraining);
  QuantAwareTraining()
      : int8_list_(Int8List()),
        fp32_list_(ProduceFloat32List()),
        transparent_list_(TransparentList()) {}
  ~QuantAwareTraining() = default;

  bool IsEnabled() const override { return true; }

  Maybe<void> Apply(const OpGraph& op_graph, JobBuilder* job_builder) const override;

 private:
  void InsertFakeQuantOp(const OpGraph& op_graph, const QATList& int8_list,
                         HashSet<OpNode*> downstream_white, JobBuilder* job_builder) const;

  const QATList& int8_list_;
  const QATList& fp32_list_;
  const QATList& transparent_list_;
};

Maybe<void> QuantAwareTraining::Apply(const OpGraph& op_graph, JobBuilder* job_builder) const {
  CHECK(GlobalJobDesc().DefaultDataType() == DataType::kFloat);

  VerifyQATList(int8_list_);
  VerifyQATList(fp32_list_);
  VerifyQATList(transparent_list_);

  std::function<std::string(OpNode* const&)> OpName4Node = [](OpNode* const& node) {
    return node->op().op_name();
  };
  HashSet<OpNode*> downstream_white;
  DfsTopoGraphTraversal(
      op_graph, false, [](OpNode* node) { return false; },
      [&](OpNode* node) {
        return IsNodeInList(int8_list_, node) || IsNodeInList(transparent_list_, node);
      },
      [&](OpNode* node) {
        return IsNodeInList(int8_list_, node) || IsKeyFound(downstream_white, node);
      },
      [&](OpNode* node) {
        INSERT_CHECK(downstream_white.insert(node));
        VLOG(3) << "FillWhiteSet(): Insert " << node->op().op_name() << " to downstream_white";
      });

  // if node in int8_list_, insert fake quant op on its input which produced by node not in
  // downstream_white if node in int8_list_, insert fake quant op on its `GetInferenceOutputNode`'s
  // output

  VLOG(3) << "downstream_white include: "
            << Container2Str<HashSet<OpNode*>, OpNode*>(downstream_white, OpName4Node);

  InsertFakeQuantOp(op_graph, int8_list_, downstream_white, job_builder);
  return Maybe<void>::Ok();
}

OpNode* GetInferenceOutputNode(const OpGraph& op_graph, OpNode& node) {
  if (node.op().op_conf().user_conf().op_type_name() == "conv2d") {
    if (node.out_edges().size() == 1) {
      OpNode* dst_node = node.SoleOutEdge()->dst_node();
      if (dst_node->op().op_conf().user_conf().op_type_name() == "relu") { return dst_node; }
    }
  }
  return &node;
}

void QuantAwareTraining::InsertFakeQuantOp(const OpGraph& op_graph, const QATList& int8_list,
                                           HashSet<OpNode*> downstream_white,
                                           JobBuilder* job_builder) const {
  HashSet<OpEdge*> white_set_edges;
  auto EdgeName4Edge = [](OpEdge* const& edge) {
    return std::string("edge of\t") + edge->src_node()->op().op_name() + "\tto\t"
           + edge->dst_node()->op().op_name();
  };
  {
    op_graph.ForEachNode([&](OpNode* node) {
      if (IsNodeInList(int8_list, node)) {
        for (OpEdge* edge : node->in_edges()) {
          if (!IsKeyFound(downstream_white, edge->src_node())) {
            VLOG(3) << "insert " << EdgeName4Edge(edge);
            INSERT_CHECK(white_set_edges.insert(edge));
          }
        }
        OpNode* inference_node = GetInferenceOutputNode(op_graph, *node);
        for (OpEdge* edge : inference_node->out_edges()) {
          VLOG(3) << "insert " << EdgeName4Edge(edge);
          INSERT_CHECK(white_set_edges.insert(edge));
        }
      }
    });
    VLOG(3) << "white_set_edges: "
              << Container2Str<HashSet<OpEdge*>, OpEdge*>(white_set_edges, EdgeName4Edge);
  }

  HashMap<std::string, std::vector<OpEdge*>> edges_group_by_lbn;
  {
    for (OpEdge* edge : white_set_edges) {
      CHECK_EQ(1, edge->lbis().size());
      std::string lbn = GenLogicalBlobName(edge->lbis().front());
      edges_group_by_lbn[lbn].push_back(edge);
    }
  }

  HashMap<std::string, OperatorConf> dst_op_name2dst_op_confs;
  for (auto& pair : edges_group_by_lbn) {
    const std::string& lbn = pair.first;
    const OpNode* src_node = pair.second.front()->src_node();

    const BlobDesc& blob_desc = src_node->LogicalBlobDesc4Lbi(GenLogicalBlobId(lbn));
    if (blob_desc.data_type() != DataType::kFloat) { continue; }

    const std::string cast_suffix = "-fake-quant";
    const auto cast_op =
        user_op::UserOpConfWrapperBuilder(ReplaceSlashToDash4Lbn(lbn) + cast_suffix)
            .Op("identity")
            .Input("in", lbn)
            .Output("out")
            // .Attr<DataType>("dtype", cast_data_type)
            .Build();

    bool cast_is_consumed = false;
    for (OpEdge* edge : pair.second) {
      CHECK(src_node == edge->src_node());
      OpNode* dst_node = edge->dst_node();
      LogicalBlobId cur_lbi = edge->lbis().front();
      CHECK_EQ(lbn, GenLogicalBlobName(cur_lbi));
      CHECK_EQ(1, edge->lbi2ibns().at(cur_lbi).size());
      const std::string& dst_ibn = edge->lbi2ibns().at(cur_lbi).front();

      cast_is_consumed = true;

      const std::string& dst_op_name = dst_node->op().op_name();
      if (!IsKeyFound(dst_op_name2dst_op_confs, dst_op_name)) {
        INSERT_CHECK(
            dst_op_name2dst_op_confs.insert(std::make_pair(dst_op_name, dst_node->op().op_conf())));
      }
      OperatorConf& dst_op_conf = dst_op_name2dst_op_confs.at(dst_op_name);
      std::string new_lbn = cast_op.op_name() + "/out_0";
      CHECK_EQ(lbn, ReplaceInputLbnInOpCustomizedConf(&dst_op_conf, dst_ibn, new_lbn));
    }

    if (cast_is_consumed) {
      job_builder->AddOps(src_node->parallel_desc().parallel_conf(),
                          std::vector<OperatorConf>{cast_op.op_conf()});
      VLOG(3) << "Insert fake quant op: " << cast_op.op_name() << " between " << lbn;
    }
  }

  std::vector<OperatorConf> dst_op_confs;
  for (const auto& pair : dst_op_name2dst_op_confs) { dst_op_confs.push_back(pair.second); }
  // make sure an op_conf can only be udpated once, cuz later update will override before
  job_builder->MutOpsOnlyOnce(dst_op_confs);
}

REGISTER_FUNCTION_PASS("QuantAwareTraining", QuantAwareTraining);

}  // namespace

}  // namespace oneflow

#endif  // WITH_CUDA