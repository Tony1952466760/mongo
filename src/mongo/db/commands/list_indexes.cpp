/**
 *    Copyright (C) 2014-2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

namespace {

/**
 * Lists the indexes for a given collection.
 *
 * Format:
 * {
 *   listIndexes: <collection name>
 * }
 *
 * Return format:
 * {
 *   indexes: [
 *     ...
 *   ]
 * }
 */
class CmdListIndexes : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "list indexes for a collection";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listIndexes ActionType on the database, or find on system.indexes for pre
        // 3.0 systems.
        const NamespaceString ns(parseNsCollectionRequired(dbname, cmdObj));
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns),
                                                           ActionType::listIndexes) ||
            authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.indexes")),
                ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list indexes on collection: "
                                    << ns.coll());
    }

    CmdListIndexes() : Command("listIndexes") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             BSONObj& cmdObj,
             string& errmsg,
             BSONObjBuilder& result) {
        const NamespaceString ns(parseNsOrUUID(opCtx, dbname, cmdObj));
        const long long defaultBatchSize = std::numeric_limits<long long>::max();
        long long batchSize;
        Status parseCursorStatus =
            CursorRequest::parseCommandCursorOptions(cmdObj, defaultBatchSize, &batchSize);
        if (!parseCursorStatus.isOK()) {
            return appendCommandStatus(result, parseCursorStatus);
        }

        AutoGetCollectionForReadCommand autoColl(opCtx, ns);
        if (!autoColl.getDb()) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NamespaceNotFound, "no database"));
        }

        const Collection* collection = autoColl.getCollection();
        if (!collection) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NamespaceNotFound, "no collection"));
        }

        const CollectionCatalogEntry* cce = collection->getCatalogEntry();
        invariant(cce);

        vector<string> indexNames;
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            indexNames.clear();
            cce->getAllIndexes(opCtx, &indexNames);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "listIndexes", ns.ns());

        auto ws = make_unique<WorkingSet>();
        auto root = make_unique<QueuedDataStage>(opCtx, ws.get());

        for (size_t i = 0; i < indexNames.size(); i++) {
            BSONObj indexSpec;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                indexSpec = cce->getIndexSpec(opCtx, indexNames[i]);
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "listIndexes", ns.ns());

            if (ns.ns() == FeatureCompatibilityVersion::kCollection &&
                indexNames[i] == FeatureCompatibilityVersion::k32IncompatibleIndexName) {
                BSONObjBuilder bob;

                for (auto&& indexSpecElem : indexSpec) {
                    auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
                    if (indexSpecElemFieldName == IndexDescriptor::kIndexVersionFieldName) {
                        // Include the index version in the command response as a decimal type
                        // instead of as a 32-bit integer. This is a new BSON type that isn't
                        // supported by versions of MongoDB earlier than 3.4 that will cause 3.2
                        // secondaries to crash when performing initial sync.
                        bob.append(IndexDescriptor::kIndexVersionFieldName,
                                   indexSpecElem.numberDecimal());
                    } else {
                        bob.append(indexSpecElem);
                    }
                }

                indexSpec = bob.obj();
            }

            WorkingSetID id = ws->allocate();
            WorkingSetMember* member = ws->get(id);
            member->keyData.clear();
            member->recordId = RecordId();
            member->obj = Snapshotted<BSONObj>(SnapshotId(), indexSpec.getOwned());
            member->transitionToOwnedObj();
            root->pushBack(id);
        }

        const NamespaceString cursorNss = NamespaceString::makeListIndexesNSS(dbname, ns.coll());
        dassert(ns == cursorNss.getTargetNSForListIndexes());

        auto statusWithPlanExecutor = PlanExecutor::make(
            opCtx, std::move(ws), std::move(root), cursorNss, PlanExecutor::NO_YIELD);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }
        auto exec = std::move(statusWithPlanExecutor.getValue());

        BSONArrayBuilder firstBatch;

        for (long long objCount = 0; objCount < batchSize; objCount++) {
            BSONObj next;
            PlanExecutor::ExecState state = exec->getNext(&next, NULL);
            if (state == PlanExecutor::IS_EOF) {
                break;
            }
            invariant(state == PlanExecutor::ADVANCED);

            // If we can't fit this result inside the current batch, then we stash it for later.
            if (!FindCommon::haveSpaceForNext(next, objCount, firstBatch.len())) {
                exec->enqueue(next);
                break;
            }

            firstBatch.append(next);
        }

        CursorId cursorId = 0LL;
        if (!exec->isEOF()) {
            exec->saveState();
            exec->detachFromOperationContext();
            auto pinnedCursor = CursorManager::getGlobalCursorManager()->registerCursor(
                opCtx,
                {std::move(exec),
                 cursorNss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                 opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                 cmdObj});
            cursorId = pinnedCursor.getCursor()->cursorid();
        }

        appendCursorResponseObject(cursorId, cursorNss.ns(), firstBatch.arr(), &result);

        return true;
    }

} cmdListIndexes;

}  // namespace
}  // namespace mongo
