// Auto-generated GX fatal stubs
#include "hle_stubs.h"
#include "runtime_log.h"

namespace {
[[noreturn]] void HaltGX(uint32_t addr, const char* name) {
    const char* symbol = name ? name : "<unknown GX symbol>";
    RT_LOGF(RT_TAG_GX,
            "unimplemented GX entry point: %s at guest address 0x%08X.\n"
            "[gx] This graphics call has no Aurora implementation bound to it yet, so the\n"
            "[gx] runtime cannot continue without silently dropping GPU state. Bind it in\n"
            "[gx] runtime/src/hle/gx/ and remove the stub from gx_fatal_stubs.cpp.\n",
            symbol, addr);
    std::fflush(stderr);
    char message[512]{};
    std::snprintf(message, sizeof(message),
                  "%s at guest address 0x%08X has no Aurora implementation bound to it, so the "
                  "runtime stopped rather than keep rendering with missing GPU state.",
                  symbol, addr);
    ShowRuntimeFatalPopup("the game called an unimplemented graphics function", message);
    std::abort();
}
} // namespace

// Every fatal stub is the same two statements with the address and the symbol
// name substituted, so the body comes from this macro. The registration is
// deliberately still spelled out per entry so the translator's runtime-native
// index sees the literal PPC_NATIVE_OVERRIDE_VOID invocation. Hiding it inside
// this macro would leave the index unable to associate an address with the stub.
// addr is the PAL identity token. The stub keeps that spelling in its C name (the override on
// the same line names it literally); the address it reports is the built region's.
#define GX_FATAL_STUB(addr, sym) \
    extern "C" void gx_stub_##addr(CpuContext* ctx) { (void)ctx; HaltGX(MKW_GADDR(addr), sym); }

GX_FATAL_STUB(8016b49c, "__GX__DefaultTexRegionCallback_8016b49c") PPC_NATIVE_OVERRIDE_VOID(8016b49c, gx_stub_8016b49c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016b590, "__GX__DefaultTlutRegionCallback_8016b590") PPC_NATIVE_OVERRIDE_VOID(8016b590, gx_stub_8016b590, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016b5b4, "__GX__Shutdown_8016b5b4") PPC_NATIVE_OVERRIDE_VOID(8016b5b4, gx_stub_8016b5b4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016c668, "GX__CPInterruptHandler_8016c668") PPC_NATIVE_OVERRIDE_VOID(8016c668, gx_stub_8016c668, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d054, "GX__SetBreakPtCallback_8016d054") PPC_NATIVE_OVERRIDE_VOID(8016d054, gx_stub_8016d054, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d098, "GX__EnableBreakPt_8016d098") PPC_NATIVE_OVERRIDE_VOID(8016d098, gx_stub_8016d098, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d138, "GX__DisableBreakPt_8016d138") PPC_NATIVE_OVERRIDE_VOID(8016d138, gx_stub_8016d138, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d1fc, "__GX__CleanGPFifo_8016d1fc") PPC_NATIVE_OVERRIDE_VOID(8016d1fc, gx_stub_8016d1fc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d3c0, "GX__SetVtxDesc_switch_8016d3c0") PPC_NATIVE_OVERRIDE_VOID(8016d3c0, gx_stub_8016d3c0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d3c4, "GX__SetVtxDesc_caseD_0_8016d3c4") PPC_NATIVE_OVERRIDE_VOID(8016d3c4, gx_stub_8016d3c4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d3d8, "GX__SetVtxDesc_caseD_1_8016d3d8") PPC_NATIVE_OVERRIDE_VOID(8016d3d8, gx_stub_8016d3d8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d3ec, "GX__SetVtxDesc_caseD_2_8016d3ec") PPC_NATIVE_OVERRIDE_VOID(8016d3ec, gx_stub_8016d3ec, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d400, "GX__SetVtxDesc_caseD_3_8016d400") PPC_NATIVE_OVERRIDE_VOID(8016d400, gx_stub_8016d400, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d414, "GX__SetVtxDesc_caseD_4_8016d414") PPC_NATIVE_OVERRIDE_VOID(8016d414, gx_stub_8016d414, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d428, "GX__SetVtxDesc_caseD_5_8016d428") PPC_NATIVE_OVERRIDE_VOID(8016d428, gx_stub_8016d428, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d43c, "GX__SetVtxDesc_caseD_6_8016d43c") PPC_NATIVE_OVERRIDE_VOID(8016d43c, gx_stub_8016d43c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d450, "GX__SetVtxDesc_caseD_7_8016d450") PPC_NATIVE_OVERRIDE_VOID(8016d450, gx_stub_8016d450, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d464, "GX__SetVtxDesc_caseD_8_8016d464") PPC_NATIVE_OVERRIDE_VOID(8016d464, gx_stub_8016d464, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d478, "GX__SetVtxDesc_caseD_9_8016d478") PPC_NATIVE_OVERRIDE_VOID(8016d478, gx_stub_8016d478, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d48c, "GX__SetVtxDesc_caseD_a_8016d48c") PPC_NATIVE_OVERRIDE_VOID(8016d48c, gx_stub_8016d48c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d4c0, "GX__SetVtxDesc_caseD_19_8016d4c0") PPC_NATIVE_OVERRIDE_VOID(8016d4c0, gx_stub_8016d4c0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d4f4, "GX__SetVtxDesc_caseD_b_8016d4f4") PPC_NATIVE_OVERRIDE_VOID(8016d4f4, gx_stub_8016d4f4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d508, "GX__SetVtxDesc_caseD_c_8016d508") PPC_NATIVE_OVERRIDE_VOID(8016d508, gx_stub_8016d508, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d51c, "GX__SetVtxDesc_caseD_d_8016d51c") PPC_NATIVE_OVERRIDE_VOID(8016d51c, gx_stub_8016d51c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d530, "GX__SetVtxDesc_caseD_e_8016d530") PPC_NATIVE_OVERRIDE_VOID(8016d530, gx_stub_8016d530, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d544, "GX__SetVtxDesc_caseD_f_8016d544") PPC_NATIVE_OVERRIDE_VOID(8016d544, gx_stub_8016d544, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d558, "GX__SetVtxDesc_caseD_10_8016d558") PPC_NATIVE_OVERRIDE_VOID(8016d558, gx_stub_8016d558, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d56c, "GX__SetVtxDesc_caseD_11_8016d56c") PPC_NATIVE_OVERRIDE_VOID(8016d56c, gx_stub_8016d56c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d580, "GX__SetVtxDesc_caseD_12_8016d580") PPC_NATIVE_OVERRIDE_VOID(8016d580, gx_stub_8016d580, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d594, "GX__SetVtxDesc_caseD_13_8016d594") PPC_NATIVE_OVERRIDE_VOID(8016d594, gx_stub_8016d594, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d5a8, "GX__SetVtxDesc_caseD_14_8016d5a8") PPC_NATIVE_OVERRIDE_VOID(8016d5a8, gx_stub_8016d5a8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d5b8, "GX__SetVtxDesc_caseD_15_8016d5b8") PPC_NATIVE_OVERRIDE_VOID(8016d5b8, gx_stub_8016d5b8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d814, "__GX__SetVCD_8016d814") PPC_NATIVE_OVERRIDE_VOID(8016d814, gx_stub_8016d814, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016d8c4, "__GX__CalculateVLim_8016d8c4") PPC_NATIVE_OVERRIDE_VOID(8016d8c4, gx_stub_8016d8c4, (CpuContext* ctx), (ctx));
// moved to gx_vertex.cpp: GX__GetVtxDesc_8016d9f0
GX_FATAL_STUB(8016da0c, "GX__GetVtxDesc_switch_8016da0c") PPC_NATIVE_OVERRIDE_VOID(8016da0c, gx_stub_8016da0c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da10, "GX__GetVtxDesc_caseD_0_8016da10") PPC_NATIVE_OVERRIDE_VOID(8016da10, gx_stub_8016da10, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da20, "GX__GetVtxDesc_caseD_1_8016da20") PPC_NATIVE_OVERRIDE_VOID(8016da20, gx_stub_8016da20, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da30, "GX__GetVtxDesc_caseD_2_8016da30") PPC_NATIVE_OVERRIDE_VOID(8016da30, gx_stub_8016da30, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da40, "GX__GetVtxDesc_caseD_3_8016da40") PPC_NATIVE_OVERRIDE_VOID(8016da40, gx_stub_8016da40, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da50, "GX__GetVtxDesc_caseD_4_8016da50") PPC_NATIVE_OVERRIDE_VOID(8016da50, gx_stub_8016da50, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da60, "GX__GetVtxDesc_caseD_5_8016da60") PPC_NATIVE_OVERRIDE_VOID(8016da60, gx_stub_8016da60, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da70, "GX__GetVtxDesc_caseD_6_8016da70") PPC_NATIVE_OVERRIDE_VOID(8016da70, gx_stub_8016da70, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da80, "GX__GetVtxDesc_caseD_7_8016da80") PPC_NATIVE_OVERRIDE_VOID(8016da80, gx_stub_8016da80, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016da90, "GX__GetVtxDesc_caseD_8_8016da90") PPC_NATIVE_OVERRIDE_VOID(8016da90, gx_stub_8016da90, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016daa0, "GX__GetVtxDesc_caseD_9_8016daa0") PPC_NATIVE_OVERRIDE_VOID(8016daa0, gx_stub_8016daa0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dab0, "GX__GetVtxDesc_caseD_a_8016dab0") PPC_NATIVE_OVERRIDE_VOID(8016dab0, gx_stub_8016dab0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dad4, "GX__GetVtxDesc_caseD_19_8016dad4") PPC_NATIVE_OVERRIDE_VOID(8016dad4, gx_stub_8016dad4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016daf8, "GX__GetVtxDesc_caseD_b_8016daf8") PPC_NATIVE_OVERRIDE_VOID(8016daf8, gx_stub_8016daf8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db08, "GX__GetVtxDesc_caseD_c_8016db08") PPC_NATIVE_OVERRIDE_VOID(8016db08, gx_stub_8016db08, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db18, "GX__GetVtxDesc_caseD_d_8016db18") PPC_NATIVE_OVERRIDE_VOID(8016db18, gx_stub_8016db18, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db28, "GX__GetVtxDesc_caseD_e_8016db28") PPC_NATIVE_OVERRIDE_VOID(8016db28, gx_stub_8016db28, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db38, "GX__GetVtxDesc_caseD_f_8016db38") PPC_NATIVE_OVERRIDE_VOID(8016db38, gx_stub_8016db38, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db48, "GX__GetVtxDesc_caseD_10_8016db48") PPC_NATIVE_OVERRIDE_VOID(8016db48, gx_stub_8016db48, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db58, "GX__GetVtxDesc_caseD_11_8016db58") PPC_NATIVE_OVERRIDE_VOID(8016db58, gx_stub_8016db58, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db68, "GX__GetVtxDesc_caseD_12_8016db68") PPC_NATIVE_OVERRIDE_VOID(8016db68, gx_stub_8016db68, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db78, "GX__GetVtxDesc_caseD_13_8016db78") PPC_NATIVE_OVERRIDE_VOID(8016db78, gx_stub_8016db78, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db88, "GX__GetVtxDesc_caseD_14_8016db88") PPC_NATIVE_OVERRIDE_VOID(8016db88, gx_stub_8016db88, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016db98, "GX__GetVtxDesc_caseD_15_8016db98") PPC_NATIVE_OVERRIDE_VOID(8016db98, gx_stub_8016db98, (CpuContext* ctx), (ctx));
// moved to gx_vertex.cpp: GX__GetVtxDescv_8016dba4
GX_FATAL_STUB(8016dc94, "GX__SetVtxAttrFmt_switch_8016dc94") PPC_NATIVE_OVERRIDE_VOID(8016dc94, gx_stub_8016dc94, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dc98, "GX__SetVtxAttrFmt_caseD_9_8016dc98") PPC_NATIVE_OVERRIDE_VOID(8016dc98, gx_stub_8016dc98, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dcb0, "GX__SetVtxAttrFmt_caseD_19_8016dcb0") PPC_NATIVE_OVERRIDE_VOID(8016dcb0, gx_stub_8016dcb0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dcec, "GX__SetVtxAttrFmt_caseD_b_8016dcec") PPC_NATIVE_OVERRIDE_VOID(8016dcec, gx_stub_8016dcec, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd00, "GX__SetVtxAttrFmt_caseD_c_8016dd00") PPC_NATIVE_OVERRIDE_VOID(8016dd00, gx_stub_8016dd00, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd14, "GX__SetVtxAttrFmt_caseD_d_8016dd14") PPC_NATIVE_OVERRIDE_VOID(8016dd14, gx_stub_8016dd14, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd2c, "GX__SetVtxAttrFmt_caseD_e_8016dd2c") PPC_NATIVE_OVERRIDE_VOID(8016dd2c, gx_stub_8016dd2c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd44, "GX__SetVtxAttrFmt_caseD_f_8016dd44") PPC_NATIVE_OVERRIDE_VOID(8016dd44, gx_stub_8016dd44, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd5c, "GX__SetVtxAttrFmt_caseD_10_8016dd5c") PPC_NATIVE_OVERRIDE_VOID(8016dd5c, gx_stub_8016dd5c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd74, "GX__SetVtxAttrFmt_caseD_11_8016dd74") PPC_NATIVE_OVERRIDE_VOID(8016dd74, gx_stub_8016dd74, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dd94, "GX__SetVtxAttrFmt_caseD_12_8016dd94") PPC_NATIVE_OVERRIDE_VOID(8016dd94, gx_stub_8016dd94, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ddac, "GX__SetVtxAttrFmt_caseD_13_8016ddac") PPC_NATIVE_OVERRIDE_VOID(8016ddac, gx_stub_8016ddac, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ddc4, "GX__SetVtxAttrFmt_caseD_14_8016ddc4") PPC_NATIVE_OVERRIDE_VOID(8016ddc4, gx_stub_8016ddc4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ddd8, "GX__SetVtxAttrFmt_caseD_15_8016ddd8") PPC_NATIVE_OVERRIDE_VOID(8016ddd8, gx_stub_8016ddd8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016de48, "GX__SetVtxAttrFmtv_switch_8016de48") PPC_NATIVE_OVERRIDE_VOID(8016de48, gx_stub_8016de48, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016de4c, "GX__SetVtxAttrFmtv_caseD_9_8016de4c") PPC_NATIVE_OVERRIDE_VOID(8016de4c, gx_stub_8016de4c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016de64, "GX__SetVtxAttrFmtv_caseD_19_8016de64") PPC_NATIVE_OVERRIDE_VOID(8016de64, gx_stub_8016de64, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dea0, "GX__SetVtxAttrFmtv_caseD_b_8016dea0") PPC_NATIVE_OVERRIDE_VOID(8016dea0, gx_stub_8016dea0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016deb4, "GX__SetVtxAttrFmtv_caseD_c_8016deb4") PPC_NATIVE_OVERRIDE_VOID(8016deb4, gx_stub_8016deb4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dec8, "GX__SetVtxAttrFmtv_caseD_d_8016dec8") PPC_NATIVE_OVERRIDE_VOID(8016dec8, gx_stub_8016dec8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dee0, "GX__SetVtxAttrFmtv_caseD_e_8016dee0") PPC_NATIVE_OVERRIDE_VOID(8016dee0, gx_stub_8016dee0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016def8, "GX__SetVtxAttrFmtv_caseD_f_8016def8") PPC_NATIVE_OVERRIDE_VOID(8016def8, gx_stub_8016def8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df10, "GX__SetVtxAttrFmtv_caseD_10_8016df10") PPC_NATIVE_OVERRIDE_VOID(8016df10, gx_stub_8016df10, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df28, "GX__SetVtxAttrFmtv_caseD_11_8016df28") PPC_NATIVE_OVERRIDE_VOID(8016df28, gx_stub_8016df28, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df48, "GX__SetVtxAttrFmtv_caseD_12_8016df48") PPC_NATIVE_OVERRIDE_VOID(8016df48, gx_stub_8016df48, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df60, "GX__SetVtxAttrFmtv_caseD_13_8016df60") PPC_NATIVE_OVERRIDE_VOID(8016df60, gx_stub_8016df60, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df78, "GX__SetVtxAttrFmtv_caseD_14_8016df78") PPC_NATIVE_OVERRIDE_VOID(8016df78, gx_stub_8016df78, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016df8c, "GX__SetVtxAttrFmtv_caseD_15_8016df8c") PPC_NATIVE_OVERRIDE_VOID(8016df8c, gx_stub_8016df8c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016dfcc, "__GX__SetVAT_8016dfcc") PPC_NATIVE_OVERRIDE_VOID(8016dfcc, gx_stub_8016dfcc, (CpuContext* ctx), (ctx));
// moved to gx_vertex.cpp: GX__GetVtxAttrFmt_8016e04c
GX_FATAL_STUB(8016e078, "GX__GetVtxAttrFmt_switch_8016e078") PPC_NATIVE_OVERRIDE_VOID(8016e078, gx_stub_8016e078, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e07c, "GX__GetVtxAttrFmt_caseD_9_8016e07c") PPC_NATIVE_OVERRIDE_VOID(8016e07c, gx_stub_8016e07c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e0a4, "GX__GetVtxAttrFmt_caseD_19_8016e0a4") PPC_NATIVE_OVERRIDE_VOID(8016e0a4, gx_stub_8016e0a4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e118, "GX__GetVtxAttrFmt_caseD_b_8016e118") PPC_NATIVE_OVERRIDE_VOID(8016e118, gx_stub_8016e118, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e13c, "GX__GetVtxAttrFmt_caseD_c_8016e13c") PPC_NATIVE_OVERRIDE_VOID(8016e13c, gx_stub_8016e13c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e160, "GX__GetVtxAttrFmt_caseD_d_8016e160") PPC_NATIVE_OVERRIDE_VOID(8016e160, gx_stub_8016e160, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e188, "GX__GetVtxAttrFmt_caseD_e_8016e188") PPC_NATIVE_OVERRIDE_VOID(8016e188, gx_stub_8016e188, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e1b0, "GX__GetVtxAttrFmt_caseD_f_8016e1b0") PPC_NATIVE_OVERRIDE_VOID(8016e1b0, gx_stub_8016e1b0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e1d8, "GX__GetVtxAttrFmt_caseD_10_8016e1d8") PPC_NATIVE_OVERRIDE_VOID(8016e1d8, gx_stub_8016e1d8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e200, "GX__GetVtxAttrFmt_caseD_11_8016e200") PPC_NATIVE_OVERRIDE_VOID(8016e200, gx_stub_8016e200, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e228, "GX__GetVtxAttrFmt_caseD_12_8016e228") PPC_NATIVE_OVERRIDE_VOID(8016e228, gx_stub_8016e228, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e250, "GX__GetVtxAttrFmt_caseD_13_8016e250") PPC_NATIVE_OVERRIDE_VOID(8016e250, gx_stub_8016e250, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e278, "GX__GetVtxAttrFmt_caseD_14_8016e278") PPC_NATIVE_OVERRIDE_VOID(8016e278, gx_stub_8016e278, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e2a0, "GX__GetVtxAttrFmt_caseD_15_8016e2a0") PPC_NATIVE_OVERRIDE_VOID(8016e2a0, gx_stub_8016e2a0, (CpuContext* ctx), (ctx));
// moved to gx_vertex.cpp: GX__GetVtxAttrFmtv_8016e2b8
GX_FATAL_STUB(8016e3a4, "GX__SetTexCoordGen2_switch_8016e3a4") PPC_NATIVE_OVERRIDE_VOID(8016e3a4, gx_stub_8016e3a4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3a8, "GX__SetTexCoordGen2_caseD_0_8016e3a8") PPC_NATIVE_OVERRIDE_VOID(8016e3a8, gx_stub_8016e3a8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3b4, "GX__SetTexCoordGen2_caseD_1_8016e3b4") PPC_NATIVE_OVERRIDE_VOID(8016e3b4, gx_stub_8016e3b4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3c0, "GX__SetTexCoordGen2_caseD_2_8016e3c0") PPC_NATIVE_OVERRIDE_VOID(8016e3c0, gx_stub_8016e3c0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3cc, "GX__SetTexCoordGen2_caseD_3_8016e3cc") PPC_NATIVE_OVERRIDE_VOID(8016e3cc, gx_stub_8016e3cc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3d8, "GX__SetTexCoordGen2_caseD_13_8016e3d8") PPC_NATIVE_OVERRIDE_VOID(8016e3d8, gx_stub_8016e3d8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3e0, "GX__SetTexCoordGen2_caseD_14_8016e3e0") PPC_NATIVE_OVERRIDE_VOID(8016e3e0, gx_stub_8016e3e0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3e8, "GX__SetTexCoordGen2_caseD_4_8016e3e8") PPC_NATIVE_OVERRIDE_VOID(8016e3e8, gx_stub_8016e3e8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3f0, "GX__SetTexCoordGen2_caseD_5_8016e3f0") PPC_NATIVE_OVERRIDE_VOID(8016e3f0, gx_stub_8016e3f0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e3f8, "GX__SetTexCoordGen2_caseD_6_8016e3f8") PPC_NATIVE_OVERRIDE_VOID(8016e3f8, gx_stub_8016e3f8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e400, "GX__SetTexCoordGen2_caseD_7_8016e400") PPC_NATIVE_OVERRIDE_VOID(8016e400, gx_stub_8016e400, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e408, "GX__SetTexCoordGen2_caseD_8_8016e408") PPC_NATIVE_OVERRIDE_VOID(8016e408, gx_stub_8016e408, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e410, "GX__SetTexCoordGen2_caseD_9_8016e410") PPC_NATIVE_OVERRIDE_VOID(8016e410, gx_stub_8016e410, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e418, "GX__SetTexCoordGen2_caseD_a_8016e418") PPC_NATIVE_OVERRIDE_VOID(8016e418, gx_stub_8016e418, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e420, "GX__SetTexCoordGen2_caseD_b_8016e420") PPC_NATIVE_OVERRIDE_VOID(8016e420, gx_stub_8016e420, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e424, "GX__SetTexCoordGen2_caseD_10_8016e424") PPC_NATIVE_OVERRIDE_VOID(8016e424, gx_stub_8016e424, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e514, "GX__SetTexCoordGen2_switch_8016e514") PPC_NATIVE_OVERRIDE_VOID(8016e514, gx_stub_8016e514, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e518, "GX__SetTexCoordGen2_caseD_0_8016e518") PPC_NATIVE_OVERRIDE_VOID(8016e518, gx_stub_8016e518, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e528, "GX__SetTexCoordGen2_caseD_1_8016e528") PPC_NATIVE_OVERRIDE_VOID(8016e528, gx_stub_8016e528, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e538, "GX__SetTexCoordGen2_caseD_2_8016e538") PPC_NATIVE_OVERRIDE_VOID(8016e538, gx_stub_8016e538, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e548, "GX__SetTexCoordGen2_caseD_3_8016e548") PPC_NATIVE_OVERRIDE_VOID(8016e548, gx_stub_8016e548, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e558, "GX__SetTexCoordGen2_caseD_4_8016e558") PPC_NATIVE_OVERRIDE_VOID(8016e558, gx_stub_8016e558, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e568, "GX__SetTexCoordGen2_caseD_5_8016e568") PPC_NATIVE_OVERRIDE_VOID(8016e568, gx_stub_8016e568, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e578, "GX__SetTexCoordGen2_caseD_6_8016e578") PPC_NATIVE_OVERRIDE_VOID(8016e578, gx_stub_8016e578, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e588, "GX__SetTexCoordGen2_caseD_7_8016e588") PPC_NATIVE_OVERRIDE_VOID(8016e588, gx_stub_8016e588, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e6e4, "__GX__Abort_8016e6e4") PPC_NATIVE_OVERRIDE_VOID(8016e6e4, gx_stub_8016e6e4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016e848, "GX__AbortFrame_8016e848") PPC_NATIVE_OVERRIDE_VOID(8016e848, gx_stub_8016e848, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016eb94, "GX__PokeAlphaMode_8016eb94") PPC_NATIVE_OVERRIDE_VOID(8016eb94, gx_stub_8016eb94, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016eba4, "GX__PokeAlphaUpdate_8016eba4") PPC_NATIVE_OVERRIDE_VOID(8016eba4, gx_stub_8016eba4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ebbc, "GX__PokeAlphaUpdate_8016ebbc") PPC_NATIVE_OVERRIDE_VOID(8016ebbc, gx_stub_8016ebbc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ebd0, "GX__PokeBlendMode_8016ebd0") PPC_NATIVE_OVERRIDE_VOID(8016ebd0, gx_stub_8016ebd0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ec2c, "GX__PokeColorUpdate_8016ec2c") PPC_NATIVE_OVERRIDE_VOID(8016ec2c, gx_stub_8016ec2c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ec40, "GX__PokeDstAlpha_8016ec40") PPC_NATIVE_OVERRIDE_VOID(8016ec40, gx_stub_8016ec40, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ec58, "GX__PokeDither_8016ec58") PPC_NATIVE_OVERRIDE_VOID(8016ec58, gx_stub_8016ec58, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016ec6c, "GX__PokeZMode_8016ec6c") PPC_NATIVE_OVERRIDE_VOID(8016ec6c, gx_stub_8016ec6c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016f23c, "__GX__SendFlushPrim_8016f23c") PPC_NATIVE_OVERRIDE_VOID(8016f23c, gx_stub_8016f23c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8016f414, "__GX__SetGenMode_8016f414") PPC_NATIVE_OVERRIDE_VOID(8016f414, gx_stub_8016f414, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801724f8, "GX__InitFogAdjTable_801724f8") PPC_NATIVE_OVERRIDE_VOID(801724f8, gx_stub_801724f8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80172fd8, "__GX__SetProjection_80172fd8") PPC_NATIVE_OVERRIDE_VOID(80172fd8, gx_stub_80172fd8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801732e8, "__GX__SetViewport_801732e8") PPC_NATIVE_OVERRIDE_VOID(801732e8, gx_stub_801732e8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173544, "__GX__SetMatrixIndex_80173544") PPC_NATIVE_OVERRIDE_VOID(80173544, gx_stub_80173544, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801735cc, "GX__SetGPMetric_801735cc") PPC_NATIVE_OVERRIDE_VOID(801735cc, gx_stub_801735cc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801736f4, "GX__SetGPMetric_switch_801736f4") PPC_NATIVE_OVERRIDE_VOID(801736f4, gx_stub_801736f4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801736f8, "GX__SetGPMetric_caseD_0_801736f8") PPC_NATIVE_OVERRIDE_VOID(801736f8, gx_stub_801736f8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173718, "GX__SetGPMetric_caseD_1_80173718") PPC_NATIVE_OVERRIDE_VOID(80173718, gx_stub_80173718, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173738, "GX__SetGPMetric_caseD_2_80173738") PPC_NATIVE_OVERRIDE_VOID(80173738, gx_stub_80173738, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173758, "GX__SetGPMetric_caseD_3_80173758") PPC_NATIVE_OVERRIDE_VOID(80173758, gx_stub_80173758, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173778, "GX__SetGPMetric_caseD_4_80173778") PPC_NATIVE_OVERRIDE_VOID(80173778, gx_stub_80173778, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173798, "GX__SetGPMetric_caseD_5_80173798") PPC_NATIVE_OVERRIDE_VOID(80173798, gx_stub_80173798, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801737b8, "GX__SetGPMetric_caseD_6_801737b8") PPC_NATIVE_OVERRIDE_VOID(801737b8, gx_stub_801737b8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801737d8, "GX__SetGPMetric_caseD_7_801737d8") PPC_NATIVE_OVERRIDE_VOID(801737d8, gx_stub_801737d8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801737f8, "GX__SetGPMetric_caseD_8_801737f8") PPC_NATIVE_OVERRIDE_VOID(801737f8, gx_stub_801737f8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173818, "GX__SetGPMetric_caseD_9_80173818") PPC_NATIVE_OVERRIDE_VOID(80173818, gx_stub_80173818, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173838, "GX__SetGPMetric_caseD_22_80173838") PPC_NATIVE_OVERRIDE_VOID(80173838, gx_stub_80173838, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173858, "GX__SetGPMetric_caseD_a_80173858") PPC_NATIVE_OVERRIDE_VOID(80173858, gx_stub_80173858, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173878, "GX__SetGPMetric_caseD_b_80173878") PPC_NATIVE_OVERRIDE_VOID(80173878, gx_stub_80173878, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173894, "GX__SetGPMetric_caseD_c_80173894") PPC_NATIVE_OVERRIDE_VOID(80173894, gx_stub_80173894, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801738b0, "GX__SetGPMetric_caseD_d_801738b0") PPC_NATIVE_OVERRIDE_VOID(801738b0, gx_stub_801738b0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801738cc, "GX__SetGPMetric_caseD_e_801738cc") PPC_NATIVE_OVERRIDE_VOID(801738cc, gx_stub_801738cc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801738e8, "GX__SetGPMetric_caseD_f_801738e8") PPC_NATIVE_OVERRIDE_VOID(801738e8, gx_stub_801738e8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173904, "GX__SetGPMetric_caseD_10_80173904") PPC_NATIVE_OVERRIDE_VOID(80173904, gx_stub_80173904, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173920, "GX__SetGPMetric_caseD_11_80173920") PPC_NATIVE_OVERRIDE_VOID(80173920, gx_stub_80173920, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(8017393c, "GX__SetGPMetric_caseD_12_8017393c") PPC_NATIVE_OVERRIDE_VOID(8017393c, gx_stub_8017393c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173958, "GX__SetGPMetric_caseD_13_80173958") PPC_NATIVE_OVERRIDE_VOID(80173958, gx_stub_80173958, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173974, "GX__SetGPMetric_caseD_14_80173974") PPC_NATIVE_OVERRIDE_VOID(80173974, gx_stub_80173974, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173990, "GX__SetGPMetric_caseD_15_80173990") PPC_NATIVE_OVERRIDE_VOID(80173990, gx_stub_80173990, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801739ac, "GX__SetGPMetric_caseD_16_801739ac") PPC_NATIVE_OVERRIDE_VOID(801739ac, gx_stub_801739ac, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801739c8, "GX__SetGPMetric_caseD_17_801739c8") PPC_NATIVE_OVERRIDE_VOID(801739c8, gx_stub_801739c8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(801739e4, "GX__SetGPMetric_caseD_18_801739e4") PPC_NATIVE_OVERRIDE_VOID(801739e4, gx_stub_801739e4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a00, "GX__SetGPMetric_caseD_19_80173a00") PPC_NATIVE_OVERRIDE_VOID(80173a00, gx_stub_80173a00, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a1c, "GX__SetGPMetric_caseD_1a_80173a1c") PPC_NATIVE_OVERRIDE_VOID(80173a1c, gx_stub_80173a1c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a38, "GX__SetGPMetric_caseD_1b_80173a38") PPC_NATIVE_OVERRIDE_VOID(80173a38, gx_stub_80173a38, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a54, "GX__SetGPMetric_caseD_1c_80173a54") PPC_NATIVE_OVERRIDE_VOID(80173a54, gx_stub_80173a54, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a70, "GX__SetGPMetric_caseD_1d_80173a70") PPC_NATIVE_OVERRIDE_VOID(80173a70, gx_stub_80173a70, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173a8c, "GX__SetGPMetric_caseD_1e_80173a8c") PPC_NATIVE_OVERRIDE_VOID(80173a8c, gx_stub_80173a8c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173aa8, "GX__SetGPMetric_caseD_1f_80173aa8") PPC_NATIVE_OVERRIDE_VOID(80173aa8, gx_stub_80173aa8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173ac4, "GX__SetGPMetric_caseD_20_80173ac4") PPC_NATIVE_OVERRIDE_VOID(80173ac4, gx_stub_80173ac4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173ae0, "GX__SetGPMetric_caseD_21_80173ae0") PPC_NATIVE_OVERRIDE_VOID(80173ae0, gx_stub_80173ae0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173af8, "GX__SetGPMetric_caseD_23_80173af8") PPC_NATIVE_OVERRIDE_VOID(80173af8, gx_stub_80173af8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b18, "GX__SetGPMetric_switch_80173b18") PPC_NATIVE_OVERRIDE_VOID(80173b18, gx_stub_80173b18, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b1c, "GX__SetGPMetric_caseD_0_80173b1c") PPC_NATIVE_OVERRIDE_VOID(80173b1c, gx_stub_80173b1c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b38, "GX__SetGPMetric_caseD_1_80173b38") PPC_NATIVE_OVERRIDE_VOID(80173b38, gx_stub_80173b38, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b54, "GX__SetGPMetric_caseD_2_80173b54") PPC_NATIVE_OVERRIDE_VOID(80173b54, gx_stub_80173b54, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b70, "GX__SetGPMetric_caseD_3_80173b70") PPC_NATIVE_OVERRIDE_VOID(80173b70, gx_stub_80173b70, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173b8c, "GX__SetGPMetric_caseD_8_80173b8c") PPC_NATIVE_OVERRIDE_VOID(80173b8c, gx_stub_80173b8c, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173ba8, "GX__SetGPMetric_caseD_15_80173ba8") PPC_NATIVE_OVERRIDE_VOID(80173ba8, gx_stub_80173ba8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173bc4, "GX__SetGPMetric_caseD_4_80173bc4") PPC_NATIVE_OVERRIDE_VOID(80173bc4, gx_stub_80173bc4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173be0, "GX__SetGPMetric_caseD_5_80173be0") PPC_NATIVE_OVERRIDE_VOID(80173be0, gx_stub_80173be0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173bfc, "GX__SetGPMetric_caseD_6_80173bfc") PPC_NATIVE_OVERRIDE_VOID(80173bfc, gx_stub_80173bfc, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173c18, "GX__SetGPMetric_caseD_7_80173c18") PPC_NATIVE_OVERRIDE_VOID(80173c18, gx_stub_80173c18, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173c34, "GX__SetGPMetric_caseD_9_80173c34") PPC_NATIVE_OVERRIDE_VOID(80173c34, gx_stub_80173c34, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173c64, "GX__SetGPMetric_caseD_a_80173c64") PPC_NATIVE_OVERRIDE_VOID(80173c64, gx_stub_80173c64, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173c94, "GX__SetGPMetric_caseD_b_80173c94") PPC_NATIVE_OVERRIDE_VOID(80173c94, gx_stub_80173c94, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173cc4, "GX__SetGPMetric_caseD_c_80173cc4") PPC_NATIVE_OVERRIDE_VOID(80173cc4, gx_stub_80173cc4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173cf4, "GX__SetGPMetric_caseD_d_80173cf4") PPC_NATIVE_OVERRIDE_VOID(80173cf4, gx_stub_80173cf4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173d24, "GX__SetGPMetric_caseD_e_80173d24") PPC_NATIVE_OVERRIDE_VOID(80173d24, gx_stub_80173d24, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173d54, "GX__SetGPMetric_caseD_f_80173d54") PPC_NATIVE_OVERRIDE_VOID(80173d54, gx_stub_80173d54, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173d84, "GX__SetGPMetric_caseD_10_80173d84") PPC_NATIVE_OVERRIDE_VOID(80173d84, gx_stub_80173d84, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173db0, "GX__SetGPMetric_caseD_11_80173db0") PPC_NATIVE_OVERRIDE_VOID(80173db0, gx_stub_80173db0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173dc0, "GX__SetGPMetric_caseD_12_80173dc0") PPC_NATIVE_OVERRIDE_VOID(80173dc0, gx_stub_80173dc0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173dd0, "GX__SetGPMetric_caseD_13_80173dd0") PPC_NATIVE_OVERRIDE_VOID(80173dd0, gx_stub_80173dd0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173de0, "GX__SetGPMetric_caseD_14_80173de0") PPC_NATIVE_OVERRIDE_VOID(80173de0, gx_stub_80173de0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173dec, "GX__SetGPMetric_caseD_16_80173dec") PPC_NATIVE_OVERRIDE_VOID(80173dec, gx_stub_80173dec, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(80173df8, "GX__ClearGPMetric_80173df8") PPC_NATIVE_OVERRIDE_VOID(80173df8, gx_stub_80173df8, (CpuContext* ctx), (ctx));
