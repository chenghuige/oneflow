#include <string>
#include <vector>
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "glog/logging.h"

#include "oneflow/core/job/job.pb.h"
#include "oneflow/core/job/job_builder.h"
#include "oneflow/core/operator/op_conf.pb.h"
#include "oneflow/xrt/api.h"
#include "oneflow/xrt/argument.h"
#include "oneflow/xrt/graph/graph.h"
#include "oneflow/xrt/passes/pass.h"
#include "oneflow/xrt/types.h"
#include "oneflow/xrt/utility/stl.h"

namespace oneflow {
namespace xrt {

extern const std::string _XrtLaunchOpType;
extern const std::string _XrtInArgumentPrefix;
extern const std::string _XrtOutArgumentPrefix;

static const std::string _ReduceSplitType = "ReduceSplit";

template <typename T>
void DoNoDuplicationAdd(util::PbVector<T> *repeat_field, const T &val) {
  if (std::find(repeat_field->begin(), repeat_field->end(), val) ==
      repeat_field->end()) {
    repeat_field->Add()->assign(val);
  }
}

int GetRepeatedIndex(const std::string &input) {
  std::vector<std::string> splits = absl::StrSplit(input, "_");
  CHECK_GT(splits.size(), 0);
  int index = 0;
  absl::SimpleAtoi(splits.back(), &index);
  return index;
};

void SetOpInputBlobName(OperatorConf *op_conf, const std::string &input,
                        const std::string &blob_name,
                        const std::string &fixed_blob_name) {
  auto *spec_conf = MutableMessageInPbMessage(op_conf, op_conf->op_type_case());
  switch (op_conf->op_type_case()) {
    case OperatorConf::kPrintConf: {
      int index = GetRepeatedIndex(input);
      *(op_conf->mutable_print_conf()->mutable_in(index)->mutable_lbn()) =
          fixed_blob_name;
      break;
    }
    default:
      ReplaceStrValInPbFdOrPbRpf(spec_conf, input, blob_name, fixed_blob_name);
  }
}

class FoldSubgraphBuilder {
 public:
  FoldSubgraphBuilder(const XrtGraph &graph, Job *job);

  virtual ~FoldSubgraphBuilder() {}

  void Build() {
    CHECK(builder_) << "Builder has not been initialized.";
    // Rebuilding folded job should takes the below steps in order
    InferIsAfterAllReduce();
    // 5.Fixup output blob names for launch nodes, and infect the
    //   changes to the input of next nodes.
    FixupInOutBlobNames();
    // 1.Add XrtLaunch operator to the job.
    BuildXrtLaunchOps();
    // 2.Replace control_in_op_name by XrtLaunch name if the operator
    //   has been folded by a XrtLaunch operator.
    FixupControlInOpNames();
    // 3.Add time shape for XrtLaunch operators.
    FixupTimeShapes();
    // 4.Add sbp parallel strategy for XrtLaunch operators.
    FixupSbpSignatures();
    // 6.Finally remove the folded operators.
    RemoveLaunchFoldedOps();
  }

 private:
  void InferIsAfterAllReduce();

  void buildFunction(const XrtGraph *sub_graph,
                     util::Set<std::string> *mutability,
                     XrtLaunchOpConf::Function *function);

  void BuildXrtLaunchOps();

  void FixupControlInOpNames();

  void FixupTimeShapes();

  void FixupSbpSignatures();

  void FixupInOutBlobNames();

  void RemoveLaunchFoldedOps();

  bool IsAfterAllReduce(const XrtNode *node);

 private:
  const XrtGraph &graph_;
  std::shared_ptr<JobBuilder> builder_;

  // Launch nodes
  std::vector<const XrtNode *> launch_nodes_;
  // Folded nodes except for argument nodes for each launch nodes
  std::vector<std::vector<const XrtNode *>> folded_nodes_;
  // TODO(hjchen2): Remove this
  util::Set<const XrtNode *> after_allreduce_nodes_;

  util::Map<std::string, std::string> fixedup_names_;
};

FoldSubgraphBuilder::FoldSubgraphBuilder(const XrtGraph &graph, Job *job)
    : graph_(graph) {
  for (const XrtNode *node : graph_.Nodes()) {
    if (node->type() == _XrtLaunchOpType) {
      launch_nodes_.push_back(node);
    }
  }

  folded_nodes_.resize(launch_nodes_.size());
  for (int i = 0; i < launch_nodes_.size(); ++i) {
    XrtGraph *sub_graph = launch_nodes_[i]->sub_graph();
    CHECK_NOTNULL(sub_graph);
    for (const XrtNode *sub_node : sub_graph->Nodes()) {
      if (!sub_node->IsArgumentNode()) {
        folded_nodes_[i].push_back(sub_node);
      }
    }
  }
  builder_ = std::make_shared<JobBuilder>(job);
}

bool IsMutableArgument(const XrtNode *node, const Argument &argument) {
  XrtEngine engine = XrtEngine::XLA;
  XrtField field = MakeXrtField(node->device(), engine);
  auto *rm = util::RegistryManager<decltype(field)>::Global();
  const auto &attrs = rm->Get(field)->LookupAttr(node->type());
  const auto &it = attrs.find("MutableVars");
  if (it != attrs.end()) {
    const std::string &key = argument.meta_data().consume_key;
    const auto &mutable_vars = any_cast<util::Set<std::string>>(it->second);
    return mutable_vars.count(key) > 0;
  }
  return false;
}

void FoldSubgraphBuilder::buildFunction(const XrtGraph *sub_graph,
                                        util::Set<std::string> *mutability,
                                        XrtLaunchOpConf::Function *function) {
  for (const XrtNode *node : sub_graph->Nodes()) {
    if (!node->IsArgumentNode()) {
      *(function->add_node()) =
          *reinterpret_cast<const OperatorConf *>(&node->param());
    } else {
      auto *argument_proto = function->add_argument();
      argument_proto->set_name(node->name());
      DeviceType device_type = XrtDeviceToDeviceType(node->device());
      argument_proto->set_device_type(device_type);
      // Usually one argument node has either inputs or outputs
      CHECK(node->in_edges().size() == 0 || node->out_edges().size() == 0);
      bool is_mutable = false;
      // Build inputs or outputs for the argument nodes
      for (const XrtEdge *edge : node->out_edges()) {
        const Argument &argument = edge->argument();
        argument_proto->set_value(argument.name());
        is_mutable |= IsMutableArgument(edge->end(), argument);
      }
      for (const XrtEdge *edge : node->in_edges()) {
        const Argument &argument = edge->argument();
        argument_proto->set_value(argument.name());
      }
      if (is_mutable) {
        mutability->insert(argument_proto->value());
      }
    }
  }
}

void AddInOutBlobNames(const XrtNode *node, XrtLaunchOpConf *launch_conf) {
  for (const XrtEdge *edge : node->in_edges()) {
    if (!edge->IsControlEdge()) {
      const Argument &arg = edge->argument();
      DoNoDuplicationAdd(launch_conf->mutable_in(), arg.name());
    }
  }

  for (const XrtEdge *edge : node->out_edges()) {
    if (!edge->IsControlEdge()) {
      const Argument &arg = edge->argument();
      std::vector<std::string> splits = absl::StrSplit(arg.name(), "/");
      CHECK_EQ(splits.size(), 2);
      DoNoDuplicationAdd(launch_conf->mutable_out(), splits[1]);
    }
  }
}

void FoldSubgraphBuilder::BuildXrtLaunchOps() {
  for (int i = 0; i < launch_nodes_.size(); ++i) {
    const XrtNode *node = launch_nodes_[i];
    // Add xrt launch operator
    OperatorConf op_conf;
    op_conf.set_name(node->name());
    DeviceType device_type = XrtDeviceToDeviceType(node->device());
    op_conf.set_device_type(device_type);

    XrtLaunchOpConf *launch_conf = op_conf.mutable_xrt_launch_conf();
    // Add inputs and outputs in launch_conf
    AddInOutBlobNames(node, launch_conf);
    // Build function and returns inputs mutability.
    util::Set<std::string> mutability;
    buildFunction(node->sub_graph(), &mutability,
                  launch_conf->mutable_function());

    for (const auto &arg_proto : launch_conf->function().argument()) {
      std::string arg_value = arg_proto.value();
      const auto &it = fixedup_names_.find(arg_value);
      if (it != fixedup_names_.end()) {
        arg_value = it->second /* fixedup blob names */;
      }
      if (mutability.count(arg_proto.value()) > 0) {
        (*launch_conf->mutable_mutability())[arg_value] = true;
      }

      // Set input and output mapping from launch op to function.
      (*launch_conf->mutable_input_output_mapping())[arg_value] =
          arg_proto.value();

      // Store the batch axis that have batch dimension, so that it's no
      // need to infer `HasBatchAxis4Lbn` for `XrtLaunch` operators.
      if (builder_->HasBatchAxis4Lbn(arg_value)) {
        auto *batch_axis = launch_conf->mutable_batch_axis();
        (*batch_axis)[arg_value] = builder_->BatchAxis4Lbn(arg_value);
      }
    }

    if (IsAfterAllReduce(node) && node->out_edges().size() == 0) {
      launch_conf->set_model_update(true);
    }

    CHECK_GT(folded_nodes_[i].size(), 0);
    const ParallelConf &parallel_conf =
        builder_->ParallelConf4OpName(folded_nodes_[i][0]->name());
    // TODO(hjchen2) check parallel conf over all folded nodes

    builder_->AddOps(parallel_conf, {op_conf});
  }
}

void FoldSubgraphBuilder::FixupControlInOpNames() {
  CHECK_EQ(launch_nodes_.size(), folded_nodes_.size());
  // Map folded node names to cluster node
  util::Map<std::string, const XrtNode *> folded_op_names;
  for (int i = 0; i < launch_nodes_.size(); ++i) {
    for (const XrtNode *node : folded_nodes_[i]) {
      folded_op_names.emplace(node->name(), launch_nodes_[i]);
    }
  }

  auto AddControlInOpName = [&](OperatorConf *conf,
                                const std::string &op_name) -> void {
    std::string ctrl_in_op_name = op_name;
    const auto &it = folded_op_names.find(op_name);
    if (it != folded_op_names.end()) {
      ctrl_in_op_name = it->second->name();
    }
    if (conf->name() != ctrl_in_op_name) {
      DoNoDuplicationAdd(conf->mutable_ctrl_in_op_name(), ctrl_in_op_name);
    }
  };

  for (const XrtNode *node : graph_.Nodes()) {
    auto *op_conf = builder_->MutableOpConf4OpName(node->name());
    if (node->sub_graph() == nullptr) {
      auto ctrl_in_op_names = op_conf->ctrl_in_op_name();
      op_conf->clear_ctrl_in_op_name();
      for (const auto &op_name : ctrl_in_op_names) {
        AddControlInOpName(op_conf, op_name);
      }
    } else {
      for (const XrtNode *sub_node : node->sub_graph()->Nodes()) {
        if (sub_node->IsArgumentNode()) {
          continue;
        }
        const auto &folded_op_conf = builder_->OpConf4OpName(sub_node->name());
        for (const auto &op_name : folded_op_conf.ctrl_in_op_name()) {
          AddControlInOpName(op_conf, op_name);
        }
      }
    }
  }
}

void FoldSubgraphBuilder::FixupInOutBlobNames() {
  for (const XrtNode *node : launch_nodes_) {
    std::string launch_op_name = node->name();
    // Fix output blob names
    util::Set<std::string> argument_names;
    for (XrtEdge *edge : node->out_edges()) {
      if (edge->IsControlEdge()) {
        continue;
      }
      const Argument &arg = edge->argument();
      int index = argument_names.size();
      if (argument_names.insert(arg.name()).second) {
        CHECK_EQ(fixedup_names_.count(arg.name()), 0);
      }
      auto it = fixedup_names_.find(arg.name());
      if (it == fixedup_names_.end()) {
        std::string fixed_blob_name =
            absl::StrCat(launch_op_name, "/out_", index);
        it = fixedup_names_.emplace(arg.name(), fixed_blob_name).first;
      }
      // Append to `batch_axis`
      if (builder_->HasBatchAxis4Lbn(arg.name())) {
        builder_->AddBatchAxis4Lbn(it->second /* fixed_blob_name */,
                                   builder_->BatchAxis4Lbn(arg.name()));
      }
      // Fix end input blob name
      const XrtNode *end = edge->end();
      if (end->type() != _XrtLaunchOpType) {
        auto *op_conf = builder_->MutableOpConf4OpName(end->name());
        const std::string &consume_key = arg.meta_data().consume_key;
        SetOpInputBlobName(op_conf, consume_key, arg.name(),
                           it->second /* fixed_blob_name */);
      }
      Argument fixed_arg(it->second /* fixed_blob_name */, arg.shape(),
                         arg.data_type(), arg.meta_data());
      edge->SetArgument(fixed_arg);
    }
  }
}

void FoldSubgraphBuilder::FixupTimeShapes() {
  for (int i = 0; i < launch_nodes_.size(); ++i) {
    CHECK_GT(folded_nodes_[i].size(), 0);
    const OpTimeShape &time_shape =
        builder_->TimeShape4OpName(folded_nodes_[i][0]->name());
    // TODO(hjchen2) check time shape for all folded nodes
    builder_->AddTimeShape4OpName(launch_nodes_[i]->name(), time_shape);
  }
}

void FoldSubgraphBuilder::FixupSbpSignatures() {
  for (const XrtNode *node : launch_nodes_) {
    SbpSignature sbp_conf;
    auto *sbp_signatures = sbp_conf.mutable_bn_in_op2sbp_parallel();
    for (const XrtEdge *edge : node->in_edges()) {
      CHECK(edge->HasAttr("sbp_policy"));
      const std::string &bn = edge->argument().meta_data().consume_key;
      (*sbp_signatures)[bn] =
          edge->Attr<std::vector<SbpParallel>>("sbp_policy")[1];
    }
    for (const XrtEdge *edge : node->out_edges()) {
      CHECK(edge->HasAttr("sbp_policy"));
      const std::string &bn = edge->argument().meta_data().produce_key;
      (*sbp_signatures)[bn] =
          edge->Attr<std::vector<SbpParallel>>("sbp_policy")[0];
    }
    // Append sbp signatures to helper
    builder_->AddSbpSignature4OpName(node->name(), sbp_conf);
  }
}

void FoldSubgraphBuilder::RemoveLaunchFoldedOps() {
  util::Set<std::string> removing_names;
  for (const XrtNode *node : launch_nodes_) {
    for (const XrtNode *sub_node : node->sub_graph()->Nodes()) {
      if (!sub_node->IsArgumentNode()) {
        removing_names.insert(sub_node->name());
      }
    }
  }
  builder_->RemoveOpByName(removing_names);
}

void FoldSubgraphBuilder::InferIsAfterAllReduce() {
  algorithm::TopologyVisit(graph_, [this](const XrtNode *node) {
    for (const XrtEdge *edge : node->in_edges()) {
      const XrtNode *start = edge->start();
      if (IsAfterAllReduce(start) || start->type() == _ReduceSplitType) {
        after_allreduce_nodes_.insert(node);
      }
    }
  });
}

bool FoldSubgraphBuilder::IsAfterAllReduce(const XrtNode *node) {
  return after_allreduce_nodes_.count(node) > 0;
}

// Rebuild job according to the nodes folded xrt graph. In order to rebuild
// the job, We will add several launch operators in the job, and remove the
// folded operators. In each launch operator, we wll reconstruct the subgraph
// and insert argument nodes if necessary.
class RebuildCompiledJobPass : public XrtPass {
 public:
  RebuildCompiledJobPass() = default;

  // params: vector of any which should contains:
  //   0 - job
  void Run(XrtGraph *graph, const XrtPassOptions &options,
           const std::vector<Any> &params) override {
    CHECK_GE(params.size(), 1)
        << "Job is required by `RebuildCompiledJobPass`.";
    auto *job = any_cast<Job *>(params[0]);

    CHECK(graph) << "Graph is required by `RebuildCompiledJobPass`.";
    FoldSubgraphBuilder(*graph, job).Build();
  }
};

REGISTER_XRT_PASS(RebuildCompiledJob, RebuildCompiledJobPass);

}  // namespace xrt
}  // namespace oneflow
