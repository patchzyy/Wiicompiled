#include "friend_code_keyboard.h"

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "input_bindings.h"
#include "memory.h"
#include "runtime_product.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_events.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace {

// UI::RegisterFriendPage
constexpr uint32_t kDigitButtonsOffset = 0x44;
constexpr uint32_t kPushButtonSize = 0x254;
constexpr uint32_t kDigitButtonCount = 10;
constexpr uint32_t kButtonDigitOffset = 0x240;
constexpr uint32_t kPageStateOffset = 0x8;
constexpr uint32_t kPageStateActive = 4;
constexpr uint32_t kVtableGetTypeInfoOffset = 0x60;
constexpr int kBackspace = -1;
constexpr size_t kMaxPendingKeys = 32;
constexpr size_t kFriendCodeDigits = 12;

std::mutex g_pendingMutex;
std::vector<int> g_pending;
std::atomic_bool g_pageFocused{false};

bool IsPaste(const SDL_KeyboardEvent& key) {
    return !key.repeat && key.key == SDLK_V && (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0 &&
           (key.mod & SDL_KMOD_ALT) == 0;
}

void AppendClipboardDigits(std::vector<int>& keys) {
    char* text = SDL_GetClipboardText();
    if (!text) {
        return;
    }
    size_t digits = 0;
    for (const char* c = text; *c && digits < kFriendCodeDigits; ++c) {
        if (*c >= '0' && *c <= '9') {
            keys.push_back(*c - '0');
            ++digits;
        }
    }
    SDL_free(text);
}

std::optional<int> KeyValue(const SDL_KeyboardEvent& key) {
    if ((key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
        return std::nullopt;
    }
    if (key.scancode == SDL_SCANCODE_BACKSPACE) {
        return kBackspace;
    }
    if (key.repeat) {
        return std::nullopt;
    }
    if (key.scancode == SDL_SCANCODE_0 || key.scancode == SDL_SCANCODE_KP_0) {
        return 0;
    }
    if (key.scancode >= SDL_SCANCODE_1 && key.scancode <= SDL_SCANCODE_9) {
        return 1 + (key.scancode - SDL_SCANCODE_1);
    }
    if (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_9) {
        return 1 + (key.scancode - SDL_SCANCODE_KP_1);
    }
    return std::nullopt;
}

bool IsFocusedRegisterFriendPage(uint32_t page) {
    try {
        const uint32_t vtable = Memory::Read32(page);
        return Memory::Read32(vtable + kVtableGetTypeInfoOffset) == MKW_GADDR(8061E804) &&
               Memory::Read32(page + kPageStateOffset) == kPageStateActive;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

std::optional<uint32_t> DigitButton(uint32_t page, int digit) {
    for (uint32_t i = 0; i < kDigitButtonCount; ++i) {
        const uint32_t button = page + kDigitButtonsOffset + i * kPushButtonSize;
        if (static_cast<int>(Memory::Read32(button + kButtonDigitOffset)) == digit) {
            return button;
        }
    }
    return std::nullopt;
}

void Type(CpuContext* ctx, uint32_t page, int key) {
    if (key == kBackspace) {
        ctx->gpr[3] = page;
        MKW_GUEST_FUNC(8061E3B4)(ctx);
        return;
    }
    const std::optional<uint32_t> button = DigitButton(page, key);
    if (!button) {
        return;
    }
    ctx->gpr[3] = page;
    ctx->gpr[4] = *button;
    ctx->gpr[5] = 0;
    MKW_GUEST_FUNC(8061E348)(ctx);
}

} // namespace

namespace FriendCodeKeyboard {

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events || !g_pageFocused.load(std::memory_order_acquire) || InputBindings::InputBlocked()) {
        return;
    }
    std::vector<int> keys;
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type != AURORA_SDL_EVENT || ev->sdl.type != SDL_EVENT_KEY_DOWN) {
            continue;
        }
        if (IsPaste(ev->sdl.key)) {
            AppendClipboardDigits(keys);
        } else if (const std::optional<int> key = KeyValue(ev->sdl.key)) {
            keys.push_back(*key);
        }
    }
    if (keys.empty()) {
        return;
    }
    std::lock_guard lock(g_pendingMutex);
    for (const int key : keys) {
        if (g_pending.size() >= kMaxPendingKeys) {
            break;
        }
        g_pending.push_back(key);
    }
}

} // namespace FriendCodeKeyboard

// Page::CheckActions, only ever called for the topmost page.
extern "C" void Page_CheckActions_80601c64(CpuContext* ctx) {
    const uint32_t page = ctx->gpr[3];
    MKW_GUEST_FUNC(80601C64)(ctx);

    const bool focused = RuntimeProduct::IsRetroRewind() && IsFocusedRegisterFriendPage(page);
    g_pageFocused.store(focused, std::memory_order_release);
    std::vector<int> keys;
    {
        std::lock_guard lock(g_pendingMutex);
        keys.swap(g_pending);
    }
    if (!focused || InputBindings::InputBlocked()) {
        return;
    }
    for (const int key : keys) {
        Type(ctx, page, key);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80601C64, Page_CheckActions_80601c64, "Page_CheckActions_80601c64");
