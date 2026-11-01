// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_ctr_framegen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "video_core/host_shaders/fg_convert_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

// The 3.1.6 frame-generation C ABI (MIT, FireBurn/FSR-Vulkan).
#include "ffx_vk_fsr3_3_1_5_bridge.h"

namespace Vulkan {

namespace {

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

bool IsBgra(vk::Format format) {
    return format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

/// The frame formats frame generation takes: 8-bit RGBA or BGRA, sRGB or not (the bytes are
/// what matters, they are copied raw).
bool Supported(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return true;
    default:
        return false;
    }
}

vk::Format Unorm(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    default:
        return format;
    }
}

void Barrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto MemoryRw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
constexpr auto ShaderRw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;

} // namespace

CtrFrameGen::CtrFrameGen(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

CtrFrameGen::~CtrFrameGen() {
    if (context) {
        scheduler.Finish();
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        context = nullptr;
    }
}

bool CtrFrameGen::Enabled() const {
    return !failed.load(std::memory_order_relaxed) &&
           BbSettings::Get().framegen.load(std::memory_order_relaxed) != BbSettings::FrameGenOff;
}

void CtrFrameGen::Fail(const char* reason) {
    if (!failed.exchange(true)) {
        std::printf("FrameGen: %s; off for this session\n", reason);
        std::fflush(stdout);
    }
}

void CtrFrameGen::Reset() {
    if (!context && !rgba_image) {
        return;
    }
    scheduler.Finish();
    RetireCompleted(true);
    if (context) {
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        context = nullptr;
    }
    rgba_view.reset();
    generated_view.reset();
    hudless_rgba_view.reset();
    rgba_image.Destroy();
    generated_image.Destroy();
    hudless_rgba_image.Destroy();
    flat_depth_image.Destroy();
    flat_motion_image.Destroy();
    for (auto& output : outputs) {
        output.storage_view.reset();
        output.image.Destroy();
        output.width = output.height = 0;
    }
    width = height = 0;
    needs_reset = true;
}

void CtrFrameGen::RetireCompleted(bool all) {
    u64 retire = 0;
    while (!in_flight.empty() && (all || scheduler.IsFree(in_flight.front().second))) {
        retire = in_flight.front().first;
        in_flight.pop_front();
    }
    if (retire != 0 && context) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, retire);
    }
}

void CtrFrameGen::CreateConvertPipeline() {
    if (convert_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    convert_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(int)};
    convert_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*convert_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(FG_CONVERT_COMP, device);
    convert_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *convert_pipeline_layout,
            }));
    device.destroyShaderModule(module);
    convert_sampler = Check(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
}

vk::ImageView CtrFrameGen::SourceView(vk::Image image, vk::Format format) {
    // Same bytes relabelled UNORM (the frame image is MUTABLE_FORMAT, so no sRGB decode). A
    // BGRA view already returns red, green and blue in place: no swizzle.
    const vk::ImageViewUsageCreateInfo sampled{.usage = vk::ImageUsageFlagBits::eSampled};
    const auto device = instance.GetDevice();
    const vk::ImageView view = Check(device.createImageView({
        .pNext = &sampled,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = Unorm(format),
        .subresourceRange = ColorRange,
    }));
    scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
    return view;
}

void CtrFrameGen::Convert(vk::CommandBuffer cmdbuf, vk::ImageView src, vk::ImageView dst,
                          u32 w, u32 h, bool swap_rb) {
    const vk::DescriptorImageInfo src_info{*convert_sampler, src, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo dst_info{{}, dst, vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .pImageInfo = &src_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &dst_info},
    }};
    const int swap = swap_rb ? 1 : 0;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *convert_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *convert_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*convert_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(swap), &swap);
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

bool CtrFrameGen::Ensure(u32 w, u32 h) {
    if (context && w == width && h == height) {
        return true;
    }
    Reset();

    FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{};
    create.physicalDevice = instance.GetPhysicalDevice();
    create.device = instance.GetDevice();
    // The game's vectors are used when they fit inside the frame, so the frame size bounds
    // the render size: one context serves both modes and never has to be rebuilt between them.
    create.maxRenderWidth = w;
    create.maxRenderHeight = h;
    create.displayWidth = w;
    create.displayHeight = h;
    create.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const auto created = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &context);
    if (created != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        context = nullptr;
        Fail(created == FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY
                 ? "context creation failed (out of memory)"
                 : "context creation failed");
        return false;
    }
    width = w;
    height = h;

    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView* view,
                          vk::Format format, vk::ImageUsageFlags usage) {
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        if (view) {
            *view = Check(device.createImageViewUnique({
                .image = vk::Image(image),
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = ColorRange,
            }));
        }
    };
    const auto rw = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    make(rgba_image, &rgba_view, vk::Format::eR8G8B8A8Unorm, rw);
    make(generated_image, &generated_view, vk::Format::eR8G8B8A8Unorm, rw);
    make(hudless_rgba_image, &hudless_rgba_view, vk::Format::eR8G8B8A8Unorm, rw);
    make(flat_depth_image, nullptr, vk::Format::eR32Sfloat,
         rw | vk::ImageUsageFlagBits::eTransferDst);
    make(flat_motion_image, nullptr, vk::Format::eR16G16Sfloat,
         rw | vk::ImageUsageFlagBits::eTransferDst);
    CreateConvertPipeline();

    // Out of Undefined: the working images to General, the stand-ins cleared (far depth, no
    // motion) and left in SHADER_READ_ONLY, the layout every import declares for them.
    const auto cmdbuf = scheduler.CommandBuffer();
    std::array<vk::ImageMemoryBarrier2, 5> first{};
    const std::array<vk::Image, 5> images{vk::Image(rgba_image), vk::Image(generated_image),
                                         vk::Image(hudless_rgba_image),
                                         vk::Image(flat_depth_image),
                                         vk::Image(flat_motion_image)};
    for (u32 i = 0; i < first.size(); ++i) {
        first[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = AllCommands,
            .dstStageMask = AllCommands,
            .dstAccessMask = MemoryRw,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = i < 3 ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferDstOptimal,
            .image = images[i],
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(first.size()),
                             .pImageMemoryBarriers = first.data()});
    const vk::ClearColorValue far{std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}};
    const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    cmdbuf.clearColorImage(vk::Image(flat_depth_image), vk::ImageLayout::eTransferDstOptimal, far,
                           ColorRange);
    cmdbuf.clearColorImage(vk::Image(flat_motion_image), vk::ImageLayout::eTransferDstOptimal,
                           zero, ColorRange);
    for (const vk::Image image : {vk::Image(flat_depth_image), vk::Image(flat_motion_image)}) {
        Barrier(cmdbuf, image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, Compute, vk::AccessFlagBits2::eShaderRead);
    }

    needs_reset = true;
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    if (ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage) ==
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("FrameGen: FSR 3.1.6 context %ux%u, %.1f MB\n", w, h,
                    usage.totalUsageInBytes / 1e6);
        std::fflush(stdout);
    }
    return true;
}

bool CtrFrameGen::EnsureOutput(u32 slot, u32 w, u32 h, vk::Format format) {
    Output& output = outputs[slot];
    if (output.image && output.width == w && output.height == h && output.format == format) {
        return true;
    }
    // The slot's previous generated frame was presented (or skipped) before the presenter
    // handed this frame out again, so nothing in flight still reads it.
    output.storage_view.reset();
    output.image.Destroy();
    const auto device = instance.GetDevice();
    // The frame's own format, so the presenter blits it like the real frame; written raw
    // through an RGBA8 storage view (same 32-bit class: MUTABLE + EXTENDED_USAGE).
    const std::array<vk::Format, 2> view_formats{format, vk::Format::eR8G8B8A8Unorm};
    const vk::ImageFormatListCreateInfo list{.viewFormatCount = u32(view_formats.size()),
                                             .pViewFormats = view_formats.data()};
    output.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    output.image.Create(vk::ImageCreateInfo{
        .pNext = &list,
        .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const vk::ImageViewUsageCreateInfo storage{.usage = vk::ImageUsageFlagBits::eStorage};
    output.storage_view = Check(device.createImageViewUnique({
        .pNext = &storage,
        .image = vk::Image(output.image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = ColorRange,
    }));
    output.width = w;
    output.height = h;
    output.format = format;
    Barrier(scheduler.CommandBuffer(), vk::Image(output.image), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral, AllCommands, vk::AccessFlagBits2::eNone, AllCommands,
            MemoryRw);
    return true;
}

bool CtrFrameGen::Record(const Inputs& in) {
    if (!Enabled()) {
        if (context) {
            Reset(); // turned off live: free the ~60 MB and start over when turned back on
        }
        return false;
    }
    if (!in.color || in.width == 0 || in.height == 0 || in.slot >= MaxSlots ||
        !Supported(in.color_format)) {
        needs_reset = true; // the history no longer follows the presented frames
        return false;
    }
    if (!Ensure(in.width, in.height) || !EnsureOutput(in.slot, in.width, in.height,
                                                      in.color_format)) {
        return false;
    }
    RetireCompleted(false);

    const Vectors& vec = in.vectors;
    const int motion_setting = BbSettings::Get().framegen_motion.load(std::memory_order_relaxed);
    const bool vectors = motion_setting == 1 && vec.fresh && vec.depth && vec.motion &&
                         vec.width != 0 &&
                         vec.height != 0 && vec.width <= in.width && vec.height <= in.height;
    const u32 render_w = vectors ? vec.width : in.width;
    const u32 render_h = vectors ? vec.height : in.height;
    const bool reset =
        in.reset || needs_reset || vectors != last_vectors || motion_setting != last_motion;
    last_vectors = vectors;
    last_motion = motion_setting;
    needs_reset = false;

    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    const auto usage = static_cast<VkImageUsageFlags>(
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst);
    const auto general = VK_IMAGE_LAYOUT_GENERAL;
    const auto read_only = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 1. The finished frame (General, written by the post-processing pass) to RGBA8, the
    //    only backbuffer format FSR 3.1.6 takes. The frame image stays in General.
    {
        const vk::MemoryBarrier2 ready{
            .srcStageMask = AllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = Compute,
            .dstAccessMask = ShaderRw,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready});
    }
    Convert(cmdbuf, SourceView(in.color, in.color_format), *rgba_view, in.width, in.height,
            false);
    // The HUD-less source: the frame without its HUD when the presenter has one, else the
    // frame itself (FSR then finds no HUD; the mode stays the same, so no history reset).
    Convert(cmdbuf, SourceView(in.hudless ? in.hudless : in.color, in.color_format),
            *hudless_rgba_view, in.width, in.height, false);
    for (const vk::Image image : {vk::Image(rgba_image), vk::Image(hudless_rgba_image)}) {
        Barrier(cmdbuf, image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                vk::AccessFlagBits2::eShaderRead);
    }

    // 2. Optical flow + interpolation preparation, then the interpolation itself. The
    //    bridge leaves sampled imports in SHADER_READ_ONLY: they are imported that way and
    //    moved there (and back) here, so every exit path restores what the frame expects.
    const vk::Image depth = vectors ? vec.depth : vk::Image(flat_depth_image);
    const vk::Image motion = vectors ? vec.motion : vk::Image(flat_motion_image);
    if (vectors) {
        for (const vk::Image image : {depth, motion}) {
            Barrier(cmdbuf, image, vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eShaderReadOnlyOptimal, AllCommands, MemoryRw, Compute,
                    vk::AccessFlagBits2::eShaderRead);
        }
    }
    const auto restore = [&] {
        for (const vk::Image image : {vk::Image(rgba_image), vk::Image(hudless_rgba_image)}) {
            Barrier(cmdbuf, image, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::ImageLayout::eGeneral, Compute, ShaderRw, AllCommands, MemoryRw);
        }
        if (vectors) {
            for (const vk::Image image : {depth, motion}) {
                Barrier(cmdbuf, image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::ImageLayout::eGeneral, Compute, ShaderRw, AllCommands, MemoryRw);
            }
        }
        scheduler.GetDynamicState().Invalidate(); // FSR records its own Vulkan state
    };

    const u64 frame_id = next_frame_id++;
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = cmdbuf;
    prepare.color = {vk::Image(rgba_image), VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height,
                     read_only, usage};
    prepare.depth = {depth, VK_FORMAT_R32_SFLOAT, render_w, render_h, read_only, usage};
    prepare.motionVectors = {motion, VK_FORMAT_R16G16_SFLOAT, render_w, render_h, read_only,
                             usage};
    prepare.renderWidth = render_w;
    prepare.renderHeight = render_h;
    // Same conventions as the upscaler that produced them: pixel motion, scale 1.
    const float jitter_sign = std::getenv("BB_FSR3_JITTER_FLIP") ? -1.0f : 1.0f;
    prepare.jitterOffsetX = vectors ? jitter_sign * vec.jitter[0] : 0.0f;
    prepare.jitterOffsetY = vectors ? jitter_sign * vec.jitter[1] : 0.0f;
    prepare.motionVectorScaleX = 1.0f;
    prepare.motionVectorScaleY = 1.0f;
    prepare.frameTimeMilliseconds = std::clamp(in.frame_ms, 1.0f, 100.0f);
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f; // the finished, tonemapped 8-bit frame
    prepare.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    const bool camera_ok = vec.near_plane > 0.0f && vec.far_plane > vec.near_plane &&
                           vec.fov > 0.01f && vec.fov < 3.1f;
    prepare.cameraNear = camera_ok ? vec.near_plane : 16.0f;
    prepare.cameraFar = camera_ok ? vec.far_plane : 150000.0f;
    prepare.cameraVerticalFovRadians = camera_ok ? vec.fov : 1.2870f;
    prepare.viewSpaceToMeters = 0.01f; // same as the upscaler
    for (int i = 0; i < 3; ++i) {
        prepare.cameraPosition[i] = vec.position[i];
        prepare.cameraUp[i] = vec.up[i];
        prepare.cameraRight[i] = vec.right[i];
        prepare.cameraForward[i] = vec.forward[i];
    }
    prepare.frameId = frame_id;
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    prepare.colorHudless = {vk::Image(hudless_rgba_image), VK_FORMAT_R8G8B8A8_UNORM, in.width,
                            in.height, read_only, usage};
    const auto prepared = ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare);
    if (prepared != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        restore();
        in_flight.emplace_back(frame_id, scheduler.CurrentTick());
        Fail(prepared == FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT
                 ? "prepare rejected (invalid argument)"
                 : "prepare failed");
        return false;
    }
    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
    dispatch.commandBuffer = cmdbuf;
    dispatch.color = prepare.color;
    dispatch.output = {vk::Image(generated_image), VK_FORMAT_R8G8B8A8_UNORM, in.width,
                       in.height, general, usage};
    dispatch.displayWidth = in.width;
    dispatch.displayHeight = in.height;
    dispatch.frameTimeMilliseconds = prepare.frameTimeMilliseconds;
    dispatch.cameraNear = prepare.cameraNear;
    dispatch.cameraFar = prepare.cameraFar;
    dispatch.viewSpaceToMeters = prepare.viewSpaceToMeters;
    dispatch.cameraVerticalFovRadians = prepare.cameraVerticalFovRadians;
    dispatch.minLuminance = prepare.minLuminance;
    dispatch.maxLuminance = prepare.maxLuminance;
    dispatch.transferFunction = prepare.transferFunction;
    dispatch.frameId = frame_id;
    dispatch.reset = prepare.reset;
    const auto dispatched = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch);
    in_flight.emplace_back(frame_id, scheduler.CurrentTick());
    if (dispatched != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        restore();
        Fail(dispatched == FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT
                 ? "dispatch rejected (invalid argument)"
                 : "dispatch failed");
        return false;
    }

    // 3. The interpolated RGBA8 frame into this presenter frame's output, in the frame's own
    //    byte order: written raw through an RGBA8 view, so a BGRA output gets red and blue
    //    swapped in the shader (verified with BB_FG_DUMP_TRIGGER dumps).
    Barrier(cmdbuf, vk::Image(generated_image), vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral, AllCommands, MemoryRw, Compute,
            vk::AccessFlagBits2::eShaderRead);
    {
        const vk::MemoryBarrier2 output_free{
            .srcStageMask = AllCommands,
            .srcAccessMask = MemoryRw,
            .dstStageMask = Compute,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &output_free});
    }
    Convert(cmdbuf, *generated_view, *outputs[in.slot].storage_view, in.width, in.height,
            IsBgra(in.color_format));
    Barrier(cmdbuf, vk::Image(outputs[in.slot].image), vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderWrite, AllCommands,
            MemoryRw);
    restore();

    ++stats.frames;
    stats.resets += reset ? 1 : 0;
    stats.game_vectors += vectors ? 1 : 0;
    stats.hudless += in.hudless ? 1 : 0;
    if (!announced) {
        announced = true;
        std::printf("FrameGen: generating at %ux%u (%s, %s)\n", in.width, in.height,
                    vectors ? "game motion + optical flow" : "optical flow",
                    in.hudless ? "HUD kept out" : "no HUD-less frame yet");
        std::fflush(stdout);
    }
    // A reset dispatch only starts the history: there is no previous frame to interpolate
    // from, so the real frame goes out alone.
    if (reset) {
        return false;
    }
    ++stats.generated;
    return true;
}

} // namespace Vulkan
