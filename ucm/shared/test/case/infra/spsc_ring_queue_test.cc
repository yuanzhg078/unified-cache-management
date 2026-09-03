/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
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
#include "template/spsc_ring_queue.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

class UCSpscRingQueueTest : public testing::Test {};

TEST_F(UCSpscRingQueueTest, Basic)
{
    UC::SpscRingQueue<size_t> queue;
    queue.Setup(16);
    size_t data;
    ASSERT_FALSE(queue.TryPop(data));
    ASSERT_TRUE(queue.TryPush(1023));
    ASSERT_TRUE(queue.TryPop(data));
    ASSERT_EQ(data, 1023);
    ASSERT_FALSE(queue.TryPop(data));
}

TEST_F(UCSpscRingQueueTest, FIFO)
{
    UC::SpscRingQueue<size_t> queue;
    queue.Setup(16);
    constexpr size_t nElem = 10;
    for (size_t i = 0; i < nElem; i++) { ASSERT_TRUE(queue.TryPush(std::move(i))); }
    for (size_t i = 0; i < nElem; i++) {
        size_t value = -1;
        ASSERT_TRUE(queue.TryPop(value));
        ASSERT_EQ(value, i);
    }
    size_t value = -1;
    ASSERT_FALSE(queue.TryPop(value));
}

TEST_F(UCSpscRingQueueTest, Full)
{
    constexpr size_t N = 10;
    UC::SpscRingQueue<size_t> queue;
    queue.Setup(N);
    constexpr size_t nElem = N - 1;
    for (size_t i = 0; i < nElem; i++) { ASSERT_TRUE(queue.TryPush(std::move(i))); }
    ASSERT_FALSE(queue.TryPush(999));
    size_t value = -1;
    ASSERT_TRUE(queue.TryPop(value));
    ASSERT_EQ(value, 0);
    ASSERT_TRUE(queue.TryPush(999));
}

TEST_F(UCSpscRingQueueTest, MoveOnly)
{
    struct MoveOnly {
        int value;
        MoveOnly() = default;
        explicit MoveOnly(int v) : value(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) = default;
        MoveOnly& operator=(MoveOnly&&) = default;
    };
    UC::SpscRingQueue<MoveOnly> queue;
    queue.Setup(9);
    EXPECT_TRUE(queue.TryPush(MoveOnly(42)));
    MoveOnly obj;
    EXPECT_TRUE(queue.TryPop(obj));
    EXPECT_EQ(obj.value, 42);
}

TEST_F(UCSpscRingQueueTest, ConsumerLoopWakesForTaskAndStop)
{
    UC::SpscRingQueue<size_t> queue;
    queue.Setup(16);
    std::atomic_bool stop{false};
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::promise<size_t> processedPromise;
    auto processed = processedPromise.get_future();

    std::thread consumer([&] {
        queue.ConsumerLoop(stop, waitMutex, waitCondition, [&processedPromise](size_t value) {
            processedPromise.set_value(value);
        });
    });
    std::uint64_t waitTimeoutCount = 0;
    for (std::uint32_t attempt = 0; attempt < 100 && waitTimeoutCount == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waitTimeoutCount += queue.TakeStats().waitTimeoutCount;
    }
    EXPECT_GT(waitTimeoutCount, 0U);
    {
        std::lock_guard<std::mutex> lock(waitMutex);
        EXPECT_TRUE(queue.TryPush(42));
    }
    queue.NotifyOne(waitCondition);

    const auto waitStatus = processed.wait_for(std::chrono::seconds(1));
    const auto queueStats = queue.TakeStats();
    {
        std::lock_guard<std::mutex> lock(waitMutex);
        stop.store(true, std::memory_order_release);
    }
    waitCondition.notify_one();
    consumer.join();

    ASSERT_EQ(waitStatus, std::future_status::ready);
    EXPECT_EQ(processed.get(), 42);
    EXPECT_EQ(queueStats.waitNotifiedCount, 1U);
    EXPECT_EQ(queueStats.notifyCount, 1U);
}

TEST_F(UCSpscRingQueueTest, ConsumerLoopCountsWaitTimeouts)
{
    UC::SpscRingQueue<size_t> queue;
    queue.Setup(16);
    std::atomic_bool stop{false};
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::thread consumer(
        [&] { queue.ConsumerLoop(stop, waitMutex, waitCondition, [](size_t) {}); });

    std::uint64_t waitTimeoutCount = 0;
    for (std::uint32_t attempt = 0; attempt < 100 && waitTimeoutCount == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waitTimeoutCount += queue.TakeStats().waitTimeoutCount;
    }
    {
        std::lock_guard<std::mutex> lock(waitMutex);
        stop.store(true, std::memory_order_release);
    }
    waitCondition.notify_one();
    consumer.join();
    waitTimeoutCount += queue.TakeStats().waitTimeoutCount;

    EXPECT_GT(waitTimeoutCount, 0U);
}
