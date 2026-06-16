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
#pragma once

#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>
#include "asu_transport/types.h"
#include "connection_internal.h"
#include "transport_task_manager.h"

namespace UC::ASU::trace {

enum class SubBatchTracePhase {
    SUBMIT,
    COMPLETE,
};

const char* OpTypeToString(TransportOpType op);
const char* TaskStateToString(TransportTaskState state);
const char* SubBatchStateToString(TransportSubBatchState state);
const char* ChannelStateToString(ChannelState state);
const char* StatusCodeToString(StatusCode code);
const char* TracePhaseToString(SubBatchTracePhase phase);

struct SubBatchTraceEntry {
    std::size_t index{0};
    std::size_t offset{0};
    std::size_t size{0};

    bool hasCid{false};
    std::uint16_t cid{0};

    bool hasChannel{false};
    std::uint32_t groupId{0};
    std::uint32_t channelId{0};
    ChannelState channelState{ChannelState::ACTIVE};

    bool hasSendSlot{false};
    std::uint32_t sendSlot{UINT32_MAX};

    bool hasFlagSlot{false};
    std::uint32_t flagSlot{UINT32_MAX};

    TransportSubBatchState state{TransportSubBatchState::PENDING};
    Status status{Status::OK()};

    std::size_t entryStatusOkCount{0};
    std::size_t entryStatusErrCount{0};
};

struct SubBatchTraceSnapshot {
    AsuId asuId{0};
    TaskId taskId{kInvalidTaskId};
    SubBatchTracePhase phase{SubBatchTracePhase::SUBMIT};
    TransportOpType opType{TransportOpType::QUERY};
    TransportTaskState taskState{TransportTaskState::PENDING};
    Status taskFinalStatus{Status::OK()};
    std::size_t totalEntries{0};
    std::size_t subBatchCount{0};
    std::vector<SubBatchTraceEntry> entries;
};

SubBatchTraceSnapshot CaptureTraceSnapshot(
    TaskId taskId, TransportOpType opType, TransportTaskState taskState,
    const Status& taskFinalStatus, std::size_t totalEntries,
    const std::vector<TransportSubBatchContext>& subBatchContexts, AsuId asuId = 0,
    SubBatchTracePhase phase = SubBatchTracePhase::SUBMIT);

SubBatchTraceSnapshot CaptureTraceSnapshot(const TransportTaskContext& ctx,
                                           SubBatchTracePhase phase = SubBatchTracePhase::SUBMIT,
                                           AsuId asuId = 0);

void PrintTraceTable(std::ostream& os, const SubBatchTraceSnapshot& snapshot);
std::string FormatTrace(const SubBatchTraceSnapshot& snapshot);
void WriteTaskTrace(const SubBatchTraceSnapshot& snapshot, std::ostream* traceOutput,
                    std::mutex& traceMu);

void DumpTrace(std::ostream& os, const TransportTaskContext& ctx);
std::string FormatTrace(const TransportTaskContext& ctx);

}  // namespace UC::ASU::trace
