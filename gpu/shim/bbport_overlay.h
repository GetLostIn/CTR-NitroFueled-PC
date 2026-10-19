// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu or the text dialog is open: the game's input is held neutral.
bool CapturesInput();
/// SacredFire: true once after the menu's "Rescan controllers" button (bbgpu_pad_rescan_take).
bool TakePadRescan();

/// Opens the settings menu (any thread): the game Options menu's Graphics tab.
void OpenMenu();

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

/// Any thread: a notice drawn over the game for a few seconds (v0.4: an unlocked trophy; kind is
/// its grade, 1 bronze to 4 platinum, 0 unknown; png: its picture, decoded here). Notices queue up.
void Notify(const std::string& title, const std::string& name, const std::string& detail, int kind,
            const void* png = nullptr, size_t png_size = 0);

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay
