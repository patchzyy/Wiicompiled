#pragma once

#include "friend_code_keyboard.h"
#include "settings_overlay.h"
#include "runtime_config.h"
#include "gx_worker.h"
#include "perf_stats.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXAurora.h>

#include <cstdlib>
#include <atomic>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" bool g_dynamicAspectRatioEnabled;
void ConfigureMkwDynamicAspect(bool widescreen, bool forceAspect169, uint32_t surfaceWidth, uint32_t surfaceHeight);
void SetMkwForceAspect169(bool enabled);
bool MkwForceAspect169Requested();
void UpdateMkwDynamicAspectSurface(uint32_t surfaceWidth, uint32_t surfaceHeight);
// Arms the "keep EGG::Frustum's projection scale" flag on every screen that
// renders to a fixed-size offscreen target. Cheap and idempotent; called from
// the GX viewport path so it beats bakes that never cross a frame boundary.
void AssertMkwOffscreenScreenBypass();
inline std::atomic_bool g_mkwDynamicAspectSurfacePending{false};

namespace WindowPlacementPersistence {
inline bool sizeDirty = false;
inline bool positionDirty = false;
inline uint32_t width = 0;
inline uint32_t height = 0;
inline int32_t x = 0;
inline int32_t y = 0;
inline std::chrono::steady_clock::time_point changedAt{};

inline void Flush(bool force = false) {
    if (!sizeDirty && !positionDirty) {
        return;
    }
    constexpr auto kSaveDelay = std::chrono::milliseconds(300);
    if (!force && std::chrono::steady_clock::now() - changedAt < kSaveDelay) {
        return;
    }
    if (sizeDirty && width != 0 && height != 0) {
        RuntimeConfigFile::SetWindowSize(width, height);
    }
    if (positionDirty) {
        RuntimeConfigFile::SetWindowPosition(x, y);
    }
    sizeDirty = false;
    positionDirty = false;
}
} // namespace WindowPlacementPersistence

// The close event can be consumed while execution is inside a guest fiber
// (for example, OSSleepThread).  Running normal C++/CRT shutdown from that
// fiber re-enters runtime teardown and can fault while the fiber machinery is
// still active.  A window close is an intentional successful exit, so end the
// process directly and do not run the crash/atexit paths.
[[noreturn]] inline void ExitForAuroraWindowClose() noexcept {
    settings_overlay::ReleaseControllers();
    WindowPlacementPersistence::Flush(true);
#if defined(_WIN32)
    ::ExitProcess(0);
#else
    std::_Exit(EXIT_SUCCESS);
#endif
}

// Update cached Aurora window/framebuffer dimensions based on pending events.
inline void ProcessAuroraEvents(const AuroraEvent* events) {
    if (!events) {
        return;
    }

    bool surfaceChanged = false;
    for (const AuroraEvent* event = events; event->type != AURORA_NONE; ++event) {
        switch (event->type) {
        case AURORA_WINDOW_MOVED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED) {
                WindowPlacementPersistence::x = event->windowPos.x;
                WindowPlacementPersistence::y = event->windowPos.y;
                WindowPlacementPersistence::positionDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            break;
        case AURORA_WINDOW_RESIZED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED &&
                event->windowSize.width != 0 && event->windowSize.height != 0) {
                WindowPlacementPersistence::width = event->windowSize.width;
                WindowPlacementPersistence::height = event->windowSize.height;
                WindowPlacementPersistence::sizeDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            surfaceChanged = true;
            break;
        case AURORA_DISPLAY_SCALE_CHANGED:
            surfaceChanged = true;
            break;
        case AURORA_EXIT:
            ExitForAuroraWindowClose();
        default:
            break;
        }
    }

    WindowPlacementPersistence::Flush();

    if (surfaceChanged) {
        // Applying the viewport policy drains GX. Event dispatch can run
        // between asynchronous end_frame and the next begin_frame, while the
        // frame worker is intentionally waiting for begin permission. Joining
        // it here creates a circular wait. Record the newest native size and
        // apply it immediately after the next frame has been prepared.
        g_mkwDynamicAspectSurfacePending.store(true, std::memory_order_release);
    }
    settings_overlay::HandleEvents(events);
    FriendCodeKeyboard::HandleEvents(events);
}

inline void ApplyPendingMkwDynamicAspectSurface() {
    // The OS can adjust a window without a resize event reaching the queue
    // (observed with hidden windows clamped to the work area), so re-read the
    // surface at every frame boundary instead of only on queued events.
    // UpdateMkwDynamicAspectSurface is idempotent and cheap for a stable size.
    (void)g_mkwDynamicAspectSurfacePending.exchange(false, std::memory_order_acq_rel);
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    AuroraGetSurfaceSize(&surfaceWidth, &surfaceHeight);
    UpdateMkwDynamicAspectSurface(surfaceWidth, surfaceHeight);
}

void GxWorkerBeginFrame();
void GxWorkerEndFrame(AuroraImguiFrame* imgui);
void SetAuroraFrameGxBegun(bool begun);

inline bool BeginAuroraFrame() {
    if (GxWorker::ShouldDefer()) {
        // The window half can touch the surface and render targets the worker draws into.
        GxWorker::Sync();
        bool begun = false;
        {
            PerfStats::WaitScope wait;
            begun = aurora_begin_frame_window();
        }
        if (!begun) {
            return false;
        }
        GxWorker::Submit([] { GxWorkerBeginFrame(); });
        ApplyPendingMkwDynamicAspectSurface();
        return true;
    }
    if (!aurora_begin_frame()) {
        return false;
    }
    SetAuroraFrameGxBegun(true);
    ApplyPendingMkwDynamicAspectSurface();
    return true;
}

// Ends the frame; with the GX worker, the game thread joins it here so the next frame never
// overlaps guest memory the worker may still read.
inline void EndAuroraFrame() {
    if (GxWorker::ShouldDefer()) {
        AuroraImguiFrame* imgui = aurora_end_frame_window();
        GxWorker::Submit([imgui] { GxWorkerEndFrame(imgui); });
        GxWorker::Sync();
        return;
    }
    aurora_end_frame();
    SetAuroraFrameGxBegun(false);
}

// Poll Aurora events and update cached window/framebuffer dimensions.
inline void UpdateAuroraAndProcessEvents() {
    ProcessAuroraEvents(aurora_update());
}
