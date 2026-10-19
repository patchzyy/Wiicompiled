using System.Buffers.Binary;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public sealed class ModFunctionDiscoveryTests
{
    [Fact]
    public void DiscoversModuleDataPointersAndPrologues()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x80];
        WriteU32(code, 0x20, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x24, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x28, 0x90010014u); // stw r0,0x14(r1)
        WriteU32(code, 0x2C, 0x4E800020u); // blr
        WriteU32(code, 0x40, moduleBase + 0x50);
        WriteU32(code, 0x50, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(KamekChunk.HeaderSize + code.Length),
            codeBlob: code,
            commands: [PointerRelocation(0x40, 0x50)]);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module prologue scan", starts[moduleBase + 0x20]);
        Assert.Equal("module data pointer", starts[moduleBase + 0x50]);
    }

    [Fact]
    public void DiscoversModuleBaseBctrThunk()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x40];
        WriteU32(code, 0x00, 0x7C6C1B78u); // mr r12,r3
        WriteU32(code, 0x04, 0x7D8903A6u); // mtctr r12
        WriteU32(code, 0x08, 0x4E800420u); // bctr
        WriteU32(code, 0x24, 0x9421FFF0u); // stwu r1,-0x10(r1)

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module base", starts[moduleBase]);
    }

    [Fact]
    public void DiscoversSynthesizedLeafFunctionPointers()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x9800];
        WriteU32(code, 0x40, 0x3C608121u); // lis r3,0x8121
        WriteU32(code, 0x44, 0x38639710u); // addi r3,r3,-0x68f0 => 0x81209710
        WriteU32(code, 0x48, 0x907EAD54u); // stw r3,-0x52ac(r30)
        WriteU32(code, 0x9710, 0x3C808038u); // lis r4,0x8038
        WriteU32(code, 0x9714, 0x38600016u); // li r3,0x16
        WriteU32(code, 0x9718, 0x80846E3Cu); // lwz r4,0x6e3c(r4)
        WriteU32(code, 0x971C, 0x38000010u); // li r0,0x10
        WriteU32(code, 0x9720, 0xB0640020u); // sth r3,0x20(r4)
        WriteU32(code, 0x9724, 0xB004001Cu); // sth r0,0x1c(r4)
        WriteU32(code, 0x9728, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[0x81209710]);
    }

    [Fact]
    public void DiscoversSynthesizedCallbackStoredAfterRegisterSaveSetup()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x300];

        // Placed past the declared code size so only the synthesized-pointer
        // scan, and not the prologue scan, can attribute this start.
        WriteU32(code, 0x80, 0x9421FFF0u); // callback: stwu r1,-0x10(r1)
        WriteU32(code, 0x84, 0x4E800020u); // blr
        WriteU32(code, 0x40, 0x9421FFF0u); // outer function
        WriteU32(code, 0x44, 0x7C0802A6u); // mflr r0

        WriteU32(code, 0x100, 0x3C608170u); // lis r3,0x8170
        WriteU32(code, 0x104, 0x3CA0817Bu); // unrelated setup
        WriteU32(code, 0x108, 0x90010024u); // save r0
        WriteU32(code, 0x10C, 0x38630080u); // addi r3,r3,0x80
        WriteU32(code, 0x110, 0x93E1001Cu); // save r31
        WriteU32(code, 0x114, 0x3FE08177u); // more setup
        WriteU32(code, 0x118, 0x38DFEC94u);
        WriteU32(code, 0x11C, 0x93C10018u);
        WriteU32(code, 0x120, 0x3FC08170u);
        WriteU32(code, 0x124, 0x93A10014u);
        WriteU32(code, 0x128, 0x3FA08174u);
        WriteU32(code, 0x12C, 0x389DCCB4u);
        WriteU32(code, 0x130, 0x93810010u);
        WriteU32(code, 0x134, 0x3F80817Bu);
        WriteU32(code, 0x138, 0x907C0180u); // stw r3,0x180(r28)

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x80,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[moduleBase + 0x80]);
    }

    [Fact]
    public void DiscoversSynthesizedLongLeafFunctionPointers()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x40000];
        WriteU32(code, 0xF574, 0x3C608124u); // lis r3,0x8124
        WriteU32(code, 0xF578, 0x38800003u); // li r4,3
        WriteU32(code, 0xF57C, 0x90010014u); // stw r0,0x14(r1)
        WriteU32(code, 0xF580, 0x3863F1C0u); // addi r3,r3,-0xe40 => 0x8123F1C0
        WriteU32(code, 0xF584, 0x907E1D50u); // stw r3,0x1d50(r30)

        WriteU32(code, 0x3F1C0, 0x3D008129u); // lis r8,0x8129
        for (var offset = 0x3F1C4; offset < 0x3F204; offset += 4)
        {
            WriteU32(code, offset, 0x60000000u); // nop-like body
        }

        WriteU32(code, 0x3F204, 0x4E800020u); // blr, past the old 64-byte leaf scan window

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[0x8123F1C0]);
    }

    [Fact]
    public void DiscoversSynthesizedPointersWrittenToDifferentRegister()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x40000];
        WriteU32(code, 0x200, 0x3FE08124u); // lis r31,0x8124
        WriteU32(code, 0x204, 0x389FE6B4u); // addi r4,r31,-0x194c => 0x8123E6B4, not directly stored
        WriteU32(code, 0x208, 0x38A50000u); // addi r5,r5,0
        WriteU32(code, 0x20C, 0x381FE6B4u); // addi r0,r31,-0x194c => 0x8123E6B4
        WriteU32(code, 0x210, 0x901E003Cu); // stw r0,0x3c(r30)

        WriteU32(code, 0x3E6B4, 0x3C60801Cu); // lis r3,0x801c
        WriteU32(code, 0x3E6B8, 0x8063AB20u); // lwz r3,-0x54e0(r3)
        WriteU32(code, 0x3E6BC, 0x3C03B180u); // addis r0,r3,-0x4e80
        WriteU32(code, 0x3E6C0, 0x28000020u); // cmplwi r0,0x20
        WriteU32(code, 0x3E6C4, 0x4D820020u); // beqlr
        WriteU32(code, 0x3E6C8, 0x3C608126u); // lis r3,0x8126
        WriteU32(code, 0x3E6CC, 0x38636650u); // addi r3,r3,0x6650
        WriteU32(code, 0x3E6D0, 0x4BFCAF08u); // b some helper
        WriteU32(code, 0x3E6D4, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[0x8123E6B4]);
    }

    [Fact]
    public void DiscoversSynthesizedComparatorPassedThroughExternalTailCall()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x20000];

        // Mirrors the Retro Rewind trampoline at 0x8171FCB4: a lone backward
        // branch referenced only as a callback argument to a base-game routine.
        WriteU32(code, 0x1FBC8, 0x9421FFF0u); // helper: stwu r1,-0x10(r1)
        WriteU32(code, 0x1FBCC, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x1FBD0, 0x4E800020u); // blr
        WriteU32(code, 0x1FCB4, EncodeBranch(moduleBase + 0x1FCB4, moduleBase + 0x1FBC8)); // trampoline

        WriteU32(code, 0x1FCCC, 0x3CC08172u); // lis r6,0x8172
        WriteU32(code, 0x1FCD0, 0x38A00018u); // li r5,0x18
        WriteU32(code, 0x1FCD4, 0x38C6FCB4u); // addi r6,r6,-0x34c => 0x8171FCB4
        WriteU32(code, 0x1FCD8, EncodeBranch(moduleBase + 0x1FCD8, 0x80011B00u)); // tail call into base sort routine

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[moduleBase + 0x1FCB4]);
    }

    [Fact]
    public void DiscoversSynthesizedCallbackPassedAsCallArgument()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x20000];

        WriteU32(code, 0x1000, 0x38600001u); // prologue-less leaf callback: li r3,1
        WriteU32(code, 0x1004, 0x4E800020u); // blr

        WriteU32(code, 0x2000, 0x3C808170u); // lis r4,0x8170
        WriteU32(code, 0x2004, 0x38841000u); // addi r4,r4,0x1000 => callback
        WriteU32(code, 0x2008, 0x48000001u | (0x3000u - 0x2008u)); // bl moduleBase+0x3000
        WriteU32(code, 0x3000, 0x4E800020u); // callee: blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module synthesized pointer", starts[moduleBase + 0x1000]);
    }

    [Fact]
    public void IgnoresAsciiDataThatDecodesAsForwardBranch()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x20000];

        // "HeyhoShipGBA.brres" - 'H' (0x48) decodes as an unconditional forward
        // branch whose target lands far outside executable MEM1.
        var name = "HeyhoShipGBA.brres\0\0"u8.ToArray();
        name.CopyTo(code, 0x1B5FC);

        WriteU32(code, 0x2000, 0x3C808172u); // lis r4,0x8172
        WriteU32(code, 0x2004, 0x3884B5FCu); // addi r4,r4,-0x4a04 => 0x8171B5FC string address
        WriteU32(code, 0x2008, 0x48000001u | (0x3000u - 0x2008u)); // bl: string passed as argument
        WriteU32(code, 0x3000, 0x4E800020u); // callee: blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.False(starts.ContainsKey(moduleBase + 0x1B5FC));
    }

    [Fact]
    public void IgnoresSynthesizedAddressWhoseRegisterDiesAtCall()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x20000];

        WriteU32(code, 0x1000, 0x38600001u); // leaf-looking target: li r3,1
        WriteU32(code, 0x1004, 0x4E800020u); // blr

        WriteU32(code, 0x2000, 0x3D808170u); // lis r12,0x8170
        WriteU32(code, 0x2004, 0x398C1000u); // addi r12,r12,0x1000
        WriteU32(code, 0x2008, 0x48000001u | (0x3000u - 0x2008u)); // bl clobbers r12 before any use
        WriteU32(code, 0x3000, 0x4E800020u); // callee: blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.DoesNotContain(starts, kvp => kvp.Value == "module synthesized pointer");
    }

    [Fact]
    public void DiscoversModuleDataPointerToLeafFunction()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x200];
        WriteU32(code, 0x20, moduleBase + 0x100);
        WriteU32(code, 0x100, 0x806300DCu); // lwz r3,0xdc(r3)
        WriteU32(code, 0x104, 0x7C802378u); // mr r0,r4
        WriteU32(code, 0x108, 0x2C030000u); // cmpwi r3,0
        WriteU32(code, 0x10C, 0x38600000u); // li r3,0
        WriteU32(code, 0x110, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: [PointerRelocation(0x20, 0x100)]);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x100]);
    }

    [Fact]
    public void DiscoversAppendedModuleDataPointerToLeafFunction()
    {
        const uint moduleBase = 0x81200000u;
        var relocatedImage = new byte[0x300];
        WriteU32(relocatedImage, 0x280, moduleBase + 0x200); // appended pointer table
        WriteU32(relocatedImage, 0x200, 0x38600001u); // li r3,1
        WriteU32(relocatedImage, 0x204, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x100,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(0x100 + KamekChunk.HeaderSize),
            codeBlob: relocatedImage[..0x100],
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, relocatedImage)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x200]);
    }

    [Fact]
    public void DiscoversShortTailEntryInsideMappedPayloadFunction()
    {
        const uint moduleBase = 0x81700000u;
        var relocatedImage = new byte[0x300];

        WriteU32(relocatedImage, 0x150, 0x9421FFF0u); // stwu r1,-0x10(r1), executable tail target
        WriteU32(relocatedImage, 0x154, 0x4E800020u); // blr
        WriteU32(relocatedImage, 0x200, 0x9421FFF0u); // mapped public payload function
        WriteU32(relocatedImage, 0x204, 0x4E800020u); // blr
        WriteU32(relocatedImage, 0x230, EncodeBranch(moduleBase + 0x230, moduleBase + 0x200)); // previous helper tail
        WriteU32(relocatedImage, 0x234, 0x38800001u); // li r4,1
        WriteU32(relocatedImage, 0x238, EncodeBranch(moduleBase + 0x238, moduleBase + 0x150)); // b target
        WriteU32(relocatedImage, 0x23C, 0x000058B4u); // inline data between tail entry and next prologue
        WriteU32(relocatedImage, 0x240, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(relocatedImage, 0x244, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x100,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(0x100 + KamekChunk.HeaderSize),
            codeBlob: relocatedImage[..0x100],
            commands: []);
        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, relocatedImage)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module tail-entry scan", starts[moduleBase + 0x234]);
        Assert.False(starts.ContainsKey(moduleBase + 0x23C));
    }

    [Fact]
    public void KeepsPrologueScanScopedToOriginalCodeSize()
    {
        const uint moduleBase = 0x81200000u;
        var relocatedImage = new byte[0x300];
        WriteU32(relocatedImage, 0x200, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(relocatedImage, 0x204, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x100,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(0x100 + KamekChunk.HeaderSize),
            codeBlob: relocatedImage[..0x100],
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, relocatedImage);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x200);
    }

    [Fact]
    public void DiscoversLeafFunctionBetweenReturnedFunctionAndNextPrologue()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x500];
        WriteU32(code, 0x320, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x324, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x328, 0x90010014u); // stw r0,0x14(r1)
        WriteU32(code, 0x32C, 0x80010014u); // lwz r0,0x14(r1)
        WriteU32(code, 0x330, 0x7C0803A6u); // mtlr r0
        WriteU32(code, 0x334, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x338, 0x4E800020u); // blr

        WriteU32(code, 0x33C, 0x3C60809Cu); // lis r3,0x809c
        WriteU32(code, 0x340, 0x3CA08129u); // lis r5,0x8129
        WriteU32(code, 0x344, 0x80831E38u); // lwz r4,0x1e38(r3)
        WriteU32(code, 0x348, 0x8065FD1Cu); // lwz r3,-0x2e4(r5)
        WriteU32(code, 0x34C, 0x2C040048u); // cmpwi r4,0x48
        WriteU32(code, 0x350, 0x4D820020u); // beqlr
        WriteU32(code, 0x354, 0x38000000u); // li r0,0
        WriteU32(code, 0x358, 0x90030060u); // stw r0,0x60(r3)
        WriteU32(code, 0x35C, 0x4E800020u); // blr

        WriteU32(code, 0x3A0, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x3A4, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module leaf boundary scan", starts[moduleBase + 0x33C]);
    }

    [Fact]
    public void DiscoversTailThunkBetweenBranchedFunctionAndNextPrologue()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x500];
        WriteU32(code, 0x240, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x244, 0x4E800020u); // blr

        WriteU32(code, 0x320, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x324, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x328, 0x90010014u); // stw r0,0x14(r1)
        WriteU32(code, 0x32C, 0x38630020u); // addi r3,r3,0x20
        WriteU32(code, 0x330, 0x48000010u); // b moduleBase+0x340

        WriteU32(code, 0x334, 0x3C608129u); // lis r3,0x8129
        WriteU32(code, 0x338, 0x38631C5Cu); // addi r3,r3,0x1c5c
        WriteU32(code, 0x33C, 0x4BFFFF04u); // b moduleBase+0x240

        WriteU32(code, 0x340, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x344, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module leaf boundary scan", starts[moduleBase + 0x334]);
    }

    [Fact]
    public void DoesNotSplitCountedLoopContinuationAfterConditionalReturn()
    {
        const uint moduleBase = 0x81750000u;
        var code = new byte[0x200];

        WriteU32(code, 0x100, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x120, 0x80050008u); // lwz r0,8(r5)
        WriteU32(code, 0x124, 0x7C002000u); // cmpw r0,r4
        WriteU32(code, 0x128, 0x40820008u); // bne moduleBase+0x130
        WriteU32(code, 0x12C, 0x4E800020u); // blr
        WriteU32(code, 0x130, 0x38A50004u); // addi r5,r5,4
        WriteU32(code, 0x134, 0x4200FFECu); // bdnz moduleBase+0x120
        WriteU32(code, 0x138, 0x4E800020u); // blr

        WriteU32(code, 0x180, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x184, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.False(starts.ContainsKey(moduleBase + 0x130));
    }

    [Fact]
    public void DiscoversLeafFunctionThatTailBranchesToBaseCode()
    {
        const uint moduleBase = 0x8174FBA0u;
        var code = new byte[0x500];

        WriteU32(code, 0x300, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x304, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x308, 0x4E800020u); // blr

        WriteU32(code, 0x30C, 0x38000008u); // li r0,8
        WriteU32(code, 0x310, 0x3880FFFFu); // li r4,-1
        WriteU32(code, 0x314, 0x90832AB4u); // stw r4,0x2ab4(r3)
        WriteU32(code, 0x318, 0x38A00000u); // li r5,0
        WriteU32(code, 0x31C, 0x80C3072Cu); // lwz r6,0x72c(r3)
        WriteU32(code, 0x320, 0x7C0903A6u); // mtctr r0
        WriteU32(code, 0x324, 0x80860000u); // lwz r4,0(r6)
        WriteU32(code, 0x328, 0x38C60004u); // addi r6,r6,4
        WriteU32(code, 0x32C, 0x98A401E9u); // stb r5,0x1e9(r4)
        WriteU32(code, 0x330, 0x4200FFF4u); // bdnz -0xc
        WriteU32(code, 0x334, 0x38000000u); // li r0,0
        WriteU32(code, 0x338, EncodeBranch(moduleBase + 0x338, 0x80841010u)); // b base code

        WriteU32(code, 0x33C, 0x9421FFC0u); // stwu r1,-0x40(r1)
        WriteU32(code, 0x340, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x344, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module leaf boundary scan", starts[moduleBase + 0x30C]);
    }

    [Fact]
    public void DiscoversLeafFunctionAfterExternalTailBranch()
    {
        const uint moduleBase = 0x81700000u;
        var code = new byte[0x44000];

        WriteU32(code, 0x42FF8, 0x806302BCu); // lwz r3,0x2bc(r3)
        WriteU32(code, 0x42FFC, 0x38843000u); // addi r4,r4,0x3000
        WriteU32(code, 0x43000, EncodeBranch(moduleBase + 0x43000, 0x807E9A38u)); // b base code

        WriteU32(code, 0x43004, 0x80630048u); // lwz r3,0x48(r3)
        WriteU32(code, 0x43008, 0x38800000u); // li r4,0
        WriteU32(code, 0x4300C, 0x80630000u); // lwz r3,0(r3)
        WriteU32(code, 0x43010, EncodeBranch(moduleBase + 0x43010, 0x805BDF44u)); // b base code

        WriteU32(code, 0x43014, 0x9421FFE0u); // stwu r1,-0x20(r1)
        WriteU32(code, 0x43018, 0x7C0802A6u); // mflr r0

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module leaf boundary scan", starts[moduleBase + 0x43004]);
    }

    [Fact]
    public void DiscoversVtableOnlyTailCallFunctionNextToInstructionThatEncodesItsTail()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x10000];

        // Virtual call whose lwz encodes the address of the tail branch below.
        WriteU32(code, 0x0FF0, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x0FF4, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x0FF8, 0x8061000Cu); // lwz r3,0xc(r1)
        WriteU32(code, 0x0FFC, 0x8184BB50u); // lwz r12,-0x44b0(r4)
        WriteU32(code, 0x1000, 0x7D8903A6u); // mtctr r12
        WriteU32(code, 0x1004, 0x4E800421u); // bctrl
        WriteU32(code, 0x1008, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x100C, 0x4E800020u); // blr

        WriteU32(code, 0xBAF0, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0xBAF4, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0xBAF8, 0x80010014u); // lwz r0,0x14(r1)
        WriteU32(code, 0xBAFC, 0x7C0803A6u); // mtlr r0
        WriteU32(code, 0xBB00, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0xBB04, 0x4E800020u); // blr

        // Frameless virtual reached only through the vtable; it ends in a tail call.
        WriteU32(code, 0xBB08, 0x38000008u); // li r0,8
        WriteU32(code, 0xBB0C, 0x3880FFFFu); // li r4,-1
        WriteU32(code, 0xBB10, 0x90832AB4u); // stw r4,0x2ab4(r3)
        WriteU32(code, 0xBB14, 0x38A00000u); // li r5,0
        WriteU32(code, 0xBB18, 0x80C3072Cu); // lwz r6,0x72c(r3)
        WriteU32(code, 0xBB1C, 0x7C0903A6u); // mtctr r0
        WriteU32(code, 0xBB20, 0x80860000u); // lwz r4,0(r6)
        WriteU32(code, 0xBB24, 0x38C60004u); // addi r6,r6,4
        WriteU32(code, 0xBB28, 0x98A401E9u); // stb r5,0x1e9(r4)
        WriteU32(code, 0xBB2C, 0x4200FFF4u); // bdnz -0xc
        WriteU32(code, 0xBB30, 0x3C80818Au); // lis r4,0x818a
        WriteU32(code, 0xBB34, 0x808441F8u); // lwz r4,0x41f8(r4)
        WriteU32(code, 0xBB38, 0x80040024u); // lwz r0,0x24(r4)
        WriteU32(code, 0xBB3C, 0x5400103Au); // slwi r0,r0,2
        WriteU32(code, 0xBB40, 0x7C000034u); // cntlzw r0,r0
        WriteU32(code, 0xBB44, 0x5400D97Eu); // srwi r0,r0,5
        WriteU32(code, 0xBB48, 0x980328E1u); // stb r0,0x28e1(r3)
        WriteU32(code, 0xBB4C, 0x98032699u); // stb r0,0x2699(r3)
        WriteU32(code, 0xBB50, EncodeBranch(moduleBase + 0xBB50, 0x80841110u)); // b base code

        // Hook target that follows directly.
        WriteU32(code, 0xBB54, 0x3CA0818Au); // lis r5,0x818a
        WriteU32(code, 0xBB58, 0x80A541F8u); // lwz r5,0x41f8(r5)
        WriteU32(code, 0xBB5C, 0x80050024u); // lwz r0,0x24(r5)
        WriteU32(code, 0xBB60, 0x5400103Bu); // slwi. r0,r0,2
        WriteU32(code, 0xBB64, 0x41820008u); // beq +8
        WriteU32(code, 0xBB68, 0x38800001u); // li r4,1
        WriteU32(code, 0xBB6C, EncodeBranch(moduleBase + 0xBB6C, 0x805F0DB4u)); // b base code

        WriteU32(code, 0xBB70, 0x9421FFC0u); // stwu r1,-0x40(r1)
        WriteU32(code, 0xBB74, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0xBB78, 0x4E800020u); // blr

        WriteU32(code, 0xF000, moduleBase + 0xBB70); // vtable
        WriteU32(code, 0xF004, moduleBase + 0xBB08);

        var chunk = CodeChunk(
            code,
            PointerRelocation(0xF000, 0xBB70),
            PointerRelocation(0xF004, 0xBB08),
            AbsoluteCommand(KamekCommandId.BranchLink, 0x80601234u, 0xBB54));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module leaf boundary scan", starts[moduleBase + 0xBB08]);
        Assert.Equal("BranchLink", starts[moduleBase + 0xBB54]);
        Assert.Equal("module data pointer", starts[moduleBase + 0xBB70]);
        Assert.False(starts.ContainsKey(moduleBase + 0xBB50));
    }

    [Fact]
    public void IgnoresUnrelocatedInstructionWordThatEncodesTrampolineAddress()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x10000];
        WriteU32(code, 0x100, 0x8184BB50u); // lwz r12,-0x44b0(r4)
        WriteU32(code, 0xBB4C, 0x98032699u); // stb r0,0x2699(r3)
        WriteU32(code, 0xBB50, EncodeBranch(moduleBase + 0xBB50, 0x80841110u)); // b base code

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code), moduleBase, code);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0xBB50);
    }

    [Fact]
    public void IgnoresUnrelocatedWordThatEncodesFunctionEpilogueAddress()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x400];
        WriteU32(code, 0x100, 0x81840220u); // lwz r12,0x220(r4)
        WriteU32(code, 0x200, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x204, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x208, 0x93C10008u); // stw r30,8(r1)
        WriteU32(code, 0x20C, 0x90010014u); // stw r0,0x14(r1)
        WriteU32(code, 0x210, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x214, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x218, 0x80010014u); // lwz r0,0x14(r1)
        WriteU32(code, 0x21C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x220, 0x83C10008u); // lwz r30,8(r1)
        WriteU32(code, 0x224, 0x7C0803A6u); // mtlr r0
        WriteU32(code, 0x228, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x22C, 0x4E800020u); // blr

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code), moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module prologue scan", starts[moduleBase + 0x200]);
        Assert.False(starts.ContainsKey(moduleBase + 0x220));
    }

    [Fact]
    public void DoesNotSplitFunctionAtAnyAddressEncodedByUnrelocatedInstructions()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x3000];

        WriteU32(code, 0x2000, 0x9421FFF0u); // stwu r1,-0x10(r1)
        WriteU32(code, 0x2004, 0x7C0802A6u); // mflr r0
        WriteU32(code, 0x2008, 0x90010014u); // stw r0,0x14(r1)
        for (var offset = 0x200C; offset < 0x20F0; offset += 4)
        {
            WriteU32(code, offset, 0x38630001u); // addi r3,r3,1
        }
        WriteU32(code, 0x20F0, 0x80010014u); // lwz r0,0x14(r1)
        WriteU32(code, 0x20F4, 0x7C0803A6u); // mtlr r0
        WriteU32(code, 0x20F8, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x20FC, 0x4E800020u); // blr

        // lwz r12,disp(r4) for every word of the function body.
        for (var offset = 0x2004; offset < 0x2100; offset += 4)
        {
            WriteU32(code, 0x100 + (offset - 0x2004), 0x81840000u | (uint)offset);
        }

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code), moduleBase, code);

        Assert.Contains(starts, start => start.Address == moduleBase + 0x2000);
        Assert.DoesNotContain(starts, start => start.Address > moduleBase + 0x2000 && start.Address < moduleBase + 0x2100);
    }

    [Fact]
    public void DiscoversRelocatedPointerToFunctionThatOnlyLooksLikeAnEpilogue()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x400];
        WriteU32(code, 0x100, moduleBase + 0x220);
        WriteU32(code, 0x21C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x220, 0x83C10008u); // lwz r30,8(r1)
        WriteU32(code, 0x224, 0x38210010u); // addi r1,r1,0x10
        WriteU32(code, 0x228, 0x4E800020u); // blr

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code, PointerRelocation(0x100, 0x220)), moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x220]);
    }

    [Fact]
    public void DiscoversConditionallyWrittenModulePointer()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x80];
        WriteU32(code, 0x40, moduleBase + 0x50);
        WriteU32(code, 0x4C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x50, 0x4E800020u); // blr

        var chunk = CodeChunk(code, RelativeCommand(KamekCommandId.CondWritePointer, 0x40, 0x50, 0));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x50]);
    }

    [Theory]
    [InlineData(KamekCommandId.Addr16Lo, 2u)]
    [InlineData(KamekCommandId.Addr16Ha, 2u)]
    [InlineData(KamekCommandId.Addr16Hi, 2u)]
    [InlineData(KamekCommandId.Addr16Lo, 0u)]
    [InlineData(KamekCommandId.Addr16Ha, 0u)]
    [InlineData(KamekCommandId.Addr16Hi, 0u)]
    [InlineData(KamekCommandId.Write32, 0u)]
    [InlineData(KamekCommandId.CondWrite32, 0u)]
    public void IgnoresWordWhoseOnlyRelocationIsNotAPointerWrite(KamekCommandId id, uint addressDelta)
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x400];
        WriteU32(code, 0x100, 0x81840220u); // lwz r12,0x220(r4)
        WriteU32(code, 0x21C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x220, 0x38600001u); // li r3,1
        WriteU32(code, 0x224, 0x4E800020u); // blr

        var chunk = CodeChunk(code, RelativeCommand(id, 0x100 + addressDelta, 0x220));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x220 && start.Reason == "module data pointer");
    }

    [Fact]
    public void IgnoresAbsolutePointerWriteWhenGatingModuleWords()
    {
        const uint moduleBase = 0x81840000u;
        var code = new byte[0x400];
        WriteU32(code, 0x100, 0x81840220u); // lwz r12,0x220(r4)
        WriteU32(code, 0x21C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x220, 0x38600001u); // li r3,1
        WriteU32(code, 0x224, 0x4E800020u); // blr

        // A base-game pointer patch whose address field happens to match the module offset.
        var chunk = CodeChunk(code, AbsoluteCommand(KamekCommandId.WritePointer, 0x100, 0x300));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x220);
    }

    [Fact]
    public void HonorsPointerRelocationInLastWordOfCode()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x100];
        WriteU32(code, 0xFC, moduleBase + 0x50);
        WriteU32(code, 0x50, 0x4E800020u); // blr

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code, PointerRelocation(0xFC, 0x50)), moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x50]);
    }

    [Fact]
    public void IgnoresUnrelocatedModuleAddressInLastWordOfCode()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x100];
        WriteU32(code, 0xFC, moduleBase + 0x50);
        WriteU32(code, 0x50, 0x4E800020u); // blr

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(CodeChunk(code), moduleBase, code);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x50);
    }

    [Fact]
    public void ScansFirstAppendedWordWithoutRelocations()
    {
        const uint moduleBase = 0x81200000u;
        var relocatedImage = new byte[0x200];
        WriteU32(relocatedImage, 0x50, 0x4E800020u); // blr
        WriteU32(relocatedImage, 0x100, moduleBase + 0x50); // first appended word

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x100,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(0x100 + KamekChunk.HeaderSize),
            codeBlob: relocatedImage[..0x100],
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, relocatedImage)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("module data pointer", starts[moduleBase + 0x50]);
    }

    [Fact]
    public void IgnoresUnrelocatedCodeWordPointingIntoAppendedImage()
    {
        const uint moduleBase = 0x81200000u;
        var relocatedImage = new byte[0x300];
        WriteU32(relocatedImage, 0x20, moduleBase + 0x200);
        WriteU32(relocatedImage, 0x1FC, 0x38630001u); // addi r3,r3,1
        WriteU32(relocatedImage, 0x200, 0x38600001u); // li r3,1
        WriteU32(relocatedImage, 0x204, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: 0x100,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(0x100 + KamekChunk.HeaderSize),
            codeBlob: relocatedImage[..0x100],
            commands: []);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, relocatedImage);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x200);
    }

    [Fact]
    public void StillRejectsRelocatedPointersToData()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x200];
        "control\0"u8.ToArray().CopyTo(code, 0x80);
        WriteU32(code, 0x20, moduleBase + 0x80);
        WriteU32(code, 0x24, moduleBase + 0x180); // zero word

        var chunk = CodeChunk(code, PointerRelocation(0x20, 0x80), PointerRelocation(0x24, 0x180));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code);

        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x80);
        Assert.DoesNotContain(starts, start => start.Address == moduleBase + 0x180);
    }

    [Fact]
    public void DiscoversCtorEntriesWithAndWithoutPointerRelocations()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x100];
        WriteU32(code, 0x40, moduleBase + 0x60);
        WriteU32(code, 0x44, moduleBase + 0x70);
        WriteU32(code, 0x5C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x60, 0x38600000u); // li r3,0
        WriteU32(code, 0x64, 0x4E800020u); // blr
        WriteU32(code, 0x6C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x70, 0x38600001u); // li r3,1
        WriteU32(code, 0x74, 0x4E800020u); // blr

        var chunk = new KamekChunk(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0x40,
            ctorEnd: 0x48,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: [PointerRelocation(0x40, 0x60)]);

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("ctor", starts[moduleBase + 0x60]);
        Assert.Equal("ctor", starts[moduleBase + 0x70]);
    }

    [Fact]
    public void DiscoversHookAndRelocationBranchTargets()
    {
        const uint moduleBase = 0x81200000u;
        var code = new byte[0x200];
        WriteU32(code, 0x5C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0x60, 0x38600001u); // li r3,1
        WriteU32(code, 0x64, 0x4E800020u); // blr
        WriteU32(code, 0x9C, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0xA0, 0x38600002u); // li r3,2
        WriteU32(code, 0xA4, 0x4E800020u); // blr
        WriteU32(code, 0xDC, 0x38630001u); // addi r3,r3,1
        WriteU32(code, 0xE0, 0x38600003u); // li r3,3
        WriteU32(code, 0xE4, 0x4E800020u); // blr
        WriteU32(code, 0x100, EncodeBranch(moduleBase + 0x100, 0x80001000u) | 1u); // bl base code

        var chunk = CodeChunk(
            code,
            AbsoluteCommand(KamekCommandId.Branch, 0x80001234u, 0x60),
            AbsoluteCommand(KamekCommandId.BranchLink, 0x80001238u, 0xA0),
            RelativeCommand(KamekCommandId.Rel24, 0x100, 0xE0));

        var starts = ModFunctionDiscovery.DiscoverKamekFunctions(chunk, moduleBase, code)
            .ToDictionary(start => start.Address, start => start.Reason);

        Assert.Equal("Branch", starts[moduleBase + 0x60]);
        Assert.Equal("BranchLink", starts[moduleBase + 0xA0]);
        Assert.Equal("Rel24", starts[moduleBase + 0xE0]);
    }

    private static KamekChunk CodeChunk(byte[] code, params KamekCommand[] commands) =>
        new(
            index: 0,
            fileOffset: 0,
            bssSize: 0,
            codeSize: (uint)code.Length,
            ctorStart: 0,
            ctorEnd: 0,
            chunkSize: (uint)(code.Length + KamekChunk.HeaderSize),
            codeBlob: code,
            commands: commands);

    private static KamekCommand RelativeCommand(KamekCommandId id, uint offset, params uint[] arguments) =>
        new(0, ((uint)id << 24) | offset, id, false, offset, arguments);

    private static KamekCommand AbsoluteCommand(KamekCommandId id, uint address, params uint[] arguments) =>
        new(0, ((uint)id << 24) | 0x00FFFFFEu, id, true, address, arguments);

    private static KamekCommand PointerRelocation(uint offset, uint target) =>
        RelativeCommand(KamekCommandId.Addr32, offset, target);

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);

    private static uint EncodeBranch(uint source, uint target)
    {
        var delta = unchecked((int)(target - source));
        return 0x48000000u | ((uint)delta & 0x03FFFFFCu);
    }
}
