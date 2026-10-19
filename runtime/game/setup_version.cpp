// Compiled per user rather than into the shared runtime archive.
extern const char* const kMkwSetupVersion =
#if defined(MKW_SETUP_VERSION)
    MKW_SETUP_VERSION;
#else
    "";
#endif
