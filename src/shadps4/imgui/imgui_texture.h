// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/imgui_texture.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <filesystem>
#include <vector>
#include <imgui.h>

#include "common/types.h"

namespace ImGui {

namespace Core::TextureManager {
struct Inner;
} // namespace Core::TextureManager

class RefCountedTexture {
    Core::TextureManager::Inner* inner;

    explicit RefCountedTexture(Core::TextureManager::Inner* inner);

public:
    struct Image {
        ImTextureID im_id;
        u32 width;
        u32 height;
    };

    static RefCountedTexture DecodePngFile(std::filesystem::path path);

    RefCountedTexture();

    RefCountedTexture(const RefCountedTexture& other);
    RefCountedTexture(RefCountedTexture&& other) noexcept;
    RefCountedTexture& operator=(const RefCountedTexture& other);
    RefCountedTexture& operator=(RefCountedTexture&& other) noexcept;

    virtual ~RefCountedTexture();

    [[nodiscard]] Image GetTexture() const;

    explicit(false) operator bool() const;
};

}; // namespace ImGui