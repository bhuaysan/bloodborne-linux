// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/renderer/texture_manager.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

#include <imgui.h>
#include "common/assert.h"
#include "imgui_impl_vulkan.h"
#include "texture_manager.h"

namespace ImGui {

namespace Core::TextureManager {
struct Inner {
    std::atomic_int count = 0;
    ImTextureID texture_id = nullptr;
    u32 width = 0;
    u32 height = 0;

    Vulkan::UploadTextureData upload_data;

    ~Inner();
};
} // namespace Core::TextureManager

using namespace Core::TextureManager;

RefCountedTexture::RefCountedTexture(Inner* inner) : inner(inner) {
    ++inner->count;
}

RefCountedTexture::RefCountedTexture() : inner(nullptr) {}

RefCountedTexture::RefCountedTexture(const RefCountedTexture& other) : inner(other.inner) {
    if (inner != nullptr) {
        ++inner->count;
    }
}

RefCountedTexture::RefCountedTexture(RefCountedTexture&& other) noexcept : inner(other.inner) {
    other.inner = nullptr;
}

RefCountedTexture& RefCountedTexture::operator=(const RefCountedTexture& other) {
    if (this == &other)
        return *this;
    inner = other.inner;
    if (inner != nullptr) {
        ++inner->count;
    }
    return *this;
}

RefCountedTexture& RefCountedTexture::operator=(RefCountedTexture&& other) noexcept {
    if (this == &other)
        return *this;
    std::swap(inner, other.inner);
    return *this;
}

RefCountedTexture::~RefCountedTexture() {
    if (inner != nullptr) {
        if (inner->count.fetch_sub(1) == 1) {
            delete inner;
        }
    }
}

RefCountedTexture::Image RefCountedTexture::GetTexture() const {
    if (inner == nullptr) {
        return {};
    }
    return Image{
        .im_id = inner->texture_id,
        .width = inner->width,
        .height = inner->height,
    };
}

RefCountedTexture::operator bool() const {
    return inner != nullptr && inner->texture_id != nullptr;
}

struct UploadJob {
    Inner* core = nullptr;
    Vulkan::UploadTextureData data;
    int tick = 0; // Used to skip the first frame when destroying to await the current frame to draw
};

static std::mutex g_upload_mtx;
static std::deque<UploadJob> g_upload_list;

namespace Core::TextureManager {

Inner::~Inner() {
    if (upload_data.im_texture != nullptr) {
        std::unique_lock lk{g_upload_mtx};
        g_upload_list.emplace_back(UploadJob{
            .data = this->upload_data,
            .tick = 2,
        });
    }
}

void Submit() {
    UploadJob upload;
    {
        std::unique_lock lk{g_upload_mtx};
        if (g_upload_list.empty()) {
            return;
        }
        // Upload one texture at a time to avoid slow down
        upload = g_upload_list.front();
        g_upload_list.pop_front();
        if (upload.tick > 0) {
            --upload.tick;
            g_upload_list.emplace_back(upload);
            return;
        }
    }
    if (upload.core != nullptr) {
        upload.core->upload_data.Upload();
        upload.core->texture_id = upload.core->upload_data.im_texture;
        if (upload.core->count.fetch_sub(1) == 1) {
            delete upload.core;
        }
    } else {
        upload.data.Destroy();
    }
}
} // namespace Core::TextureManager

} // namespace ImGui