#pragma once

#include "abi_bridge.h"

void VI_HLE_ForceRetrace(CpuContext*);
void VI_HLE_PollRetrace(CpuContext*);
void VI_HLE_WaitForNextRetracePoll();
bool VI_HLE_IsAdvancingRetrace();
void Audio_HLE_Poll(CpuContext*);
bool OS_HLE_InterruptsEnabled() noexcept;

#define PPC_NATIVE_OVERRIDE_VOID(address, name, args, call)
