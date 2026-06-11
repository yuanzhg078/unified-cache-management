/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include "sub_batch_trace.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "logger.h"

namespace UC::ASU::trace {

// Enum to string helpers

const char* OpTypeToString(TransportOpType op)
{
    switch (op) {
        case TransportOpType::QUERY: return "QUERY";
        case TransportOpType::LOAD: return "LOAD";
        case TransportOpType::STORE: return "STORE";
        case TransportOpType::BATCH_LOAD: return "BATCH_LOAD";
        case TransportOpType::BATCH_STORE: return "BATCH_STORE";
        case TransportOpType::DELETE: return "DELETE";
        case TransportOpType::KEEP_ALIVE: return "KEEP_ALIVE";
        default: return "UNKNOWN";
    }
}

const char* TaskStateToString(TransportTaskState s)
{
    switch (s) {
        case TransportTaskState::PENDING: return "PENDING";
        case TransportTaskState::INFLIGHT: return "INFLIGHT";
        case TransportTaskState::COMPLETED: return "COMPLETED";
        case TransportTaskState::CANCELED: return "CANCELED";
        default: return "UNKNOWN";
    }
}

const char* SubBatchStateToString(TransportSubBatchState s)
{
    switch (s) {
        case TransportSubBatchState::PENDING: return "PENDING";
        case TransportSubBatchState::COMPLETED: return "COMPLETED";
        default: return "UNKNOWN";
    }
}

const char* ChannelStateToString(ChannelState s)
{
    switch (s) {
        case ChannelState::ACTIVE: return "ACTIVE";
        case ChannelState::DRAINING: return "DRAINING";
        case ChannelState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

const char* StatusCodeToString(StatusCode code)
{
    switch (code) {
        case StatusCode::OK: return "OK";
        case StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case StatusCode::NOT_INITIALIZED: return "NOT_INITIALIZED";
        case StatusCode::TIMEOUT: return "TIMEOUT";
        case StatusCode::SUBMIT_TIMEOUT: return "SUBMIT_TIMEOUT";
        case StatusCode::RESULT_TIMEOUT: return "RESULT_TIMEOUT";
        case StatusCode::NOT_FOUND: return "NOT_FOUND";
        case StatusCode::PARTIAL_FAILED: return "PARTIAL_FAILED";
        case StatusCode::CONNECTION_ERROR: return "CONNECTION_ERROR";
        case StatusCode::NO_ACTIVE_CONNECTION: return "NO_ACTIVE_CONNECTION";
        case StatusCode::IO_ERROR: return "IO_ERROR";
        case StatusCode::BUFFER_NOT_REGISTERED: return "BUFFER_NOT_REGISTERED";
        case StatusCode::BUFFER_NOT_SUPPORTED: return "BUFFER_NOT_SUPPORTED";
        case StatusCode::TASK_NOT_FOUND: return "TASK_NOT_FOUND";
        case StatusCode::RESOURCE_BUSY: return "RESOURCE_BUSY";
        case StatusCode::UNSUPPORTED: return "UNSUPPORTED";
        case StatusCode::IN_PROGRESS: return "IN_PROGRESS";
        case StatusCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case StatusCode::CANCELED: return "CANCELED";
        case StatusCode::ASU_ENTRY_RETRY_ADVISED: return "ASU_ENTRY_RETRY_ADVISED";
        case StatusCode::ASU_ENTRY_NO_RETRY_ADVISED: return "ASU_ENTRY_NO_RETRY_ADVISED";
        case StatusCode::ASU_ENTRY_KEY_NOT_FOUND: return "ASU_ENTRY_KEY_NOT_FOUND";
        case StatusCode::ASU_ENTRY_DATA_NOT_EXIST: return "ASU_ENTRY_DATA_NOT_EXIST";
        case StatusCode::ASU_ENTRY_DELETE_FAILED: return "ASU_ENTRY_DELETE_FAILED";
        case StatusCode::ASU_ENTRY_KEY_NOT_EXIST: return "ASU_ENTRY_KEY_NOT_EXIST";
        case StatusCode::ASU_ENTRY_KEY_EXIST: return "ASU_ENTRY_KEY_EXIST";
        case StatusCode::ASU_CQE_INVALID_COMMAND_OPCODE: return "ASU_CQE_INVALID_COMMAND_OPCODE";
        case StatusCode::ASU_CQE_INVALID_FIELD_IN_COMMAND:
            return "ASU_CQE_INVALID_FIELD_IN_COMMAND";
        case StatusCode::ASU_CQE_INTERNAL_ERROR: return "ASU_CQE_INTERNAL_ERROR";
        case StatusCode::ASU_CQE_WRITE_FAULT: return "ASU_CQE_WRITE_FAULT";
        case StatusCode::ASU_CQE_UNRECOVERED_READ_ERROR: return "ASU_CQE_UNRECOVERED_READ_ERROR";
        case StatusCode::ASU_CQE_KEY_NOT_EXIST: return "ASU_CQE_KEY_NOT_EXIST";
        case StatusCode::ASU_CQE_OUT_OF_CREATE_SIZE: return "ASU_CQE_OUT_OF_CREATE_SIZE";
        case StatusCode::ASU_CQE_IO_TIMEOUT: return "ASU_CQE_IO_TIMEOUT";
        case StatusCode::ASU_CQE_KEY_ALREADY_EXISTED: return "ASU_CQE_KEY_ALREADY_EXISTED";
        case StatusCode::ASU_CQE_RESOURCE_BUSY: return "ASU_CQE_RESOURCE_BUSY";
        case StatusCode::ASU_CQE_CHECK_RESULT_BUFFER: return "ASU_CQE_CHECK_RESULT_BUFFER";
        default: return "UNKNOWN";
    }
}

const char* TracePhaseToString(SubBatchTracePhase phase)
{
    switch (phase) {
        case SubBatchTracePhase::SUBMIT: return "SUBMIT";
        case SubBatchTracePhase::COMPLETE: return "COMPLETE";
        default: return "UNKNOWN";
    }
}

// Capture

SubBatchTraceSnapshot CaptureTraceSnapshot(
    TaskId taskId, TransportOpType opType, TransportTaskState taskState,
    const Status& taskFinalStatus, std::size_t totalEntries,
    const std::vector<TransportSubBatchContext>& subBatchContexts, AsuId asuId,
    SubBatchTracePhase phase)
{
    SubBatchTraceSnapshot snap;
    snap.asuId = asuId;
    snap.taskId = taskId;
    snap.phase = phase;
    snap.opType = opType;
    snap.taskState = taskState;
    snap.taskFinalStatus = taskFinalStatus;
    snap.totalEntries = totalEntries;
    snap.subBatchCount = subBatchContexts.size();
    snap.entries.reserve(subBatchContexts.size());

    std::size_t offset = 0;
    for (std::size_t i = 0; i < subBatchContexts.size(); ++i) {
        const auto& sub = subBatchContexts[i];
        SubBatchTraceEntry e;
        e.index = i;
        e.offset = offset;
        e.size = sub.entryStatus.size();
        offset += e.size;

        e.hasCid = (sub.cid != 0);
        e.cid = sub.cid;

        e.hasChannel = (sub.channel != nullptr);
        if (e.hasChannel) {
            auto* grp = sub.channel->GetGroup();
            e.groupId = grp ? grp->GetGroupId() : 0;
            e.channelId = sub.channel->GetChannelId();
            e.channelState = sub.channel->GetState();
        }

        e.hasSendSlot = (sub.sendSge.slot_index != UINT32_MAX);
        e.sendSlot = sub.sendSge.slot_index;

        e.hasFlagSlot = (sub.flagBuffer.slot_index != UINT32_MAX);
        e.flagSlot = sub.flagBuffer.slot_index;

        e.state = sub.state;
        e.status = sub.status;

        e.entryStatusOkCount =
            static_cast<std::size_t>(std::count_if(sub.entryStatus.begin(), sub.entryStatus.end(),
                                                   [](const Status& s) { return s.ok(); }));
        e.entryStatusErrCount = e.size - e.entryStatusOkCount;

        snap.entries.push_back(e);
    }

    return snap;
}

SubBatchTraceSnapshot CaptureTraceSnapshot(const TransportTaskContext& ctx,
                                           SubBatchTracePhase phase, AsuId asuId)
{
    const std::size_t total = IsEntryBatchOp(ctx.opType) ? ctx.entries.size
                              : IsKeyBatchOp(ctx.opType) ? ctx.keys.size
                                                         : static_cast<std::size_t>(0);

    return CaptureTraceSnapshot(ctx.taskId, ctx.opType, ctx.state.load(std::memory_order_acquire),
                                ctx.finalStatus, total, ctx.subBatchContexts, asuId, phase);
}

// Formatting

namespace {

std::string FormatStatus(const Status& s)
{
    if (s.ok()) { return "OK"; }
    return StatusCodeToString(s.code);
}

std::string FormatStatusWithMessage(const Status& s)
{
    if (s.ok()) { return "OK"; }
    if (s.message.empty()) { return StatusCodeToString(s.code); }
    return std::string(StatusCodeToString(s.code)) + ": " + s.message;
}

std::string FormatKeyRange(std::size_t offset, std::size_t size)
{
    std::ostringstream oss;
    if (size == 0) {
        oss << "[" << offset << ".." << offset << "]";
    } else {
        oss << "[" << offset << ".." << (offset + size - 1) << "]";
    }
    return oss.str();
}

std::string FormatChannel(const SubBatchTraceEntry& e)
{
    if (!e.hasChannel) { return "--"; }

    std::ostringstream oss;
    oss << "g=" << e.groupId << ",ch=" << e.channelId << ","
        << ChannelStateToString(e.channelState);
    return oss.str();
}

std::string FormatSlot(std::uint32_t slot, bool hasSlot)
{
    if (!hasSlot) { return "--"; }
    return std::to_string(slot);
}

std::string FormatEntryStatusCount(const SubBatchTraceEntry& e)
{
    if (e.state != TransportSubBatchState::COMPLETED) { return "--"; }

    std::ostringstream oss;
    oss << e.entryStatusOkCount << "/" << e.entryStatusErrCount;
    return oss.str();
}

}  // namespace

void PrintTraceTable(std::ostream& os, const SubBatchTraceSnapshot& snap)
{
    const bool isKeyOp = IsKeyBatchOp(snap.opType);
    const char* itemCountLabel = isKeyOp ? "total_keys" : "total_entries";

    os << "ASU transport trace [" << TracePhaseToString(snap.phase) << "]\n"
       << "  task_id=" << snap.taskId << " asu_id=" << snap.asuId
       << " op=" << OpTypeToString(snap.opType) << "\n"
       << "  task_state=" << TaskStateToString(snap.taskState) << "\n"
       << "  task_status=" << FormatStatus(snap.taskFinalStatus) << "\n"
       << "  " << itemCountLabel << "=" << snap.totalEntries
       << " sub_batches=" << snap.subBatchCount << "\n\n";

    if (snap.entries.empty()) { return; }

    os << std::left << std::setw(10) << "batch_idx" << std::setw(12) << "key_range" << std::setw(6)
       << "size" << std::setw(6) << "cid" << std::setw(22) << "channel" << std::setw(11)
       << "send_slot" << std::setw(11) << "flag_slot" << std::setw(12) << "entry_ok/err"
       << std::setw(12) << "state" << "status"
       << "\n"
       << std::setw(10) << "---------" << std::setw(12) << "---------" << std::setw(6) << "----"
       << std::setw(6) << "---" << std::setw(22) << "-------" << std::setw(11) << "---------"
       << std::setw(11) << "---------" << std::setw(12) << "------------" << std::setw(12)
       << "-----"
       << "------"
       << "\n";

    for (const auto& e : snap.entries) {
        os << std::setw(10) << e.index;
        os << std::setw(12) << FormatKeyRange(e.offset, e.size);
        os << std::setw(6) << e.size;

        if (e.hasCid) {
            os << std::setw(6) << e.cid;
        } else {
            os << std::setw(6) << "--";
        }

        os << std::setw(22) << FormatChannel(e);
        os << std::setw(11) << FormatSlot(e.sendSlot, e.hasSendSlot);
        os << std::setw(11) << FormatSlot(e.flagSlot, e.hasFlagSlot);
        os << std::setw(12) << FormatEntryStatusCount(e);

        os << std::setw(12) << SubBatchStateToString(e.state);
        os << FormatStatusWithMessage(e.status);
        os << "\n";
    }
}

std::string FormatTrace(const SubBatchTraceSnapshot& snap)
{
    std::ostringstream oss;
    PrintTraceTable(oss, snap);
    return oss.str();
}

void WriteTaskTrace(const SubBatchTraceSnapshot& snapshot, std::ostream* traceOutput,
                    std::mutex& traceMu)
{
    if (traceOutput == nullptr) {
        UC_INFO_UNLIMITED("ASU sub-batch trace:\n{}", FormatTrace(snapshot));
        return;
    }

    std::lock_guard<std::mutex> traceLock(traceMu);
    auto& traceOs = *traceOutput;
    traceOs << '\n';
    PrintTraceTable(traceOs, snapshot);
    traceOs << std::flush;
}

// Convenience

void DumpTrace(std::ostream& os, const TransportTaskContext& ctx)
{
    PrintTraceTable(os, CaptureTraceSnapshot(ctx));
}

std::string FormatTrace(const TransportTaskContext& ctx)
{
    return FormatTrace(CaptureTraceSnapshot(ctx));
}

}  // namespace UC::ASU::trace
