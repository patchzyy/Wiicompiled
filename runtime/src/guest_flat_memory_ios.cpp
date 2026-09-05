#include "guest_flat_memory.h"

// iOS variant of the macOS backend. <mach/mach_vm.h> is not in the iOS SDK, so
// the vm_* calls from <mach/mach.h> are used instead, and the backing store is
// an anonymous mapping rather than a file in /tmp, which the sandbox denies.

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace GuestFlat {

uint8_t* g_flatGuestBase = nullptr;

bool g_requiresCheckedAccess = false;

namespace {
struct Mapping { uint32_t base; uint64_t size; uint8_t* host; };
std::mutex g_mutex;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
uint8_t* g_base = nullptr;
bool g_active = false;

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}
bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}
} // namespace

bool IsActive() { return g_active; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    g_requiresCheckedAccess = static_cast<size_t>(getpagesize()) > kGuestPageSize;
    if (g_active) {
        if (!Same(g_layout, regions)) throw std::runtime_error("flat guest layout cannot be remapped");
        return;
    }

    // The free window differs per device, so let the kernel place it.
    vm_address_t address = 0;
    if (vm_allocate(mach_task_self(), &address, static_cast<vm_size_t>(kGuestSpaceSize),
                    VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        throw std::runtime_error("unable to reserve a 4 GiB iOS guest address space; a device with\n"
                                 "less than 4 GB of RAM may need the extended-virtual-addressing entitlement");
    }
    g_base = reinterpret_cast<uint8_t*>(address);

    struct Store { Backing kind; uint32_t owned; uint64_t size; uint8_t* mem; };
    std::vector<Store> stores;

    // Initialize() is called inside a try that reports a dialog rather than
    // aborting, so a throw here has to give the reservation back.
    struct Rollback {
        vm_address_t reservation;
        std::vector<Store>* stores;
        bool armed = true;
        ~Rollback() {
            if (!armed) return;
            if (stores != nullptr) {
                for (const auto& s : *stores) {
                    if (s.mem != nullptr) munmap(s.mem, static_cast<size_t>(s.size));
                }
            }
            if (reservation != 0) {
                vm_deallocate(mach_task_self(), reservation,
                              static_cast<vm_size_t>(kGuestSpaceSize));
            }
        }
    } rollback{address, &stores};
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(),
            [&](const Store& s) { return s.kind == r.backing && s.owned == owned; });
        const uint64_t need = Offset(r) + r.size;
        if (it == stores.end()) stores.push_back({r.backing, owned, need, nullptr});
        else it->size = std::max(it->size, need);
    }

    for (auto& s : stores) {
        void* mem = mmap(nullptr, static_cast<size_t>(s.size), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) throw std::runtime_error("unable to create iOS guest backing store");
        s.mem = static_cast<uint8_t*>(mem);
    }

    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(),
            [&](const Store& x) { return x.kind == r.backing && x.owned == owned; });
        uint8_t* host = s.mem + Offset(r);

        // Alias the same physical pages into the reservation. VM_FLAGS_OVERWRITE
        // replaces the placeholder in place; copy=false is what makes this a
        // shared alias rather than a copy-on-write duplicate.
        vm_address_t target = reinterpret_cast<vm_address_t>(g_base + r.base);
        vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
        const kern_return_t kr = vm_remap(
            mach_task_self(), &target, static_cast<vm_size_t>(r.size), 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
            reinterpret_cast<vm_address_t>(host), FALSE, &cur, &max, VM_INHERIT_SHARE);
        if (kr != KERN_SUCCESS) {
            throw std::runtime_error(std::string("vm_remap failed for the iOS guest alias: ") +
                                     mach_error_string(kr));
        }
        if (target != reinterpret_cast<vm_address_t>(g_base + r.base)) {
            throw std::runtime_error("vm_remap placed the iOS guest alias at the wrong address");
        }
        if (mprotect(g_base + r.base, static_cast<size_t>(r.size), PROT_READ | PROT_WRITE) != 0) {
            throw std::runtime_error(std::string("mprotect failed on the iOS guest alias: ") +
                                     std::strerror(errno));
        }
        g_mappings.push_back({r.base, r.size, host});
    }

    g_layout = regions;
    // Publish the base only now. Until the aliases are mapped the reservation is
    // an unbacked placeholder, and the header advertises this pointer as the base
    // for every translated access - so a caller that trusted it early would fault.
    rollback.armed = false;
    g_flatGuestBase = g_base;
    g_active = true;
}

uint8_t* HostPointer(uint32_t a) {
    for (const auto& m : g_mappings)
        if (a >= m.base && uint64_t(a - m.base) < m.size) return m.host + (a - m.base);
    return nullptr;
}
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }
} // namespace GuestFlat
