#include "oneflow/core/job_completer/add_lbi_diff_watcher.h"
#include "oneflow/core/job/lbi_diff_watcher_info.pb.h"
#include "oneflow/core/operator/operator.h"

namespace oneflow {

void AddLbiDiffWatherOpConfs(const HashMap<LogicalBlobId, LogicalBlobId>& lbi2diff_lbi,
                             JobBuilder* job_builder) {
  const auto& map = Global<LbiDiffWatcherInfo>::Get()->job_name2lbi_and_watcher_uuids();
  if (map.find(GlobalJobDesc().job_name()) == map.end()) { return; }
  const auto& pair_list = map.at(GlobalJobDesc().job_name()).lbi_and_uuid_pair();
  std::vector<OperatorConf> op_confs;
  for (const LbiAndDiffWatcherUuidPair& pair : pair_list) {
    if (lbi2diff_lbi.find(pair.lbi()) == lbi2diff_lbi.end()) { continue; }
    OperatorConf foreign_watcher_op;
    foreign_watcher_op.set_name("System-LbiDiffWatcher-ForeignWatcher-" + NewUniqueId());
    auto* foreign_watcher_conf = foreign_watcher_op.mutable_foreign_watch_conf();
    foreign_watcher_conf->set_in(GenLogicalBlobName(lbi2diff_lbi.at(pair.lbi())));
    foreign_watcher_conf->set_handler_uuid(pair.watcher_uuid());
    op_confs.push_back(foreign_watcher_op);
  }
  ParallelConf parallel_conf;
  parallel_conf.add_device_name("0:cpu:0");
  job_builder->AddOps(parallel_conf, op_confs);
}

}  // namespace oneflow