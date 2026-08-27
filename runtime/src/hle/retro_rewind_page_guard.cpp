#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cstdint>

// Retro Rewind's Kamek patch at 0x8185DC30 resolves a pause-menu page by
// index (0-255 from the table at +8, 256-511 from the extended table at
// +1032) and activates it. On Wine/Proton and native Linux, page index 0x19
// resolves to a null pointer; the original patch calls Page::Activate on it
// with no null check, which crashes several calls deep with "missing
// indirect jump target".
//
// This is a faithful reimplementation of that translated function (verified
// against generated/build_shards/retro_mod's own output for 0x8185DC30),
// with one behavioral change: when the resolved page is null, skip
// Page::Activate and the page->+16 write before it, matching
// InvokeIndirectCpu's null-target handling (abi_bridge.h) instead of
// crashing.
extern "C" void func_80601AEC(CpuContext* ctx); // Page::Activate

extern "C" void RetroRewind_ActivatePageByIndex_NullGuarded(CpuContext* ctx)
{
    uint32_t r1 = ctx->gpr[1];
    const uint32_t manager = ctx->gpr[3];
    const uint32_t index = ctx->gpr[4];
    uint32_t subIndex = ctx->gpr[5];
    const uint32_t callerR31 = ctx->gpr[31];
    uint32_t cr = ctx->cr;
    const uint32_t xer = ctx->xer;

    // Prologue: push a 16-byte stack frame, save LR and the caller's r31.
    MemoryInline::FlatWriteRam32(r1 - 16, r1);
    r1 = r1 - 16;
    const uint32_t savedLr = ctx->lr;
    MemoryInline::FlatWriteRam32(r1 + 20, savedLr);
    MemoryInline::FlatWriteRam32(r1 + 12, callerR31);

    if (subIndex == 255u) {
        subIndex = MemoryInline::FlatRead32(manager + 4);
    }

    uint32_t page;
    if (index < 256u) {
        page = MemoryInline::FlatRead32(manager + (index * 4u) + 8);
    } else {
        page = MemoryInline::FlatRead32(manager + ((index - 256u) * 4u) + 1032);
    }

    if (page == 0) {
        // Also skip the page-stack push below: pushing a null entry just moves
        // the crash to Section::Update, which walks that stack every frame.
        RT_LOG(RT_TAG_OS) << "Retro Rewind: page index 0x" << std::hex << index
            << " has no registered page (table slot is null); "
               "skipping it entirely instead of crashing." << std::dec << std::endl;
        // Dump populated page-table slots for diagnostics.
        RT_LOG(RT_TAG_OS) << "Retro Rewind: manager=0x" << std::hex << manager
            << " page table dump (index=pointer, only non-null shown):" << std::dec << std::endl;
        for (uint32_t i = 0; i < 64u; ++i) {
            const uint32_t slot = MemoryInline::FlatRead32(manager + (i * 4u) + 8);
            if (slot != 0) {
                RT_LOG(RT_TAG_OS) << "  [" << i << "]=0x" << std::hex << slot << std::dec << std::endl;
            }
        }
    } else {
        uint32_t counter = MemoryInline::FlatRead32(manager + 892);
        const uint32_t compareValue = subIndex + 65536u;
        SetCRResident(cr, xer, 0, compareValue, 65535u);

        counter = counter + 1u;
        MemoryInline::FlatWrite32(manager + 892, counter);
        MemoryInline::FlatWrite32(manager + (counter * 4u) + 848, page);
        if ((cr & 0x20000000u) == 0) {
            MemoryInline::FlatWrite32(page + 16, subIndex);
        }

        ctx->lr = 0x8185DCA4u;
        ctx->gpr[1] = r1;
        ctx->gpr[3] = page;
        ctx->gpr[4] = index;
        ctx->gpr[5] = subIndex;
        ctx->gpr[31] = page;
        ctx->cr = cr;
        func_80601AEC(ctx); // Page::Activate(page)
        page = ctx->gpr[31]; // Activate leaves r31 callee-saved as the page
    }

    // Epilogue: restore the caller's r31/LR, pop the stack frame, and return
    // the resolved page pointer (possibly null) in r3, matching the original.
    ctx->lr = savedLr;
    ctx->gpr[0] = savedLr;
    ctx->gpr[1] = r1 + 16;
    ctx->gpr[3] = page;
    ctx->gpr[4] = index;
    ctx->gpr[5] = subIndex;
    ctx->gpr[31] = callerR31;
    ctx->cr = cr;
}
PPC_NATIVE_OVERRIDE_VOID(8185DC30, RetroRewind_ActivatePageByIndex_NullGuarded, (CpuContext* ctx), (ctx));
