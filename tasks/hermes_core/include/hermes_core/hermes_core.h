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

#ifndef CHI_hermes_core_H_
#define CHI_hermes_core_H_

#include "hermes_core_tasks.h"

namespace hermes {

/** Create hermes_core requests */
class Client : public ModuleClient {
 public:
  /** Default constructor */
  Client() = default;

  /** Destructor */
  ~Client() = default;

  /** Create a task state */
  void Create(const hipc::MemContext &mctx, const DomainQuery &dom_query,
              const DomainQuery &affinity, const std::string &pool_name,
              const CreateContext &ctx = CreateContext()) {
    FullPtr<CreateTask> task =
        AsyncCreate(mctx, dom_query, affinity, pool_name, ctx);
    task->Wait();
    Init(task->ctx_.id_);
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(Create);

  /** Destroy task state + queue */
  HSHM_INLINE
  void Destroy(const hipc::MemContext &mctx, const DomainQuery &dom_query) {
    CHI_ADMIN->DestroyContainer(mctx, dom_query, id_);
  }

  /**====================================
   * Tag Operations
   * ===================================*/

  /** Update statistics after blob PUT (fire & forget) */
  CHI_TASK_METHODS(TagUpdateSize);

  /** Create a tag or get the ID of existing tag */
  HSHM_INLINE
  TagId GetOrCreateTag(const hipc::MemContext &mctx,
                       const DomainQuery &dom_query,
                       const chi::string &tag_name, bool blob_owner,
                       size_t backend_size, u32 flags,
                       const Context &ctx = Context()) {
    FullPtr<GetOrCreateTagTask> task = AsyncGetOrCreateTag(
        mctx, dom_query, tag_name, blob_owner, backend_size, flags, ctx);
    task->Wait();
    TagId tag_id = task->tag_id_;
    CHI_CLIENT->DelTask(mctx, task);
    return tag_id;
  }
  CHI_TASK_METHODS(GetOrCreateTag);

  /** Get tag ID */
  TagId GetTagId(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 const chi::string &tag_name) {
    FullPtr<GetTagIdTask> task = AsyncGetTagId(mctx, dom_query, tag_name);
    task->Wait();
    TagId tag_id = task->tag_id_;
    CHI_CLIENT->DelTask(mctx, task);
    return tag_id;
  }
  CHI_TASK_METHODS(GetTagId);

  /** Get tag name */
  chi::string GetTagName(const hipc::MemContext &mctx,
                         const DomainQuery &dom_query, const TagId &tag_id) {
    FullPtr<GetTagNameTask> task = AsyncGetTagName(mctx, dom_query, tag_id);
    task->Wait();
    chi::string tag_name(task->tag_name_.str());
    CHI_CLIENT->DelTask(mctx, task);
    return tag_name;
  }
  CHI_TASK_METHODS(GetTagName);

  /** Destroy tag */
  void DestroyTag(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                  const TagId &tag_id) {
    FullPtr<DestroyTagTask> task = AsyncDestroyTag(mctx, dom_query, tag_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(DestroyTag);

  /** Add a blob to a tag */
  void TagAddBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                  const TagId &tag_id, const BlobId &blob_id) {
    FullPtr<TagAddBlobTask> task =
        AsyncTagAddBlob(mctx, dom_query, tag_id, blob_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TagAddBlob);

  /** Remove a blob from a tag */
  void TagRemoveBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                     const TagId &tag_id, const BlobId &blob_id) {
    FullPtr<TagRemoveBlobTask> task =
        AsyncTagRemoveBlob(mctx, dom_query, tag_id, blob_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TagRemoveBlob);

  /** Clear blobs from a tag */
  void TagClearBlobs(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                     const TagId &tag_id) {
    FullPtr<TagClearBlobsTask> task =
        AsyncTagClearBlobs(mctx, dom_query, tag_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TagClearBlobs);

  /** Get the size of a bucket */
  size_t GetSize(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 const TagId &tag_id) {
    FullPtr<TagGetSizeTask> task = AsyncTagGetSize(mctx, dom_query, tag_id);
    task->Wait();
    size_t size = task->size_;
    CHI_CLIENT->DelTask(mctx, task);
    return size;
  }
  CHI_TASK_METHODS(TagGetSize);

  /** Get contained blob ids */
  std::vector<BlobId> TagGetContainedBlobIds(const hipc::MemContext &mctx,
                                             const DomainQuery &dom_query,
                                             const TagId &tag_id) {
    FullPtr<TagGetContainedBlobIdsTask> task =
        AsyncTagGetContainedBlobIds(mctx, dom_query, tag_id);
    task->Wait();
    std::vector<BlobId> blob_ids = task->blob_ids_.vec();
    CHI_CLIENT->DelTask(mctx, task);
    return blob_ids;
  }
  CHI_TASK_METHODS(TagGetContainedBlobIds);

  /** Flush tag */
  void TagFlush(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                const TagId &tag_id) {
    FullPtr<TagFlushTask> task = AsyncTagFlush(mctx, dom_query, tag_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TagFlush);

  /**====================================
   * Blob Operations
   * ===================================*/

  /**
   * Get \a blob_name BLOB from \a bkt_id bucket
   * */
  BlobId GetOrCreateBlob(const hipc::MemContext &mctx,
                         const DomainQuery &dom_query, const TagId &tag_id,
                         const chi::string &blob_name) {
    FullPtr<GetOrCreateBlobIdTask> task =
        AsyncGetOrCreateBlobId(mctx, dom_query, tag_id, blob_name);
    task->Wait();
    BlobId blob_id = task->blob_id_;
    CHI_CLIENT->DelTask(mctx, task);
    return blob_id;
  }
  CHI_TASK_METHODS(GetOrCreateBlobId);

  /**
   * Create a blob's metadata
   *
   * @param tag_id id of the bucket
   * @param blob_name semantic blob name
   * @param blob_id the id of the blob
   * @param blob_off the offset of the data placed in existing blob
   * @param blob_size the amount of data being placed
   * @param blob a SHM pointer to the data to place
   * @param score the current score of the blob
   * @param replace whether to replace the blob if it exists
   * @param[OUT] did_create whether the blob was created or not
   * */
  size_t PutBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 TagId tag_id, const chi::string &blob_name,
                 const BlobId &blob_id, size_t blob_off, size_t blob_size,
                 const hipc::Pointer &blob, float score, u32 task_flags,
                 u32 hermes_flags, Context ctx = Context()) {
    FullPtr<PutBlobTask> task =
        AsyncPutBlob(mctx, dom_query, tag_id, blob_name, blob_id, blob_off,
                     blob_size, blob, score, task_flags, hermes_flags, ctx);
    task->Wait();
    size_t true_size = task->data_size_;
    CHI_CLIENT->DelTask(mctx, task);
    return true_size;
  }
  CHI_TASK_METHODS(PutBlob);

  /** Get a blob's data */
  size_t GetBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 const TagId &tag_id, const BlobId &blob_id, size_t off,
                 ssize_t data_size, hipc::Pointer &data, u32 hermes_flags,
                 const Context &ctx = Context()) {
    FullPtr<GetBlobTask> task =
        AsyncGetBlob(mctx, dom_query, tag_id, chi::string(""), blob_id, off,
                     data_size, data, hermes_flags, ctx);
    task->Wait();
    data = task->data_;
    size_t true_size = task->data_size_;
    CHI_CLIENT->DelTask(mctx, task);
    return true_size;
  }
  CHI_TASK_METHODS(GetBlob);

  /**
   * Reorganize a blob
   *
   * @param blob_id id of the blob being reorganized
   * @param score the new score of the blob
   * @param node_id the node to reorganize the blob to
   * */
  CHI_TASK_METHODS(ReorganizeBlob);

  /**
   * Tag a blob
   *
   * @param blob_id id of the blob being tagged
   * @param tag_name tag name
   * */
  void TagBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
               const TagId &tag_id, const BlobId &blob_id, const TagId &tag) {
    FullPtr<TagBlobTask> task =
        AsyncTagBlob(mctx, dom_query, tag_id, blob_id, tag);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TagBlob);

  /**
   * Check if blob has a tag
   * */
  bool BlobHasTag(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                  const TagId &tag_id, const BlobId &blob_id,
                  const TagId &tag) {
    FullPtr<BlobHasTagTask> task =
        AsyncBlobHasTag(mctx, dom_query, tag_id, blob_id, tag);
    task->Wait();
    bool has_tag = task->has_tag_;
    CHI_CLIENT->DelTask(mctx, task);
    return has_tag;
  }
  CHI_TASK_METHODS(BlobHasTag);

  /**
   * Get \a blob_name BLOB from \a bkt_id bucket
   * */
  BlobId GetBlobId(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                   const TagId &tag_id, const chi::string &blob_name) {
    FullPtr<GetBlobIdTask> task =
        AsyncGetBlobId(mctx, dom_query, tag_id, blob_name);
    task->Wait();
    BlobId blob_id = task->blob_id_;
    CHI_CLIENT->DelTask(mctx, task);
    return blob_id;
  }
  CHI_TASK_METHODS(GetBlobId);

  /**
   * Get \a blob_name BLOB name from \a blob_id BLOB id
   * */
  std::string GetBlobName(const hipc::MemContext &mctx,
                          const DomainQuery &dom_query, const TagId &tag_id,
                          const BlobId &blob_id) {
    FullPtr<GetBlobNameTask> task =
        AsyncGetBlobName(mctx, dom_query, tag_id, blob_id);
    task->Wait();
    std::string blob_name = task->blob_name_.str();
    CHI_CLIENT->DelTask(mctx, task);
    return blob_name;
  }
  CHI_TASK_METHODS(GetBlobName);

  /**
   * Get \a size from \a blob_id BLOB id
   * */
  size_t GetBlobSize(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                     const TagId &tag_id, const chi::string &blob_name,
                     const BlobId &blob_id) {
    FullPtr<GetBlobSizeTask> task =
        AsyncGetBlobSize(mctx, dom_query, tag_id, blob_name, blob_id);
    task->Wait();
    size_t size = task->size_;
    CHI_CLIENT->DelTask(mctx, task);
    return size;
  }
  CHI_TASK_METHODS(GetBlobSize);

  /**
   * Get \a score from \a blob_id BLOB id
   * */
  float GetBlobScore(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                     const TagId &tag_id, const BlobId &blob_id) {
    FullPtr<GetBlobScoreTask> task =
        AsyncGetBlobScore(mctx, dom_query, tag_id, blob_id);
    task->Wait();
    float score = task->score_;
    CHI_CLIENT->DelTask(mctx, task);
    return score;
  }
  CHI_TASK_METHODS(GetBlobScore);

  /**
   * Get \a blob_id blob's buffers
   * */
  std::vector<BufferInfo> GetBlobBuffers(const hipc::MemContext &mctx,
                                         const DomainQuery &dom_query,
                                         const TagId &tag_id,
                                         const BlobId &blob_id) {
    FullPtr<GetBlobBuffersTask> task =
        AsyncGetBlobBuffers(mctx, dom_query, tag_id, blob_id);
    task->Wait();
    std::vector<BufferInfo> buffers(task->buffers_.vec());
    CHI_CLIENT->DelTask(mctx, task);
    return buffers;
  }
  CHI_TASK_METHODS(GetBlobBuffers)

  /**
   * Truncate a blob to a new size
   * */
  void TruncateBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                    const TagId &tag_id, const BlobId &blob_id,
                    size_t new_size) {
    FullPtr<TruncateBlobTask> task =
        AsyncTruncateBlob(mctx, dom_query, tag_id, blob_id, new_size);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(TruncateBlob);

  /**
   * Destroy \a blob_id blob in \a bkt_id bucket
   * */
  void DestroyBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                   const TagId &tag_id, const BlobId &blob_id,
                   u32 blob_flags = 0) {
    FullPtr<DestroyBlobTask> task =
        AsyncDestroyBlob(mctx, dom_query, tag_id, blob_id, blob_flags);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(DestroyBlob);

  /** FlushBlob task */
  void FlushBlob(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 const BlobId &blob_id) {
    FullPtr<FlushBlobTask> task = AsyncFlushBlob(mctx, dom_query, blob_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(FlushBlob);

  /** FlushData task */
  void FlushData(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                 int period_sec = 5) {
    FullPtr<FlushDataTask> task = AsyncFlushData(mctx, dom_query, period_sec);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(FlushData);

  /** PollBlobMetadata task */
  std::vector<BlobInfo> PollBlobMetadata(const hipc::MemContext &mctx,
                                         const DomainQuery &dom_query,
                                         const std::string &filter,
                                         int max_count) {
    FullPtr<PollBlobMetadataTask> task =
        AsyncPollBlobMetadata(mctx, dom_query, filter, max_count);
    task->Wait();
    std::vector<BlobInfo> stats = task->GetStats();
    CHI_CLIENT->DelTask(mctx, task);
    return stats;
  }
  CHI_TASK_METHODS(PollBlobMetadata);

  /** PollTargetMetadata task */
  std::vector<TargetStats> PollTargetMetadata(const hipc::MemContext &mctx,
                                              const DomainQuery &dom_query,
                                              const std::string &filter,
                                              int max_count) {
    FullPtr<PollTargetMetadataTask> task =
        AsyncPollTargetMetadata(mctx, dom_query, filter, max_count);
    task->Wait();
    std::vector<TargetStats> stats = task->GetStats();
    CHI_CLIENT->DelTask(mctx, task);
    return stats;
  }
  CHI_TASK_METHODS(PollTargetMetadata);

  /** PollTagMetadata task */
  std::vector<TagInfo> PollTagMetadata(const hipc::MemContext &mctx,
                                       const DomainQuery &dom_query,
                                       const std::string &filter,
                                       int max_count) {
    FullPtr<PollTagMetadataTask> task =
        AsyncPollTagMetadata(mctx, dom_query, filter, max_count);
    task->Wait();
    std::vector<TagInfo> stats = task->GetStats();
    CHI_CLIENT->DelTask(mctx, task);
    return stats;
  }
  CHI_TASK_METHODS(PollTagMetadata);

  /** PollAccessPattern task */
  std::vector<IoStat> PollAccessPattern(const hipc::MemContext &mctx,
                                        const DomainQuery &dom_query,
                                        hshm::min_u64 last_access = 0) {
    FullPtr<PollAccessPatternTask> task =
        AsyncPollAccessPattern(mctx, dom_query, last_access);
    task->Wait();
    std::vector<IoStat> stats = task->io_pattern_.vec();
    CHI_CLIENT->DelTask(mctx, task);
    return stats;
  }
  CHI_TASK_METHODS(PollAccessPattern);

  /**
   * ========================================
   * STAGING Tasks
   * ========================================
   * */

  /** RegisterStager task */
  void RegisterStager(const hipc::MemContext &mctx,
                      const DomainQuery &dom_query,
                      const hermes::BucketId &bkt_id,
                      const chi::string &tag_name, const chi::string &params) {
    FullPtr<RegisterStagerTask> task =
        AsyncRegisterStager(mctx, dom_query, bkt_id, tag_name, params);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(RegisterStager);

  /** UnregisterStager task */
  void UnregisterStager(const hipc::MemContext &mctx,
                        const DomainQuery &dom_query, const BucketId &bkt_id) {
    FullPtr<UnregisterStagerTask> task =
        AsyncUnregisterStager(mctx, dom_query, bkt_id);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(UnregisterStager);

  /** StageIn task */
  void StageIn(const hipc::MemContext &mctx, const DomainQuery &dom_query,
               const BucketId &bkt_id, const chi::string &blob_name,
               float score) {
    FullPtr<StageInTask> task =
        AsyncStageIn(mctx, dom_query, bkt_id, blob_name, score);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(StageIn);

  /** StageOut task */
  void StageOut(const hipc::MemContext &mctx, const DomainQuery &dom_query,
                const BucketId &bkt_id, const chi::string &blob_name,
                const hipc::Pointer &data, size_t data_size, u32 task_flags) {
    FullPtr<StageOutTask> task = AsyncStageOut(
        mctx, dom_query, bkt_id, blob_name, data, data_size, task_flags);
    task->Wait();
    CHI_CLIENT->DelTask(mctx, task);
  }
  CHI_TASK_METHODS(StageOut);
};

}  // namespace hermes

#endif  // CHI_hermes_core_H_
