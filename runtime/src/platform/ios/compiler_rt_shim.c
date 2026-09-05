// Apple's clang links libclang_rt.ios.a implicitly; upstream clang on a Linux
// host has no iOS compiler-rt, and the only builtins this program needs from
// it are the two @available() checks. They are thin wrappers over dyld, so
// provide them here rather than asking people to extract a file from Xcode.
#include <stdint.h>

typedef struct {
    uint32_t platform;
    uint32_t version;
} dyld_build_version_t;

extern int _availability_version_check(uint32_t count, dyld_build_version_t versions[]);

int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t subminor) {
    dyld_build_version_t wanted = {platform, (major << 16) | (minor << 8) | subminor};
    return _availability_version_check(1, &wanted);
}

int32_t __isOSVersionAtLeast(int32_t major, int32_t minor, int32_t subminor) {
    return __isPlatformVersionAtLeast(2 /* PLATFORM_IOS */, (uint32_t)major, (uint32_t)minor, (uint32_t)subminor);
}
