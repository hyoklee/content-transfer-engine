/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "hermes_core/hermes_core.h"

#include <string>

#include "bdev/bdev.h"
#include "chimaera/api/chimaera_runtime.h"
#include "chimaera/chimaera_types.h"
#include "chimaera/monitor/monitor.h"
#include "chimaera/work_orchestrator/work_orchestrator.h"
#include "chimaera_admin/chimaera_admin.h"
#include "hermes/data_stager/stager_factory.h"
#include "hermes/dpe/dpe_factory.h"
#include "hermes/hermes.h"

/** NOTE(llogan): std::hash function for string. This is because NVCC is bugged
 */
namespace std {
template <>
struct hash<chi::string> {
  size_t operator()(const chi::string &text) const { return text.Hash(); }
};
}  // namespace std

namespace hermes {

#define HERMES_LANES 32

struct FlushInfo {
  BlobInfo *blob_info_;
  FullPtr<StageOutTask> stage_task_;
  size_t mod_count_;
};

/** Type name simplification for the various map types */
typedef std::unordered_map<chi::string, TagId> TAG_ID_MAP_T;
typedef std::unordered_map<TagId, TagInfo> TAG_MAP_T;
typedef std::unordered_map<chi::string, BlobId> BLOB_ID_MAP_T;
typedef std::unordered_map<BlobId, BlobInfo> BLOB_MAP_T;
typedef hipc::circular_mpsc_queue<IoStat> IO_PATTERN_LOG_T;
typedef std::unordered_map<TagId, std::shared_ptr<AbstractStager>> STAGER_MAP_T;

struct HermesLane {
  TAG_ID_MAP_T tag_id_map_;
  TAG_MAP_T tag_map_;
  BLOB_ID_MAP_T blob_id_map_;
  BLOB_MAP_T blob_map_;
  STAGER_MAP_T stager_map_;
  chi::CoMutex stager_map_lock_;
  chi::CoRwLock tag_map_lock_;
  chi::CoRwLock blob_map_lock_;
};

class Server : public Module {
 public:
  CLS_CONST LaneGroupId kDefaultGroup = 0;
  Client client_;
  std::vector<HermesLane> tls_;
  std::atomic<u64> id_alloc_;
  std::vector<TargetInfo> targets_;
  std::unordered_map<TargetId, TargetInfo *> target_map_;
  chi::RollingAverage monitor_[Method::kCount];
  IO_PATTERN_LOG_T io_pattern_;
  TargetInfo *fallback_target_;

 private:
  /** Get the globally unique blob name */
  const chi::string GetBlobNameWithBucket(const TagId &tag_id,
                                          const chi::string &blob_name) {
    return BlobInfo::GetBlobNameWithBucket(tag_id, blob_name);
  }

 public:
  Server() = default;

  /** Construct hermes_core */
  void Create(CreateTask *task, RunContext &rctx) {
    // Create a set of lanes for holding tasks
    HERMES_CONF->ServerInit();
    client_.Init(id_);
    CreateLaneGroup(kDefaultGroup, HERMES_LANES, QUEUE_LOW_LATENCY);
    tls_.resize(HERMES_LANES);
    io_pattern_.resize(8192);
    // Create block devices
    targets_.reserve(
        128);  // TODO(llogan): Calculate number of buffering devices
    // for (int i = 0; i < 3; ++i) {
    int i = 0;
    for (DeviceInfo &dev : HERMES_SERVER_CONF.devices_) {
      dev.mount_point_ =
          hshm::Formatter::format("{}/{}", dev.mount_dir_, dev.dev_name_);
      targets_.emplace_back();
      TargetInfo &target = targets_.back();
      NodeId node_id = CHI_CLIENT->node_id_ + i;
      HILOG(kInfo, "Creating target: {}", dev.dev_name_);
      target.client_.Create(
          HSHM_DEFAULT_MEM_CTX,
          DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                     node_id),
          DomainQuery::GetGlobalBcast(),
          hshm::Formatter::format("hermes_{}/{}", dev.dev_name_, node_id),
          dev.mount_point_, dev.capacity_);
      target.id_ = target.client_.id_;
      if (target_map_.find(target.id_) != target_map_.end()) {
        targets_.pop_back();
        continue;
      }
      HILOG(kInfo, "Created target: {}", target.id_);
      target.poll_stats_ = target.client_.AsyncPollStats(
          HSHM_DEFAULT_MEM_CTX,
          chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                          node_id),
          25);
      HILOG(kInfo, "Polling stats async for target: {}", target.id_);
      target.poll_stats_->stats_ = target.client_.PollStats(
          HSHM_DEFAULT_MEM_CTX,
          chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                          node_id));
      target.stats_ = &target.poll_stats_->stats_;
      target_map_[target.id_] = &target;
      HILOG(kInfo, "Polling stats for target: {}", target.id_);
    }
    // }
    fallback_target_ = &targets_.back();
    // Create flushing task
    client_.AsyncFlushData(
        HSHM_DEFAULT_MEM_CTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kLocalContainers, 0),
        5);  // OK
  }
  void MonitorCreate(MonitorModeId mode, CreateTask *task, RunContext &rctx) {}

  /** Route a task to a lane */
  Lane *MapTaskToLane(const Task *task) override {
    // Route tasks to lanes based on their properties
    // E.g., a strongly consistent filesystem could map tasks to a lane
    // by the hash of an absolute filename path.

    // Can I route put / get tasks to nodes here? I feel like yes.

    return GetLaneByHash(kDefaultGroup, task->prio_, 0);
  }

  /** Destroy hermes_core */
  void Destroy(DestroyTask *task, RunContext &rctx) {}
  void MonitorDestroy(MonitorModeId mode, DestroyTask *task, RunContext &rctx) {
  }

  /**
   * ========================================
   * CACHING Methods
   * ========================================
   * */

  /** Get blob info struct */
  BlobInfo *GetBlobInfo(const std::string &blob_name, BlobId blob_id) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    BLOB_MAP_T &blob_map = tls.blob_map_;
    // Check if blob name is cached on this node
    if (!blob_name.empty()) {
      auto it = blob_id_map.find(blob_name);
      if (it != blob_id_map.end()) {
        return nullptr;
      }
      blob_id = it->second;
    }
    // Check if blob ID is cached on this node
    if (!blob_id.IsNull()) {
      auto it = blob_map.find(blob_id);
      if (it != blob_map.end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  /** Get tag info struct */
  TagInfo *GetTagInfo(const std::string &tag_name, TagId tag_id) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    TAG_ID_MAP_T &tag_id_map = tls.tag_id_map_;
    TAG_MAP_T &tag_map = tls.tag_map_;
    // Check if tag name is cached on this node
    if (!tag_name.empty()) {
      auto it = tag_id_map.find(tag_name);
      if (it != tag_id_map.end()) {
        return nullptr;
      }
      tag_id = it->second;
    }
    // Check if tag ID is cached on this node
    if (!tag_id.IsNull()) {
      auto it = tag_map.find(tag_id);
      if (it != tag_map.end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  template <typename TaskT>
  void BlobCacheWriteRoute(TaskT *task) {
    std::string blob_name;
    BlobId blob_id(BlobId::GetNull());
    TagId tag_id(task->tag_id_);
    if constexpr (std::is_base_of_v<TaskT, BlobWithId>) {
      blob_id = task->blob_id_;
    }
    if constexpr (std::is_base_of_v<TaskT, BlobWithName>) {
      blob_name = task->blob_name_.str();
    }
    BlobInfo *blob_info = GetBlobInfo(blob_name, blob_id);
    if (blob_info || task->IsDirect()) {
      return;
    }
    task->dom_query_ = chi::DomainQuery::GetDirectHash(
        chi::SubDomainId::kGlobalContainers,
        HashBlobNameOrId(tag_id, blob_name, blob_id));
    task->SetDirect();
    task->UnsetRouted();
    // HILOG(kInfo, "Routing to: {}", task->dom_query_);
  }

  template <typename TaskT>
  void BlobCacheReadRoute(TaskT *task) {
    std::string blob_name;
    BlobId blob_id(BlobId::GetNull());
    TagId tag_id(task->tag_id_);
    if constexpr (std::is_base_of_v<TaskT, BlobWithId>) {
      blob_id = task->blob_id_;
    }
    if constexpr (std::is_base_of_v<TaskT, BlobWithName>) {
      blob_name = task->blob_name_.str();
    }
    BlobInfo *blob_info = GetBlobInfo(blob_name, blob_id);
    if (blob_info || task->IsDirect()) {
      return;
    }
    task->dom_query_ = chi::DomainQuery::GetDirectHash(
        chi::SubDomainId::kGlobalContainers,
        HashBlobNameOrId(tag_id, blob_name, blob_id));
    task->SetDirect();
    task->UnsetRouted();
    // HILOG(kInfo, "Routing to: {}", task->dom_query_);
  }

  template <typename TaskT>
  void TagCacheWriteRoute(TaskT *task) {
    std::string tag_name;
    TagId tag_id(TagId::GetNull());
    if constexpr (std::is_base_of_v<TaskT, TagWithId>) {
      tag_id = task->tag_id_;
    }
    if constexpr (std::is_base_of_v<TaskT, TagWithName>) {
      tag_name = task->tag_name_.str();
    }
    TagInfo *tag_info = GetTagInfo(tag_name, tag_id);
    if (tag_info || task->IsDirect()) {
      return;
    }
    task->dom_query_ = chi::DomainQuery::GetDirectHash(
        chi::SubDomainId::kGlobalContainers, HashTagNameOrId(tag_id, tag_name));
    task->SetDirect();
    task->UnsetRouted();
    // HILOG(kInfo, "Routing to: {}", task->dom_query_);
  }

  template <typename TaskT>
  void TagCacheReadRoute(TaskT *task) {
    std::string tag_name;
    TagId tag_id(TagId::GetNull());
    if constexpr (std::is_base_of_v<TaskT, TagWithId>) {
      tag_id = task->tag_id_;
    }
    if constexpr (std::is_base_of_v<TaskT, TagWithName>) {
      tag_name = task->tag_name_.str();
    }
    TagInfo *tag_info = GetTagInfo(tag_name, tag_id);
    if (tag_info || task->IsDirect()) {
      return;
    }
    task->dom_query_ = chi::DomainQuery::GetDirectHash(
        chi::SubDomainId::kGlobalContainers, HashTagNameOrId(tag_id, tag_name));
    task->SetDirect();
    task->UnsetRouted();
    // HILOG(kInfo, "Routing to: {}", task->dom_query_);
  }

  void PutBlobBegin(PutBlobTask *task, char *data, size_t data_size,
                    RunContext &rctx) {}

  void PutBlobEnd(PutBlobTask *task, RunContext &rctx) {}

  void GetBlobBegin(GetBlobTask *task, RunContext &rctx) {}

  void GetBlobEnd(GetBlobTask *task, RunContext &rctx) {}

  /**
   * ========================================
   * TAG Methods
   * ========================================
   * */

  /** Get or create a tag */
  void GetOrCreateTag(GetOrCreateTagTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_ID_MAP_T &tag_id_map = tls.tag_id_map_;
    TAG_MAP_T &tag_map = tls.tag_map_;
    chi::string tag_name(task->tag_name_);
    bool did_create = false;
    if (tag_name.size() > 0) {
      did_create = tag_id_map.find(tag_name) == tag_id_map.end();
    }

    // Emplace bucket if it does not already exist
    TagId tag_id;
    if (did_create) {
      TAG_MAP_T &tag_map = tls.tag_map_;
      tag_id.unique_ = id_alloc_.fetch_add(1);
      tag_id.hash_ = HashTagName(tag_name);
      tag_id.node_id_ = CHI_CLIENT->node_id_;
      HILOG(kDebug, "Creating tag for the first time: {} {}", tag_name.str(),
            tag_id);
      tag_id_map.emplace(tag_name, tag_id);
      tag_map.emplace(tag_id, TagInfo());
      TagInfo &tag = tag_map[tag_id];
      tag.name_ = tag_name;
      tag.tag_id_ = tag_id;
      tag.owner_ = task->blob_owner_;
      tag.internal_size_ = task->backend_size_;
      if (task->flags_.Any(HERMES_SHOULD_STAGE)) {
        client_.RegisterStager(HSHM_DEFAULT_MEM_CTX,
                               chi::DomainQuery::GetGlobalBcast(), tag_id,
                               chi::string(task->tag_name_.str()),
                               chi::string(task->params_.str()));
        tag.flags_.SetBits(HERMES_SHOULD_STAGE);
      }
    } else {
      if (tag_name.size()) {
        HILOG(kDebug, "Found existing tag: {}", tag_name.str());
        tag_id = tag_id_map[tag_name];
      } else {
        HILOG(kDebug, "Found existing tag: {}", task->tag_id_);
        tag_id = task->tag_id_;
      }
    }

    task->tag_id_ = tag_id;
    // task->did_create_ = did_create;
  }
  void MonitorGetOrCreateTag(MonitorModeId mode, GetOrCreateTagTask *task,
                             RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<GetOrCreateTagTask>(task);
        return;
      }
    }
  }
  /** Get an existing tag ID */
  void GetTagId(GetTagIdTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_ID_MAP_T &tag_id_map = tls.tag_id_map_;
    chi::string tag_name(task->tag_name_);
    auto it = tag_id_map.find(tag_name);
    if (it == tag_id_map.end()) {
      task->tag_id_ = TagId::GetNull();
      return;
    }
    task->tag_id_ = it->second;
  }
  void MonitorGetTagId(MonitorModeId mode, GetTagIdTask *task,
                       RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheReadRoute<GetTagIdTask>(task);
        return;
      }
    }
  }

  /** Get the name of a tag */
  void GetTagName(GetTagNameTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    task->tag_name_ = it->second.name_;
  }
  void MonitorGetTagName(MonitorModeId mode, GetTagNameTask *task,
                         RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheReadRoute<GetTagNameTask>(task);
        return;
      }
    }
  }

  /** Destroy a tag */
  void DestroyTag(DestroyTagTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwWriteLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    if (tag.owner_) {
      for (BlobId &blob_id : tag.blobs_) {
        client_.AsyncDestroyBlob(HSHM_DEFAULT_MEM_CTX,
                                 chi::DomainQuery::GetDirectHash(
                                     chi::SubDomainId::kLocalContainers, 0),
                                 task->tag_id_, blob_id,
                                 DestroyBlobTask::kKeepInTag,
                                 TASK_FIRE_AND_FORGET);  // TODO(llogan): route
      }
    }
    if (tag.flags_.Any(HERMES_SHOULD_STAGE)) {
      client_.UnregisterStager(HSHM_DEFAULT_MEM_CTX,
                               chi::DomainQuery::GetGlobalBcast(),
                               task->tag_id_);  // OK
    }
    // Remove tag from maps
    TAG_ID_MAP_T &tag_id_map = tls.tag_id_map_;
    tag_id_map.erase(tag.name_);
    tag_map.erase(it);
  }
  void MonitorDestroyTag(MonitorModeId mode, DestroyTagTask *task,
                         RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<DestroyTagTask>(task);
        return;
      }
    }
  }

  /** Add a blob to the tag */
  void TagAddBlob(TagAddBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    tag.blobs_.emplace_back(task->blob_id_);
  }
  void MonitorTagAddBlob(MonitorModeId mode, TagAddBlobTask *task,
                         RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<TagAddBlobTask>(task);
        return;
      }
    }
  }

  /** Remove a blob from the tag */
  void TagRemoveBlob(TagRemoveBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    auto blob_it =
        std::find(tag.blobs_.begin(), tag.blobs_.end(), task->blob_id_);
    tag.blobs_.erase(blob_it);
  }
  void MonitorTagRemoveBlob(MonitorModeId mode, TagRemoveBlobTask *task,
                            RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<TagRemoveBlobTask>(task);
        return;
      }
    }
  }

  /** Clear blobs from the tag */
  void TagClearBlobs(TagClearBlobsTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    if (tag.owner_) {
      for (BlobId &blob_id : tag.blobs_) {
        client_.AsyncDestroyBlob(HSHM_DEFAULT_MEM_CTX,
                                 chi::DomainQuery::GetDirectHash(
                                     chi::SubDomainId::kLocalContainers, 0),
                                 task->tag_id_, blob_id,
                                 DestroyBlobTask::kKeepInTag,
                                 TASK_FIRE_AND_FORGET);  // TODO(llogan): route
      }
    }
    tag.blobs_.clear();
    tag.internal_size_ = 0;
  }
  void MonitorTagClearBlobs(MonitorModeId mode, TagClearBlobsTask *task,
                            RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<TagClearBlobsTask>(task);
        return;
      }
    }
  }

  /** Get the size of a tag */
  void TagGetSize(TagGetSizeTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      task->size_ = 0;
      return;
    }
    TagInfo &tag = it->second;
    task->size_ = tag.internal_size_;
  }
  void MonitorTagGetSize(MonitorModeId mode, TagGetSizeTask *task,
                         RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheReadRoute<TagGetSizeTask>(task);
        return;
      }
    }
  }

  /** Update the size of a tag */
  void TagUpdateSize(TagUpdateSizeTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    TagInfo &tag = tag_map[task->tag_id_];
    ssize_t internal_size = (ssize_t)tag.internal_size_;
    if (task->mode_ == UpdateSizeMode::kAdd) {
      internal_size += task->update_;
    } else {
      internal_size = std::max(task->update_, internal_size);
    }
    HILOG(kDebug,
          "Updating size of tag {} from {} to {} with update {} (mode={})",
          task->tag_id_, tag.internal_size_, internal_size, task->update_,
          task->mode_);
    tag.internal_size_ = (size_t)internal_size;
  }
  void MonitorTagUpdateSize(MonitorModeId mode, TagUpdateSizeTask *task,
                            RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheWriteRoute<TagUpdateSizeTask>(task);
        return;
      }
    }
  }

  /** Get the set of blobs in the tag */
  void TagGetContainedBlobIds(TagGetContainedBlobIdsTask *task,
                              RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    hipc::vector<BlobId> &blobs = task->blob_ids_;
    blobs.reserve(tag.blobs_.size());
    for (BlobId &blob_id : tag.blobs_) {
      blobs.emplace_back(blob_id);
    }
  }
  void MonitorTagGetContainedBlobIds(MonitorModeId mode,
                                     TagGetContainedBlobIdsTask *task,
                                     RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        TagCacheReadRoute<TagGetContainedBlobIdsTask>(task);
        return;
      }
    }
  }

  /** Flush tag */
  void TagFlush(TagFlushTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    auto it = tag_map.find(task->tag_id_);
    if (it == tag_map.end()) {
      return;
    }
    TagInfo &tag = it->second;
    for (BlobId &blob_id : tag.blobs_) {
      client_.FlushBlob(HSHM_DEFAULT_MEM_CTX,
                        chi::DomainQuery::GetDirectHash(
                            chi::SubDomainId::kLocalContainers, 0),
                        blob_id);  // TODO(llogan): route
    }
    // Flush blobs
  }
  void MonitorTagFlush(MonitorModeId mode, TagFlushTask *task,
                       RunContext &rctx) {}

  /**
   * ========================================
   * BLOB Methods
   * ========================================
   * */

  /** Get or create a blob ID */
  BlobId GetOrCreateBlobId(HermesLane &tls, TagId &tag_id, u32 name_hash,
                           const chi::string &blob_name, bitfield32_t &flags) {
    chi::string blob_name_unique = GetBlobNameWithBucket(tag_id, blob_name);
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    auto it = blob_id_map.find(blob_name_unique);
    if (it == blob_id_map.end()) {
      BlobId blob_id =
          BlobId(CHI_CLIENT->node_id_, name_hash, id_alloc_.fetch_add(1));
      blob_id_map.emplace(blob_name_unique, blob_id);
      flags.SetBits(HERMES_BLOB_DID_CREATE);
      BLOB_MAP_T &blob_map = tls.blob_map_;
      blob_map.emplace(blob_id, BlobInfo());
      BlobInfo &blob_info = blob_map[blob_id];
      blob_info.name_ = blob_name;
      blob_info.blob_id_ = blob_id;
      blob_info.tag_id_ = tag_id;
      blob_info.blob_size_ = 0;
      blob_info.max_blob_size_ = 0;
      blob_info.score_ = 1;
      blob_info.mod_count_ = 0;
      blob_info.access_freq_ = 0;
      blob_info.last_flush_ = 0;
      return blob_id;
    }
    return it->second;
  }
  void GetOrCreateBlobId(GetOrCreateBlobIdTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    chi::string blob_name(task->blob_name_);
    bitfield32_t flags;
    task->blob_id_ = GetOrCreateBlobId(tls, task->tag_id_,
                                       HashBlobName(task->tag_id_, blob_name),
                                       blob_name, flags);
  }
  void MonitorGetOrCreateBlobId(MonitorModeId mode, GetOrCreateBlobIdTask *task,
                                RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetOrCreateBlobIdTask>(task);
        return;
      }
    }
  }

  /** Get the blob ID */
  void GetBlobId(GetBlobIdTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    chi::string blob_name(task->blob_name_);
    chi::string blob_name_unique =
        GetBlobNameWithBucket(task->tag_id_, blob_name);
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    auto it = blob_id_map.find(blob_name_unique);
    if (it == blob_id_map.end()) {
      task->blob_id_ = BlobId::GetNull();
      HILOG(kDebug, "Failed to find blob {} in {}", blob_name.str(),
            task->tag_id_);
      return;
    }
    task->blob_id_ = it->second;
  }
  void MonitorGetBlobId(MonitorModeId mode, GetBlobIdTask *task,
                        RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobIdTask>(task);
        return;
      }
    }
  }

  /** Get blob name */
  void GetBlobName(GetBlobNameTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    task->blob_name_ = blob.name_;
  }
  void MonitorGetBlobName(MonitorModeId mode, GetBlobNameTask *task,
                          RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobNameTask>(task);
        return;
      }
    }
  }

  /** Get the blob size */
  void GetBlobSize(GetBlobSizeTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    if (task->blob_id_.IsNull()) {
      bitfield32_t flags;
      task->blob_id_ = GetOrCreateBlobId(
          tls, task->tag_id_, HashBlobName(task->tag_id_, task->blob_name_),
          chi::string(task->blob_name_), flags);
    }
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      task->size_ = 0;
      return;
    }
    BlobInfo &blob = it->second;
    task->size_ = blob.blob_size_;
  }
  void MonitorGetBlobSize(MonitorModeId mode, GetBlobSizeTask *task,
                          RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobSizeTask>(task);
        return;
      }
    }
  }

  /** Get the score of a blob */
  void GetBlobScore(GetBlobScoreTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    task->score_ = blob.score_;
  }
  void MonitorGetBlobScore(MonitorModeId mode, GetBlobScoreTask *task,
                           RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobScoreTask>(task);
        return;
      }
    }
  }

  /** Get blob buffers */
  void GetBlobBuffers(GetBlobBuffersTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    task->buffers_ = blob.buffers_;
  }
  void MonitorGetBlobBuffers(MonitorModeId mode, GetBlobBuffersTask *task,
                             RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobBuffersTask>(task);
        return;
      }
    }
  }

  /** Put a blob */
  void PutBlob(PutBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    // Get blob ID
    chi::string blob_name(task->blob_name_);
    if (task->blob_id_.IsNull()) {
      task->blob_id_ = GetOrCreateBlobId(tls, task->tag_id_,
                                         HashBlobName(task->tag_id_, blob_name),
                                         blob_name, task->flags_);
    }

    // Get blob struct
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob_info = it->second;
    chi::ScopedCoRwWriteLock blob_info_lock(blob_info.lock_);

    // Stage Blob
    if (task->flags_.Any(HERMES_SHOULD_STAGE) &&
        blob_info.last_flush_ == (size_t)0) {
      // TODO(llogan): Don't hardcore score = 1
      blob_info.last_flush_ = 1;
      client_.StageIn(HSHM_DEFAULT_MEM_CTX,
                      chi::DomainQuery::GetDirectHash(
                          chi::SubDomainId::kLocalContainers, 0),
                      task->tag_id_, blob_info.name_, 1);  // OK
    }

    // Determine amount of additional buffering space needed
    ssize_t bkt_size_diff = 0;
    size_t needed_space = task->blob_off_ + task->data_size_;
    size_t size_diff = 0;
    if (needed_space > blob_info.max_blob_size_) {
      size_diff = needed_space - blob_info.max_blob_size_;
    }
    size_t min_blob_size = task->blob_off_ + task->data_size_;
    if (min_blob_size > blob_info.blob_size_) {
      blob_info.blob_size_ = task->blob_off_ + task->data_size_;
    }
    bkt_size_diff += (ssize_t)size_diff;
    HILOG(kDebug, "The size diff is {} bytes (bkt diff {})", size_diff,
          bkt_size_diff);

    // Use DPE
    std::vector<TargetInfo> targets = targets_;
    std::vector<PlacementSchema> schema_vec;
    if (size_diff > 0) {
      Context ctx;
      auto *dpe = DpeFactory::Get(ctx.dpe_);
      ctx.blob_score_ = task->score_;
      dpe->Placement({size_diff}, targets, ctx, schema_vec);
    }

    // Allocate blob buffers
    for (PlacementSchema &schema : schema_vec) {
      schema.plcmnts_.emplace_back(0, fallback_target_->id_);
      for (size_t sub_idx = 0; sub_idx < schema.plcmnts_.size(); ++sub_idx) {
        // Allocate chi::blocks
        SubPlacement &placement = schema.plcmnts_[sub_idx];
        TargetInfo &bdev = *target_map_[placement.tid_];
        if (placement.size_ == 0) {
          continue;
        }
        std::vector<chi::Block> blocks = bdev.client_.Allocate(
            HSHM_DEFAULT_MEM_CTX,
            chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                            bdev.id_.node_id_),
            placement.size_);
        // Convert to BufferInfo
        size_t t_alloc = 0;
        for (chi::Block &block : blocks) {
          if (block.size_ == 0) {
            continue;
          }
          blob_info.buffers_.emplace_back(placement.tid_, block);
          t_alloc += block.size_;
        }
        // HILOG(kInfo, "(node {}) Placing {}/{} bytes in target {} of bw {}",
        //       CHI_CLIENT->node_id_, t_alloc, placement.size_, placement.tid_,
        //       bdev.stats_->write_bw_);
        // Spill to next tier
        if (t_alloc < placement.size_) {
          SubPlacement &next_placement = schema.plcmnts_[sub_idx + 1];
          size_t diff = placement.size_ - t_alloc;
          next_placement.size_ += diff;
        }
        bdev.stats_->free_ -= t_alloc;
      }
    }

    // Place blob in buffers
    std::vector<FullPtr<chi::bdev::WriteTask>> write_tasks;
    write_tasks.reserve(blob_info.buffers_.size());
    size_t blob_off = task->blob_off_, buf_off = 0;
    size_t buf_left = 0, buf_right = 0;
    size_t blob_right = task->blob_off_ + task->data_size_;
    HILOG(kDebug, "Number of buffers {}", blob_info.buffers_.size());
    bool found_left = false;
    for (BufferInfo &buf : blob_info.buffers_) {
      buf_right = buf_left + buf.size_;
      if (blob_off >= blob_right) {
        break;
      }
      if (buf_left <= blob_off && blob_off < buf_right) {
        found_left = true;
      }
      if (found_left) {
        size_t rel_off = blob_off - buf_left;
        size_t tgt_off = buf.off_ + rel_off;
        size_t buf_size = buf.size_ - rel_off;
        if (buf_right > blob_right) {
          buf_size = blob_right - (buf_left + rel_off);
        }
        HILOG(kDebug, "Writing {} bytes at off {} from target {}", buf_size,
              tgt_off, buf.tid_);
        TargetInfo &target = *target_map_[buf.tid_];
        FullPtr<chi::bdev::WriteTask> write_task = target.client_.AsyncWrite(
            HSHM_DEFAULT_MEM_CTX,
            chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                            0),
            task->data_ + buf_off, tgt_off, buf_size);
        write_tasks.emplace_back(write_task);
        buf_off += buf_size;
        blob_off = buf_right;
      }
      buf_left += buf.size_;
    }
    blob_info.max_blob_size_ = blob_off;

    // Wait for the placements to complete
    task->Wait(write_tasks);
    for (FullPtr<chi::bdev::WriteTask> &write_task : write_tasks) {
      CHI_CLIENT->DelTask(HSHM_DEFAULT_MEM_CTX, write_task);
    }

    // Update information
    if (task->flags_.Any(HERMES_SHOULD_STAGE)) {
      STAGER_MAP_T &stager_map = tls.stager_map_;
      chi::ScopedCoMutex stager_map_lock(tls.stager_map_lock_);
      auto it = stager_map.find(task->tag_id_);
      if (it == stager_map.end()) {
        HELOG(kWarning, "Could not find stager for tag {}. Not updating size",
              task->tag_id_);
      } else {
        std::shared_ptr<AbstractStager> &stager = it->second;
        stager->UpdateSize(HSHM_DEFAULT_MEM_CTX, client_, task->tag_id_,
                           blob_info.name_.str(), task->blob_off_,
                           task->data_size_);
      }
    } else {
      client_.AsyncTagUpdateSize(HSHM_DEFAULT_MEM_CTX,
                                 chi::DomainQuery::GetDirectHash(
                                     chi::SubDomainId::kGlobalContainers, 0),
                                 task->tag_id_, bkt_size_diff,
                                 UpdateSizeMode::kAdd);
    }
    if (task->flags_.Any(HERMES_BLOB_DID_CREATE)) {
      client_.AsyncTagAddBlob(HSHM_DEFAULT_MEM_CTX,
                              chi::DomainQuery::GetDirectHash(
                                  chi::SubDomainId::kGlobalContainers, 0),
                              task->tag_id_, task->blob_id_);
    }
    //    if (task->flags_.Any(HERMES_HAS_DERIVED)) {
    //      client_.AsyncRegisterData(task->task_node_ + 1,
    //                                task->tag_id_,
    //                                task->blob_name_->str(),
    //                                task->blob_id_,
    //                                task->blob_off_,
    //                                task->data_size_);
    //    }

    // Free data
    HILOG(kDebug, "Completing PUT for {}", blob_name.str());
    blob_info.UpdateWriteStats();
    IoStat *stat;
    hshm::qtok_t qtok = io_pattern_.push(IoStat{
        IoType::kWrite, task->blob_id_, task->tag_id_, task->data_size_, 0});
    io_pattern_.peek(stat, qtok);
    stat->id_ = qtok.id_;
  }
  void MonitorPutBlob(MonitorModeId mode, PutBlobTask *task, RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheWriteRoute<PutBlobTask>(task);
        return;
      }
    }
  }

  /** Get a blob */
  void GetBlob(GetBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    // Get blob struct
    if (task->blob_id_.IsNull()) {
      chi::string blob_name(task->blob_name_);
      task->blob_id_ = GetOrCreateBlobId(tls, task->tag_id_,
                                         HashBlobName(task->tag_id_, blob_name),
                                         blob_name, task->flags_);
    }

    // Get blob map struct
    BLOB_MAP_T &blob_map = tls.blob_map_;
    BlobInfo &blob_info = blob_map[task->blob_id_];

    // Stage Blob
    if (task->flags_.Any(HERMES_SHOULD_STAGE) &&
        blob_info.last_flush_ == (size_t)0) {
      // TODO(llogan): Don't hardcore score = 1
      blob_info.last_flush_ = 1;
      client_.StageIn(HSHM_DEFAULT_MEM_CTX,
                      chi::DomainQuery::GetDirectHash(
                          chi::SubDomainId::kLocalContainers, 0),
                      task->tag_id_, blob_info.name_, 1);  // OK
    }

    // Get blob struct
    chi::ScopedCoRwReadLock blob_info_lock(blob_info.lock_);

    // Read blob from buffers
    std::vector<FullPtr<chi::bdev::ReadTask>> read_tasks;
    read_tasks.reserve(blob_info.buffers_.size());
    HILOG(kDebug,
          "Getting blob {} of size {} starting at offset {} "
          "(total_blob_size={}, buffers={})",
          task->blob_id_, task->data_size_, task->blob_off_,
          blob_info.blob_size_, blob_info.buffers_.size());
    size_t blob_off = task->blob_off_;
    size_t buf_left = 0, buf_right = 0;
    size_t buf_off = 0;
    size_t blob_right = task->blob_off_ + task->data_size_;
    bool found_left = false;
    for (BufferInfo &buf : blob_info.buffers_) {
      buf_right = buf_left + buf.size_;
      if (blob_off >= blob_right) {
        break;
      }
      if (buf_left <= blob_off && blob_off < buf_right) {
        found_left = true;
      }
      if (found_left) {
        size_t rel_off = blob_off - buf_left;
        size_t tgt_off = buf.off_ + rel_off;
        size_t buf_size = buf.size_ - rel_off;
        if (buf_right > blob_right) {
          buf_size = blob_right - (buf_left + rel_off);
        }
        HILOG(kDebug, "Loading {} bytes at off {} from target {}", buf_size,
              tgt_off, buf.tid_);
        TargetInfo &target = *target_map_[buf.tid_];
        FullPtr<chi::bdev::ReadTask> read_task = target.client_.AsyncRead(
            HSHM_DEFAULT_MEM_CTX,
            chi::DomainQuery::GetDirectHash(chi::SubDomainId::kGlobalContainers,
                                            0),
            task->data_ + buf_off, tgt_off, buf_size);
        read_tasks.emplace_back(read_task);
        buf_off += buf_size;
        blob_off = buf_right;
      }
      buf_left += buf.size_;
    }
    task->Wait(read_tasks);
    for (FullPtr<chi::bdev::ReadTask> &read_task : read_tasks) {
      CHI_CLIENT->DelTask(HSHM_DEFAULT_MEM_CTX, read_task);
    }
    task->data_size_ = buf_off;
    blob_info.UpdateReadStats();
    IoStat *stat;
    hshm::qtok_t qtok = io_pattern_.push(IoStat{
        IoType::kRead, task->blob_id_, task->tag_id_, task->data_size_, 0});
    io_pattern_.peek(stat, qtok);
    stat->id_ = qtok.id_;
  }
  void MonitorGetBlob(MonitorModeId mode, GetBlobTask *task, RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<GetBlobTask>(task);
        return;
      }
    }
  }

  /** Truncate a blob (TODO) */
  void TruncateBlob(TruncateBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
  }
  void MonitorTruncateBlob(MonitorModeId mode, TruncateBlobTask *task,
                           RunContext &rctx) {}

  /** Destroy blob */
  void DestroyBlob(DestroyBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwWriteLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    // Free blob buffers
    for (BufferInfo &buf : blob.buffers_) {
      TargetInfo &target = *target_map_[buf.tid_];
      target.client_.Free(HSHM_DEFAULT_MEM_CTX,
                          chi::DomainQuery::GetDirectHash(
                              chi::SubDomainId::kGlobalContainers, 0),
                          buf);
      target.stats_->free_ += buf.size_;
    }
    // Remove blob from the tag
    if (!task->flags_.Any(DestroyBlobTask::kKeepInTag)) {
      client_.TagRemoveBlob(HSHM_DEFAULT_MEM_CTX,
                            chi::DomainQuery::GetDirectHash(
                                chi::SubDomainId::kLocalContainers, 0),
                            blob.tag_id_, task->blob_id_);  // Route
    }
    // Remove the blob from the maps
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    blob_id_map.erase(blob.GetBlobNameWithBucket());
    blob_map.erase(it);
  }
  void MonitorDestroyBlob(MonitorModeId mode, DestroyBlobTask *task,
                          RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheWriteRoute<DestroyBlobTask>(task);
        return;
      }
    }
  }

  /** Tag a blob */
  void TagBlob(TagBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    blob.tags_.push_back(task->tag_);
  }
  void MonitorTagBlob(MonitorModeId mode, TagBlobTask *task, RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheWriteRoute<TagBlobTask>(task);
        return;
      }
    }
  }

  /** Check if blob has a tag */
  void BlobHasTag(BlobHasTagTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    auto it = blob_map.find(task->blob_id_);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob = it->second;
    task->has_tag_ = std::find(blob.tags_.begin(), blob.tags_.end(),
                               task->tag_) != blob.tags_.end();
  }
  void MonitorBlobHasTag(MonitorModeId mode, BlobHasTagTask *task,
                         RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheReadRoute<BlobHasTagTask>(task);
        return;
      }
    }
  }

  /** Change blob composition */
  void ReorganizeBlob(ReorganizeBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    BLOB_MAP_T &blob_map = tls.blob_map_;
    // Get blob ID
    chi::string blob_name(task->blob_name_);
    if (task->blob_id_.IsNull()) {
      auto blob_id_map_it = blob_id_map.find(blob_name);
      if (blob_id_map_it == blob_id_map.end()) {
        return;
      }
      task->blob_id_ = blob_id_map_it->second;
    }
    // Get blob struct
    auto blob_map_it = blob_map.find(task->blob_id_);
    if (blob_map_it == blob_map.end()) {
      return;
    }
    BlobInfo &blob_info = blob_map_it->second;
    // Check if it is worth updating the score
    // TODO(llogan)
    // Set the new score
    if (task->is_user_score_) {
      blob_info.user_score_ = task->score_;
      blob_info.score_ = blob_info.user_score_;
    } else {
      blob_info.score_ = task->score_;
    }
    // Get the blob
    FullPtr<char> data =
        CHI_CLIENT->AllocateBuffer(HSHM_DEFAULT_MEM_CTX, blob_info.blob_size_);
    client_.GetBlob(
        HSHM_DEFAULT_MEM_CTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kLocalContainers, 0),
        task->tag_id_, task->blob_id_, 0, blob_info.blob_size_, data.shm_,
        0);  // OK
    // Put the blob with the new score
    client_.AsyncPutBlob(
        HSHM_DEFAULT_MEM_CTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kLocalContainers, 0),
        task->tag_id_, chi::string(""), task->blob_id_, 0, blob_info.blob_size_,
        data.shm_, blob_info.score_, TASK_FIRE_AND_FORGET | TASK_DATA_OWNER,
        0);  // OK
  }
  void MonitorReorganizeBlob(MonitorModeId mode, ReorganizeBlobTask *task,
                             RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kSchedule: {
        BlobCacheWriteRoute<ReorganizeBlobTask>(task);
        return;
      }
    }
  }

  /** FlushBlob */
  void _FlushBlob(HermesLane &tls, BlobId blob_id, RunContext &rctx) {
    BLOB_MAP_T &blob_map = tls.blob_map_;
    // Can we find the blob
    auto it = blob_map.find(blob_id);
    if (it == blob_map.end()) {
      return;
    }
    BlobInfo &blob_info = it->second;
    FlushInfo flush_info;
    flush_info.blob_info_ = &blob_info;
    flush_info.mod_count_ = blob_info.mod_count_;
    // Is the blob already flushed?
    if (blob_info.last_flush_ <= 0 ||
        flush_info.mod_count_ <= blob_info.last_flush_) {
      return;
    }
    HILOG(kDebug, "Flushing blob {} (mod_count={}, last_flush={})",
          blob_info.blob_id_, flush_info.mod_count_, blob_info.last_flush_);
    // If the worker is being flushed
    if (rctx.worker_props_.Any(CHI_WORKER_IS_FLUSHING)) {
      ++rctx.flush_->count_;
    }
    FullPtr<char> data =
        CHI_CLIENT->AllocateBuffer(HSHM_DEFAULT_MEM_CTX, blob_info.blob_size_);
    client_.GetBlob(
        HSHM_DEFAULT_MEM_CTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kLocalContainers, 0),
        blob_info.tag_id_, blob_info.blob_id_, 0, blob_info.blob_size_,
        data.shm_, 0);  // OK
    adapter::BlobPlacement plcmnt;
    plcmnt.DecodeBlobName(blob_info.name_, 4096);
    HILOG(kDebug, "Flushing blob {} with first entry {}", plcmnt.page_,
          (int)data.ptr_[0]);
    client_.StageOut(
        HSHM_DEFAULT_MEM_CTX,
        chi::DomainQuery::GetDirectHash(chi::SubDomainId::kLocalContainers, 0),
        blob_info.tag_id_, blob_info.name_, data.shm_, blob_info.blob_size_,
        TASK_DATA_OWNER);  // OK
    HILOG(kDebug, "Finished flushing blob {} with first entry {}", plcmnt.page_,
          (int)data.ptr_[0]);
    blob_info.last_flush_ = flush_info.mod_count_;
  }
  void FlushBlob(FlushBlobTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    _FlushBlob(tls, task->blob_id_, rctx);
  }
  void MonitorFlushBlob(MonitorModeId mode, FlushBlobTask *task,
                        RunContext &rctx) {}

  /** Flush blobs back to storage */
  void FlushData(FlushDataTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_ID_MAP_T &blob_id_map = tls.blob_id_map_;
    BLOB_MAP_T &blob_map = tls.blob_map_;
    for (auto &it : blob_map) {
      BlobInfo &blob_info = it.second;
      // Update blob scores
      //      float new_score = MakeScore(blob_info, now);
      //      blob_info.score_ = new_score;
      //      if (ShouldReorganize<true>(blob_info, new_score,
      //      task->task_node_)) {
      //        Context ctx;
      //        FullPtr<ReorganizeBlobTask> reorg_task =
      //            blob_mdm_.AsyncReorganizeBlob(task->task_node_ + 1,
      //                                          blob_info.tag_id_,
      //                                          chi::string(""),
      //                                          blob_info.blob_id_,
      //                                          new_score, false, ctx,
      //                                          TASK_LOW_LATENCY);
      //        reorg_task->Wait<TASK_YIELD_CO>(task);
      //        CHI_CLIENT->DelTask(HSHM_DEFAULT_MEM_CTX, reorg_task);
      //      }
      //      blob_info.access_freq_ = 0;

      // Flush data
      _FlushBlob(tls, blob_info.blob_id_, rctx);
    }
  }
  void MonitorFlushData(MonitorModeId mode, FlushDataTask *task,
                        RunContext &rctx) {}

  /** Monitor function used by all metadata poll functions */
  template <typename PollTaskT, typename MD>
  void MonitorPollMetadata(MonitorModeId mode, PollTaskT *task,
                           RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kReplicaAgg: {
        std::vector<FullPtr<Task>> &replicas = *rctx.replicas_;
        std::vector<MD> stats_agg;
        stats_agg.reserve(task->max_count_);
        for (FullPtr<Task> &replica : replicas) {
          PollTaskT *replica_task = replica.Cast<PollTaskT>().ptr_;
          // Merge replicas
          auto stats = replica_task->GetStats();
          size_t append_count = stats.size();
          if (task->max_count_ > 0 && stats_agg.size() < task->max_count_) {
            append_count =
                std::min(append_count, task->max_count_ - stats_agg.size());
          }
          stats_agg.insert(stats_agg.end(), stats.begin(),
                           stats.begin() + append_count);
        }
        task->SetStats(stats_agg);
      }
    }
  }

  /** Poll blob metadata */
  void PollBlobMetadata(PollBlobMetadataTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock blob_map_lock(tls.blob_map_lock_);
    BLOB_MAP_T &blob_map = tls.blob_map_;
    std::vector<BlobInfo> blob_mdms;
    blob_mdms.reserve(blob_map.size());
    std::string filter = task->filter_.str();
    for (const std::pair<BlobId, BlobInfo> &blob_part : blob_map) {
      const BlobInfo &blob_info = blob_part.second;
      if (!filter.empty()) {
        if (!std::regex_match(blob_info.name_.str(), std::regex(filter))) {
          continue;
        }
      }
      blob_mdms.emplace_back(blob_info);
    }
    task->SetStats(blob_mdms);
  }
  void MonitorPollBlobMetadata(MonitorModeId mode, PollBlobMetadataTask *task,
                               RunContext &rctx) {
    MonitorPollMetadata<PollBlobMetadataTask, BlobInfo>(mode, task, rctx);
  }

  /** Poll target metadata */
  void PollTargetMetadata(PollTargetMetadataTask *task, RunContext &rctx) {
    std::vector<TargetStats> target_mdms;
    target_mdms.reserve(targets_.size());
    for (const TargetInfo &bdev_client : targets_) {
      bool is_remote = bdev_client.id_.node_id_ != CHI_CLIENT->node_id_;
      if (is_remote) {
        continue;
      }
      TargetStats stats;
      stats.tgt_id_ = bdev_client.id_;
      stats.node_id_ = CHI_CLIENT->node_id_;
      stats.rem_cap_ = bdev_client.stats_->free_;
      stats.max_cap_ = bdev_client.stats_->max_cap_;
      stats.bandwidth_ = bdev_client.stats_->write_bw_;
      stats.latency_ = bdev_client.stats_->write_latency_;
      stats.score_ = bdev_client.score_;
      target_mdms.emplace_back(stats);
    }
    task->SetStats(target_mdms);
  }
  void MonitorPollTargetMetadata(MonitorModeId mode,
                                 PollTargetMetadataTask *task,
                                 RunContext &rctx) {
    MonitorPollMetadata<PollTargetMetadataTask, TargetStats>(mode, task, rctx);
  }

  /** The PollTagMetadata method */
  void PollTagMetadata(PollTagMetadataTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoRwReadLock tag_map_lock(tls.tag_map_lock_);
    TAG_MAP_T &tag_map = tls.tag_map_;
    std::vector<TagInfo> stats;
    std::string filter = task->filter_.str();
    for (auto &it : tag_map) {
      TagInfo &tag = it.second;
      if (!filter.empty()) {
        if (!std::regex_match(tag.name_.str(), std::regex(filter))) {
          continue;
        }
      }
      stats.emplace_back(tag);
    }
    task->SetStats(stats);
  }
  void MonitorPollTagMetadata(MonitorModeId mode, PollTagMetadataTask *task,
                              RunContext &rctx) {
    MonitorPollMetadata<PollTagMetadataTask, TagInfo>(mode, task, rctx);
  }

  /** The PollAccessPattern method */
  void PollAccessPattern(PollAccessPatternTask *task, RunContext &rctx) {
    std::vector<IoStat> io_pattern;
    int depth = io_pattern_.GetDepth();
    int qsize = io_pattern_.GetSize();
    int iter_size = std::min(depth, qsize);
    io_pattern.reserve(iter_size);
    for (int i = 0; i < iter_size; ++i) {
      IoStat *stat;
      hshm::qtok_t qtok = io_pattern_.peek(stat, i);
      if (task->last_access_ > 0 && stat->id_ < task->last_access_) {
        continue;
      }
      io_pattern.emplace_back(*stat);
    }
    std::sort(io_pattern.begin(), io_pattern.end(),
              [](const IoStat &a, const IoStat &b) { return a.id_ < b.id_; });
    task->io_pattern_ = io_pattern;
    if (!io_pattern.empty()) {
      task->last_access_ = io_pattern.back().id_;
    }
  }
  void MonitorPollAccessPattern(MonitorModeId mode, PollAccessPatternTask *task,
                                RunContext &rctx) {}

  /**
   * ========================================
   * STAGING Tasks
   * ========================================
   * */

  /** The RegisterStager method */
  void RegisterStager(RegisterStagerTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoMutex stager_map_lock(tls.stager_map_lock_);
    STAGER_MAP_T &stager_map = tls.stager_map_;
    std::string tag_name = task->tag_name_.str();
    std::string params = task->params_.str();
    HILOG(kDebug, "Registering stager {}: {}", task->bkt_id_, tag_name);
    std::shared_ptr<AbstractStager> stager =
        StagerFactory::Get(tag_name, params);
    stager->RegisterStager(HSHM_DEFAULT_MEM_CTX, task->tag_name_.str(),
                           task->params_.str());
    stager_map.emplace(task->bkt_id_, std::move(stager));
    HILOG(kDebug, "Finished registering stager {}: {}", task->bkt_id_,
          tag_name);
  }
  void MonitorRegisterStager(MonitorModeId mode, RegisterStagerTask *task,
                             RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kReplicaAgg: {
        std::vector<FullPtr<Task>> &replicas = *rctx.replicas_;
      }
    }
  }

  /** The UnregisterStager method */
  void UnregisterStager(UnregisterStagerTask *task, RunContext &rctx) {
    HILOG(kDebug, "Unregistering stager {}", task->bkt_id_);
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoMutex stager_map_lock(tls.stager_map_lock_);
    STAGER_MAP_T &stager_map = tls.stager_map_;
    if (stager_map.find(task->bkt_id_) == stager_map.end()) {
      return;
    }
    stager_map.erase(task->bkt_id_);
  }
  void MonitorUnregisterStager(MonitorModeId mode, UnregisterStagerTask *task,
                               RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kReplicaAgg: {
        std::vector<FullPtr<Task>> &replicas = *rctx.replicas_;
      }
    }
  }

  /** The StageIn method */
  void StageIn(StageInTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoMutex stager_map_lock(tls.stager_map_lock_);
    STAGER_MAP_T &stager_map = tls.stager_map_;
    STAGER_MAP_T::iterator it = stager_map.find(task->bkt_id_);
    if (it == stager_map.end()) {
      // HELOG(kError, "Could not find stager for bucket: {}", task->bkt_id_);
      // TODO(llogan): Probably should add back...
      // task->SetModuleComplete();
      return;
    }
    std::shared_ptr<AbstractStager> &stager = it->second;
    stager->StageIn(HSHM_DEFAULT_MEM_CTX, client_, task->bkt_id_,
                    task->blob_name_.str(), task->score_);
  }
  void MonitorStageIn(MonitorModeId mode, StageInTask *task, RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kReplicaAgg: {
        std::vector<FullPtr<Task>> &replicas = *rctx.replicas_;
      }
    }
  }

  /** The StageOut method */
  void StageOut(StageOutTask *task, RunContext &rctx) {
    HermesLane &tls = tls_[CHI_CUR_LANE->lane_id_];
    chi::ScopedCoMutex stager_map_lock(tls.stager_map_lock_);
    STAGER_MAP_T &stager_map = tls.stager_map_;
    STAGER_MAP_T::iterator it = stager_map.find(task->bkt_id_);
    if (it == stager_map.end()) {
      HELOG(kError, "Could not find stager for bucket: {}", task->bkt_id_);
      return;
    }
    std::shared_ptr<AbstractStager> &stager = it->second;
    stager->StageOut(HSHM_DEFAULT_MEM_CTX, client_, task->bkt_id_,
                     task->blob_name_.str(), task->data_, task->data_size_);
  }
  void MonitorStageOut(MonitorModeId mode, StageOutTask *task,
                       RunContext &rctx) {
    switch (mode) {
      case MonitorMode::kReplicaAgg: {
        std::vector<FullPtr<Task>> &replicas = *rctx.replicas_;
      }
    }
  }

 public:
#include "hermes_core/hermes_core_lib_exec.h"
};

}  // namespace hermes

CHI_TASK_CC(hermes::Server, "hermes_core");
