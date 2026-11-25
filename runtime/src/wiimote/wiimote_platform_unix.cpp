#if !defined(_WIN32)

#include "wiimote_platform.h"

#include "runtime_log.h"

#include <SDL3/SDL_hidapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>

#include <filesystem>
#endif

namespace Wiimote::Platform {
namespace Detail {

std::string Narrow(const wchar_t* text) {
    std::string out;
    if (text != nullptr) {
        for (const wchar_t* c = text; *c != L'\0'; ++c) out += static_cast<char>(*c < 128 ? *c : '?');
    }
    return out;
}

std::string HexDigits(const std::string& text) {
    std::string hex;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return hex;
}

std::mutex g_noticeMutex;
std::string g_notice;

void SetNotice(const std::string& notice) {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_notice = notice;
}

#if defined(__linux__)

// BlueZ over the system bus. libdbus-1 is loaded at run time so a system
// without it still runs (pairing then falls back to the desktop's tools).
extern "C" {
struct BzConnection;
struct BzMessage;
struct BzError {
    const char* name;
    const char* message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void* padding1;
};
// Mirrors DBusMessageIter.
struct BzIter {
    void* dummy1;
    void* dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    int pad2;
    void* pad3;
};
}

constexpr int kBusSystem = 1;
constexpr int kTypeInvalid = 0;
constexpr int kTypeString = 's';
constexpr int kTypeObjectPath = 'o';
constexpr int kTypeBoolean = 'b';
constexpr int kTypeInt16 = 'n';
constexpr int kTypeArray = 'a';
constexpr int kTypeVariant = 'v';
constexpr int kTypeDictEntry = 'e';
constexpr int kShortTimeoutMs = 2000;
constexpr int kPairTimeoutMs = 30000;
constexpr auto kInquiryTime = std::chrono::milliseconds(3840);

struct Device {
    std::string path;
    std::string address;
    std::string name;
    bool paired = false;
    bool connected = false;
    bool trusted = false;
    bool discovered = false; // has an RSSI: answered the current inquiry
};

class BlueZ {
public:
    bool Load() {
        if (m_loaded) return m_conn != nullptr;
        m_loaded = true;
        m_handle = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (m_handle == nullptr) return false;
        const bool ok = Bind(bus_get_private, "dbus_bus_get_private") &&
                        Bind(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
                        Bind(error_init, "dbus_error_init") && Bind(error_free, "dbus_error_free") &&
                        Bind(message_new_method_call, "dbus_message_new_method_call") &&
                        Bind(message_append_args, "dbus_message_append_args") &&
                        Bind(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
                        Bind(message_unref, "dbus_message_unref") &&
                        Bind(message_iter_init, "dbus_message_iter_init") &&
                        Bind(message_iter_init_append, "dbus_message_iter_init_append") &&
                        Bind(message_iter_open_container, "dbus_message_iter_open_container") &&
                        Bind(message_iter_close_container, "dbus_message_iter_close_container") &&
                        Bind(message_iter_append_basic, "dbus_message_iter_append_basic") &&
                        Bind(message_iter_next, "dbus_message_iter_next") &&
                        Bind(message_iter_recurse, "dbus_message_iter_recurse") &&
                        Bind(message_iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
                        Bind(message_iter_get_basic, "dbus_message_iter_get_basic");
        if (!ok) return false;
        BzError error;
        error_init(&error);
        m_conn = bus_get_private(kBusSystem, &error);
        error_free(&error);
        if (m_conn != nullptr) connection_set_exit_on_disconnect(m_conn, 0);
        return m_conn != nullptr;
    }

    // org.freedesktop.DBus.ObjectManager.GetManagedObjects on org.bluez.
    bool Objects(std::string& adapter, std::vector<Device>& devices) {
        BzMessage* reply = Call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", kShortTimeoutMs);
        if (reply == nullptr) return false;
        BzIter root;
        BzIter objects;
        if (message_iter_init(reply, &root) && message_iter_get_arg_type(&root) == kTypeArray) {
            message_iter_recurse(&root, &objects);
            while (message_iter_get_arg_type(&objects) == kTypeDictEntry) {
                BzIter entry;
                message_iter_recurse(&objects, &entry);
                const char* path = nullptr;
                message_iter_get_basic(&entry, &path);
                message_iter_next(&entry);
                ReadObject(path != nullptr ? path : "", entry, adapter, devices);
                message_iter_next(&objects);
            }
        }
        message_unref(reply);
        return true;
    }

    bool Simple(const std::string& path, const char* interface, const char* method, int timeoutMs,
                std::string* error = nullptr) {
        BzMessage* reply = Call(path.c_str(), interface, method, timeoutMs, nullptr, error);
        if (reply == nullptr) return false;
        message_unref(reply);
        return true;
    }

    bool RemoveDevice(const std::string& adapter, const std::string& device) {
        const char* path = device.c_str();
        BzMessage* reply = Call(adapter.c_str(), "org.bluez.Adapter1", "RemoveDevice", kShortTimeoutMs,
                                [&](BzMessage* message) {
                                    return message_append_args(message, kTypeObjectPath, &path, kTypeInvalid) != 0;
                                });
        if (reply == nullptr) return false;
        message_unref(reply);
        return true;
    }

    bool SetTrusted(const std::string& device) {
        BzMessage* reply = Call(device.c_str(), "org.freedesktop.DBus.Properties", "Set", kShortTimeoutMs,
                                [&](BzMessage* message) {
                                    const char* interface = "org.bluez.Device1";
                                    const char* property = "Trusted";
                                    if (!message_append_args(message, kTypeString, &interface, kTypeString, &property,
                                                             kTypeInvalid)) {
                                        return false;
                                    }
                                    BzIter args;
                                    BzIter variant;
                                    message_iter_init_append(message, &args);
                                    if (!message_iter_open_container(&args, kTypeVariant, "b", &variant)) return false;
                                    const uint32_t value = 1;
                                    message_iter_append_basic(&variant, kTypeBoolean, &value);
                                    return message_iter_close_container(&args, &variant) != 0;
                                });
        if (reply == nullptr) return false;
        message_unref(reply);
        return true;
    }

private:
    template <typename Fn>
    bool Bind(Fn& fn, const char* symbol) {
        fn = reinterpret_cast<Fn>(dlsym(m_handle, symbol));
        return fn != nullptr;
    }

    template <typename Append = std::nullptr_t>
    BzMessage* Call(const char* path, const char* interface, const char* method, int timeoutMs,
                    Append append = nullptr, std::string* errorText = nullptr) {
        BzMessage* message = message_new_method_call("org.bluez", path, interface, method);
        if (message == nullptr) return nullptr;
        if constexpr (!std::is_same_v<Append, std::nullptr_t>) {
            if (!append(message)) {
                message_unref(message);
                return nullptr;
            }
        }
        BzError error;
        error_init(&error);
        BzMessage* reply = send_with_reply_and_block(m_conn, message, timeoutMs, &error);
        message_unref(message);
        if (reply == nullptr && errorText != nullptr && error.name != nullptr) {
            *errorText = error.name;
        }
        error_free(&error);
        return reply;
    }

    // One object's a{sa{sv}} interface map.
    void ReadObject(const std::string& path, BzIter& interfaces, std::string& adapter, std::vector<Device>& devices) {
        if (message_iter_get_arg_type(&interfaces) != kTypeArray) return;
        BzIter list;
        message_iter_recurse(&interfaces, &list);
        while (message_iter_get_arg_type(&list) == kTypeDictEntry) {
            BzIter entry;
            message_iter_recurse(&list, &entry);
            const char* interface = nullptr;
            message_iter_get_basic(&entry, &interface);
            message_iter_next(&entry);
            const std::string name = interface != nullptr ? interface : "";
            if (name == "org.bluez.Adapter1" && adapter.empty()) {
                adapter = path;
            } else if (name == "org.bluez.Device1") {
                Device device;
                device.path = path;
                ReadDevice(entry, device);
                devices.push_back(device);
            }
            message_iter_next(&list);
        }
    }

    void ReadDevice(BzIter& properties, Device& device) {
        if (message_iter_get_arg_type(&properties) != kTypeArray) return;
        BzIter list;
        message_iter_recurse(&properties, &list);
        while (message_iter_get_arg_type(&list) == kTypeDictEntry) {
            BzIter entry;
            message_iter_recurse(&list, &entry);
            const char* key = nullptr;
            message_iter_get_basic(&entry, &key);
            message_iter_next(&entry);
            BzIter value;
            message_iter_recurse(&entry, &value);
            const std::string name = key != nullptr ? key : "";
            const int type = message_iter_get_arg_type(&value);
            if (type == kTypeString && (name == "Address" || name == "Name")) {
                const char* text = nullptr;
                message_iter_get_basic(&value, &text);
                (name == "Address" ? device.address : device.name) = text != nullptr ? text : "";
            } else if (type == kTypeBoolean) {
                uint32_t flag = 0;
                message_iter_get_basic(&value, &flag);
                if (name == "Paired") device.paired = flag != 0;
                if (name == "Connected") device.connected = flag != 0;
                if (name == "Trusted") device.trusted = flag != 0;
            } else if (type == kTypeInt16 && name == "RSSI") {
                device.discovered = true;
            }
            message_iter_next(&list);
        }
    }

    bool m_loaded = false;
    void* m_handle = nullptr;
    BzConnection* m_conn = nullptr;
    BzConnection* (*bus_get_private)(int, BzError*) = nullptr;
    void (*connection_set_exit_on_disconnect)(BzConnection*, unsigned int) = nullptr;
    void (*error_init)(BzError*) = nullptr;
    void (*error_free)(BzError*) = nullptr;
    BzMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    unsigned int (*message_append_args)(BzMessage*, int, ...) = nullptr;
    BzMessage* (*send_with_reply_and_block)(BzConnection*, BzMessage*, int, BzError*) = nullptr;
    void (*message_unref)(BzMessage*) = nullptr;
    unsigned int (*message_iter_init)(BzMessage*, BzIter*) = nullptr;
    void (*message_iter_init_append)(BzMessage*, BzIter*) = nullptr;
    unsigned int (*message_iter_open_container)(BzIter*, int, const char*, BzIter*) = nullptr;
    unsigned int (*message_iter_close_container)(BzIter*, BzIter*) = nullptr;
    unsigned int (*message_iter_append_basic)(BzIter*, int, const void*) = nullptr;
    unsigned int (*message_iter_next)(BzIter*) = nullptr;
    void (*message_iter_recurse)(BzIter*, BzIter*) = nullptr;
    int (*message_iter_get_arg_type)(BzIter*) = nullptr;
    void (*message_iter_get_basic)(BzIter*, void*) = nullptr;
};

// Every call happens on the discovery thread or the power-off worker.
std::timed_mutex g_busMutex;

BlueZ* Bus() {
    static BlueZ bus;
    static const bool ok = bus.Load();
    return ok ? &bus : nullptr;
}

// A remote that was just paired needs a few seconds to connect; until then it
// must not be taken for a stale pairing.
std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> g_recent;

bool RecentlyPaired(const std::string& path) {
    const auto now = std::chrono::steady_clock::now();
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [&](const auto& entry) { return now - entry.second > std::chrono::seconds(30); }),
                   g_recent.end());
    return std::any_of(g_recent.begin(), g_recent.end(), [&](const auto& entry) { return entry.first == path; });
}

bool IsRemoteName(const std::string& name) {
    return name.rfind("Nintendo RVL-CNT-01", 0) == 0 && name.find("-UC") == std::string::npos;
}

std::string DriverOf(const std::string& hidrawPath) {
    const std::filesystem::path node(hidrawPath);
    std::error_code error;
    const auto driver =
        std::filesystem::read_symlink(std::filesystem::path("/sys/class/hidraw") / node.filename() / "device" / "driver",
                                      error);
    return error ? std::string() : driver.filename().string();
}

#endif

} // namespace Detail

using namespace Detail;

Capabilities Query() {
#if defined(__linux__)
    // Cached: adapters rarely change and this is asked often.
    static std::mutex mutex;
    static Capabilities cached;
    static std::chrono::steady_clock::time_point checked{};
    std::lock_guard<std::mutex> cacheLock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (checked == std::chrono::steady_clock::time_point{} || now - checked > std::chrono::seconds(10)) {
        // A search holds the bus for seconds; keep the cached answer rather than wait.
        std::unique_lock<std::timed_mutex> lock(g_busMutex, std::try_to_lock);
        if (!lock.owns_lock()) return cached;
        checked = now;
        std::string adapter;
        std::vector<Device> devices;
        BlueZ* bus = Bus();
        cached.pairing = cached.powerOff = bus != nullptr && bus->Objects(adapter, devices) && !adapter.empty();
    }
    return cached;
#else
    return {};
#endif
}

int DiscoverAndPair(const std::atomic<bool>& cancel, std::string& status) {
#if defined(__linux__)
    std::lock_guard<std::timed_mutex> lock(g_busMutex);
    BlueZ* bus = Bus();
    std::string adapter;
    std::vector<Device> devices;
    if (bus == nullptr || !bus->Objects(adapter, devices) || adapter.empty()) {
        status = "No Bluetooth adapter found.";
        return 0;
    }
    bus->Simple(adapter, "org.bluez.Adapter1", "StartDiscovery", kShortTimeoutMs);
    int connected = 0;
    const auto deadline = std::chrono::steady_clock::now() + kInquiryTime;
    std::vector<std::string> handled;
    while (!cancel && std::chrono::steady_clock::now() < deadline) {
        devices.clear();
        bus->Objects(adapter, devices);
        for (const Device& device : devices) {
            if (!IsRemoteName(device.name) || device.connected || !device.discovered) continue;
            if (std::find(handled.begin(), handled.end(), device.path) != handled.end() || RecentlyPaired(device.path)) {
                continue;
            }
            if (device.paired) {
                // In pairing mode while paired: the remote forgot this PC.
                bus->RemoveDevice(adapter, device.path);
                continue;
            }
            handled.push_back(device.path);
            // BlueZ's wiimote plugin answers the PIN (SYNC or 1+2).
            std::string error;
            if (bus->Simple(device.path, "org.bluez.Device1", "Pair", kPairTimeoutMs, &error)) {
                bus->SetTrusted(device.path);
                g_recent.emplace_back(device.path, std::chrono::steady_clock::now());
                if (bus->Simple(device.path, "org.bluez.Device1", "Connect", kPairTimeoutMs, &error)) {
                    ++connected;
                    status = "Wii Remote paired. It reconnects with any button from now on.";
                    RT_LOG(RT_TAG_HLE) << "Wii Remote " << device.address << " paired" << std::endl;
                    continue;
                }
            }
            status = "Could not pair the Wii Remote. Press its red SYNC button and try again.";
            RT_LOG(RT_TAG_HLE) << "Wii Remote " << device.address << ": pairing failed (" << error << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    bus->Simple(adapter, "org.bluez.Adapter1", "StopDiscovery", kShortTimeoutMs);
    return connected;
#else
    (void)cancel;
    status = "Pair the Wii Remote in the system's Bluetooth settings.";
    return 0;
#endif
}

bool PowerOff(const std::string& address) {
#if defined(__linux__)
    std::unique_lock<std::timed_mutex> lock(g_busMutex, std::chrono::milliseconds(kShortTimeoutMs));
    if (!lock.owns_lock()) return false;
    BlueZ* bus = Bus();
    std::string adapter;
    std::vector<Device> devices;
    if (bus == nullptr || !bus->Objects(adapter, devices)) return false;
    for (const Device& device : devices) {
        if (HexDigits(device.address) == address) {
            return bus->Simple(device.path, "org.bluez.Device1", "Disconnect", kShortTimeoutMs);
        }
    }
#else
    (void)address;
#endif
    return false;
}

std::string AddressFor(const SDL_hid_device_info& info) {
    const std::string hex = HexDigits(Narrow(info.serial_number));
    return hex.size() == 12 && hex != "000000000000" ? hex : std::string();
}

std::string NameFor(const std::string& address, const SDL_hid_device_info& info) {
    (void)address;
    return Narrow(info.product_string);
}

bool MaybeConnected(const std::string& address) {
    (void)address;
    return true;
}

bool Usable(const SDL_hid_device_info& info) {
#if defined(__linux__)
    if (info.path == nullptr) return false;
    if (DriverOf(info.path) == "wiimote") {
        SetNotice("The Linux hid-wiimote driver holds the Wii Remote, so the game cannot use it. Run "
                  "\"sudo modprobe -r hid_wiimote\" and add \"blacklist hid_wiimote\" to /etc/modprobe.d/wiimote.conf.");
        return false;
    }
    if (access(info.path, R_OK | W_OK) != 0) {
        SetNotice("No permission to open the Wii Remote. Add the udev rule "
                  "KERNEL==\"hidraw*\", KERNELS==\"0005:057E:*\", MODE=\"0666\" and reconnect it.");
        return false;
    }
    SetNotice({});
#else
    (void)info;
#endif
    return true;
}

std::string Notice() {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
#if defined(__APPLE__)
    if (g_notice.empty()) return "Pair Wii Remotes in System Settings > Bluetooth, then press a button on them.";
#endif
    return g_notice;
}

} // namespace Wiimote::Platform

#endif
