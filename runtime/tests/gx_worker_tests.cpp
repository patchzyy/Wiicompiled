#define MKW_GX_WORKER_TESTING 1
#include "../src/hle/gx/gx_worker.cpp"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <stdexcept>

namespace PerfStats {
bool Enabled() noexcept { return false; }
void AddWait(std::chrono::steady_clock::duration) noexcept {}
} // namespace PerfStats

namespace {
std::vector<std::pair<uint32_t, uint32_t>> g_fifoReplay;
std::function<void()> g_service;
std::function<void()> g_beforeRecheck;

void ServiceWait() { g_service(); }
void BeforeRecheck() { g_beforeRecheck(); }

template <typename F>
bool WaitUntil(F&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

class Gate {
public:
    ~Gate() { Open(); }
    void Block() {
        std::unique_lock lock{m_mutex};
        m_entered = true;
        m_cv.notify_all();
        m_cv.wait(lock, [&] { return m_open; });
    }
    bool WaitEntered() {
        std::unique_lock lock{m_mutex};
        return m_cv.wait_for(lock, std::chrono::seconds{2}, [&] { return m_entered; });
    }
    bool WaitOpened() {
        std::unique_lock lock{m_mutex};
        return m_cv.wait_for(lock, std::chrono::seconds{2}, [&] { return m_open; });
    }
    void Open() {
        std::lock_guard lock{m_mutex};
        m_open = true;
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_entered = false;
    bool m_open = false;
};

class GxWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        GxWorker::g_testEnabled = true;
        GxWorker::g_testWakeEachRecord = false;
        GxWorker::g_testNotifications = 0;
        GxWorker::g_testWaits = 0;
        g_fifoReplay.clear();
        GxWorker::Start();
    }
    void TearDown() override {
        GxWorker::g_testBeforeTailRecheck = nullptr;
        GxWorker::SetWaitCallback(nullptr);
        GxWorker::Stop();
        g_service = {};
        g_beforeRecheck = {};
    }
};

TEST_F(GxWorkerTest, DoesNotNotifyBeforeTheRequestedTail) {
    Gate first;
    Gate last;
    uint32_t completed = 0;
    GxWorker::Submit([&] { first.Block(); });
    for (uint32_t i = 0; i < 512; ++i) {
        GxWorker::Submit([&, i] { EXPECT_EQ(completed++, i); });
    }
    GxWorker::Submit([&] { last.Block(); });
    std::thread release{[&] {
        EXPECT_TRUE(WaitUntil([] { return GxWorker::g_testWaits.load() != 0; }));
        first.Open();
        EXPECT_TRUE(last.WaitEntered());
        EXPECT_EQ(GxWorker::g_testNotifications.load(), 0u);
        last.Open();
    }};
    GxWorker::Sync();
    release.join();
    EXPECT_EQ(completed, 512u);
    EXPECT_GE(GxWorker::g_testWaits.load(), 1u);
    EXPECT_EQ(GxWorker::g_waitTarget.load(), GxWorker::kNotWaiting);
}

TEST_F(GxWorkerTest, ReachingTargetNotifiesTheBlockedProducer) {
    Gate gate;
    GxWorker::Submit([&] { gate.Block(); });
    GxWorker::Publish();
    if (!gate.WaitEntered()) {
        gate.Open();
        GxWorker::Sync();
        FAIL() << "The worker did not enter the gate";
    }
    std::unique_lock lock{GxWorker::g_tailMutex};
    GxWorker::g_waitTarget.store(GxWorker::g_write);
    gate.Open();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (GxWorker::g_testNotifications.load() == 0) {
        if (GxWorker::g_tailCv.wait_until(lock, deadline) == std::cv_status::timeout) {
            ADD_FAILURE() << "The worker did not signal the target";
            break;
        }
    }
    EXPECT_EQ(GxWorker::g_tail.load(), GxWorker::g_write);
    GxWorker::g_waitTarget.store(GxWorker::kNotWaiting);
}

TEST_F(GxWorkerTest, RechecksCompletionAfterPublishingTheWaitTarget) {
    Gate gate;
    GxWorker::Submit([&] { gate.Block(); });
    g_beforeRecheck = [&] {
        GxWorker::g_testBeforeTailRecheck = nullptr;
        gate.Open();
        EXPECT_TRUE(WaitUntil([] { return GxWorker::g_tail.load() >= GxWorker::g_waitTarget.load(); }));
    };
    GxWorker::g_testBeforeTailRecheck = BeforeRecheck;
    GxWorker::Sync();
    EXPECT_EQ(GxWorker::g_testWaits.load(), 0u);
    EXPECT_EQ(GxWorker::g_waitTarget.load(), GxWorker::kNotWaiting);
}

void CheckWorkSubmittedByCallback(bool syncInsideCallback) {
    Gate gate;
    uint32_t callbacks = 0;
    std::atomic_uint32_t completed{0};
    GxWorker::Submit([&] { gate.Block(); ++completed; });
    g_service = [&] {
        if (++callbacks == 1) {
            GxWorker::Submit([&] { ++completed; });
            gate.Open();
            if (syncInsideCallback) {
                GxWorker::Sync();
                EXPECT_EQ(completed.load(), 2u);
            }
        }
    };
    GxWorker::SetWaitCallback(ServiceWait);
    std::thread watchdog{[&] {
        if (!gate.WaitOpened()) {
            ADD_FAILURE() << "The wait callback did not release the worker gate";
            gate.Open();
        }
    }};
    GxWorker::Sync();
    watchdog.join();
    EXPECT_GE(callbacks, 1u);
    EXPECT_EQ(completed.load(), 2u);
    GxWorker::SetWaitCallback(nullptr);
    GxWorker::Publish();
    GxWorker::Sync();
    EXPECT_EQ(GxWorker::g_waitTarget.load(), GxWorker::kNotWaiting);
}

TEST_F(GxWorkerTest, NestedSyncDrainsWorkBeforeReturningToCallback) {
    CheckWorkSubmittedByCallback(true);
}

TEST_F(GxWorkerTest, OuterSyncDrainsWorkSubmittedByCallback) {
    CheckWorkSubmittedByCallback(false);
}

TEST_F(GxWorkerTest, RingBackpressureAndWrapPreserveOrderWithoutServicingGuestCallbacks) {
    Gate gate;
    uint32_t callbacks = 0;
    uint32_t completed = 0;
    GxWorker::Submit([&] { gate.Block(); });
    std::thread release{[&] {
        EXPECT_TRUE(WaitUntil([] { return GxWorker::g_waitTarget.load() != GxWorker::kNotWaiting; }));
        gate.Open();
    }};
    g_service = [&] { ++callbacks; };
    GxWorker::SetWaitCallback(ServiceWait);
    for (uint32_t i = 0; i < 160; ++i) {
        std::array<uint8_t, 65536> bytes{};
        bytes.front() = static_cast<uint8_t>(i);
        bytes.back() = static_cast<uint8_t>(i ^ 0xff);
        GxWorker::Submit([&, bytes, i] {
            EXPECT_EQ(completed++, i);
            EXPECT_EQ(bytes.front(), static_cast<uint8_t>(i));
            EXPECT_EQ(bytes.back(), static_cast<uint8_t>(i ^ 0xff));
        });
    }
    release.join();
    EXPECT_EQ(callbacks, 0u);
    GxWorker::SetWaitCallback(nullptr);
    GxWorker::Sync();
    EXPECT_EQ(completed, 160u);
}

TEST_F(GxWorkerTest, FifoFlushInlineAndRestartRetainTheirBoundaries) {
    GxWorker::FifoWrite(0x61, 1);
    GxWorker::FifoWrite(0x12345678, 4);
    const std::array<uint8_t, 3> burst{1, 2, 3};
    GxWorker::FifoBurst(burst.data(), burst.size());
    GxWorker::Submit([] { EXPECT_EQ(g_fifoReplay.size(), 5u); });
    {
        GxWorker::InlineScope scope;
        EXPECT_FALSE(GxWorker::ShouldDefer());
        EXPECT_EQ(g_fifoReplay, (std::vector<std::pair<uint32_t, uint32_t>>{
                                   {0x61, 1}, {0x12345678, 4}, {1, 1}, {2, 1}, {3, 1}}));
    }
    EXPECT_TRUE(GxWorker::ShouldDefer());
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    GxWorker::Submit([owned = std::move(lifetime)] {});
    GxWorker::Stop();
    EXPECT_TRUE(weak.expired());
    GxWorker::g_testEnabled = false;
    GxWorker::Start();
    EXPECT_FALSE(GxWorker::Active());
    bool ran = false;
    GxWorker::Run([&] { ran = true; });
    EXPECT_TRUE(ran);
    GxWorker::g_testEnabled = true;
    GxWorker::Start();
    GxWorker::Submit([&] { ran = false; });
    GxWorker::Sync();
    EXPECT_FALSE(ran);
}

TEST_F(GxWorkerTest, ThrowingCommandsStillCompleteTheirRecordAndReleaseTheirPayload) {
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    bool completed = false;
    GxWorker::Submit([owned = std::move(lifetime)] { throw std::runtime_error("injected failure"); });
    GxWorker::Submit([&] { completed = true; });
    GxWorker::Sync();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(weak.expired());
}

#if GTEST_HAS_DEATH_TEST
TEST(GxWorkerDeathTest, AbandonWakesTheIdleConsumerAndSignalsExit) {
    ASSERT_EXIT({
        GxWorker::Start();
        if (!WaitUntil([] { return GxWorker::g_consumerWaiting.load(); })) {
            std::_Exit(2);
        }
        {
            std::lock_guard lock{GxWorker::g_headMutex};
        }
        GxWorker::Abandon();
        std::_Exit(!GxWorker::Active() && GxWorker::g_thread == nullptr && GxWorker::g_exited.load() ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(GxWorkerDeathTest, AbandonDoesNotExecuteTheFollowingRecord) {
    ASSERT_EXIT({
        Gate gate;
        std::atomic<bool> ran{false};
        GxWorker::Start();
        GxWorker::Submit([&] { gate.Block(); });
        GxWorker::Submit([&] { ran = true; });
        GxWorker::Publish();
        if (!gate.WaitEntered()) {
            std::_Exit(2);
        }
        std::thread release{[&] {
            WaitUntil([] { return GxWorker::g_abandon.load(); });
            gate.Open();
        }};
        GxWorker::Abandon();
        release.join();
        std::_Exit(!ran.load() && GxWorker::g_exited.load() ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}
#endif

TEST_F(GxWorkerTest, DISABLED_WakeBenchmark) {
    constexpr uint32_t recordsPerBatch = 20000;
    std::array<std::vector<int64_t>, 2> times;
    std::array<uint64_t, 2> notifications{};
    std::array<uint64_t, 2> waits{};
    for (uint32_t batch = 0; batch < 40; ++batch) {
        for (uint32_t order = 0; order < 2; ++order) {
            const uint32_t mode = (batch + order) % 2;
            GxWorker::g_testWakeEachRecord = mode != 0;
            GxWorker::g_testNotifications = 0;
            GxWorker::g_testWaits = 0;
            uint32_t completed = 0;
            Gate gate;
            const auto start = std::chrono::steady_clock::now();
            GxWorker::Submit([&] { gate.Block(); });
            for (uint32_t i = 1; i < recordsPerBatch; ++i) {
                GxWorker::Submit([&] { ++completed; });
            }
            GxWorker::Submit([&] {
                ++completed;
                GxWorker::g_testAcknowledgeRecord = true;
            });
            std::thread release{[&] {
                EXPECT_TRUE(WaitUntil([] { return GxWorker::g_testWaits.load() != 0; }));
                gate.Open();
            }};
            GxWorker::Sync();
            const auto end = std::chrono::steady_clock::now();
            release.join();
            times[mode].push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

            // The worker accounts for notifications after publishing the tail.
            ASSERT_TRUE(WaitUntil([] { return GxWorker::g_testAccountedTail.load() == GxWorker::g_write; }));
            notifications[mode] += GxWorker::g_testNotifications.load();
            waits[mode] += GxWorker::g_testWaits.load();
            EXPECT_EQ(completed, recordsPerBatch);
        }
    }
    for (uint32_t mode = 0; mode < 2; ++mode) {
        std::sort(times[mode].begin(), times[mode].end());
        std::printf("wake_each=%u records=800000 notifications=%llu waits=%llu median_us=%lld p95_us=%lld\n", mode,
                    static_cast<unsigned long long>(notifications[mode]), static_cast<unsigned long long>(waits[mode]),
                    static_cast<long long>((times[mode][19] + times[mode][20]) / 2),
                    static_cast<long long>(times[mode][37]));
    }
    GxWorker::g_testWakeEachRecord = false;
}
} // namespace

void GxWorkerReplayFifoWrite(uint32_t value, uint32_t sizeBytes) { g_fifoReplay.emplace_back(value, sizeBytes); }
void GxWorkerReplayFifoBurst(const uint8_t* data, uint32_t sizeBytes) {
    for (uint32_t i = 0; i < sizeBytes; ++i) {
        GxWorkerReplayFifoWrite(data[i], 1);
    }
}
