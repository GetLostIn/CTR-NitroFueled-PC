// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once
#include <array>
#include <mutex>

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage { English, Russian, Spanish };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// SacredFire: game patches (patches/CrashTeamRacing.xml), selected by these bbport.ini keys and
/// compiled into the executable at start by scripts/patches.py; a restart applies a change.
/// The patches are community work from illusionyy/PS-Game-Patch
/// (https://github.com/illusionyy/PS-Game-Patch, patches/xml/CrashTeamRacingUS-Orbis.xml):
/// "60 FPS Unlock" and "Disable Motion Blur" by illusion, "Resolution Patch (1080p for PS4 Pro)"
/// by TroyWarez. SacredFire only corrected the last one's address (docs/sacredfire/08-patches.md).
inline constexpr Effect GamePatches[] = {
    {"patch_60fps", "60 FPS unlock (illusion)", "60 FPS (illusion)", false},
    {"patch_no_motion_blur", "Disable motion blur (illusion)", "Без размытия в движении (illusion)", false},
    {"patch_pro_1080p", "1080p in PS4 Pro mode (TroyWarez; with BB_NEO=1)", "1080p в режиме PS4 Pro (TroyWarez; с BB_NEO=1)", false},
};
inline constexpr int GamePatchCount = int(sizeof(GamePatches) / sizeof(GamePatches[0]));
/// SacredFire: bbport.ini render_res, the scene size CTR renders (through its PS4 Pro path above
/// 1080p; scripts/patches.py and run.py apply it at start, docs/sacredfire/13-resolution.md).
inline constexpr int RenderResHeights[] = {480, 720, 1080, 1440, 1620, 2160};
inline constexpr int RenderResCount = 6;
/// Live output resolutions: the upscaler's output and the UI host targets.
/// v0.3: 16:9 modes plus the ultrawide outputs (Settings > Display > Aspect ratio).
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840, 2560, 3440, 5120};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160, 1080, 1440, 1440};
inline constexpr int OutputCount = 7;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

/// SacredFire: controller ports for split screen (bbport.ini port.1..4: SDL GUID or part of the
/// gamepad name; the runtime re-reads the file when the menu saves it).
constexpr int ControllerPorts = 4;
/// SacredFire: the upscaler (bbport.ini temporal=off|fsr1|fsr3|dlss|fsr4): FSR 1 (spatial, the
/// display pass's stretch) or temporal upscaling/AA of the finished frame (vk_ctr_fsr3; FSR 3
/// where DLSS cannot run, FSR 4 where INT8 and its models allow).
enum Temporal : int { TemporalOff = 0, TemporalFsr1 = 1, TemporalFsr3 = 2, TemporalDlss = 3,
                      TemporalFsr4 = 4, TemporalCount = 5 };
inline constexpr const char* TemporalNames[TemporalCount] = {"off", "fsr1", "fsr3", "dlss", "fsr4"};
/// SacredFire: frame generation (bbport.ini framegen=off|fsr3): AMD FSR 3.1.6 frame
/// interpolation of the finished frame at flip time (vk_ctr_framegen, docs/sacredfire/
/// 16-framegen.md). One generated frame is shown before every real one.
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenCount = 2 };
inline constexpr const char* FrameGenNames[FrameGenCount] = {"off", "fsr3"};
/// bbport.ini dlss_quality: auto (render_res decides the render size) or one of NVIDIA's modes,
/// which set the render size from dlss_output (scripts/patches.py render_plan; at start).
inline constexpr const char* DlssQualityNames[] = {"auto", "dlaa", "quality", "balanced",
                                                   "performance", "ultra_performance"};
inline constexpr int DlssQualityCount = 6;
/// bbport.ini dlss_output: the picture DLSS outputs with a quality mode (1080p or 4K).
inline constexpr int DlssOutputHeights[] = {1080, 2160};
/// bbport.ini dlss_model: transformer (NVIDIA's default presets K/L/M) or cnn (preset E).
inline constexpr const char* DlssModelNames[] = {"transformer", "cnn"};

struct Values {
    std::array<std::array<char, 64>, ControllerPorts> ports{}; ///< guarded by PortsMutex()
    std::atomic<MenuLanguage> menu_language{MenuLanguage::Russian};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// SacredFire: frames per second the present thread lets through (0: no limit). The 60 FPS
    /// patch lets the game run as fast as the machine goes; 60 is the pace it was made for.
    /// Applies at once. BB_FPS_LIMIT overrides it.
    std::atomic<int> fps_limit{60};
    /// SacredFire: frame generation (FrameGen; bbport.ini framegen=off|fsr3, BB_FRAMEGEN):
    /// one interpolated frame between each pair of game frames, presented halfway between
    /// them (the display shows twice the game's rate; the game, its FPS limit and the guest
    /// vblank are untouched). Works with any upscaler; FSR 3/4 and DLSS add the game's
    /// motion vectors. Applies live.
    std::atomic<int> framegen{FrameGenOff};
    /// SacredFire: frame generation's motion (bbport.ini framegen_motion=flow|game,
    /// BB_FRAMEGEN_MOTION): 0 optical flow alone (default; CTR's velocity leaves moving karts
    /// with the camera's motion, which smears them), 1 the game's motion vectors plus optical
    /// flow when a temporal upscaler wrote them. Applies at once (history reset).
    std::atomic<int> framegen_motion{0};
    /// SacredFire: graphics pipelines compiled on worker threads (BB_ASYNC_PIPELINES): a new
    /// shader no longer stops the game; a pass drawn every frame may miss a frame or two.
    /// Experimental (STATUS.md, issue #2). Applies at once. BB_ASYNC_PIPELINES overrides it.
    std::atomic<bool> async_pipelines{false};
    /// SacredFire: the game's 2x MSAA (G-buffer and forward passes; foliage's alpha edges need
    /// it). On by default: the dotted lines single-sample colour over the 2x depth buffer drew on
    /// NVIDIA (GitHub issues #16/#18) are fixed by truncating coverage in mixed-sample pipelines
    /// (vk_graphics_pipeline.cpp, VK_NV_coverage_reduction_mode). Read once at start;
    /// BB_NO_MSAA=1/0 overrides it. AmdGpu::BbNoMsaa() is the switch the renderer reads.
    std::atomic<bool> msaa{true};
    /// SacredFire: exclusive fullscreen on the game's display (BB_DISPLAY). Applies at once;
    /// BB_FULLSCREEN=1 at start. Saved to bbport.ini by the menu.
    std::atomic<bool> fullscreen{false};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<bool> game_patches[GamePatchCount]{}; ///< SacredFire: see GamePatches
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> render_res{-1}; ///< index into RenderResHeights, -1 when not set
    /// SacredFire: the upscaler (Temporal: off, FSR 1, FSR 3, DLSS).
    std::atomic<int> temporal{TemporalFsr1};
    std::atomic<int> dlss_quality{0}; ///< index into DlssQualityNames (at start)
    std::atomic<int> dlss_output{0};  ///< index into DlssOutputHeights (at start)
    std::atomic<int> dlss_model{0};   ///< index into DlssModelNames (live)
    // Older keys while loading (fsr1=, fsr3= before temporal=).
    bool temporal_seen = false;
    int legacy_fsr1 = -1;
    bool legacy_fsr3 = false;
    std::atomic<float> fsr3_sharpness{0.3f}; ///< FSR 3's RCAS, 0 off .. 1
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// v0.3 native ultrawide aspect (bbport.ini aspect_ratio): 0=16:9, 1=21:9, 2=32:9.
    /// Applied at start by run.py (needs a restart); upscaling is off while ultrawide.
    std::atomic<int> aspect_ratio{0};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    bool startup_game_patches[GamePatchCount]{};
    int startup_model_lod = 0;
    int startup_render_res = -1;
    int startup_temporal = TemporalFsr1, startup_dlss_quality = 0, startup_dlss_output = 0;
    int startup_output_res = OutputDefault;
    int startup_aspect_ratio = 0;
    int startup_live_resolution = 0;
};

/// SacredFire: whether the render size the settings ask for differs from the one the game
/// started with (render_res, or a DLSS quality mode and output): a restart applies it.
bool RenderSizeNeedsRestart();

Values& Get();
std::mutex& PortsMutex();
/// Localized overlay text; the Language selector itself stays in English.
const char* MenuText(const char* english, const char* russian);
/// v0.4: the Spanish of an English menu text (bbport_menu_es.cpp); the English one if missing.
const char* Spanish(const char* english);
/// An English-only label in the menu's language (Spanish when chosen, else English).
inline const char* Label(const char* english) { return MenuText(english, english); }

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
