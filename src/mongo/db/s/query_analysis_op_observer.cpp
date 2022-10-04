/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

const auto docToDeleteDocration = OperationContext::declareDecoration<BSONObj>();

bool isFeatureFlagEnabled() {
    return serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        analyze_shard_key::gFeatureFlagAnalyzeShardKey.isEnabled(
            serverGlobalParams.featureCompatibility);
}

}  // namespace

void QueryAnalysisOpObserver::onInserts(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        std::vector<InsertStatement>::const_iterator begin,
                                        std::vector<InsertStatement>::const_iterator end,
                                        bool fromMigrate) {
    if (!isFeatureFlagEnabled()) {
        return;
    }

    if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
        for (auto it = begin; it != end; ++it) {
            const auto& insertedDoc = it->doc;
            opCtx->recoveryUnit()->onCommit([opCtx, insertedDoc](boost::optional<Timestamp>) {
                analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationInsert(
                    insertedDoc);
            });
        }
    }
}

void QueryAnalysisOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (!isFeatureFlagEnabled()) {
        return;
    }

    if (args.nss == NamespaceString::kConfigQueryAnalyzersNamespace) {
        const auto& updatedDoc = args.updateArgs->updatedDoc;
        opCtx->recoveryUnit()->onCommit([opCtx, updatedDoc](boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationUpdate(
                updatedDoc);
        });
    }
}

void QueryAnalysisOpObserver::aboutToDelete(OperationContext* opCtx,
                                            NamespaceString const& nss,
                                            const UUID& uuid,
                                            BSONObj const& doc) {
    if (!isFeatureFlagEnabled()) {
        return;
    }

    if (nss == NamespaceString::kConfigQueryAnalyzersNamespace) {
        docToDeleteDocration(opCtx) = doc;
    }
}

void QueryAnalysisOpObserver::onDelete(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& uuid,
                                       StmtId stmtId,
                                       const OplogDeleteEntryArgs& args) {
    if (!isFeatureFlagEnabled()) {
        return;
    }

    if (nss == NamespaceString::kConfigQueryAnalyzersNamespace) {
        auto& doc = docToDeleteDocration(opCtx);
        invariant(!doc.isEmpty());
        opCtx->recoveryUnit()->onCommit([opCtx, doc](boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationDelete(doc);
        });
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
