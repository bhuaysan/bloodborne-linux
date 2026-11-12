// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/renderer/texture_manager.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <filesystem>
#include <vector>

#include "common/types.h"
#include "imgui/imgui_texture.h"

namespace vk {
class CommandBuffer;
}

namespace ImGui::Core::TextureManager {

struct Inner;

void StartWorker();

void StopWorker();

void DecodePngFile(std::filesystem::path path, Inner* core);

void Submit();

}; // namespace ImGui::Core::TextureManager