#pragma once

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

// addr_hex is the PAL identity of the guest function (see region/guest_region.h). Both forms it
// needs - the translated function's symbol and its address - come straight from the region
// header, so there is nothing to assemble here.
#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type MKW_GUEST_FUNC(addr_hex) arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(MKW_GADDR(addr_hex), name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void MKW_GUEST_FUNC(addr_hex) arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(MKW_GADDR(addr_hex), name)
