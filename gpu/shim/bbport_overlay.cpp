// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <miniz.h>
#include "bbport_perf.h"
#include "bbport_settings.h"
#include "../../src/runtime_friends.h"
#include "../../src/runtime_net_ping.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture: the trophy pictures
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
// OpenMenu (a game thread): opened on the window thread.
std::atomic<bool> open_request{false};
// SacredFire: the ultrawide question at start (first start, or every start with ultrawide_ask).
std::atomic<bool> ultrawide_prompt{false};
bool ultrawide_prompt_checked = false;
std::atomic<bool> pad_rescan_requested{false}; // SacredFire: the menu's "Rescan controllers"
// SacredFire: what each controller port plays with now (runtime_pad.c's pad thread, SetPadStatus).
struct PadStatus {
    std::string name; // "" none, "keyboard", or the gamepad's name
    bool pressing = false;
};
std::mutex pad_status_mutex;
PadStatus pad_status[4];
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;

// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// v0.4: notices over the game (an unlocked trophy), one at a time, each for a few seconds, in the
// launcher's look: the trophy's picture, its grade, name and what it was for.
struct Notice {
    std::string title, name, detail;
    int kind = 0; // trophy grade: 1 bronze, 2 silver, 3 gold, 4 platinum, 0 unknown
    std::shared_ptr<std::vector<unsigned char>> rgba; // the picture, RGBA, icon_size x icon_size
    ImTextureData* texture = nullptr;                // made from rgba on the present thread
    std::chrono::steady_clock::time_point start{};
};
std::mutex notice_mutex;
std::deque<Notice> notices;
constexpr float kNoticeSeconds = 7.0f;
constexpr int kIconSize = 128;
std::vector<std::unique_ptr<ImTextureData>> notice_textures; // kept: a handful per session

/// An 8-bit, non-interlaced PNG (grey, RGB, grey+alpha, RGBA) to RGBA, scaled to kIconSize.
bool DecodePng(const unsigned char* png, size_t size, std::vector<unsigned char>& out) {
    if (size < 33 || std::memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    auto be32 = [](const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
    uint32_t w = 0, h = 0;
    int channels = 0;
    std::vector<unsigned char> idat;
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t len = be32(png + at);
        const unsigned char* type = png + at + 4;
        if (at + 12 + len > size) return false;
        const unsigned char* data = png + at + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(data);
            h = be32(data + 4);
            const int depth = data[8], color = data[9], interlace = data[12];
            channels = color == 0 ? 1 : color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 0;
            if (depth != 8 || interlace || !channels || !w || !h || w > 4096 || h > 4096) return false;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    if (!channels || idat.empty()) return false;
    const size_t stride = size_t(w) * channels;
    std::vector<unsigned char> raw((stride + 1) * h);
    mz_ulong raw_size = mz_ulong(raw.size());
    if (mz_uncompress(raw.data(), &raw_size, idat.data(), mz_ulong(idat.size())) != MZ_OK || raw_size != raw.size()) return false;
    std::vector<unsigned char> pixels(stride * h);
    for (uint32_t y = 0; y < h; ++y) {
        const unsigned char filter = raw[y * (stride + 1)];
        const unsigned char* src = &raw[y * (stride + 1) + 1];
        unsigned char* row = &pixels[y * stride];
        const unsigned char* up = y ? &pixels[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(channels) ? row[x - channels] : 0, b = up ? up[x] : 0,
                      c = up && x >= size_t(channels) ? up[x - channels] : 0;
            int v = src[x];
            switch (filter) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            row[x] = (unsigned char)v;
        }
    }
    // Box filter down to kIconSize (trophy pictures are 240 x 240).
    out.assign(size_t(kIconSize) * kIconSize * 4, 0);
    for (int oy = 0; oy < kIconSize; ++oy) {
        for (int ox = 0; ox < kIconSize; ++ox) {
            const uint32_t x0 = ox * w / kIconSize, x1 = std::max(x0 + 1, (ox + 1) * w / kIconSize);
            const uint32_t y0 = oy * h / kIconSize, y1 = std::max(y0 + 1, (oy + 1) * h / kIconSize);
            uint32_t sum[4]{}, n = 0;
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = x0; x < x1; ++x, ++n) {
                    const unsigned char* px = &pixels[y * stride + x * channels];
                    const unsigned char r = px[0], g = channels >= 3 ? px[1] : px[0], bl = channels >= 3 ? px[2] : px[0];
                    const unsigned char al = channels == 4 ? px[3] : channels == 2 ? px[1] : 255;
                    sum[0] += r; sum[1] += g; sum[2] += bl; sum[3] += al;
                }
            }
            for (int k = 0; k < 4; ++k) out[(size_t(oy) * kIconSize + ox) * 4 + k] = (unsigned char)(sum[k] / n);
        }
    }
    return true;
}

/// The notice to draw now (its age in seconds), dropping finished ones. Present thread, with the
/// ImGui context: makes the picture's texture the first time.
Notice* CurrentNotice(float& age) {
    std::scoped_lock lock{notice_mutex};
    const auto now = std::chrono::steady_clock::now();
    while (!notices.empty()) {
        Notice& n = notices.front();
        if (n.start == std::chrono::steady_clock::time_point{}) {
            n.start = now;
        }
        age = std::chrono::duration<float>(now - n.start).count();
        if (age < kNoticeSeconds) {
            if (!n.texture && n.rgba && !n.rgba->empty()) {
                auto texture = std::make_unique<ImTextureData>();
                texture->Create(ImTextureFormat_RGBA32, kIconSize, kIconSize);
                std::memcpy(texture->GetPixels(), n.rgba->data(), n.rgba->size());
                texture->UsedRect = {0, 0, (unsigned short)kIconSize, (unsigned short)kIconSize};
                ImGui::RegisterUserTexture(texture.get());
                n.texture = texture.get();
                notice_textures.push_back(std::move(texture));
            }
            return &n;
        }
        notices.pop_front();
    }
    return nullptr;
}

// The launcher's palette (launcher/windows/launcher.cpp ApplyTheme).
const ImVec4 kMenuAccent{1.0f, 0.54f, 0.12f, 1.0f};
const ImVec4 kMenuGrey{0.58f, 0.61f, 0.70f, 1.0f};
const ImVec4 kMenuYellow{1.0f, 0.78f, 0.30f, 1.0f};
const ImVec4 kMenuText{0.92f, 0.93f, 0.96f, 1.0f};

void ApplyMenuTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 12.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.PopupRounding = 8.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.ScrollbarRounding = 8.0f;
    st.WindowPadding = ImVec2(16.0f, 12.0f);
    st.FramePadding = ImVec2(10.0f, 5.0f);
    st.ItemSpacing = ImVec2(10.0f, 7.0f);
    st.WindowBorderSize = 1.0f;
    st.TabBarOverlineSize = 2.0f;
    st.SeparatorTextBorderSize = 2.0f;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec4* c = st.Colors;
    const ImVec4 bg{0.067f, 0.075f, 0.102f, 0.96f}, panel{0.098f, 0.110f, 0.149f, 1.0f}, card{0.118f, 0.133f, 0.188f, 1.0f},
        frame{0.149f, 0.169f, 0.231f, 1.0f}, hover{0.184f, 0.208f, 0.314f, 1.0f}, active{0.220f, 0.251f, 0.388f, 1.0f},
        border{0.173f, 0.192f, 0.259f, 1.0f};
    c[ImGuiCol_Text] = kMenuText;
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.56f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.11f, 0.24f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.11f, 0.24f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = panel;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = hover;
    c[ImGuiCol_FrameBgActive] = active;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = active;
    c[ImGuiCol_CheckMark] = kMenuAccent;
    c[ImGuiCol_SliderGrab] = kMenuAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.63f, 0.26f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(1.0f, 0.54f, 0.12f, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.54f, 0.12f, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.54f, 0.12f, 0.45f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = card;
    c[ImGuiCol_TabSelectedOverline] = kMenuAccent;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = card;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = hover;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_NavCursor] = kMenuAccent;
    c[ImGuiCol_PlotHistogram] = kMenuAccent;
}

/// A section title in the accent colour.
void MenuSection(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kMenuAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
// Heights of the corner counters last drawn (the ping counter goes under the FPS one, notices
// under both); the ping one is 0 while it is not shown.
float corner_fps_height = 0.0f, corner_ping_height = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The widget runs before `v` is read: in one call, the order the arguments are evaluated in is
// unspecified (GCC went right to left; clang, the Windows build, left to right stored the old value).
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// RyuNET friends (src/runtime_friends.c): the Friends tab and its notices.

const char* FriendResultText(const std::string& action, const char* word) {
    const std::string w = word;
    if (w == "sent") {
        return action == "invite" ? BbSettings::MenuText("Invitation sent to %s", "Приглашение отправлено: %s")
                                  : BbSettings::MenuText("Friend request sent to %s", "Запрос отправлен: %s");
    }
    if (w == "friends") return BbSettings::MenuText("%s is now your friend", "%s теперь ваш друг");
    if (w == "removed") return BbSettings::MenuText("Removed: %s", "Удалено: %s");
    if (w == "busy") return BbSettings::MenuText("%s has too many pending requests", "У %s слишком много запросов");
    if (w == "full") return BbSettings::MenuText("The friend list is full", "Список друзей заполнен");
    if (w == "unknown") return BbSettings::MenuText("No registered player is called %s", "Нет игрока с ником %s");
    if (w == "not_friends") return BbSettings::MenuText("%s is not your friend", "%s не ваш друг");
    if (w == "none") return BbSettings::MenuText("That request is gone", "Этого запроса больше нет");
    if (w == "refused") return BbSettings::MenuText("The server refused it", "Сервер отказал");
    return BbSettings::MenuText("Could not reach the server", "Сервер недоступен");
}

/// Friend notices: checked every frame. The menu's options leave out friends coming online, or
/// every notice from friends (the Invite button's own result still shows).
void PollFriends() {
    const auto& s = BbSettings::Get();
    // A newer release (run.py update_notice: BB_UPDATE_AVAILABLE), once, after the boot screens.
    static const auto started = std::chrono::steady_clock::now();
    static bool update_told = false;
    if (!update_told && std::chrono::steady_clock::now() - started > std::chrono::seconds(15)) {
        update_told = true;
        if (const char* tag = std::getenv("BB_UPDATE_AVAILABLE"); tag && *tag) {
            Notify(BbSettings::MenuText("Update available", "Доступно обновление"), tag,
                   BbSettings::MenuText("Update from the launcher", "Обновите через лаунчер"), 6, nullptr, 0);
        }
    }
    static bool privacy_sent = false;
    if (!privacy_sent) { // the server learns "appear offline" once per game
        privacy_sent = true;
        runtime_friends_set_hidden(s.friends_hide_me);
    }
    // The server administrators' messages: always shown, whatever the
    // friends options say.
    char message[RF_MESSAGE];
    while (runtime_friends_take_message(message)) {
        Notify(BbSettings::MenuText("Message from the server", "Сообщение сервера"), message, "", 7, nullptr, 0);
    }
    char name[RF_NAME];
    for (int kind; (kind = runtime_friends_take_event(name));) {
        const bool own = kind == RF_EVENT_INVITED || kind == RF_EVENT_NO_ONE_ONLINE;
        if ((s.friends_no_notices && !own) || (s.friends_hide_activity && kind == RF_EVENT_ONLINE)) {
            continue;
        }
        const char *title = "", *detail = "";
        std::string shown = name;
        switch (kind) {
        case RF_EVENT_ONLINE:
            title = BbSettings::MenuText("Friend online", "Друг в сети");
            detail = BbSettings::MenuText("has come online", "в сети");
            break;
        case RF_EVENT_REQUEST:
            title = BbSettings::MenuText("Friend request", "Запрос в друзья");
            detail = BbSettings::MenuText("Menu (Insert / L3+R3) > Friends to accept",
                                          "Меню (Insert / L3+R3) > Друзья, чтобы принять");
            break;
        case RF_EVENT_INVITE:
            title = BbSettings::MenuText("Invitation", "Приглашение");
            detail = BbSettings::MenuText("invites you to their room: Menu > Friends > Join",
                                          "зовёт вас в свою комнату: Меню > Друзья > Войти");
            break;
        case RF_EVENT_INVITED: {
            // The game's Invite button: every friend online ("+N" for several).
            title = BbSettings::MenuText("Invitation sent", "Приглашение отправлено");
            if (name[0] == '+') {
                char count[64];
                std::snprintf(count, sizeof count, BbSettings::MenuText("%d friends", "друзей: %d"), std::atoi(name + 1));
                shown = count;
            }
            break;
        }
        case RF_EVENT_NO_ONE_ONLINE:
            title = BbSettings::MenuText("Invitation", "Приглашение");
            shown = BbSettings::MenuText("No friend online", "Нет друзей в сети");
            detail = BbSettings::MenuText("Add friends in Menu (Insert / L3+R3) > Friends",
                                          "Добавьте друзей: Меню (Insert / L3+R3) > Друзья");
            break;
        case RF_EVENT_ACCEPTED:
            title = BbSettings::MenuText("New friend", "Новый друг");
            detail = BbSettings::MenuText("accepted your request", "принял ваш запрос");
            break;
        default: continue;
        }
        Notify(title, shown, detail, 5, nullptr, 0);
    }
}

constexpr ImVec4 kFriendGreen{0.36f, 0.85f, 0.47f, 1.0f};

/// A filled green dot (online) or a grey ring (offline), the height of a line of text.
void StatusDot(bool online) {
    const float size = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 centre(p.x + size * 0.5f, p.y + ImGui::GetFrameHeight() * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (online) {
        dl->AddCircleFilled(centre, size * 0.3f, ImGui::GetColorU32(kFriendGreen));
    } else {
        dl->AddCircle(centre, size * 0.28f, ImGui::GetColorU32(kMenuGrey), 0, 1.5f * base_scale);
    }
    ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
}

// The Report tab's player (filled in by a search result's Report button in the Friends tab, which
// also brings that tab up).
char g_report_who[RF_NAME] = "";
bool g_show_report_tab = false;

// Report tab: a player, to the server's administrators.
void ReportSection(const RfView& view, char (&who)[RF_NAME]) {
    static int reason = 0;
    static char comment[RF_REPORT_COMMENT] = "";
    static bool sent_here = false;
    static const char* const reasons[] = {"cheating", "name", "behavior", "other"};
    MenuSection(BbSettings::MenuText("Report a player", "Пожаловаться на игрока"));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    ImGui::InputTextWithHint("##report_who", BbSettings::MenuText("Nickname", "Ник"), who, sizeof who);
    ImGui::SameLine();
    const char* labels[] = {BbSettings::MenuText("Cheating / hacks", "Читы / взлом"),
                            BbSettings::MenuText("Offensive name", "Оскорбительный ник"),
                            BbSettings::MenuText("Behavior", "Поведение"), BbSettings::MenuText("Other reason", "Другое")};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    ImGui::Combo("##report_reason", &reason, labels, 4);
    ImGui::TextDisabled("%s", BbSettings::MenuText("Comment (optional): what happened, which track, when",
                                                   "Комментарий (необязательно): что случилось, трасса, когда"));
    ImGui::InputTextMultiline("##report_comment", comment, 301,
                              ImVec2(ImGui::GetFontSize() * 23.0f, ImGui::GetTextLineHeight() * 3.5f));
    ImGui::BeginDisabled(view.reporting || std::strlen(who) < 3);
    if (ImGui::Button(BbSettings::MenuText("Send report", "Отправить жалобу"))) {
        runtime_friends_report(who, reasons[reason], comment);
        sent_here = true;
    }
    ImGui::EndDisabled();
    const std::string result = view.report_result;
    if (view.reporting) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Sending...", "Отправка..."));
    } else if (sent_here && !result.empty()) {
        if (result == "sent") {
            ImGui::TextColored(kFriendGreen, "%s", BbSettings::MenuText("Report sent. Thank you: the administrators will review it.",
                                                                       "Жалоба отправлена. Спасибо: администраторы её рассмотрят."));
            who[0] = '\0';
            comment[0] = '\0';
            reason = 0;
        } else {
            const char* why = result == "already"   ? BbSettings::MenuText("You already reported this player today.", "Вы уже жаловались на этого игрока сегодня.")
                              : result == "limit"   ? BbSettings::MenuText("You have sent the most reports for today.", "Лимит жалоб на сегодня исчерпан.")
                              : result == "unknown" ? BbSettings::MenuText("No registered player has that nickname.", "Нет игрока с таким ником.")
                              : result == "self"    ? BbSettings::MenuText("You cannot report yourself.", "Нельзя пожаловаться на себя.")
                                                    : BbSettings::MenuText("The report could not be sent; try again later.", "Не удалось отправить жалобу; попробуйте позже.");
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", why);
        }
        if (result == "sent") sent_here = false; // the fields are cleared; the next report starts anew
    }
}

/// The players in the room the game talks to, with their round trip (src/runtime_net_ping.h), as
/// lines of text: in the Room tab and in the corner counter (show_ping). Their count.
int PingLines() {
    PingPeer peers[PING_MAX_PEERS];
    const int count = runtime_net_ping_view(peers, PING_MAX_PEERS);
    int older = 0;
    for (int i = 0; i < count; ++i) {
        const PingPeer& p = peers[i];
        if (p.ms == PING_OLDER_PORT) {
            ++older;
            continue;
        }
        const char* name = p.name[0] ? p.name : "...";
        if (p.ms == PING_NO_ANSWER) {
            ImGui::TextColored(kMenuGrey, "%s  -", name);
            continue;
        }
        const ImVec4 color = p.ms < 80 ? kFriendGreen : p.ms < 150 ? kMenuYellow : ImVec4{1.0f, 0.42f, 0.38f, 1.0f};
        ImGui::Text("%s", name);
        ImGui::SameLine();
        ImGui::TextColored(color, "%d ms", p.ms);
    }
    if (older) {
        ImGui::TextColored(kMenuGrey, BbSettings::MenuText("+%d with an older version (no ping)",
                                                           "+%d со старой версией (без пинга)"),
                           older);
    }
    return count;
}

bool InOnlineRoom() {
    PingPeer peers[PING_MAX_PEERS];
    return runtime_net_ping_view(peers, PING_MAX_PEERS) > 0;
}

void RoomTab() {
    auto& s = BbSettings::Get();
    MenuSection(BbSettings::MenuText("Players in the room", "Игроки в комнате"));
    if (!InOnlineRoom()) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("You are not in an online room right now.",
                                                       "Сейчас вы не в онлайн-комнате."));
    } else {
        PingLines();
    }
    ImGui::Spacing();
    bool ping = s.show_ping;
    if (ImGui::Checkbox(BbSettings::MenuText("Ping counter in corner", "Счётчик пинга в углу"), &ping)) {
        s.show_ping = ping;
        BbSettings::Save();
    }
    ImGui::TextDisabled("%s", BbSettings::MenuText("Round trip to each player in your online room, in ms. Players with an "
                                                   "older version show no ping.",
                                                   "Задержка до каждого игрока в вашей онлайн-комнате, в мс. У игроков "
                                                   "со старой версией пинга нет."));
}

void ReportTab() {
    static RfView view;
    static unsigned seen = ~0u;
    if (const unsigned serial = runtime_friends_serial(); serial != seen) {
        runtime_friends_view(&view);
        seen = view.serial;
    }
    if (view.state == RF_OFF) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
                                     "Reporting a player needs online play on and a nickname of your own (launcher > Online).",
                                     "Для жалобы нужен онлайн и свой ник (лаунчер > Онлайн)."));
        return;
    }
    ReportSection(view, g_report_who);
}

void FriendsTab() {
    static RfView view;
    static unsigned seen = ~0u;
    static char query[RF_NAME] = "";
    static std::string last_action;
    if (const unsigned serial = runtime_friends_serial(); serial != seen) {
        runtime_friends_view(&view);
        seen = view.serial;
    }
    const ImVec4 green = kFriendGreen;
    auto act = [](const char* action, const char* who) {
        last_action = action;
        runtime_friends_action(action, who);
    };

    if (view.state == RF_OFF) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
                                     "Friends need online play on and a nickname of your own (launcher > Online).",
                                     "Для друзей нужен онлайн и свой ник (лаунчер > Онлайн)."));
        return;
    }
    if (view.state == RF_WAITING) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Connecting to the server...", "Подключение к серверу..."));
        return;
    }
    if (view.state == RF_REFUSED) {
        ImGui::TextColored(kMenuYellow, "%s",
                           BbSettings::MenuText("The server did not accept this nickname for friends: it must be "
                                                "registered (it is when you log in once with this launcher).",
                                                "Сервер не принял этот ник для друзей: он должен быть "
                                                "зарегистрирован."));
        return;
    }
    if (view.state == RF_UNREACHABLE) {
        ImGui::TextColored(kMenuYellow, "%s", BbSettings::MenuText("The server is not answering; retrying...",
                                                                   "Сервер не отвечает; повтор..."));
    }
    if (view.result[0] && !last_action.empty()) {
        char line[160];
        std::snprintf(line, sizeof line, FriendResultText(last_action, view.result), view.result_who);
        ImGui::TextColored(kMenuGrey, "%s", line);
    }

    if (view.invite_count) {
        MenuSection(BbSettings::MenuText("Invitations", "Приглашения"));
        for (int i = 0; i < view.invite_count; ++i) {
            const RfInvite& inv = view.invites[i];
            ImGui::PushID(i);
            ImGui::Text(BbSettings::MenuText("%s invites you to their room", "%s зовёт вас в комнату"), inv.from);
            ImGui::SameLine();
            if (ImGui::Button(BbSettings::MenuText("Join", "Войти"))) {
                runtime_friends_join(inv.from);
                SetOpen(false);
            }
            ImGui::PopID();
        }
    }
    if (view.incoming_count) {
        MenuSection(BbSettings::MenuText("Friend requests", "Запросы в друзья"));
        for (int i = 0; i < view.incoming_count; ++i) {
            ImGui::PushID(view.incoming[i]);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(view.incoming[i]);
            ImGui::SameLine();
            if (ImGui::Button(BbSettings::MenuText("Accept", "Принять"))) act("accept", view.incoming[i]);
            ImGui::SameLine();
            if (ImGui::Button(BbSettings::MenuText("Decline", "Отклонить"))) act("decline", view.incoming[i]);
            ImGui::PopID();
        }
    }

    MenuSection(BbSettings::MenuText("Add a friend", "Добавить друга"));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    const bool enter = ImGui::InputTextWithHint("##friend_search", BbSettings::MenuText("Nickname", "Ник"), query,
                                                sizeof query, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(BbSettings::MenuText("Search", "Найти")) || enter) && std::strlen(query) >= 2) {
        runtime_friends_search(query);
    }
    if (view.searching) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Searching...", "Поиск..."));
    } else if (view.search_query[0] && !view.search_count) {
        ImGui::TextDisabled(BbSettings::MenuText("No registered nickname starts with \"%s\"",
                                                 "Нет ников, начинающихся с \"%s\""),
                            view.search_query);
    }
    for (int i = 0; i < view.search_count; ++i) {
        const RfPlayer& p = view.search[i];
        const std::string relation = p.relation;
        ImGui::PushID(1000 + i);
        ImGui::AlignTextToFramePadding();
        StatusDot(p.online);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.name);
        ImGui::SameLine();
        if (relation == "friend") {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Friend", "Друг"));
        } else if (relation == "sent") {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Request sent", "Запрос отправлен"));
        } else if (relation == "received") {
            if (ImGui::Button(BbSettings::MenuText("Accept", "Принять"))) act("accept", p.name);
        } else if (ImGui::Button(BbSettings::MenuText("Add", "Добавить"))) {
            act("request", p.name);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(BbSettings::MenuText("Report", "Пожаловаться"))) {
            std::snprintf(g_report_who, sizeof g_report_who, "%s", p.name);
            g_show_report_tab = true;
        }
        ImGui::PopID();
    }
    if (view.outgoing_count) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", BbSettings::MenuText("Requests you sent:", "Ваши запросы:"));
        for (int i = 0; i < view.outgoing_count; ++i) {
            ImGui::PushID(2000 + i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(view.outgoing[i]);
            ImGui::SameLine();
            if (ImGui::SmallButton(BbSettings::MenuText("Cancel", "Отменить"))) act("remove", view.outgoing[i]);
            ImGui::PopID();
        }
    }

    char title[64];
    std::snprintf(title, sizeof title, BbSettings::MenuText("Friends (%d)", "Друзья (%d)"), view.friend_count);
    MenuSection(title);
    if (!view.friend_count) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("No friends yet: search for a nickname above.",
                                                       "Друзей пока нет: найдите ник выше."));
    }
    static std::string confirm_remove;
    auto friend_row = [&](const RfFriend& f) {
        ImGui::PushID(f.name);
        ImGui::AlignTextToFramePadding();
        StatusDot(f.online);
        ImGui::SameLine();
        ImGui::TextUnformatted(f.name);
        ImGui::SameLine();
        if (!f.online) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Offline", "Не в сети"));
        } else if (f.players > 1) {
            ImGui::TextColored(green, BbSettings::MenuText("In a room (%d/%d)", "В комнате (%d/%d)"), f.players, f.max);
        } else {
            ImGui::TextColored(green, "%s", BbSettings::MenuText("Online", "В сети"));
        }
        if (f.online) {
            ImGui::SameLine();
            if (ImGui::Button(BbSettings::MenuText("Invite", "Пригласить"))) act("invite", f.name);
        }
        ImGui::SameLine();
        if (confirm_remove == f.name) {
            if (ImGui::Button(BbSettings::MenuText("Sure? Remove", "Точно удалить?"))) {
                act("remove", f.name);
                confirm_remove.clear();
            }
        } else if (ImGui::SmallButton(BbSettings::MenuText("Remove", "Удалить"))) {
            confirm_remove = f.name;
        }
        ImGui::PopID();
    };
    // online friends first and open; offline ones folded (a long list hid everything below it)
    int online = 0;
    for (int i = 0; i < view.friend_count; ++i) {
        online += view.friends[i].online ? 1 : 0;
    }
    if (view.friend_count) {
        char head[64];
        std::snprintf(head, sizeof head, BbSettings::MenuText("Online (%d)###friends_online", "В сети (%d)###friends_online"), online);
        if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!online) {
                ImGui::TextDisabled("%s", BbSettings::MenuText("No friends online.", "Нет друзей в сети."));
            }
            for (int i = 0; i < view.friend_count; ++i) {
                if (view.friends[i].online) friend_row(view.friends[i]);
            }
        }
        if (view.friend_count > online) {
            std::snprintf(head, sizeof head, BbSettings::MenuText("Offline (%d)###friends_offline", "Не в сети (%d)###friends_offline"),
                          view.friend_count - online);
            if (ImGui::CollapsingHeader(head)) {
                for (int i = 0; i < view.friend_count; ++i) {
                    if (!view.friends[i].online) friend_row(view.friends[i]);
                }
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(BbSettings::MenuText("Options", "Настройки"))) {
        auto& s = BbSettings::Get();
        Checkbox(BbSettings::MenuText("Don't show friends' activity", "Не показывать активность друзей"),
                 s.friends_hide_activity);
        Hint(BbSettings::MenuText("No notice when a friend comes online.", "Без уведомлений, когда друг заходит в сеть."));
        bool hide_me = s.friends_hide_me;
        if (ImGui::Checkbox(BbSettings::MenuText("Don't show my activity", "Не показывать мою активность"), &hide_me)) {
            Store(s.friends_hide_me, hide_me, true);
            runtime_friends_set_hidden(hide_me);
        }
        Hint(BbSettings::MenuText("Your friends see you offline and get no notice when you come online.",
                                  "Друзья видят вас не в сети и не получают уведомлений."));
        Checkbox(BbSettings::MenuText("Don't show notifications", "Не показывать уведомления"), s.friends_no_notices);
        Hint(BbSettings::MenuText("No friend notices (requests, invitations, friends online). This tab still shows "
                                  "them.",
                                  "Без уведомлений от друзей (запросы, приглашения, друзья в сети). Эта вкладка "
                                  "их по-прежнему показывает."));
    }

}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (bbport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    // SacredFire: the menu title.
    if (!ImGui::Begin(
            BbSettings::MenuText("CTR Nitro-Fueled PC  ·  Insert / L3+R3###bbport_settings",
                                 "CTR Nitro-Fueled PC  ·  Insert / L3+R3###bbport_settings"),
            &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    ImGui::TextColored(kMenuGrey, BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                       frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    // sacredfire: bbport's temporal upscalers, reactive mask and output scaling read the scene of
    // the game bbport was built for and stay off here (bbport_settings.cpp); their sections show
    // only with BB_UPSCALER_MENU=1, for experiments.
    static const bool upscaler_menu = std::getenv("BB_UPSCALER_MENU") != nullptr;
    if (upscaler_menu) {
    ImGui::SeparatorText(BbSettings::MenuText("Temporal upscaler", "Временной апскейлер"));
    const char* upscalers[] = {
        BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
        BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)")};
    static const char* later[] = {"DLSS", "XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(BbSettings::MenuText("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                                                     : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                         "— не поддерживается видеокартой"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BbSettings::MenuText("— in development", "— в работе"));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           BbSettings::MenuText("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"),
                           problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(BbSettings::MenuText(
                "The mode selected above is active. You can select FSR 4 again.",
                "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                "(output matches the DLL). One model for Native through Performance and another "
                "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL "
                "and Proton).",
                "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                "Proton)."));
        } else {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause). Assets: tools/fetch_fsr4_assets.sh.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        Checkbox(BbSettings::MenuText("FSR 4: auto exposure", "FSR 4: авто-экспозиция"),
                 s.fsr4_auto_exposure);
        Checkbox(BbSettings::MenuText("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter"),
                 s.fsr4_invert_jitter);
        Hint(BbSettings::MenuText(
            "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
            "it to decide when to discard previous frames. Changes apply immediately, without a "
            "restart.",
            "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
            "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(BbSettings::MenuText("Preset", "Пресет"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(
                label, sizeof(label),
                BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                BbSettings::PresetName(i), scale,
                int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
            "TAA anti-aliases the scene at output resolution, without an FSR model or upscaling. "
            "Your saved FSR preset is restored when you select FSR.",
            "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
            "Сохранённый пресет FSR восстановится при выборе FSR."));
    }
    ImGui::Text(
        BbSettings::MenuText("Active scene render: %d x %d", "Активный рендер сцены: %d x %d"),
        s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(BbSettings::MenuText("Startup preset: %s", "Пресет при запуске: %s"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(BbSettings::MenuText(
                "When output is not 1080p, the entire game renders at the preset resolution "
                "(startup patch): "
                "this is fastest on Steam Deck and weaker GPUs. Preset or output resolution "
                "changes "
                "require a restart. Enable Live resolution changes below to change them without "
                "restarting (post-processing then stays at 1080p, which is slower).",
                "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."));
        } else {
            Hint(BbSettings::MenuText(
                "BB_RENDER_RES fixes the scene size at startup. Remove this explicit environment "
                "variable to change resolution and presets without restarting the game.",
                "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                "для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(BbSettings::MenuText(
            "Native AA: FSR acts as anti-aliasing. Other presets reduce the scene render "
            "resolution "
            "relative to the output. The UI renders at output resolution. "
            "The preset applies from the next frame without restarting the game.",
            "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение "
            "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
            "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(BbSettings::MenuText("Sharpening (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(BbSettings::MenuText("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(BbSettings::MenuText(
        "Up to 1: the upscaler's own sharpening (RCAS). Above 1 adds another RCAS pass. "
        "Ctrl+click the slider to enter an exact value.",
        "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
        "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Subpixel jitter", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(BbSettings::MenuText(
        "Each frame shifts the scene by a fraction of a pixel, letting the upscaler reconstruct "
        "more detail from multiple frames. Without it, only history-based anti-aliasing remains.",
        "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
        "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(BbSettings::MenuText("Reactive mask", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(BbSettings::MenuText("Enable mask", "Включить маску"), s.reactive);
    Hint(BbSettings::IsFsr4(s.upscaler)
             ? BbSettings::MenuText(
                   "Marks transparent effects (haze, light, water, particles). FSR 4 takes no "
                   "mask itself: its output is blended with the current frame there, so the "
                   "effects do not drag previous frames along.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы). FSR 4 сам маску не "
                   "принимает: там его результат смешивается с текущим кадром, и эффекты не "
                   "тянут за собой прошлые кадры.")
             : BbSettings::MenuText(
                   "Marks transparent effects (particles, haze) so the upscaler relies less on "
                   "previous frames. Reduces trails behind effects, but shimmering returns "
                   "underneath them.",
                   "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше "
                   "опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними "
                   "возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(BbSettings::MenuText("Scale", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(BbSettings::MenuText("Threshold", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(BbSettings::MenuText("Maximum", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(BbSettings::MenuText("Show mask (debug)", "Показать маску (отладка)"),
                        &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Character motion vectors", "Векторы движения персонажей"),
             s.object_motion);
    Hint(BbSettings::MenuText(
        "Accurate vectors for animated objects: clothing and weapons break up less "
        "during movement. The static scene does not get an additional pass. "
        "Changes apply after restarting the game.",
        "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
        "при движении. Статичная сцена не получает дополнительный проход. "
        "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(BbSettings::MenuText("Show motion vectors (debug)",
                                             "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(BbSettings::MenuText(
        "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
        "Blue: the pixel received an accurate object vector, not just camera motion. "
        "The upscaler treats a moving object with no blue or red/green as "
        "stationary, causing trails.",
        "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
        "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
        "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
        "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(BbSettings::MenuText("Output resolution", "Разрешение вывода"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(BbSettings::MenuText("Output resolution", "Разрешение вывода"),
                          outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(BbSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
    } else {
        Hint(BbSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Changing the size resets FSR history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
    }
    const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                BbSettings::MenuText("On", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(
            BbSettings::MenuText("Live resolution changes", "Смена разрешения на лету"),
            live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(BbSettings::MenuText(
        "On: output resolution and preset change without restarting, but game post-processing "
        "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
        "Off: everything renders at the preset resolution, and changes require a restart. Auto "
        "enables this on powerful discrete GPUs. Applies after restarting the game.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
        "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
        "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
        "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    }
    // Game patches (patches/CrashTeamRacing.xml): bbport's own game-effect patches went with its
    // Bloodborne profile (removed 2026-10-09).
    // v0.4: the menu in tabs, in the launcher's look (ApplyMenuTheme).
    if (!ImGui::BeginTabBar("menu_tabs")) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabItem(BbSettings::MenuText("General", "Общие"))) {
    MenuSection(BbSettings::MenuText("Display", "Экран"));
    {
        Checkbox(BbSettings::MenuText("Fullscreen", "Полный экран"), s.fullscreen);
        Hint(BbSettings::MenuText("Fullscreen on the game's display (BB_DISPLAY). "
                                  "Applies at once; BB_FULLSCREEN=1 at start.",
                                  "Полный экран на дисплее игры (BB_DISPLAY). "
                                  "Сразу; BB_FULLSCREEN=1 при запуске."));
        Checkbox(BbSettings::MenuText("Borderless window", "Без рамок"), s.borderless);
        Hint(BbSettings::MenuText("Remove window borders in windowed mode.",
                                  "Убрать рамку окна в оконном режиме."));
    }

    // SacredFire: the game's volume (runtime_audio.c reads it for every buffer).
    MenuSection(BbSettings::MenuText("Sound", "Звук"));
    {
        int volume = s.volume;
        const bool changed = ImGui::SliderInt(BbSettings::MenuText("Volume", "Громкость"), &volume, 0, 200, "%d%%");
        Store(s.volume, std::clamp(volume, 0, 200), changed);
        Hint(BbSettings::MenuText("100% is the level the game was mixed at; above it the loudest peaks are "
                                  "rounded off. Applies at once.",
                                  "100% — уровень, на котором сведена игра; выше самые громкие пики "
                                  "сглаживаются. Сразу."));
    }

    MenuSection(BbSettings::MenuText("Other", "Прочее"));
    Checkbox(BbSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу"), s.show_fps);
    Checkbox(BbSettings::MenuText("Ping counter in corner", "Счётчик пинга в углу"), s.show_ping);
    {
        // Where trophy and friend notices show (NoticeBox), in NoticePosition order.
        const char* places[] = {BbSettings::MenuText("Top right", "Вверху справа"),
                                BbSettings::MenuText("Top left", "Вверху слева"),
                                BbSettings::MenuText("Bottom right", "Внизу справа"),
                                BbSettings::MenuText("Bottom left", "Внизу слева"),
                                BbSettings::MenuText("Top centre", "Вверху по центру"),
                                BbSettings::MenuText("Bottom centre", "Внизу по центру")};
        int place = std::clamp(s.notice_position.load(), 0, int(BbSettings::NoticeCount) - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
        if (ImGui::Combo(BbSettings::MenuText("Notifications position", "Положение уведомлений"), &place, places,
                         int(BbSettings::NoticeCount))) {
            Store(s.notice_position, place, true);
            Notify(BbSettings::MenuText("Notifications", "Уведомления"), BbSettings::MenuText("They show here", "Они появятся здесь"),
                   "", 0, nullptr, 0);
        }
        Hint(BbSettings::MenuText("Trophies, friends and other notices. Applies at once.",
                                  "Трофеи, друзья и другие уведомления. Сразу."));
    }
    {
        // Always readable, whatever the menu's language.
        static const char* languages[] = {"English", "Русский", "Español"};
        int language = s.menu_language == BbSettings::MenuLanguage::Russian   ? 1
                       : s.menu_language == BbSettings::MenuLanguage::Spanish ? 2
                                                                              : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Language / Idioma", &language, languages, 3)) {
            s.menu_language = language == 1   ? BbSettings::MenuLanguage::Russian
                              : language == 2 ? BbSettings::MenuLanguage::Spanish
                                              : BbSettings::MenuLanguage::English;
            BbSettings::Save();
        }
    }
    ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem(BbSettings::MenuText("Friends", "Друзья"))) {
        FriendsTab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(BbSettings::MenuText("Room", "Комната"))) {
        RoomTab();
        ImGui::EndTabItem();
    }
    {
        const ImGuiTabItemFlags bring_up = g_show_report_tab ? ImGuiTabItemFlags_SetSelected : 0;
        g_show_report_tab = false;
        if (ImGui::BeginTabItem(BbSettings::MenuText("Report", "Пожаловаться"), nullptr, bring_up)) {
            ReportTab();
            ImGui::EndTabItem();
        }
    }

    const bool picture_tab = ImGui::BeginTabItem(BbSettings::MenuText("Picture", "Изображение"));
    if (picture_tab) {
    MenuSection(BbSettings::MenuText("Resolution and upscaling", "Разрешение и апскейл"));
    // SacredFire: the scene resolution (render_res) replaces the 1080p Pro patch's checkbox; a
    // DLSS quality mode sets it instead (scripts/patches.py render_plan).
    const bool dlss_sized = s.temporal == BbSettings::TemporalDlss && s.dlss_quality > 0;
    // SacredFire: ultrawide (the monitor's own size instead of 16:9); it replaces render_res.
    const bool ultrawide = s.ultrawide == 1;
    if (BbSettings::UltrawideAvailable() || ultrawide) {
        bool on = ultrawide;
        char label[96];
        std::snprintf(label, sizeof label, "%s (%dx%d)", BbSettings::MenuText("Ultrawide", "Ультраширокий"),
                      s.display_w.load(), s.display_h.load());
        if (ImGui::Checkbox(label, &on)) {
            BbSettings::SetUltrawide(on);
            dirty = true;
            std::printf("Menu: ultrawide = %d (applies after the restart)\n", int(on));
        }
        Checkbox(BbSettings::MenuText("Ask about ultrawide at every start", "Спрашивать при каждом запуске"),
                 s.ultrawide_ask);
        Hint(BbSettings::MenuText(
            "Draws the game at the monitor's size instead of 16:9 with black bars. Replaces the "
            "internal resolution. Applies after restarting the game.",
            "Игра рисуется в размере монитора вместо 16:9. Применяется после перезапуска игры."));
    }
    {
        static const char* labels[] = {"854 x 480", "1280 x 720", "1920 x 1080",
                                       "2560 x 1440 (PS4 Pro)", "2880 x 1620", "3840 x 2160 (4K)"};
        const int current = s.render_res;
        const char* preview =
            dlss_sized    ? BbSettings::MenuText("(DLSS quality decides)", "(решает качество DLSS)")
            : current >= 0 ? labels[current]
                           : BbSettings::MenuText("(game patches decide)", "(решают патчи)");
        ImGui::BeginDisabled(dlss_sized || ultrawide);
        if (ImGui::BeginCombo(BbSettings::MenuText("Internal resolution", "Внутреннее разрешение"),
                              preview)) {
            for (int r = 0; r < BbSettings::RenderResCount; ++r) {
                if (ImGui::Selectable(labels[r], r == current)) {
                    Store(s.render_res, r, r != current);
                    std::printf("Menu: render_res = %d (applies after the restart)\n",
                                BbSettings::RenderResHeights[r]);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path "
            "and the picture is output at 4K; on a 1080p screen that smooths jagged edges. 4K needs "
            "a strong GPU.",
            "Размер, в котором игра рисует 3D-сцену. Выше 1080p используется режим PS4 Pro, кадр "
            "выводится в 4K; на экране 1080p это сглаживает края. Для 4K нужна мощная видеокарта."));
    }
    // SacredFire: one upscaler at a time (bbport.ini temporal): FSR 1 (spatial) or the temporal
    // ones, FSR 3, DLSS and FSR 4 (vk_ctr_fsr3). Live; DLSS's quality mode and output need a
    // restart. FSR 4 is listed only with its model files and an INT8-capable GPU.
    {
        static constexpr const char* labels[BbSettings::TemporalCount] = {
            "Off", "AMD FSR 1", "AMD FSR 3 (experimental)", "NVIDIA DLSS (experimental)",
            "AMD FSR 4 (experimental)"};
        const int current = std::clamp(s.temporal.load(), 0, BbSettings::TemporalCount - 1);
        const bool fsr4 = BbSettings::Fsr4Available();
        if (ImGui::BeginCombo(BbSettings::MenuText("Upscaling", "Апскейл"), BbSettings::Label(labels[current]))) {
            for (int t = 0; t < BbSettings::TemporalCount; ++t) {
                if (t == BbSettings::TemporalFsr4 && !fsr4 && t != current) continue;
                if (ImGui::Selectable(BbSettings::Label(labels[t]), t == current)) {
                    Store(s.temporal, t, t != current);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. "
            "FSR 3 and DLSS rebuild detail over several frames from the game's depth and motion, "
            "and smooth edges even at the same resolution. DLSS needs an NVIDIA RTX card and FSR 4 "
            "a GPU with INT8 support (FSR 3 runs otherwise). Applies at once.",
            "FSR 1 увеличивает сцену чётче простого растяжения. FSR 3 и DLSS восстанавливают "
            "детали за несколько кадров. DLSS нужна видеокарта RTX. Применяется сразу."));
    }
    if (s.temporal == BbSettings::TemporalDlss) {
        static constexpr const char* qualities[BbSettings::DlssQualityCount] = {
            "Auto (internal resolution)", "DLAA (native)", "Quality", "Balanced", "Performance",
            "Ultra Performance"};
        const int quality = std::clamp(s.dlss_quality.load(), 0, BbSettings::DlssQualityCount - 1);
        if (ImGui::BeginCombo(BbSettings::MenuText("DLSS quality", "Качество DLSS"), BbSettings::Label(qualities[quality]))) {
            for (int q = 0; q < BbSettings::DlssQualityCount; ++q) {
                if (ImGui::Selectable(BbSettings::Label(qualities[q]), q == quality)) {
                    Store(s.dlss_quality, q, q != quality);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Auto keeps the internal resolution above. The others render at NVIDIA's share of the "
            "DLSS output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA "
            "100%. Applies after the restart.",
            "Авто: внутреннее разрешение выше. Остальные: доля от вывода DLSS. После перезапуска."));
        if (quality > 0) {
            static constexpr const char* outputs[] = {"1920 x 1080", "3840 x 2160 (4K)"};
            const int output = s.dlss_output ? 1 : 0;
            if (ImGui::BeginCombo(BbSettings::MenuText("DLSS output", "Вывод DLSS"), outputs[output])) {
                for (int o = 0; o < 2; ++o) {
                    if (ImGui::Selectable(outputs[o], o == output)) {
                        Store(s.dlss_output, o, o != output);
                    }
                }
                ImGui::EndCombo();
            }
        }
        static constexpr const char* models[] = {"Transformer (recommended)", "CNN"};
        const int model = s.dlss_model ? 1 : 0;
        if (ImGui::BeginCombo(BbSettings::MenuText("DLSS model", "Модель DLSS"), BbSettings::Label(models[model]))) {
            for (int m = 0; m < 2; ++m) {
                if (ImGui::Selectable(BbSettings::Label(models[m]), m == model)) {
                    Store(s.dlss_model, m, m != model);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. "
            "CNN: the older model (preset E), lighter; NVIDIA marks it deprecated. Applies at once.",
            "Transformer: текущая модель NVIDIA. CNN: старая, легче. Применяется сразу."));
    }
    if (s.temporal == BbSettings::TemporalFsr3 || s.temporal == BbSettings::TemporalFsr4) {
        Slider(BbSettings::MenuText("FSR sharpness", "Резкость FSR"), s.fsr3_sharpness, 0.0f, 1.0f);
    }

    ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(BbSettings::MenuText("Game", "Игра"))) {
    MenuSection(BbSettings::MenuText("Game patches (restart required)", "Патчи игры (после перезапуска)"));
    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        const auto& patch = BbSettings::GamePatches[g];
        if (std::strcmp(patch.key, "patch_pro_1080p") == 0) continue; // see Internal resolution
        const bool before = s.game_patches[g];
        Checkbox(BbSettings::MenuText(patch.label, patch.label_ru), s.game_patches[g]);
        if (before != s.game_patches[g]) {
            std::printf("Menu: %s = %d (applies after the restart)\n", patch.key, int(s.game_patches[g]));
        }
    }
    Hint(BbSettings::MenuText("From patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch), applied to the executable at start.",
                              "Из patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch), применяются к исполняемому файлу при запуске."));

    ImGui::EndTabItem();
    }
    // SacredFire: controller ports for split screen. Each port takes one connected gamepad; the
    // runtime logs the matching user in and reads that pad for it. Port 1 is the player.
    if (ImGui::BeginTabItem(BbSettings::MenuText("Controllers", "Контроллеры"))) {
    MenuSection(BbSettings::MenuText("Controllers (split screen)", "Контроллеры (разделённый экран)"));
    {
        bool split_auto = s.split_auto;
        if (ImGui::Checkbox(BbSettings::MenuText("Assign controllers automatically (each one connected is the next player)",
                                                 "Назначать контроллеры автоматически (каждый подключённый — следующий игрок)"),
                            &split_auto)) {
            s.split_auto = split_auto;
            BbSettings::Save();
        }
        ImGui::TextDisabled("%s", BbSettings::MenuText("Players 2-4 left on (none) get the controllers in the order they were "
                                                       "connected. Off: one person can switch controllers freely.",
                                                       "Игроки 2-4 с (нет) получают контроллеры в порядке подключения. "
                                                       "Выкл.: один человек может свободно менять контроллеры."));
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        // Identical controllers share one GUID and name: they are listed once ("x2"), and choosing
        // that entry for two ports gives each port one of them (runtime_pad.c). Listed twice, they
        // were two menu items with one ImGui ID (issue #43).
        std::vector<std::pair<std::string, std::string>> pads; // guid, label
        std::vector<int> copies;
        for (int i = 0; ids && i < count; ++i) {
            char guid[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
            const auto known = std::find_if(pads.begin(), pads.end(), [&](const auto& pad) { return pad.first == guid; });
            if (known != pads.end()) {
                ++copies[size_t(known - pads.begin())];
                continue;
            }
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            pads.emplace_back(guid, std::string(name ? name : "gamepad") + "  [" + std::string(guid).substr(0, 8) + "]");
            copies.push_back(1);
        }
        SDL_free(ids);
        for (size_t i = 0; i < pads.size(); ++i) {
            if (copies[i] > 1) pads[i].second += "  x" + std::to_string(copies[i]);
        }
        bool changed = false;
        for (int p = 0; p < BbSettings::ControllerPorts; ++p) {
            std::string current;
            {
                std::lock_guard lk{BbSettings::PortsMutex()};
                current = s.ports[size_t(p)].data();
            }
            // Ports 2-4 can take the keyboard: that player plays on it and port 1 keeps its gamepad.
            const char* keyboard = BbSettings::MenuText("Keyboard", "Клавиатура");
            std::string label = current.empty() ? (p == 0 ? BbSettings::MenuText("(first / last used)", "(первый / последний использованный)") : BbSettings::MenuText("(none)", "(нет)")) : current;
            if (p > 0 && current == "keyboard") label = keyboard;
            for (const auto& [guid, text] : pads) {
                if (guid == current) label = text;
            }
            char title[32];
            std::snprintf(title, sizeof title, "%s %d", BbSettings::MenuText("Port", "Порт"), p + 1);
            if (ImGui::BeginCombo(title, label.c_str())) {
                if (ImGui::Selectable(p == 0 ? BbSettings::MenuText("(first / last used)", "(первый / последний использованный)") : BbSettings::MenuText("(none)", "(нет)"), current.empty())) {
                    std::lock_guard lk{BbSettings::PortsMutex()};
                    s.ports[size_t(p)][0] = 0;
                    changed = true;
                }
                if (p > 0 && ImGui::Selectable(keyboard, current == "keyboard")) {
                    std::lock_guard lk{BbSettings::PortsMutex()};
                    for (auto& other : s.ports) {  // one player on the keyboard
                        if (std::strcmp(other.data(), "keyboard") == 0) other[0] = 0;
                    }
                    std::snprintf(s.ports[size_t(p)].data(), s.ports[size_t(p)].size(), "keyboard");
                    changed = true;
                }
                for (size_t i = 0; i < pads.size(); ++i) {
                    const auto& [guid, text] = pads[i];
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(text.c_str(), guid == current)) {
                        std::lock_guard lk{BbSettings::PortsMutex()};
                        std::snprintf(s.ports[size_t(p)].data(), s.ports[size_t(p)].size(), "%s", guid.c_str());
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            // What the port plays with right now, automatic ports and identical controllers
            // included; green while it presses something, to tell the controllers apart.
            PadStatus now;
            {
                std::lock_guard lk{pad_status_mutex};
                now = pad_status[p];
            }
            ImGui::Indent();
            if (now.name.empty()) {
                ImGui::TextColored(kMenuGrey, "%s", BbSettings::MenuText("No controller", "Нет контроллера"));
            } else {
                const char* with = now.name == "keyboard" ? keyboard : now.name.c_str();
                ImGui::TextColored(now.pressing ? kFriendGreen : kMenuText, "%s %s%s",
                                   BbSettings::MenuText("Playing:", "Играет:"), with,
                                   now.pressing ? BbSettings::MenuText("  (pressing)", "  (нажато)") : "");
            }
            ImGui::Unindent();
        }
        if (changed) {
            BbSettings::Save();
        }
        ImGui::TextDisabled("%s", BbSettings::MenuText("Press a button on a controller: its player's line turns green.",
                                                       "Нажмите кнопку на контроллере: строка его игрока станет зелёной."));
        ImGui::TextDisabled("%s", BbSettings::MenuText("Ports 2-4 log in as Player 2-4 while their gamepad is connected "
                                                       "(the keyboard: always; port 1 then has the gamepad only)",
                                                 "Порты 2-4 входят как Player 2-4, пока подключён их геймпад "
                                                 "(клавиатура: всегда; у порта 1 тогда только геймпад)"));
    }

    // SacredFire: the Steam Deck sometimes shows a controller connected after launch only to a
    // fresh enumeration; the runtime restarts SDL's gamepad subsystem on the next input sample.
    if (ImGui::Button(BbSettings::MenuText("Rescan controllers", "Переопознать контроллеры"))) {
        pad_rescan_requested.store(true, std::memory_order_relaxed);
    }
    Hint(BbSettings::MenuText("For a controller connected after the game started and not seen by it.",
                              "Для контроллера, подключённого после запуска игры и не замеченного ею."));
    ImGui::EndTabItem();
    }

    // SacredFire: performance (both apply at once).
    if (ImGui::BeginTabItem(BbSettings::MenuText("Performance", "Производительность"))) {
    MenuSection(BbSettings::MenuText("Performance", "Производительность"));
    {
        static constexpr int limits[] = {0, 30, 60, 72, 90, 120, 144, 240};
        const char* names[] = {BbSettings::MenuText("No limit", "Без ограничения"), "30", "60",
                               BbSettings::MenuText("72 (half of 144 Hz)", "72 (половина 144 Гц)"),
                               "90", "120", "144", "240"};
        const int current = s.fps_limit;
        int index = -1;
        for (int i = 0; i < int(std::size(limits)); ++i) {
            if (limits[i] == current) {
                index = i;
            }
        }
        char other[32];
        std::snprintf(other, sizeof(other), "%d", current);
        if (ImGui::BeginCombo(BbSettings::MenuText("FPS limit", "Ограничение FPS"),
                              index >= 0 ? names[index] : other)) {
            for (int i = 0; i < int(std::size(limits)); ++i) {
                if (ImGui::Selectable(names[i], i == index)) {
                    Store(s.fps_limit, limits[i], true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "With the 60 FPS patch the game runs as fast as your PC allows. 60 is the pace it was "
            "made for; on a 144 Hz screen without G-Sync/FreeSync, 72 divides the refresh evenly "
            "(240 does the same on a 240 Hz screen). Applies at once.",
            "С патчем 60 FPS игра идёт так быстро, как позволяет ПК. 60 — темп, для которого она "
            "сделана; на экране 144 Гц без G-Sync/FreeSync 72 делит частоту ровно (240 — то же "
            "на экране 240 Гц). Сразу."));
        Checkbox(BbSettings::MenuText("Compile shaders in the background (experimental)",
                                      "Компилировать шейдеры в фоне (экспериментально)"),
                 s.async_pipelines);
        Hint(BbSettings::MenuText(
            "A shader seen for the first time no longer pauses the game: an effect may appear a "
            "frame or two late instead. Shaders seen before load at start either way. Experimental.",
            "Новый шейдер больше не останавливает игру: эффект может появиться на кадр-два позже. "
            "Уже виденные шейдеры загружаются при запуске в любом случае. Экспериментально."));
    }

    ImGui::EndTabItem();
    }
    ImGui::EndTabBar();

    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution ||
        BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        restart |= s.game_patches[g] != s.startup_game_patches[g];
    }
    restart |= BbSettings::RenderSizeNeedsRestart();
    if (restart) {
        ImGui::TextColored(kMenuYellow, "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }


    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    // bbport 0.51 (issues 74, 116): leaving the game from the pad (this menu opens with L3+R3). A
    // second press confirms; the window thread then closes like the window's own close button.
    static bool quit_armed;
    ImGui::SameLine();

    const char *quit_button_text = quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз для выхода")
                                              : BbSettings::MenuText("Quit game", "Выйти из игры");

    // keep a stable ID so changing the label preserves gamepad focus
    const std::string quit_button_text_with_id = std::string(quit_button_text) + "###quit_game";

    if (ImGui::Button(quit_button_text_with_id.c_str())) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to bbport.ini",
                                             "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                                                           : "");
    corner_fps_height = ImGui::GetWindowHeight();
    ImGui::End();
}

/// show_ping: the players in the online room and their round trip, in the top right corner under
/// the FPS counter; nothing outside a room.
void PingCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const float below = BbSettings::Get().show_fps ? corner_fps_height + 6.0f * base_scale : 0.0f;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad + below),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##ping", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    PingLines();
    corner_ping_height = ImGui::GetWindowHeight() + 6.0f * base_scale;
    ImGui::End();
}

// RyuNET server announcements ("maintenance in 5 minutes"): the launcher writes
// ryunet_announce.txt next to bbport.ini (id, level, expires, text) and each new one is drawn
// over the picture for a few seconds. No input is taken: the game goes on underneath.
struct Announcement {
    std::string text;
    int level = 0; // 0 info, 1 warn, 2 urgent
    std::chrono::steady_clock::time_point until{};
};
std::mutex announce_mutex;
Announcement announce;
std::string announce_last_id;
std::chrono::steady_clock::time_point announce_checked{};
std::filesystem::file_time_type announce_stamp{};
constexpr float kAnnounceSeconds = 12.0f;

std::filesystem::path AnnouncePath() {
    const char* config = std::getenv("BB_CONFIG");
    const std::filesystem::path dir =
        config && *config ? std::filesystem::path(config).parent_path() : std::filesystem::path();
    return dir / "ryunet_announce.txt";
}

/// At most once a second: a new announcement in the file starts showing.
void PollAnnouncement() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{announce_mutex};
    if (now - announce_checked < std::chrono::seconds(1)) {
        return;
    }
    announce_checked = now;
    std::error_code ec;
    const auto path = AnnouncePath();
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || stamp == announce_stamp) {
        return;
    }
    announce_stamp = stamp;
    std::ifstream in(path, std::ios::binary);
    std::string line, id, text, level;
    long long expires = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("id=", 0) == 0) {
            id = line.substr(3);
        } else if (line.rfind("level=", 0) == 0) {
            level = line.substr(6);
        } else if (line.rfind("expires=", 0) == 0) {
            expires = std::atoll(line.c_str() + 8);
        } else if (line.rfind("text=", 0) == 0) {
            text = line.substr(5);
        }
    }
    if (id.empty() || text.empty() || id == announce_last_id || expires <= static_cast<long long>(std::time(nullptr))) {
        return;
    }
    announce_last_id = id;
    announce.text = text;
    announce.level = level == "urgent" ? 2 : level == "warn" ? 1 : 0;
    announce.until = now + std::chrono::milliseconds(static_cast<int>(kAnnounceSeconds * 1000));
    std::printf("Overlay: server announcement (%s): %s\n", level.c_str(), text.c_str());
}

bool AnnouncementActive() {
    PollAnnouncement();
    std::scoped_lock lock{announce_mutex};
    return std::chrono::steady_clock::now() < announce.until;
}

void AnnouncementBanner() {
    Announcement a;
    {
        std::scoped_lock lock{announce_mutex};
        a = announce;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= a.until) {
        return;
    }
    const float left = std::chrono::duration<float>(a.until - now).count();
    const float alpha = std::clamp(std::min((kAnnounceSeconds - left) / 0.3f, left / 1.0f), 0.0f, 1.0f);
    static const ImVec4 colors[3] = {ImVec4(0.12f, 0.33f, 0.62f, 1.0f), ImVec4(0.72f, 0.47f, 0.05f, 1.0f),
                                     ImVec4(0.68f, 0.12f, 0.12f, 1.0f)};
    const ImVec4 bg = colors[std::clamp(a.level, 0, 2)];
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x * 0.7f, 900.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * base_scale, 10.0f * base_scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
    ImGui::Begin("##announcement", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextDisabled("RyuNET");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(a.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void NoticeBox(const Notice& n, float age) {
    // Slides in from the top, stays, fades out.
    const float in = std::min(age / 0.35f, 1.0f), out = std::clamp((kNoticeSeconds - age) / 0.6f, 0.0f, 1.0f);
    const float alpha = std::min(in, out);
    static const ImVec4 grades[] = {
        ImVec4(1.0f, 0.54f, 0.12f, 1.0f),  // unknown: the port's orange
        ImVec4(0.82f, 0.54f, 0.32f, 1.0f), // bronze
        ImVec4(0.80f, 0.83f, 0.88f, 1.0f), // silver
        ImVec4(1.0f, 0.80f, 0.29f, 1.0f),  // gold
        ImVec4(0.55f, 0.83f, 1.0f, 1.0f),  // platinum
        ImVec4(0.36f, 0.85f, 0.47f, 1.0f), // a friend (PollFriends)
        ImVec4(0.45f, 0.70f, 1.0f, 1.0f),  // an update (PollFriends)
        ImVec4(1.0f, 0.54f, 0.12f, 1.0f),  // a message from the server (PollFriends)
    };
    static const char* const grade_es[] = {"", "Bronce", "Plata", "Oro", "Platino"};
    static const char* const grade_en[] = {"", "Bronze", "Silver", "Gold", "Platinum"};
    const int kind = std::clamp(n.kind, 0, 7);
    const ImVec4 color = grades[kind];
    const bool spanish = BbSettings::Get().menu_language == BbSettings::MenuLanguage::Spanish ||
                         n.title.rfind("Logro", 0) == 0;
    const float s = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where (the menu's Notifications position): a corner, or the top centre. It slides in from the
    // nearest edge; at the top right it goes under the FPS counter when that shows.
    const auto& settings = BbSettings::Get();
    const int where = std::clamp(settings.notice_position.load(), 0, int(BbSettings::NoticeCount) - 1);
    const bool centre = where == BbSettings::NoticeTopCentre || where == BbSettings::NoticeBottomCentre;
    const bool right = where == BbSettings::NoticeTopRight || where == BbSettings::NoticeBottomRight;
    const bool bottom = where == BbSettings::NoticeBottomRight || where == BbSettings::NoticeBottomLeft ||
                        where == BbSettings::NoticeBottomCentre;
    const float width = (centre ? 520.0f : 440.0f) * s, icon = 84.0f * s, pad = 14.0f * s;
    const float margin = 22.0f * s, slide = (1.0f - in) * 80.0f * s;
    const float under_fps = where == BbSettings::NoticeTopRight
                                ? (settings.show_fps ? 34.0f * s : 0.0f) + corner_ping_height
                                : 0.0f;
    const float x = centre  ? viewport->WorkPos.x + viewport->WorkSize.x * 0.5f
                    : right ? viewport->WorkPos.x + viewport->WorkSize.x - margin
                            : viewport->WorkPos.x + margin;
    const float y = bottom ? viewport->WorkPos.y + viewport->WorkSize.y - margin + slide
                           : viewport->WorkPos.y + margin + under_fps - slide;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always,
                            ImVec2(centre ? 0.5f : right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.118f, 0.133f, 0.188f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(color.x, color.y, color.z, 0.55f));
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 c = ImGui::GetColorU32(color);
    // The picture (or a cup in the grade's colour), with a ring in that colour.
    if (n.texture) {
        dl->AddImageRounded(ImTextureRef(n.texture->GetTexRef()), p, ImVec2(p.x + icon, p.y + icon), ImVec2(0, 0),
                            ImVec2(1, 1), ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 10.0f * s);
    } else if (kind == 7) {
        // A message: a speech bubble with three dots.
        dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), ImGui::GetColorU32(ImVec4(0.149f, 0.169f, 0.231f, 1.0f)), 10.0f * s);
        const ImVec2 a(p.x + icon * 0.18f, p.y + icon * 0.22f), b(p.x + icon * 0.82f, p.y + icon * 0.66f);
        dl->AddRectFilled(a, b, c, icon * 0.12f);
        dl->AddTriangleFilled(ImVec2(p.x + icon * 0.30f, b.y - 1.0f), ImVec2(p.x + icon * 0.46f, b.y - 1.0f),
                              ImVec2(p.x + icon * 0.28f, p.y + icon * 0.82f), c);
        const ImU32 dot = ImGui::GetColorU32(ImVec4(0.149f, 0.169f, 0.231f, 1.0f));
        for (int i = 0; i < 3; ++i) {
            dl->AddCircleFilled(ImVec2(p.x + icon * (0.36f + 0.14f * i), p.y + icon * 0.44f), icon * 0.045f, dot);
        }
    } else if (kind == 6) {
        // An update: a download arrow over a tray, in the update colour.
        dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), ImGui::GetColorU32(ImVec4(0.149f, 0.169f, 0.231f, 1.0f)), 10.0f * s);
        const float cx = p.x + icon * 0.5f, w = icon * 0.09f;
        dl->AddRectFilled(ImVec2(cx - w, p.y + icon * 0.20f), ImVec2(cx + w, p.y + icon * 0.52f), c);
        dl->AddTriangleFilled(ImVec2(cx - icon * 0.22f, p.y + icon * 0.48f), ImVec2(cx + icon * 0.22f, p.y + icon * 0.48f),
                              ImVec2(cx, p.y + icon * 0.70f), c);
        dl->AddRectFilled(ImVec2(p.x + icon * 0.24f, p.y + icon * 0.76f), ImVec2(p.x + icon * 0.76f, p.y + icon * 0.82f), c,
                          2.0f * s);
    } else if (kind == 5) {
        // A friend: head and shoulders in the friend colour.
        dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), ImGui::GetColorU32(ImVec4(0.149f, 0.169f, 0.231f, 1.0f)), 10.0f * s);
        const float cx = p.x + icon * 0.5f;
        dl->AddCircleFilled(ImVec2(cx, p.y + icon * 0.38f), icon * 0.15f, c);
        dl->PathArcTo(ImVec2(cx, p.y + icon * 0.86f), icon * 0.27f, 3.14159265f, 6.28318531f);
        dl->PathFillConvex(c);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), ImGui::GetColorU32(ImVec4(0.149f, 0.169f, 0.231f, 1.0f)), 10.0f * s);
        const float w = icon * 0.5f, x = p.x + icon * 0.25f, top = p.y + icon * 0.2f;
        dl->AddRectFilled(ImVec2(x + w * 0.15f, top), ImVec2(x + w * 0.85f, top + w * 0.55f), c, w * 0.3f,
                          ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(x + w * 0.44f, top + w * 0.55f), ImVec2(x + w * 0.56f, top + w * 0.85f), c);
        dl->AddRectFilled(ImVec2(x + w * 0.22f, top + w * 0.85f), ImVec2(x + w * 0.78f, top + w), c, 2.0f * s);
    }
    dl->AddRect(p, ImVec2(p.x + icon, p.y + icon), c, 10.0f * s, 0, 2.0f * s);
    ImGui::Dummy(ImVec2(icon, icon));
    ImGui::SameLine(0.0f, 14.0f * s);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - icon - pad * 2.0f - 14.0f * s);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    std::string header = n.title;
    if (kind >= 1 && kind <= 4) header += std::string("  ·  ") + (spanish ? grade_es[kind] : grade_en[kind]);
    ImGui::TextColored(color, "%s", header.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(n.name.c_str());
    ImGui::PopFont();
    if (!n.detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMenuGrey);
        ImGui::TextUnformatted(n.detail.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::End();
}

// SacredFire: "Ultrawide?" with Yes / No and "Ask me every time". A choice that differs from the
// one the game started with restarts it (the size is patched into the executable at start).
void UltrawidePrompt() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##ultrawide", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Ultrawide %dx%d?", s.display_w.load(), s.display_h.load());
    ImGui::Separator();
    ImGui::TextUnformatted("Yes: the game fills the whole monitor (gameplay is drawn wider).");
    ImGui::TextUnformatted("No: 16:9 with black bars, as on the PS4.");
    ImGui::Spacing();
    int choice = -1;
    if (ImGui::Button("Yes, ultrawide")) choice = 1;
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("No, 16:9")) choice = 0;
    ImGui::Spacing();
    bool ask = s.ultrawide_ask;
    if (ImGui::Checkbox("Ask me every time the game starts", &ask)) {
        s.ultrawide_ask = ask;
    }
    ImGui::TextDisabled("Cross / Enter: choose   Insert or L3+R3: settings menu later");
    ImGui::End();
    if (choice >= 0) {
        BbSettings::SetUltrawide(choice == 1);
        BbSettings::Save();
        ultrawide_prompt = false;
        std::printf("Ultrawide: %s (ask every time: %d)\n", choice ? "on" : "off", int(s.ultrawide_ask));
        if (BbSettings::RenderSizeNeedsRestart()) {
            runtime_restart();
        }
    }
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ApplyMenuTheme();

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 96; // the font atlas and the trophy pictures
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    if (open_request.exchange(false)) {
        std::scoped_lock lock{imgui_mutex};
        if (initialized) {
            SetOpen(true);
        }
    }
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open || ultrawide_prompt;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (menu_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    PollFriends();
    if (initialized && !ultrawide_prompt_checked) {
        // Once, at the first presented frame: the first start, or every start with ultrawide_ask.
        ultrawide_prompt_checked = true;
        const auto& s = BbSettings::Get();
        if (BbSettings::UltrawideAvailable() && (s.ultrawide < 0 || s.ultrawide_ask)) {
            ultrawide_prompt = true;
        }
    }
    bool notice = false;
    {
        std::scoped_lock lock{notice_mutex};
        notice = !notices.empty();
    }
    return initialized && (menu_open || prompt_active || ultrawide_prompt || BbSettings::Get().show_fps ||
                           (BbSettings::Get().show_ping && InOnlineRoom()) || AnnouncementActive() || notice);
}

void Notify(const std::string& title, const std::string& name, const std::string& detail, int kind,
            const void* png, size_t png_size) {
    Notice n;
    n.title = title;
    n.name = name;
    n.detail = detail;
    n.kind = kind;
    if (png && png_size) {
        auto rgba = std::make_shared<std::vector<unsigned char>>();
        if (DecodePng(static_cast<const unsigned char*>(png), png_size, *rgba)) n.rgba = rgba;
    }
    std::scoped_lock lock{notice_mutex};
    if (notices.size() < 16) {
        notices.push_back(std::move(n));
    }
}

void OpenMenu() {
    open_request = true;
}

void QuitGame() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

bool MenuOpen() {
    return menu_open;
}

bool TakePadRescan() {
    return pad_rescan_requested.exchange(false, std::memory_order_relaxed);
}

void SetPadStatus(int port, const std::string& name, bool pressing) {
    if (port < 0 || port >= int(std::size(pad_status))) return;
    std::lock_guard lk{pad_status_mutex};
    pad_status[port].name = name;
    pad_status[port].pressing = pressing;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active || ultrawide_prompt;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    BbPerf::Frame(ms); // the performance diagnostic (bbport_perf.h)
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    corner_ping_height = 0.0f;
    if (BbSettings::Get().show_ping && !menu_open && InOnlineRoom()) {
        PingCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    {
        float age = 0.0f;
        if (Notice* notice = CurrentNotice(age)) {
            const Notice copy = *notice;
            NoticeBox(copy, age);
        }
    }
    AnnouncementBanner();
    if (ultrawide_prompt && !menu_open) {
        UltrawidePrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
