/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/async_results_merger.h"

namespace mongo {

/**
 * A stage used only internally to merge results that are being gathered from remote hosts, possibly
 * including this host.
 *
 * Does not assume ownership of cursors until the first call to getNext(). This is to allow this
 * stage to be used on mongos without actually iterating the cursors. For example, when this stage
 * is parsed on mongos it may later be decided that the merging should happen on one of the shards.
 * Then this stage is forwarded to the merging shard, and it should not kill the cursors when it
 * goes out of scope on mongos.
 */
class DocumentSourceMergeCursors : public DocumentSource {
public:
    static constexpr StringData kStageName = "$mergeCursors"_sd;

    /**
     * Parses a serialized version of this stage.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * Creates a new DocumentSourceMergeCursors from the given 'remoteCursors'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        std::vector<ClusterClientCursorParams::RemoteCursor>&& remoteCursors,
        executor::TaskExecutor*,
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    /**
     * Absorbs a subsequent $sort if it's merging pre-sorted streams.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container);
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext*) final;

    /**
     * Serializes this stage to be sent to perform the merging on a different host.
     */
    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     // TODO SERVER-33683: Permit $mergeCursors with readConcern
                                     // level "snapshot".
                                     TransactionRequirement::kNotAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    GetNextResult getNext() final;

protected:
    void doDispose() final;

private:
    DocumentSourceMergeCursors(executor::TaskExecutor*,
                               std::unique_ptr<ClusterClientCursorParams>,
                               const boost::intrusive_ptr<ExpressionContext>&);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    executor::TaskExecutor* _executor;

    // '_arm' is not populated until the first call to getNext(). If getNext() is never called we
    // will not create an AsyncResultsMerger. If we did so the destruction of this stage would cause
    // the cursors within the ARM to be killed prematurely. For example, if this stage is parsed on
    // mongos then forwarded to the shards, it should not kill the cursors when it goes out of scope
    // on mongos.
    std::unique_ptr<ClusterClientCursorParams> _armParams;
    boost::optional<AsyncResultsMerger> _arm;
};

}  // namespace mongo
