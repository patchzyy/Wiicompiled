#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

// Dedicated GX thread (Dolphin "dual core"): the game thread records GX HLE calls in order and
// the worker executes them. While active, GX work runs on the worker except inside inline scopes,
// which run on the game thread after the worker has caught up.
namespace GxWorker {

namespace detail {
inline constexpr size_t kAlign = 16;

extern std::atomic<bool> g_active;
extern constinit thread_local bool t_onWorker;
extern constinit thread_local int t_inlineDepth;
extern bool g_fifoPending;

void* Reserve(size_t payloadSize, void (*run)(void*));
void Commit();
void FlushFifo();
} // namespace detail

// Starts the worker unless [video] gx_worker disables it. Call once Aurora is up.
void Start();
void Stop();
// Stops without running queued work; bounded, for fatal exits.
void Abandon();

inline bool Active() noexcept { return detail::g_active.load(std::memory_order_relaxed); }
inline bool OnWorker() noexcept { return detail::t_onWorker; }
// True when a GX entry point on this thread must be recorded instead of executed.
inline bool ShouldDefer() noexcept {
    return Active() && !detail::t_onWorker && detail::t_inlineDepth == 0;
}

template <typename F>
void Submit(F&& fn) {
    using Fn = std::decay_t<F>;
    static_assert(alignof(Fn) <= detail::kAlign, "over-aligned GX command");
    if (detail::g_fifoPending) {
        detail::FlushFifo();
    }
    void* storage = detail::Reserve(sizeof(Fn), [](void* p) {
        struct Destroy {
            Fn* f;
            ~Destroy() { f->~Fn(); }
        } guard{static_cast<Fn*>(p)};
        (*guard.f)();
    });
    new (storage) Fn(std::forward<F>(fn));
    detail::Commit();
}

// Records fn when deferring, otherwise runs it now. Read call-time guest memory before calling.
template <typename F>
void Run(F&& fn) {
    if (ShouldDefer()) {
        Submit(std::forward<F>(fn));
    } else {
        fn();
    }
}

// Gather-pipe writes, coalesced into one record per run of consecutive writes and replayed
// with their original sizes.
void FifoWrite(uint32_t value, uint32_t sizeBytes);
void FifoBurst(const uint8_t* data, uint32_t sizeBytes);

// Called about every millisecond while the game thread waits in Sync.
void SetWaitCallback(void (*callback)());

// Blocks until every recorded command has executed.
void Sync();

// GX entry points run inline on the game thread while inside (after a Sync), for work whose
// results the guest needs immediately.
void BeginInline();
void EndInline();

class InlineScope {
public:
    InlineScope() { BeginInline(); }
    ~InlineScope() { EndInline(); }
    InlineScope(const InlineScope&) = delete;
    InlineScope& operator=(const InlineScope&) = delete;
};

} // namespace GxWorker

// Records the enclosing call for the GX worker and returns, or falls through when it should run
// here (worker thread, inline scope, worker disabled).
#define GX_DEFER_CALL(fn, ...)                                                                     \
    do {                                                                                           \
        if (GxWorker::ShouldDefer()) {                                                             \
            GxWorker::Submit([=] { fn(__VA_ARGS__); });                                            \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define GX_DEFER_CALL0(fn)                                                                         \
    do {                                                                                           \
        if (GxWorker::ShouldDefer()) {                                                             \
            GxWorker::Submit([] { fn(); });                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (false)

// Runs the rest of the enclosing function on this thread once the worker has caught up.
#define GX_SYNC_INLINE() GxWorker::InlineScope gxInlineScope_
