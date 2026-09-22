#pragma once

#include "common.hpp"
#include "texture.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::efb_ram {

inline constexpr size_t MaxAsyncReadbackSlots = 32;

void schedule(void* dest, uint32_t width, uint32_t height, GXTexFmt format, TextureHandle texture) noexcept;
bool has_pending(void* dest = nullptr) noexcept;
bool prepare_downloads(void* dest = nullptr);
void encode_downloads(const wgpu::CommandEncoder& encoder, void* dest = nullptr) noexcept;
bool complete_downloads() noexcept;
void cancel() noexcept;

// Frame-latent readbacks for probe-sized CPU-consumed copies: they ride the frame's own encode and
// land in guest RAM a frame later. Per frame, from the worker: seal, encode, then after_submit.
void seal_async_downloads() noexcept;
void encode_async_downloads(const wgpu::CommandEncoder& encoder) noexcept;
void after_submit() noexcept;
// Drops requests sealed for a frame that will never be encoded.
void abort_async() noexcept;
void shutdown() noexcept;

} // namespace aurora::gfx::efb_ram
