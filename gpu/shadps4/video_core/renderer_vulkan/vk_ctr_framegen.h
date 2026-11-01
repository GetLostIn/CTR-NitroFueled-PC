// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: AMD FSR 3.1.6 frame generation for CTR (docs/sacredfire/16-framegen.md).
//
// Runs at flip time on the presenter's finished frame (post-processing done, HUD included;
// the port's menu and FPS counter are drawn later, on every present). Each real frame is fed
// to FSR 3.1.6 frame interpolation (optical flow + interpolation, the validated
// `ffx-vulkan::fsr3-vk-framegeneration-3.1.6` slice of FireBurn/FSR-Vulkan, MIT), which
// returns the frame halfway between it and the previous one. The presenter shows that
// generated frame first and the real one half a frame later.
//
// The game's motion (depth R32F + pixel motion RG16F from CtrFsr3's inputs pass, when FSR 3,
// FSR 4 or DLSS ran this frame) is used when it is fresh and no larger than the frame;
// otherwise flat far depth and zero motion stand in and the optical flow carries the motion
// alone (like driver-level frame generation), so it works with every upscaler and in menus.
//
// Threading: everything here runs on the GPU thread that calls Presenter::PrepareFrame (the
// recording threads are drained by Scheduler::CommandBuffer first); OutputImage() is read by
// the swap thread for a frame whose generation was recorded and submitted before.

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

class CtrFrameGen {
public:
    /// Presenter frames that can hold a generated image at once (Frame::id).
    static constexpr u32 MaxSlots = 8;

    CtrFrameGen(const Instance& instance, Scheduler& scheduler);
    ~CtrFrameGen();

    CtrFrameGen(const CtrFrameGen&) = delete;
    CtrFrameGen& operator=(const CtrFrameGen&) = delete;

    /// The setting asks for frame generation (bbport.ini framegen, BB_FRAMEGEN) and it has not
    /// failed in this session.
    [[nodiscard]] bool Enabled() const;

    struct Vectors {
        bool fresh = false; ///< the upscaler wrote them this frame
        vk::Image depth{}, motion{};
        u32 width = 0, height = 0;
        float jitter[2]{};
        float near_plane = 16.0f, far_plane = 150000.0f, fov = 1.2870f;
        float position[3]{0.0f, 0.0f, 0.0f};
        float up[3]{0.0f, 1.0f, 0.0f};
        float right[3]{1.0f, 0.0f, 0.0f};
        float forward[3]{0.0f, 0.0f, -1.0f};
    };

    struct Inputs {
        u32 slot = 0;             ///< the presenter frame's id (< MaxSlots)
        vk::Image color{};        ///< the finished frame, General layout, MUTABLE_FORMAT
        vk::Format color_format{}; ///< its format (the swapchain's)
        u32 width = 0, height = 0;
        Vectors vectors{};
        /// The same frame without the HUD (General, same format and size), or null: FSR then
        /// interpolates the scene alone and takes the HUD from `color`.
        vk::Image hudless{};
        float frame_ms = 16.6f;   ///< since the previous real frame
        bool reset = false;       ///< a discontinuity the caller knows of (load, pause)
    };

    /// Records conversion, preparation and interpolation for this real frame into the draw
    /// scheduler's command buffer. True when OutputImage(slot) now holds the generated frame
    /// to present before this one (General layout, the frame's format and size). False on
    /// a history reset (the first frame, a size or mode change), with the setting off or for
    /// frames it cannot take (10-bit HDR); on a failure it also turns itself off.
    bool Record(const Inputs& in);

    /// The generated image of `slot` (valid after Record returned true for it).
    [[nodiscard]] vk::Image OutputImage(u32 slot) const {
        return slot < MaxSlots ? vk::Image(outputs[slot].image) : vk::Image{};
    }

    /// Drops the context and every image (waits for the GPU).
    void Reset();

    struct Stats {
        u64 frames = 0, generated = 0, resets = 0, game_vectors = 0, hudless = 0;
    };
    [[nodiscard]] const Stats& GetStats() const {
        return stats;
    }

private:
    bool Ensure(u32 width, u32 height);
    bool EnsureOutput(u32 slot, u32 width, u32 height, vk::Format format);
    void CreateConvertPipeline();
    /// A raw (UNORM) sampled view of the presenter's frame image, freed once this frame's
    /// work completed.
    vk::ImageView SourceView(vk::Image image, vk::Format format);
    void Convert(vk::CommandBuffer cmdbuf, vk::ImageView src, vk::ImageView dst, u32 width,
                 u32 height, bool swap_rb);
    void RetireCompleted(bool all);
    void Fail(const char* reason);

    const Instance& instance;
    Scheduler& scheduler;

    FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
    u32 width = 0, height = 0;
    std::atomic<bool> failed{false};
    bool announced = false;
    bool needs_reset = true;
    bool last_vectors = false;
    int last_motion = -1;
    u64 next_frame_id = 1;
    /// Recorded frame ids and the draw tick that carries them: retired once the GPU is past it.
    std::deque<std::pair<u64, u64>> in_flight;

    // FSR 3.1.6 takes R8G8B8A8_UNORM: the converted frame and the interpolated output.
    VideoCore::UniqueImage rgba_image, generated_image, hudless_rgba_image;
    vk::UniqueImageView rgba_view, generated_view, hudless_rgba_view;
    // Optical-flow-only stand-ins at the frame size: far depth, zero motion.
    VideoCore::UniqueImage flat_depth_image, flat_motion_image;

    // The generated frame per presenter frame, in the frame's own format (the presenter blits
    // it exactly like the real one), written through an RGBA8 storage view.
    struct Output {
        Output() {}
        VideoCore::UniqueImage image;
        vk::UniqueImageView storage_view;
        u32 width = 0, height = 0;
        vk::Format format{};
    };
    std::array<Output, MaxSlots> outputs;

    // Convert pass (fg_convert.comp): raw 8-bit copy with an optional red/blue swap.
    vk::UniqueSampler convert_sampler;
    vk::UniqueDescriptorSetLayout convert_desc_layout;
    vk::UniquePipelineLayout convert_pipeline_layout;
    vk::UniquePipeline convert_pipeline;
    Stats stats{};
};

} // namespace Vulkan
