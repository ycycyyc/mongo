/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/write_op.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/collection_uuid_mismatch.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding
namespace mongo {
namespace {

struct WriteErrorComp {
    bool operator()(const write_ops::WriteError& errorA,
                    const write_ops::WriteError& errorB) const {
        return errorA.getIndex() < errorB.getIndex();
    }
};

/**
 * Helper to determine whether a shard is already targeted with a different shardVersion, which
 * necessitates a new batch. This happens when a batch write includes a multi target write and
 * a single target write.
 */
bool wasShardAlreadyTargetedWithDifferentShardVersion(
    const NamespaceString& nss,
    const std::vector<std::unique_ptr<TargetedWrite>>& writes,
    const std::map<NamespaceString, std::set<ShardId>>& nsShardIdMap,
    const std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>>& nsEndpointMap) {
    auto endpointSetIt = nsEndpointMap.find(nss);
    if (endpointSetIt == nsEndpointMap.end()) {
        // We haven't targeted this namespace yet.
        return false;
    }

    for (auto&& write : writes) {
        if (endpointSetIt->second.find(&write->endpoint) == endpointSetIt->second.end()) {
            // This is a new endpoint for this namespace.
            auto shardIdSetIt = nsShardIdMap.find(nss);
            invariant(shardIdSetIt != nsShardIdMap.end());
            if (shardIdSetIt->second.find(write->endpoint.shardName) !=
                shardIdSetIt->second.end()) {
                // And because we have targeted this shardId for this namespace before, this implies
                // a shard is already targeted under a different endpoint/shardVersion, necessitates
                // a new batch.
                LOGV2_DEBUG(9986802,
                            4,
                            "New batch required as this shard was already targeted with a "
                            "different shard version",
                            "shard"_attr = write->endpoint.shardName);
                return true;
            }
        }
    }
    return false;
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */
bool isNewBatchRequiredOrdered(
    const NamespaceString& nss,
    const std::vector<std::unique_ptr<TargetedWrite>>& writes,
    const TargetedBatchMap& batchMap,
    const std::map<NamespaceString, std::set<ShardId>>& nsShardIdMap,
    const std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>>& nsEndpointMap) {
    // If this write targets a different shard, it needs to go in a different batch.
    for (auto&& write : writes) {
        if (batchMap.find(write->endpoint.shardName) == batchMap.end()) {
            LOGV2_DEBUG(9986803,
                        5,
                        "New batch required as this write targets a different shard",
                        "shard"_attr = write->endpoint.shardName);
            return true;
        }
    }

    // If we already targeted this shard with a different shard version, then we also need a new
    // batch.
    return wasShardAlreadyTargetedWithDifferentShardVersion(
        nss, writes, nsShardIdMap, nsEndpointMap);
}

bool isNewBatchRequiredUnordered(
    const NamespaceString& nss,
    const std::vector<std::unique_ptr<TargetedWrite>>& writes,
    const std::map<NamespaceString, std::set<ShardId>>& nsShardIdMap,
    const std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>>& nsEndpointMap) {
    return wasShardAlreadyTargetedWithDifferentShardVersion(
        nss, writes, nsShardIdMap, nsEndpointMap);
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */
bool wouldMakeBatchesTooBig(const std::vector<std::unique_ptr<TargetedWrite>>& writes,
                            const TargetedBatchMap& batchMap) {
    for (auto&& write : writes) {
        TargetedBatchMap::const_iterator it = batchMap.find(write->endpoint.shardName);
        if (it == batchMap.end()) {
            // If this is the first item in the batch, it can't be too big
            continue;
        }

        if (it->second->getNumOps() >= write_ops::kMaxWriteBatchSize) {
            // Too many items in batch
            return true;
        }

        invariant(write->estimatedSizeBytes > 0);
        if (it->second->getEstimatedSizeBytes() + write->estimatedSizeBytes > BSONObjMaxUserSize) {
            // Batch would be too big
            return true;
        }
    }

    return false;
}

/**
 * Given *either* a batch error or an array of per-item errors, copies errors we're interested in
 * into a TrackedErrorMap
 */
void trackErrors(const ShardEndpoint& endpoint,
                 const std::vector<write_ops::WriteError>& itemErrors,
                 TrackedErrors* trackedErrors) {
    for (auto&& error : itemErrors) {
        if (trackedErrors->isTracking(error.getStatus().code())) {
            trackedErrors->addError(ShardError(endpoint, error));
        }
    }
}

int getEncryptionInformationSize(const BatchedCommandRequest& req) {
    if (!req.getWriteCommandRequestBase().getEncryptionInformation()) {
        return 0;
    }
    return req.getWriteCommandRequestBase().getEncryptionInformation().value().toBSON().objsize();
}

}  // namespace

boost::optional<WriteConcernErrorDetail> mergeWriteConcernErrors(
    const std::vector<ShardWCError>& wcErrors) {
    if (!wcErrors.size())
        return boost::none;

    StringBuilder msg;
    auto errCode = wcErrors.front().error.toStatus().code();
    if (wcErrors.size() != 1) {
        msg << "Multiple errors reported :: ";
    }

    for (auto it = wcErrors.begin(); it != wcErrors.end(); ++it) {
        if (it != wcErrors.begin()) {
            msg << " :: and :: ";
        }

        msg << it->error.toString() << " at " << it->shardName;
    }

    WriteConcernErrorDetail wce;
    wce.setStatus(Status(errCode, msg.str()));

    return boost::optional<WriteConcernErrorDetail>(wce);
}

/**
 * Attempts to populate the actualCollection field of a CollectionUUIDMismatch error if it is not
 * populated already, contacting the primary shard if necessary.
 */
void populateCollectionUUIDMismatch(OperationContext* opCtx,
                                    write_ops::WriteError* error,
                                    boost::optional<std::string>* actualCollection,
                                    bool* hasContactedPrimaryShard) {
    if (error->getStatus() != ErrorCodes::CollectionUUIDMismatch) {
        return;
    }

    LOGV2_DEBUG(9986812,
                4,
                "Encountered collection uuid mismatch when processing errors",
                "error"_attr = redact(error->getStatus()));

    auto info = error->getStatus().extraInfo<CollectionUUIDMismatchInfo>();
    if (info->actualCollection()) {
        return;
    }

    if (*actualCollection) {
        error->setStatus({CollectionUUIDMismatchInfo{info->dbName(),
                                                     info->collectionUUID(),
                                                     info->expectedCollection(),
                                                     **actualCollection},
                          error->getStatus().reason()});
        return;
    }

    if (*hasContactedPrimaryShard) {
        return;
    }

    error->setStatus(populateCollectionUUIDMismatch(opCtx, error->getStatus()));
    if (error->getStatus() == ErrorCodes::CollectionUUIDMismatch) {
        *hasContactedPrimaryShard = true;
        if (auto& populatedActualCollection =
                error->getStatus().extraInfo<CollectionUUIDMismatchInfo>()->actualCollection()) {
            *actualCollection = populatedActualCollection;
        }
    }
}

bool shouldCoordinateMultiWrite(OperationContext* opCtx,
                                PauseMigrationsDuringMultiUpdatesEnablement& pauseMigrations) {
    if (!pauseMigrations.isEnabled()) {
        // If the cluster parameter is off, return false.
        return false;
    }

    if (opCtx->isCommandForwardedFromRouter()) {
        // Coordinating a multi update involves running the update on a mongod (the db primary
        // shard), but it uses the same codepath as mongos. This flag is set to prevent the mongod
        // from repeatedly coordinating the update and forwarding it to itself.
        return false;
    }

    if (TransactionRouter::get(opCtx)) {
        // Similar to the upsert case, if we are in a transaction then the whole of the operation
        // will execute either before or after any conflicting chunk migrations, so the problem is
        // avoided.
        return false;
    }

    return true;
}

// 'baseCommandSizeBytes' specifies the base size of a batch command request prior to adding any
// individual operations to it. This function will ensure that 'baseCommandSizeBytes' plus the
// result of calling 'getWriteSizeFn' on each write added to a batch will not result in a command
// over BSONObjMaxUserSize.
StatusWith<WriteType> targetWriteOps(OperationContext* opCtx,
                                     std::vector<WriteOp>& writeOps,
                                     bool ordered,
                                     bool recordTargetErrors,
                                     PauseMigrationsDuringMultiUpdatesEnablement& pauseMigrations,
                                     GetTargeterFn getTargeterFn,
                                     GetWriteSizeFn getWriteSizeFn,
                                     int baseCommandSizeBytes,
                                     TargetedBatchMap& batchMap) {
    //
    // Targeting of unordered batches is fairly simple - each remaining write op is targeted,
    // and each of those targeted writes are grouped into a batch for a particular shard
    // endpoint.
    //
    // Targeting of ordered batches is a bit more complex - to respect the ordering of the
    // batch, we can only send:
    // A) a single targeted batch to one shard endpoint
    // B) multiple targeted batches, but only containing targeted writes for a single write op
    //
    // This means that any multi-shard write operation must be targeted and sent one-by-one.
    // Subsequent single-shard write operations can be batched together if they go to the same
    // place.
    //
    // Ex: ShardA : { skey : a->k }, ShardB : { skey : k->z }
    //
    // Ordered insert batch of: [{ skey : a }, { skey : b }, { skey : x }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : x }]
    //
    // Ordered update Batch of :
    //  [{ skey : a }{ $push },
    //   { skey : b }{ $push },
    //   { skey : [c, x] }{ $push },
    //   { skey : y }{ $push },
    //   { skey : z }{ $push }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : [c,x] }],
    //  [{ skey : y }, { skey : z }]
    //
    // For retryable timeseries updates and writes without shard key, they are always batched in its
    // own batch for both the ordered and unordered cases. This means that when we encounter a
    // retryable timeseries update or a write without shard key, we would either send out what we
    // previously have batched first (e.g. when !batchMap.empty()) or send out that single write in
    // its own batch.

    // Returns WriteType::WriteWithoutShardKeyWithId if there is any write of that type in the
    // batch. We send WriteType::WriteWithoutShardKeyWithId in batches of only such writes.

    boost::optional<WriteType> writeType;

    std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>> nsEndpointMap;
    std::map<NamespaceString, std::set<ShardId>> nsShardIdMap;

    for (auto& writeOp : writeOps) {
        // If the previous iteration encountered a write that doesn't support grouping (such as
        // WithoutShardKeyOrId or TimeseriesRetryableUpdate), then that write should be sent in
        // its own batch.
        if (writeType && !writeTypeSupportsGrouping(*writeType)) {
            break;
        }

        // Skip 'writeOp' if it is not in the "Ready" state.
        if (writeOp.getWriteState() != WriteOpState_Ready) {
            continue;
        }

        const auto& targeter = getTargeterFn(writeOp);

        const bool enableMultiWriteBlockingMigrations =
            shouldCoordinateMultiWrite(opCtx, pauseMigrations);

        auto [targetStatus, result] = [&]() -> std::pair<Status, WriteOp::TargetWritesResult> {
            try {
                return {Status::OK(),
                        writeOp.targetWrites(opCtx, targeter, enableMultiWriteBlockingMigrations)};
            } catch (const DBException& ex) {
                return {ex.toStatus(), WriteOp::TargetWritesResult{}};
            }
        }();

        if (!targetStatus.isOK()) {
            write_ops::WriteError targetError(0, targetStatus);
            LOGV2_DEBUG(9986811,
                        4,
                        "Encountered targeting error",
                        "error"_attr = redact(targetError.getStatus()));

            auto cancelBatches = [&]() {
                for (TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end();) {
                    for (auto&& write : it->second->getWrites()) {
                        // NOTE: We may repeatedly cancel a write op here, but that's fast and we
                        // want to cancel before erasing the TargetedWrite* (which owns the
                        // cancelled targeting info) for reporting reasons.
                        writeOps[write->writeOpRef.first].resetWriteToReady(opCtx);
                    }

                    it = batchMap.erase(it);
                }
                LOGV2_DEBUG(9986805,
                            4,
                            "Canceled batches due to targeting error",
                            "error"_attr = redact(targetError.getStatus()));
                dassert(batchMap.empty());
            };

            if (TransactionRouter::get(opCtx)) {
                writeOp.setOpError(targetError);

                // Cleanup all the writes we have targetted in this call so far since we are going
                // to abort the entire transaction.
                cancelBatches();

                return targetStatus;
            } else if (!recordTargetErrors) {
                // Cancel current batch state with an error
                cancelBatches();
                return targetStatus;
            } else if (!ordered || batchMap.empty()) {
                // Record an error for this batch
                writeOp.setOpError(targetError);

                if (ordered) {
                    return writeType ? *writeType : WriteType::Ordinary;
                }

                continue;
            } else {
                dassert(ordered && !batchMap.empty());

                // Send out what we have, but don't record an error yet, since there may be an error
                // in the writes before this point.
                writeOp.resetWriteToReady(opCtx);
                break;
            }
        }

        // Set up a guard so that 'writeOp' will be reset to "ready" if we exit this block
        // before 'finishedProcessingWriteOp' gets set to true.
        bool finishedProcessingWriteOp = false;
        ON_BLOCK_EXIT([&] {
            if (!finishedProcessingWriteOp) {
                writeOp.resetWriteToReady(opCtx);
            }
        });

        // If writes are ordered and we have a targeted endpoint, make sure we don't need to send
        // these targeted writes to any other endpoints.
        if (ordered && !batchMap.empty()) {
            dassert(batchMap.size() == 1u);
            if (isNewBatchRequiredOrdered(
                    targeter.getNS(), result.writes, batchMap, nsShardIdMap, nsEndpointMap)) {
                break;
            }
        }

        // If writes are unordered and we already have targeted endpoints, make sure we don't target
        // the same shard with a different shardVersion. We can continue to look for the next writes
        // that can still be included in the same batch.
        if (!ordered &&
            isNewBatchRequiredUnordered(
                targeter.getNS(), result.writes, nsShardIdMap, nsEndpointMap)) {
            continue;
        }

        // If the WriteType of 'writeOp' is different than the current batch's WriteType, then
        // 'writeOp' cannot be added to the current batch. Also, if 'writeOp' doesn't support
        // grouping, then 'writeOp' cannot be added to the current batch because it must be in
        // a batch by itself.
        if (writeType &&
            (result.writeType != *writeType || !writeTypeSupportsGrouping(result.writeType))) {
            if (ordered) {
                break;
            } else {
                continue;
            }
        }

        // Call the getWriteSizeFn() callback to get the estimated size for each TargetedWrite.
        // The getWriteSizeFn() callback may be stateful, so once it is called we are committed
        // to either adding the WriteOp in the batch or stopping the batch here and breaking
        // (i.e. "continue" is not allowed after this point).
        for (auto&& write : result.writes) {
            write->estimatedSizeBytes = getWriteSizeFn(writeOp, write->endpoint.shardName);
        }

        if (wouldMakeBatchesTooBig(result.writes, batchMap)) {
            invariant(!batchMap.empty());
            LOGV2_DEBUG(9986804, 5, "Making a new batch to avoid making batch size too large");
            break;
        }

        // Targeting succeeded.
        writeType = result.writeType;
        writeOp.setWriteType(result.writeType);
        finishedProcessingWriteOp = true;

        // Add each TargetedWrite to the appropriate TargetedBatch.
        for (auto&& write : result.writes) {
            const auto& shardId = write->endpoint.shardName;
            TargetedBatchMap::iterator batchIt = batchMap.find(shardId);
            if (batchIt == batchMap.end()) {
                auto newBatch = std::make_unique<TargetedWriteBatch>(shardId, baseCommandSizeBytes);
                batchIt = batchMap.emplace(shardId, std::move(newBatch)).first;
            }

            nsEndpointMap[targeter.getNS()].insert(&write->endpoint);
            nsShardIdMap[targeter.getNS()].insert(shardId);

            auto estWriteSizeBytes = write->estimatedSizeBytes;

            batchIt->second->addWrite(std::move(write), estWriteSizeBytes);
        }
        LOGV2_DEBUG(9986807, 5, "Targeting complete for child batch to shard");

        // Break if we're ordered and we have more than one endpoint - later writes cannot be
        // enforced as ordered across multiple shard endpoints.
        if (ordered && batchMap.size() > 1u) {
            break;
        }
    }

    return writeType ? *writeType : WriteType::Ordinary;
}

BatchWriteOp::BatchWriteOp(OperationContext* opCtx, const BatchedCommandRequest& clientRequest)
    : _opCtx(opCtx),
      _clientRequest(clientRequest),
      _batchTxnNum(_opCtx->getTxnNumber()),
      _inTransaction(bool(TransactionRouter::get(opCtx))),
      _isRetryableWrite(opCtx->isRetryableWrite()) {
    _writeOps.reserve(_clientRequest.sizeWriteOps());

    for (size_t i = 0; i < _clientRequest.sizeWriteOps(); ++i) {
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i), _inTransaction);
    }
}

StatusWith<WriteType> BatchWriteOp::targetBatch(const NSTargeter& targeter,
                                                bool recordTargetErrors,
                                                TargetedBatchMap* targetedBatches) {
    const bool ordered = _clientRequest.getWriteCommandRequestBase().getOrdered();

    auto targetStatus = targetWriteOps(
        _opCtx,
        _writeOps,
        ordered,
        recordTargetErrors,
        _pauseMigrationsDuringMultiUpdatesParameter,
        // getTargeterFn:
        [&](const WriteOp& writeOp) -> const NSTargeter& { return targeter; },
        // assignWriteSizeFn:
        [&](const WriteOp& writeOp, ShardId& shard) {
            // If retryable writes are used, MongoS needs to send an additional array of stmtId(s)
            // corresponding to the statements that got routed to each individual shard, so they
            // need to be accounted in the potential request size so it does not exceed the max BSON
            // size.
            const int writeSizeBytes = writeOp.getWriteItem().getSizeForBatchWriteBytes() +
                getEncryptionInformationSize(_clientRequest) +
                write_ops::kWriteCommandBSONArrayPerElementOverheadBytes +
                (_batchTxnNum ? write_ops::kStmtIdSize +
                         write_ops::kWriteCommandBSONArrayPerElementOverheadBytes
                              : 0);

            // For unordered writes, the router must return an entry for each failed write. This
            // constant is a pessimistic attempt to ensure that if a request to a shard hits
            // "retargeting needed" error and has to return number of errors equivalent to the
            // number of writes in the batch, the response size will not exceed the max BSON size.
            //
            // The constant of 272 is chosen as an approximation of the size of the BSON
            // representation of the StaleConfigInfo (which contains the shard id) and the adjacent
            // error message.
            const int errorResponsePotentialSizeBytes =
                ordered ? 0 : write_ops::kWriteCommandBSONArrayPerElementOverheadBytes + 272;
            return std::max(writeSizeBytes, errorResponsePotentialSizeBytes);
        },
        _clientRequest.getBaseCommandSizeEstimate(_opCtx),
        *targetedBatches);

    if (!targetStatus.isOK()) {
        return targetStatus;
    }

    // Note: It is fine to use 'getAproxNShardsOwningChunks' here because the result is only used to
    // update stats.
    _nShardsOwningChunks = targeter.getAproxNShardsOwningChunks();

    return targetStatus;
}

BatchedCommandRequest BatchWriteOp::buildBatchRequest(
    const TargetedWriteBatch& targetedBatch,
    const NSTargeter& targeter,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    const auto batchType = _clientRequest.getBatchType();

    boost::optional<std::vector<int32_t>> stmtIdsForOp;
    if (_isRetryableWrite) {
        stmtIdsForOp.emplace();
    }

    boost::optional<std::vector<BSONObj>> insertDocs;
    boost::optional<std::vector<write_ops::UpdateOpEntry>> updates;
    boost::optional<std::vector<write_ops::DeleteOpEntry>> deletes;

    for (auto&& targetedWrite : targetedBatch.getWrites()) {
        const WriteOpRef& writeOpRef = targetedWrite->writeOpRef;

        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                if (!insertDocs)
                    insertDocs.emplace();
                insertDocs->emplace_back(
                    _clientRequest.getInsertRequest().getDocuments().at(writeOpRef.first));
                break;
            case BatchedCommandRequest::BatchType_Update:
                if (!updates)
                    updates.emplace();
                updates->emplace_back(
                    _clientRequest.getUpdateRequest().getUpdates().at(writeOpRef.first));
                updates->back().setSampleId(targetedWrite->sampleId);

                // If we are using the two phase write protocol introduced in PM-1632, we allow
                // shard key updates without specifying the full shard key in the query if we
                // execute the update in a retryable write/transaction.
                if (allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
                    uassert(
                        ErrorCodes::InvalidOptions,
                        "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter",
                        !updates->back().getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());
                    updates->back().setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                        allowShardKeyUpdatesWithoutFullShardKeyInQuery);
                }
                break;
            case BatchedCommandRequest::BatchType_Delete:
                if (!deletes)
                    deletes.emplace();
                deletes->emplace_back(
                    _clientRequest.getDeleteRequest().getDeletes().at(writeOpRef.first));
                deletes->back().setSampleId(targetedWrite->sampleId);
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if (stmtIdsForOp) {
            stmtIdsForOp->push_back(write_ops::getStmtIdForWriteAt(
                _clientRequest.getWriteCommandRequestBase(), writeOpRef.first));
        }
    }

    BatchedCommandRequest request([&] {
        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                return BatchedCommandRequest([&] {
                    write_ops::InsertCommandRequest insertOp(targeter.getNS());
                    insertOp.setDocuments(std::move(*insertDocs));
                    return insertOp;
                }());
            case BatchedCommandRequest::BatchType_Update: {
                return BatchedCommandRequest([&] {
                    write_ops::UpdateCommandRequest updateOp(targeter.getNS());
                    updateOp.setUpdates(std::move(*updates));
                    // Each child batch inherits its let params/runtime constants from the parent
                    // batch.
                    updateOp.setLegacyRuntimeConstants(_clientRequest.getLegacyRuntimeConstants());
                    updateOp.setLet(_clientRequest.getLet());
                    return updateOp;
                }());
            }
            case BatchedCommandRequest::BatchType_Delete:
                return BatchedCommandRequest([&] {
                    write_ops::DeleteCommandRequest deleteOp(targeter.getNS());
                    deleteOp.setDeletes(std::move(*deletes));
                    // Each child batch inherits its let params from the parent batch.
                    deleteOp.setLet(_clientRequest.getLet());
                    deleteOp.setLegacyRuntimeConstants(_clientRequest.getLegacyRuntimeConstants());
                    return deleteOp;
                }());
        }
        MONGO_UNREACHABLE;
    }());

    request.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;

        wcb.setBypassDocumentValidation(
            _clientRequest.getWriteCommandRequestBase().getBypassDocumentValidation());
        wcb.setOrdered(_clientRequest.getWriteCommandRequestBase().getOrdered());
        wcb.setCollectionUUID(_clientRequest.getWriteCommandRequestBase().getCollectionUUID());

        wcb.setEncryptionInformation(
            _clientRequest.getWriteCommandRequestBase().getEncryptionInformation());

        if (targeter.isTrackedTimeSeriesBucketsNamespace() &&
            !_clientRequest.getNS().isTimeseriesBucketsCollection() &&
            !isRawDataOperation(_opCtx)) {
            wcb.setIsTimeseriesNamespace(true);
        }

        if (_isRetryableWrite) {
            wcb.setStmtIds(std::move(stmtIdsForOp));
        }

        wcb.setBypassEmptyTsReplacement(
            _clientRequest.getWriteCommandRequestBase().getBypassEmptyTsReplacement());

        return wcb;
    }());

    // For BatchWriteOp, all writes in the batch should share the same endpoint since they target
    // the same shard and namespace. So we just use the endpoint from the first write.
    const auto& endpoint = targetedBatch.getWrites()[0]->endpoint;

    auto shardVersion = endpoint.shardVersion;
    if (shardVersion)
        request.setShardVersion(*shardVersion);

    auto dbVersion = endpoint.databaseVersion;
    if (dbVersion)
        request.setDbVersion(*dbVersion);

    return request;
}

void BatchWriteOp::noteBatchResponse(const TargetedWriteBatch& targetedBatch,
                                     const BatchedCommandResponse& response,
                                     TrackedErrors* trackedErrors) {
    if (!response.getOk()) {
        write_ops::WriteError error(0, response.getTopLevelStatus());

        // Treat command errors exactly like other failures of the batch.
        //
        // Note that no errors will be tracked from these failures - as-designed.
        noteBatchError(targetedBatch, error);
        return;
    }

    int firstTargetedWriteOpIdx = targetedBatch.getWrites().front()->writeOpRef.first;
    bool isWriteWithoutShardKeyWithId =
        _writeOps[firstTargetedWriteOpIdx].getWriteType() == WriteType::WithoutShardKeyWithId;
    int batchSize = targetedBatch.getNumOps();
    // A WriteWithoutShardKeyWithId batch of size 1 can be completed if it found n=1 from a
    // previous shard. In this case skip processing the response.
    if (batchSize == 1 && isWriteWithoutShardKeyWithId &&
        _writeOps[firstTargetedWriteOpIdx].getWriteState() == WriteOpState_Completed) {
        return;
    }

    bool shouldDeferWriteWithoutShardKeyResponse = isWriteWithoutShardKeyWithId && batchSize > 1;
    if (!shouldDeferWriteWithoutShardKeyResponse) {
        // Increment stats for this batch
        _incBatchStats(response);
    } else {
        if (!_deferredResponses) {
            _deferredResponses.emplace();
        }
        _deferredResponses->push_back(std::make_pair(&targetedBatch, &response));
    }

    //
    // Assign errors to particular items.
    // Write Concern errors are stored and handled later.
    //

    // Special handling for write concern errors, save for later
    if (response.isWriteConcernErrorSet()) {
        auto wce = *response.getWriteConcernError();
        auto shardId = targetedBatch.getWrites()[0]->endpoint.shardName;
        if (_writeOps[firstTargetedWriteOpIdx].getWriteType() == WriteType::WithoutShardKeyWithId) {
            if (!_deferredWCErrors) {
                _deferredWCErrors.emplace();
            }
            (*_deferredWCErrors)[firstTargetedWriteOpIdx].push_back(ShardWCError(shardId, wce));
        } else {
            // For BatchWriteOp, all writes in the batch should share the same endpoint since they
            // target the same shard and namespace. So we just use the endpoint from the first
            // write.
            _wcErrors.emplace_back(shardId, wce);
        }
    }

    std::vector<write_ops::WriteError> itemErrors;

    // Handle batch and per-item errors
    if (response.isErrDetailsSet()) {
        // Per-item errors were set
        itemErrors.insert(
            itemErrors.begin(), response.getErrDetails().begin(), response.getErrDetails().end());

        // Sort per-item errors by index
        std::sort(itemErrors.begin(), itemErrors.end(), WriteErrorComp());
    }

    //
    // Go through all pending responses of the op and sorted remote responses, populate errors
    // This will either set all errors to the batch error or apply per-item errors as-needed
    //
    // If the batch is ordered, cancel all writes after the first error for retargeting.
    //

    const bool ordered = _clientRequest.getWriteCommandRequestBase().getOrdered();

    auto itemErrorIt = itemErrors.begin();
    int index = 0;
    write_ops::WriteError* lastError = nullptr;
    for (auto&& write : targetedBatch.getWrites()) {
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];
        invariant(writeOp.getWriteState() == WriteOpState_Pending, writeOp.getWriteStateAsString());

        // See if we have an error for the write
        write_ops::WriteError* writeError = nullptr;

        if (itemErrorIt != itemErrors.end() && itemErrorIt->getIndex() == index) {
            // We have an per-item error for this write op's index
            writeError = &(*itemErrorIt);
            ++itemErrorIt;
        }

        // Finish the response (with error, if needed)
        if (!writeError) {
            if (!ordered || !lastError) {
                if (writeOp.getWriteType() == WriteType::WithoutShardKeyWithId) {
                    writeOp.noteWriteWithoutShardKeyWithIdResponse(
                        _opCtx,
                        *write,
                        response.getN(),
                        targetedBatch.getNumOps(),
                        /* bulkWriteReplyItem */ boost::none);
                } else {
                    writeOp.noteWriteComplete(_opCtx, *write);
                }
            } else {
                // We didn't actually apply this write - cancel so we can retarget
                dassert(writeOp.getNumTargeted() == 1u);
                writeOp.resetWriteToReady(_opCtx);
            }
        } else {
            writeOp.noteWriteError(_opCtx, *write, *writeError);
            lastError = writeError;
        }
        ++index;
    }

    // Track errors we care about, whether batch or individual errors
    if (nullptr != trackedErrors) {
        // For BatchWriteOp, all writes in the batch should share the same endpoint since they
        // target the same shard and namespace. So we just use the endpoint from the first write.
        trackErrors(targetedBatch.getWrites()[0]->endpoint, itemErrors, trackedErrors);
    }

    // Track upserted ids if we need to
    if (response.isUpsertDetailsSet()) {
        const std::vector<BatchedUpsertDetail*>& upsertedIds = response.getUpsertDetails();
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = upsertedIds.begin();
             it != upsertedIds.end();
             ++it) {
            // The child upserted details don't have the correct index for the full batch
            const BatchedUpsertDetail* childUpsertedId = *it;

            // Work backward from the child batch item index to the batch item index
            int childBatchIndex = childUpsertedId->getIndex();
            int batchIndex = targetedBatch.getWrites()[childBatchIndex]->writeOpRef.first;

            // Push the upserted id with the correct index into the batch upserted ids
            auto upsertedId = std::make_unique<BatchedUpsertDetail>();
            upsertedId->setIndex(batchIndex);
            upsertedId->setUpsertedID(childUpsertedId->getUpsertedID());
            _upsertedIds.push_back(std::move(upsertedId));
        }
    }
}

WriteOp& BatchWriteOp::getWriteOp(int index) {
    return _writeOps[index];
}

void BatchWriteOp::noteBatchError(const TargetedWriteBatch& targetedBatch,
                                  const write_ops::WriteError& error,
                                  const WriteConcernErrorDetail* wce) {
    // Treat errors to get a batch response as failures of the contained writes
    BatchedCommandResponse emulatedResponse;
    emulatedResponse.setStatus(Status::OK());
    emulatedResponse.setN(0);
    if (wce) {
        auto wceCopy = std::make_unique<WriteConcernErrorDetail>(*wce);
        emulatedResponse.setWriteConcernError(wceCopy.release());
    }

    const int numErrors = _clientRequest.getWriteCommandRequestBase().getOrdered()
        ? 1
        : targetedBatch.getWrites().size();

    for (int i = 0; i < numErrors; i++) {
        write_ops::WriteError errorClone = error;
        errorClone.setIndex(i);
        emulatedResponse.addToErrDetails(std::move(errorClone));
    }

    noteBatchResponse(targetedBatch, emulatedResponse, nullptr);
}

void BatchWriteOp::abortBatch(const write_ops::WriteError& error) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandRequestBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        // Can only be called with no outstanding batches
        dassert(writeOp.getWriteState() != WriteOpState_Pending);

        if (writeOp.getWriteState() < WriteOpState_Completed) {
            writeOp.setOpError(error);

            // Only one error if we're ordered
            if (orderedOps)
                break;
        }
    }

    dassert(isFinished());
}

bool BatchWriteOp::isFinished() {
    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandRequestBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        if (writeOp.getWriteState() < WriteOpState_Completed)
            return false;
        else if (orderedOps && writeOp.getWriteState() == WriteOpState_Error)
            return true;
    }

    return true;
}

void BatchWriteOp::buildClientResponse(BatchedCommandResponse* batchResp) {
    // Note: we aggressively abandon the batch when encountering errors during transactions, so
    // it can be in a state that is not "finished" even for unordered batches.
    dassert(_inTransaction || isFinished());

    // Result is OK
    batchResp->setStatus(Status::OK());

    // For non-verbose, it's all we need.
    if (!_opCtx->getWriteConcern().requiresWriteAcknowledgement()) {
        return;
    }

    //
    // Find all the errors in the batch
    //

    std::vector<WriteOp*> errOps;

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];

        if (writeOp.getWriteState() == WriteOpState_Error) {
            errOps.push_back(&writeOp);
        }
    }

    //
    // Build the per-item errors.
    //

    if (!errOps.empty()) {
        boost::optional<std::string> collectionUUIDMismatchActualCollection;

        for (std::vector<WriteOp*>::iterator it = errOps.begin(); it != errOps.end(); ++it) {
            WriteOp& writeOp = **it;
            write_ops::WriteError error = writeOp.getOpError();
            auto status = error.getStatus();

            // For CollectionUUIDMismatch error, check if there is a response from a shard that
            // aleady has the actualCollection information. If there is none, make an additional
            // call to the primary shard to fetch this info in case the collection is unsharded or
            // the targeted shard does not own any chunk of the collection with the requested uuid.
            if (!collectionUUIDMismatchActualCollection &&
                status.code() == ErrorCodes::CollectionUUIDMismatch) {
                collectionUUIDMismatchActualCollection =
                    status.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection();
            }

            batchResp->addToErrDetails(std::move(error));
        }

        bool hasContactedPrimaryShard = false;
        for (auto& error : batchResp->getErrDetails()) {
            populateCollectionUUIDMismatch(
                _opCtx, &error, &collectionUUIDMismatchActualCollection, &hasContactedPrimaryShard);
        }
    }

    if (auto wce = mergeWriteConcernErrors(_wcErrors)) {
        batchResp->setWriteConcernError(new WriteConcernErrorDetail(wce.value()));
    }

    //
    // Append the upserted ids, if required
    //

    if (_upsertedIds.size() != 0) {
        batchResp->setUpsertDetails(transitional_tools_do_not_use::unspool_vector(_upsertedIds));
    }

    // Stats
    const int nValue = _numInserted + _numUpserted + _numMatched + _numDeleted;
    batchResp->setN(nValue);
    if (_clientRequest.getBatchType() == BatchedCommandRequest::BatchType_Update &&
        _numModified >= 0) {
        batchResp->setNModified(_numModified);
    }
    if (!_retriedStmtIds.empty()) {
        batchResp->setRetriedStmtIds(_retriedStmtIds);
    }
}

int BatchWriteOp::numWriteOpsIn(WriteOpState opState) const {
    // TODO: This could be faster, if we tracked this info explicitly
    return std::accumulate(
        _writeOps.begin(), _writeOps.end(), 0, [opState](int sum, const WriteOp& writeOp) {
            return sum + (writeOp.getWriteState() == opState ? 1 : 0);
        });
}

boost::optional<int> BatchWriteOp::getNShardsOwningChunks() {
    return _nShardsOwningChunks;
}

void BatchWriteOp::_incBatchStats(const BatchedCommandResponse& response) {
    const auto batchType = _clientRequest.getBatchType();

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        _numInserted += response.getN();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        int numUpserted = 0;
        if (response.isUpsertDetailsSet()) {
            numUpserted = response.sizeUpsertDetails();
        }
        _numMatched += (response.getN() - numUpserted);
        long long numModified = response.getNModified();

        if (numModified >= 0)
            _numModified += numModified;
        else
            _numModified = -1;  // sentinel used to indicate we omit the field downstream

        _numUpserted += numUpserted;
    } else {
        dassert(batchType == BatchedCommandRequest::BatchType_Delete);
        _numDeleted += response.getN();
    }

    if (auto retriedStmtIds = response.getRetriedStmtIds(); !retriedStmtIds.empty()) {
        _retriedStmtIds.insert(_retriedStmtIds.end(), retriedStmtIds.begin(), retriedStmtIds.end());
    }
}

void BatchWriteOp::handleDeferredWriteConcernErrors() {
    if (_deferredWCErrors) {
        for (auto& it : *_deferredWCErrors) {
            auto& op = _writeOps[it.first];
            auto& wcErrors = it.second;
            invariant(op.getWriteType() == WriteType::WithoutShardKeyWithId);
            if (op.getWriteState() >= WriteOpState_Completed) {
                _wcErrors.insert(_wcErrors.end(), wcErrors.begin(), wcErrors.end());
            } else {
                // If we are here for any op it means that the whole batch is retried.
                break;
            }
        }
        _deferredWCErrors = boost::none;
    }
}

void BatchWriteOp::handleDeferredResponses(bool hasAnyStaleShardResponse) {
    if (!_deferredResponses) {
        return;
    }

    for (unsigned long idx = 0; idx < _deferredResponses->size(); idx++) {
        auto [targetedWriteBatch, response] = _deferredResponses->at(idx);
        for (auto& write : targetedWriteBatch->getWrites()) {
            WriteOp& writeOp = _writeOps[write->writeOpRef.first];
            if (hasAnyStaleShardResponse) {
                if (writeOp.getWriteState() != WriteOpState_Ready) {
                    writeOp.resetWriteToReady(_opCtx);
                }
            } else if (writeOp.getWriteState() != WriteOpState_Error) {
                writeOp.noteWriteWithoutShardKeyWithIdResponse(
                    _opCtx, *write, response->getN(), targetedWriteBatch->getNumOps(), boost::none);
            }
        }
        if (!hasAnyStaleShardResponse) {
            _incBatchStats(*response);
        }
    }
    _deferredResponses = boost::none;
}


void TrackedErrors::startTracking(int errCode) {
    dassert(!isTracking(errCode));
    _errorMap.emplace(errCode, std::vector<ShardError>());
}

bool TrackedErrors::isTracking(int errCode) const {
    return _errorMap.count(errCode) != 0;
}

void TrackedErrors::addError(ShardError error) {
    TrackedErrorMap::iterator seenIt = _errorMap.find(error.error.getStatus().code());
    if (seenIt == _errorMap.end())
        return;
    seenIt->second.emplace_back(std::move(error));
}

const std::vector<ShardError>& TrackedErrors::getErrors(int errCode) const {
    dassert(isTracking(errCode));
    return _errorMap.find(errCode)->second;
}

}  // namespace mongo
