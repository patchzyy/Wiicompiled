#include "steam_input_layout.h"

#include "runtime_log.h"

#include <aurora/aurora.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <optional>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#else
#include <dirent.h>
#include <fstream>
#include <sstream>
#endif

extern char** environ;
#endif

namespace SteamInputLayout {
namespace {

using Clock = std::chrono::steady_clock;

enum class Handoff { None, Unconfirmed, Confirmed };

constexpr std::chrono::milliseconds kHandoffTimeout{2000};
#if defined(__linux__)
// xdg-open can return before the Steam forwarder it started has delivered the URL.
constexpr std::chrono::milliseconds kDeliverySpacing{1500};
#else
constexpr std::chrono::milliseconds kDeliverySpacing{0};
#endif

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    // Serializes requests to Steam so they arrive in order.
    std::timed_mutex delivery;
    bool enabled = false;
    bool stopping = false;
    bool desired = false;
    bool sent = false;
    bool maybeForced = false;
    uint32_t appId = 0;
    SDL_WindowID windowId = 0;
    std::atomic<Clock::rep> lastDelivery{0};
    std::atomic<bool> released{false};
};

// Never destroyed: the detached worker and late exit paths may still touch it.
std::atomic<State*> g_state{nullptr};
thread_local bool t_isWorker = false;
thread_local bool t_inRelease = false;

// The worker launches and publishes its request helper under this lock, so an exit that
// can't wait for the worker can cancel it: either the helper is published, or it never starts.
std::mutex g_helperMutex;
std::atomic<bool> g_helperCancelled{false};
#if defined(_WIN32)
HANDLE g_helper = nullptr;
#else
pid_t g_helper = 0;
#endif

// These locks are only held briefly; failing to get one means a crash inside it.
bool TryLockBriefly(std::unique_lock<std::mutex>& lock,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
    const auto deadline = Clock::now() + timeout;
    while (!lock.try_lock()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Steam applies the launching app's own layout to everything it starts, children included.
bool LaunchedBySteam() {
    for (const char* name : {"SteamGameId", "SteamAppId", "SteamClientLaunch"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return true;
        }
    }
#if defined(_WIN32)
    if (GetModuleHandleW(L"GameOverlayRenderer64.dll") != nullptr) {
        return true;
    }
#endif
    return false;
}

#if defined(_WIN32)

std::vector<BYTE> TokenUserOf(HANDLE process) {
    std::vector<BYTE> user;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return user;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    user.resize(size);
    if (size == 0 || !GetTokenInformation(token, TokenUser, user.data(), size, &size)) {
        user.clear();
    }
    CloseHandle(token);
    return user;
}

// A stale pid can be reused by another signed-in user's Steam.
bool OwnedByCurrentUser(HANDLE process) {
    const auto theirs = TokenUserOf(process);
    const auto ours = TokenUserOf(GetCurrentProcess());
    return !theirs.empty() && !ours.empty() &&
           EqualSid(reinterpret_cast<const TOKEN_USER*>(theirs.data())->User.Sid,
                    reinterpret_cast<const TOKEN_USER*>(ours.data())->User.Sid);
}

// ActiveProcess\pid outlives a Steam crash, so the process behind it is verified too.
std::optional<std::wstring> RunningSteamExecutable() {
    DWORD pid = 0;
    DWORD size = sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
                     RRF_RT_REG_DWORD, nullptr, &pid, &size) != ERROR_SUCCESS ||
        pid == 0) {
        return std::nullopt;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return std::nullopt;
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    DWORD exitCode = 0;
    const bool alive = QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
                       GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE &&
                       OwnedByCurrentUser(process);
    CloseHandle(process);
    if (!alive) {
        return std::nullopt;
    }
    path.resize(length);
    if (_wcsicmp(std::filesystem::path(path).filename().c_str(), L"steam.exe") != 0) {
        return std::nullopt;
    }
    return path;
}

// Same command line as Steam's registered steam:// handler, sent to the verified client.
Handoff OpenSteamUrl(const std::string& url, bool bounded) {
    const auto steam = RunningSteamExecutable();
    if (!steam) {
        return Handoff::None;
    }
    std::wstring commandLine = L"\"" + *steam + L"\" -- \"" + std::wstring(url.begin(), url.end()) + L"\"";
    const std::wstring directory = std::filesystem::path(*steam).parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::unique_lock launch(g_helperMutex, std::defer_lock);
    if (!bounded) {
        launch.lock();
        if (g_helperCancelled) {
            return Handoff::None;
        }
    }
    // The worker's helper starts suspended so a cancellation that couldn't take the lock while
    // CreateProcessW ran still stops it before it can reach Steam.
    if (!CreateProcessW(steam->c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        bounded ? 0 : CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &info)) {
        return Handoff::None;
    }
    if (!bounded) {
        g_helper = info.hProcess;
        launch.unlock();
        if (g_helperCancelled) {
            TerminateProcess(info.hProcess, 1);
        } else {
            ResumeThread(info.hThread);
        }
    }
    CloseHandle(info.hThread);
    const DWORD waited = WaitForSingleObject(info.hProcess, bounded ? static_cast<DWORD>(kHandoffTimeout.count()) : INFINITE);
    if (!bounded) {
        std::lock_guard lock(g_helperMutex);
        g_helper = nullptr;
    }
    DWORD exitCode = 1;
    const bool succeeded = waited == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &exitCode) && exitCode == 0;
    CloseHandle(info.hProcess);
    return succeeded ? Handoff::Confirmed : Handoff::Unconfirmed;
}

void CancelWorkerRequests() {
    g_helperCancelled = true;
    std::unique_lock lock(g_helperMutex, std::defer_lock);
    if (!TryLockBriefly(lock, kHandoffTimeout)) {
        return;
    }
    if (g_helper != nullptr && TerminateProcess(g_helper, 1)) {
        WaitForSingleObject(g_helper, static_cast<DWORD>(kHandoffTimeout.count()));
    }
}

#else

// Only a live client of this user; a URL handler with no client running would start Steam.
bool SteamRunning() {
    const uid_t uid = getuid();
#if defined(__APPLE__)
    const int count = proc_listallpids(nullptr, 0);
    if (count <= 0) {
        return false;
    }
    std::vector<pid_t> pids(static_cast<size_t>(count) + 32);
    const int filled = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < filled; ++i) {
        char name[2 * MAXCOMLEN + 1] = {};
        if (proc_name(pids[i], name, sizeof(name)) <= 0 || std::string(name) != "steam_osx") {
            continue;
        }
        proc_bsdinfo info{};
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
            info.pbi_uid == uid && info.pbi_status != SZOMB) {
            return true;
        }
    }
    return false;
#else
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return false;
    }
    bool found = false;
    while (const dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') {
            continue;
        }
        std::ifstream status(std::string("/proc/") + entry->d_name + "/status");
        std::string line;
        bool named = false;
        bool live = false;
        bool owned = false;
        while (std::getline(status, line)) {
            if (line.rfind("Name:", 0) == 0) {
                named = line == "Name:\tsteam";
            } else if (line.rfind("State:", 0) == 0) {
                live = line.find_first_of("ZX") == std::string::npos;
            } else if (line.rfind("Uid:", 0) == 0) {
                std::istringstream fields(line.substr(4));
                uid_t real = 0;
                owned = static_cast<bool>(fields >> real) && real == uid;
                break;
            }
        }
        if (named && live && owned) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
#endif
}

Handoff OpenSteamUrl(const std::string& url, bool bounded) {
    if (!SteamRunning()) {
        return Handoff::None;
    }
    // The worker's helper waits for a go-ahead on fd 3 before it runs the URL handler, so a
    // cancellation that couldn't take the lock while posix_spawn ran still stops it in time.
    static char gateScript[] = "read -r go <&3 || exit 1; exec 3<&-; exec \"$0\" \"$@\"";
#if defined(__APPLE__)
    const char* program = "/usr/bin/open";
    char* argv[] = {const_cast<char*>("open"), const_cast<char*>("-g"), const_cast<char*>(url.c_str()), nullptr};
    char* gated[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), gateScript, const_cast<char*>(program),
                     const_cast<char*>("-g"), const_cast<char*>(url.c_str()), nullptr};
    constexpr int kNoSignal = 0;
#else
    const char* program = "xdg-open";
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(url.c_str()), nullptr};
    char* gated[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), gateScript, const_cast<char*>(program),
                     const_cast<char*>(url.c_str()), nullptr};
    constexpr int kNoSignal = MSG_NOSIGNAL;
#endif
    int gate[2] = {-1, -1};
    if (!bounded) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, gate) != 0) {
            return Handoff::None;
        }
        // dup2(3, 3) keeps FD_CLOEXEC on macOS, which would close the gate in the child.
        if (gate[0] == 3) {
            std::swap(gate[0], gate[1]);
        }
        fcntl(gate[0], F_SETFD, FD_CLOEXEC);
        fcntl(gate[1], F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
        int noSigPipe = 1;
        setsockopt(gate[1], SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
    }
    const auto closeGate = [&gate] {
        for (int& end : gate) {
            if (end != -1) {
                close(end);
                end = -1;
            }
        }
    };
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        closeGate();
        return Handoff::None;
    }
    if (!bounded) {
        posix_spawn_file_actions_adddup2(&actions, gate[0], 3);
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // A crash handler can call this with signals blocked; the child should not inherit that.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    pid_t child = 0;
    std::unique_lock launch(g_helperMutex, std::defer_lock);
    if (!bounded) {
        launch.lock();
    }
    int error = ECANCELED;
    if (bounded) {
        error = posix_spawnp(&child, program, &actions, &attributes, argv, environ);
    } else if (!g_helperCancelled) {
        error = posix_spawn(&child, "/bin/sh", &actions, &attributes, gated, environ);
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        closeGate();
        return Handoff::None;
    }
    int status = 0;
    if (!bounded) {
        g_helper = child;
        if (!g_helperCancelled) {
            send(gate[1], "1\n", 2, kNoSignal);
        }
        closeGate();
        launch.unlock();
        // Wait without reaping so the pid can't be reused while it is still published.
        siginfo_t info{};
        while (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT) == -1 && errno == EINTR) {
        }
        {
            std::lock_guard lock(g_helperMutex);
            g_helper = 0;
        }
    }
    const auto deadline = Clock::now() + kHandoffTimeout;
    for (;;) {
        const pid_t waited = waitpid(child, &status, bounded ? WNOHANG : 0);
        if (waited == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? Handoff::Confirmed : Handoff::Unconfirmed;
        }
        if (waited == -1 && errno != EINTR) {
            return Handoff::Unconfirmed;
        }
        if (waited == 0) {
            if (Clock::now() >= deadline) {
                try {
                    std::thread([child] { waitpid(child, nullptr, 0); }).detach();
                } catch (...) {
                }
                return Handoff::Unconfirmed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void CancelWorkerRequests() {
    g_helperCancelled = true;
    std::unique_lock lock(g_helperMutex, std::defer_lock);
    if (!TryLockBriefly(lock, kHandoffTimeout)) {
        return;
    }
    if (g_helper == 0 || kill(g_helper, SIGKILL) != 0) {
        return;
    }
    // The worker only reaps after clearing g_helper under this lock, so the pid stays ours.
    const auto deadline = Clock::now() + kHandoffTimeout;
    for (;;) {
        siginfo_t info{};
        if (waitid(P_PID, static_cast<id_t>(g_helper), &info, WEXITED | WNOWAIT | WNOHANG) != 0 ||
            info.si_pid != 0 || Clock::now() >= deadline) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

#endif

// Caller holds state.delivery. Only exit paths bound the wait; the worker waits for each
// request to finish so a slow one can never land after the next.
Handoff Deliver(State& state, bool forced, uint32_t appId, bool bounded) {
    const auto earliest = Clock::time_point(Clock::duration(state.lastDelivery.load())) + kDeliverySpacing;
    if (const auto now = Clock::now(); now < earliest) {
        std::this_thread::sleep_for(earliest - now);
    }
    const Handoff delivered = OpenSteamUrl("steam://forceinputappid/" + std::to_string(forced ? appId : 0), bounded);
    if (delivered != Handoff::None) {
        state.lastDelivery = Clock::now().time_since_epoch().count();
        if (forced) {
            RT_LOG(RT_TAG_RUNTIME) << "Steam Input: applying the controller layout of app " << appId << std::endl;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "Steam Input: restored the desktop controller layout" << std::endl;
        }
    }
    return delivered;
}

void Worker(State& state) {
    t_isWorker = true;
    std::unique_lock lock(state.mutex);
    for (;;) {
        state.wake.wait(lock, [&] { return state.stopping || state.desired != state.sent; });
        if (state.stopping) {
            return;
        }
        const bool target = state.desired;
        const uint32_t appId = state.appId;
        // A failed request is retried on the next focus change rather than in a loop.
        state.sent = target;
        lock.unlock();
        {
            std::lock_guard delivery(state.delivery);
            {
                // Release may have run while this thread waited for the delivery lock.
                std::lock_guard guard(state.mutex);
                if (state.stopping) {
                    return;
                }
            }
            const Handoff delivered = Deliver(state, target, appId, false);
            std::lock_guard guard(state.mutex);
            state.maybeForced = target ? delivered != Handoff::None || state.maybeForced
                                       : state.maybeForced && delivered != Handoff::Confirmed;
        }
        lock.lock();
    }
}

} // namespace

void Initialize(bool enabled, uint32_t appId) {
    if (!enabled || appId == 0 || g_state.load() != nullptr) {
        return;
    }
    if (LaunchedBySteam()) {
        RT_LOG(RT_TAG_RUNTIME) << "Steam Input: launched by Steam, leaving its controller layout alone" << std::endl;
        return;
    }
    SDL_Window* window = aurora_get_window();
    if (window == nullptr) {
        return;
    }
    auto* state = new State();
    state->enabled = true;
    state->appId = appId;
    state->windowId = SDL_GetWindowID(window);
    state->desired = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    try {
        std::thread(Worker, std::ref(*state)).detach();
    } catch (const std::exception& error) {
        RT_LOG(RT_TAG_RUNTIME) << "Steam Input: " << error.what() << std::endl;
        delete state;
        return;
    }
    g_state.store(state);
}

void HandleEvents(const AuroraEvent* events) {
    State* state = g_state.load();
    if (state == nullptr || events == nullptr) {
        return;
    }
    int focus = -1;
    for (const AuroraEvent* event = events; event->type != AURORA_NONE; ++event) {
        if (event->type != AURORA_SDL_EVENT) {
            continue;
        }
        const SDL_Event& sdl = event->sdl;
        const bool gained = sdl.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if ((gained || sdl.type == SDL_EVENT_WINDOW_FOCUS_LOST) && sdl.window.windowID == state->windowId) {
            focus = gained ? 1 : 0;
        }
    }
    if (focus < 0) {
        return;
    }
    std::lock_guard lock(state->mutex);
    if (state->enabled) {
        state->desired = focus == 1;
        state->wake.notify_one();
    }
}

void Release() noexcept {
    State* state = g_state.load();
    if (state == nullptr) {
        return;
    }
    const auto releaseWait = kHandoffTimeout + kDeliverySpacing + std::chrono::milliseconds(500);
    try {
        {
            std::unique_lock lock(state->mutex, std::defer_lock);
            if (!TryLockBriefly(lock)) {
                return;
            }
            if (!state->enabled) {
                lock.unlock();
                // Another thread is restoring; exiting under it would kill its request.
                const auto deadline = Clock::now() + 2 * releaseWait;
                while (!t_inRelease && !state->released.load() && Clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return;
            }
            t_inRelease = true;
            state->enabled = false;
            state->stopping = true;
            state->wake.notify_one();
        }
        // Let an in-flight request land first so the restore is the last one Steam sees.
        std::unique_lock delivery(state->delivery, std::defer_lock);
        const bool ordered = !t_isWorker && delivery.try_lock_for(releaseWait);
        bool maybeForced = true;
        if (ordered) {
            std::unique_lock lock(state->mutex, std::defer_lock);
            maybeForced = !TryLockBriefly(lock) || state->maybeForced;
        }
        // A request still in flight, or about to start, could otherwise land after the restore.
        CancelWorkerRequests();
        if (maybeForced) {
            // Discovery and spawning can block as well; they must not hold up the exit.
            auto done = std::make_shared<std::atomic<bool>>(false);
            try {
                std::thread([state, done] {
                    Deliver(*state, false, 0, true);
                    done->store(true);
                }).detach();
            } catch (const std::system_error&) {
                done->store(true);
            }
            const auto deadline = Clock::now() + releaseWait;
            while (!done->load() && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    } catch (...) {
    }
    state->released.store(true);
}

} // namespace SteamInputLayout
