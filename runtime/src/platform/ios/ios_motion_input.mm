#import "ios_motion_input.h"

#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <cmath>
#include <mutex>

namespace IosMotionInput {
namespace {

std::mutex g_mutex;
Sample g_sample{};
bool g_hasSample = false;
CMMotionManager* g_manager = nil;
NSOperationQueue* g_queue = nil;

// Core Motion timestamps and NSProcessInfo.systemUptime use the same monotonic clock.
// A stalled delivery queue must not leave steering driven by an old orientation sample.
constexpr NSTimeInterval kMaximumSampleAge = 0.25;

bool IsFinite(const CMAcceleration value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void ClearSample() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sample = Sample{};
    g_hasSample = false;
}

void EnsureManager() {
    if (g_manager != nil) {
        return;
    }
    g_manager = [[CMMotionManager alloc] init];
    g_manager.deviceMotionUpdateInterval = 1.0 / 60.0;
    g_queue = [[NSOperationQueue alloc] init];
    g_queue.maxConcurrentOperationCount = 1;
    g_queue.name = @"org.wiicompiled.motion";
}

} // namespace

bool IsAvailable() {
    @autoreleasepool {
        EnsureManager();
        return g_manager.deviceMotionAvailable;
    }
}

void Start() {
    @autoreleasepool {
        EnsureManager();
        if (!g_manager.deviceMotionAvailable || g_manager.deviceMotionActive) {
            return;
        }
        ClearSample();
        [g_manager startDeviceMotionUpdatesToQueue:g_queue
                                        withHandler:^(CMDeviceMotion* motion, NSError* error) {
            if (error != nil) {
                ClearSample();
                return;
            }
            if (motion == nil || !IsFinite(motion.gravity) ||
                !IsFinite(motion.userAcceleration) || !std::isfinite(motion.timestamp)) {
                return;
            }

            Sample sample{};
            sample.gravity = {static_cast<float>(motion.gravity.x),
                              static_cast<float>(motion.gravity.y),
                              static_cast<float>(motion.gravity.z)};
            sample.userAcceleration = {static_cast<float>(motion.userAcceleration.x),
                                       static_cast<float>(motion.userAcceleration.y),
                                       static_cast<float>(motion.userAcceleration.z)};
            sample.timestamp = motion.timestamp;

            std::lock_guard<std::mutex> lock(g_mutex);
            g_sample = sample;
            g_hasSample = true;
        }];
    }
}

void Stop() {
    @autoreleasepool {
        if (g_manager != nil) {
            [g_manager stopDeviceMotionUpdates];
        }
        ClearSample();
    }
}

bool Read(Sample& sample) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_hasSample) {
        return false;
    }
    if (NSProcessInfo.processInfo.systemUptime - g_sample.timestamp > kMaximumSampleAge) {
        return false;
    }
    sample = g_sample;
    return true;
}

LandscapeOrientation CurrentLandscapeOrientation() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]] || scene.activationState != UISceneActivationStateForegroundActive) {
            continue;
        }
        UIWindowScene* windowScene = static_cast<UIWindowScene*>(scene);
        const UIInterfaceOrientation orientation = windowScene.effectiveGeometry.interfaceOrientation;
        if (orientation == UIInterfaceOrientationLandscapeLeft) {
            return LandscapeOrientation::Left;
        }
        if (orientation == UIInterfaceOrientationLandscapeRight) {
            return LandscapeOrientation::Right;
        }
    }
    return LandscapeOrientation::Unknown;
}

} // namespace IosMotionInput
