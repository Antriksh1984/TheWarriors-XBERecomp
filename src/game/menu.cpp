// The settings panel on F10: mouse and keyboard, key bindings, the controller and the display. It holds the input
// while open (kbm::SetPanelOpen) and saves to the settings file as things change.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "game/kbm.h"

namespace warriors {

namespace {

using Clock = std::chrono::steady_clock;
namespace kbm = warriors::kbm;

bool g_open = false;
int g_capture = -1, g_capture_slot = 0;  // the binding taking the next key
bool g_capture_armed = false;            // what was held when the capture started has been let go
bool g_dirty = false;
Clock::time_point g_changed;
std::vector<app::Mode> g_modes;

void Changed() {
    g_dirty = true;
    g_changed = Clock::now();
}

void SaveSoon(bool now) {
    if (g_dirty && (now || Clock::now() - g_changed > std::chrono::milliseconds(500))) {
        app::Save();
        g_dirty = false;
    }
}

void Open(bool open) {
    if (open == g_open) return;
    g_open = open;
    g_capture = -1;
    kbm::SetPanelOpen(open);
    if (!open) SaveSoon(true);
}

// the monitor's modes, largest first
void ReadModes() {
    g_modes.clear();
    HMONITOR mon = MonitorFromWindow(HWND(app::Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (std::none_of(g_modes.begin(), g_modes.end(),
                         [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; }))
            g_modes.push_back(m);
    }
    std::sort(g_modes.begin(), g_modes.end(), [](const app::Mode& a, const app::Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
}

void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.TabRounding = 4;
    st.GrabRounding = 4;
    st.WindowPadding = {18, 14};
    st.FramePadding = {10, 6};
    st.ItemSpacing = {10, 8};
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = {0.07f, 0.06f, 0.06f, 0.96f};
    c[ImGuiCol_Border] = {0.55f, 0.12f, 0.10f, 0.8f};
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg] = {0.42f, 0.07f, 0.06f, 1.0f};
    c[ImGuiCol_Tab] = {0.22f, 0.08f, 0.07f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.62f, 0.14f, 0.11f, 1.0f};
    c[ImGuiCol_TabActive] = {0.50f, 0.10f, 0.08f, 1.0f};
    c[ImGuiCol_FrameBg] = {0.17f, 0.14f, 0.14f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.30f, 0.16f, 0.14f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.40f, 0.14f, 0.12f, 1.0f};
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_CheckMark] = {0.90f, 0.30f, 0.22f, 1.0f};
    c[ImGuiCol_SliderGrabActive] = {1.0f, 0.42f, 0.32f, 1.0f};
    c[ImGuiCol_Button] = {0.30f, 0.10f, 0.09f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.55f, 0.14f, 0.11f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.70f, 0.18f, 0.14f, 1.0f};
    c[ImGuiCol_Header] = {0.30f, 0.10f, 0.09f, 1.0f};
    c[ImGuiCol_HeaderHovered] = {0.50f, 0.13f, 0.10f, 1.0f};
    c[ImGuiCol_HeaderActive] = {0.62f, 0.15f, 0.12f, 1.0f};
    c[ImGuiCol_Separator] = {0.35f, 0.12f, 0.10f, 1.0f};
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.62f, 0.58f, 0.56f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.45f, 0.35f, 1.0f});
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// a row: label left, the control from the middle
void Label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::SetNextItemWidth(-1);
}

bool Check(const char* label, bool& v) {
    Label(label);
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox("##v", &v);
    ImGui::PopID();
    if (changed) Changed();
    return changed;
}

bool Slider(const char* label, float& v, float lo, float hi, const char* fmt) {
    Label(label);
    ImGui::PushID(label);
    bool changed = ImGui::SliderFloat("##v", &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    if (changed) Changed();
    return changed;
}

void MouseTab() {
    kbm::Settings& s = kbm::Get();
    Section("KEYBOARD AND MOUSE");
    Check("Keyboard and mouse", s.enabled);
    Hint("Player 1's keys and mouse, together with a controller on the same port.");
    Section("MOUSE");
    Slider("Sensitivity", s.mouse_sensitivity, 0.1f, 5.0f, "%.2f");
    Check("Invert vertical look", s.invert_mouse_y);
    Slider("Smoothing", s.mouse_smoothing, 0.0f, 0.9f, s.mouse_smoothing < 0.005f ? "Off" : "%.2f");
    Check("Mouse buttons in menus", s.menu_mouse);
    Hint("The mouse turns the camera itself, one to one, as in Rockstar's PC games: no stick speed, no dead zone. "
         "In menus the left button selects, the right button backs out and the wheel scrolls.");
}

// a key box: click to bind, right click to clear
void KeyBox(int control, int slot, float width) {
    bool capturing = g_capture == control && g_capture_slot == slot;
    std::string text = capturing ? "Press a key..." : kbm::BindingText(control, slot);
    if (text.empty()) text = "-";
    ImGui::PushID(control * 2 + slot);
    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.75f, 0.22f, 0.16f, 1.0f});
    if (ImGui::Button(text.c_str(), {width, 0})) {
        g_capture = control;
        g_capture_slot = slot;
        g_capture_armed = false;
    }
    if (capturing) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        kbm::Bind(control, slot, 0);
        Changed();
    }
    ImGui::PopID();
}

// true while a capture runs or ended this frame, when Escape is the capture's
bool PollCapture() {
    if (g_capture < 0) return false;
    int vk = kbm::PressedKey();
    if (!g_capture_armed) {
        g_capture_armed = vk == 0;
        return true;
    }
    if (!vk) return true;
    if (vk != VK_ESCAPE) {
        kbm::Bind(g_capture, g_capture_slot, vk);
        Changed();
    }
    g_capture = -1;
    return true;
}

void BindingsTab() {
    Hint("Click a box and press a key or mouse button to bind it, right click to clear it. Escape cancels.");
    float box = std::max(110.0f, ImGui::GetContentRegionAvail().x * 0.24f);
    auto group = [&](const char* title, int from, int to) {
        Section(title);
        for (int c = from; c < to; c++) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kbm::ActionInfo(c).label);
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * box - ImGui::GetStyle().ItemSpacing.x);
            KeyBox(c, 0, box);
            ImGui::SameLine();
            KeyBox(c, 1, box);
        }
    };
    group("ON FOOT", kbm::kMoveUp, kbm::kWalk + 1);
    group("ACTIONS", kbm::kA, kbm::kRT + 1);
    group("D-PAD, START AND BACK", kbm::kUp, kbm::kRThumb + 1);
    group("RAGE", kbm::kRage, kbm::kRage + 1);
    group("CAMERA KEYS", kbm::kLookUp, kbm::kLookRight + 1);
    ImGui::Spacing();
    if (ImGui::Button("Restore default keys")) {
        kbm::ResetBindings();
        Changed();
    }
}

void ControllerTab() {
    kbm::Settings& s = kbm::Get();
    Section("CAMERA");
    Slider("Camera speed", s.stick_camera_speed, 0.25f, 3.0f, "%.2fx");
    Check("Invert horizontal", s.invert_stick_x);
    Check("Invert vertical", s.invert_stick_y);
    Slider("Stick dead zone", s.stick_deadzone, 0.05f, 0.5f, "%.2f");
    Section("VIBRATION");
    Check("Vibration", s.vibration);
    if (s.vibration) Slider("Strength", s.vibration_strength, 0.0f, 1.0f, "%.2f");
    Section("BUTTONS");
    Hint("What each button on the controller presses.");
    for (int c = 0; c < 16; c++) {
        Label(kbm::PadControlName(c));
        ImGui::PushID(100 + c);
        if (ImGui::BeginCombo("##map", kbm::PadControlName(kbm::PadMap(c)))) {
            for (int to = 0; to < 16; to++)
                if (ImGui::Selectable(kbm::PadControlName(to), kbm::PadMap(c) == to)) {
                    kbm::SetPadMap(c, to);
                    Changed();
                }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Restore default buttons")) {
        for (int c = 0; c < 16; c++) kbm::SetPadMap(c, c);
        Changed();
    }
}

void DisplayTab() {
    app::Settings& s = app::Get();
    Section("DISPLAY");
    const char* modes[] = {"Fullscreen", "Borderless", "Windowed"};
    int mode = int(s.display_mode);
    Label("Display mode");
    if (ImGui::Combo("##mode", &mode, modes, 3)) {
        s.display_mode = app::DisplayMode(mode);
        app::ApplyDisplay();
        Changed();
    }
    if (g_modes.empty()) ReadModes();
    char current[32];
    if (s.width && s.height) std::snprintf(current, sizeof(current), "%u x %u", s.width, s.height);
    else std::snprintf(current, sizeof(current), "Desktop");
    Label("Resolution");
    if (ImGui::BeginCombo("##res", current)) {
        if (ImGui::Selectable("Desktop", !s.width)) {
            s.width = s.height = 0;
            app::ApplyDisplay();
            Changed();
        }
        for (const app::Mode& m : g_modes) {
            char t[32];
            std::snprintf(t, sizeof(t), "%u x %u", m.width, m.height);
            if (ImGui::Selectable(t, s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                app::ApplyDisplay();
                Changed();
            }
        }
        ImGui::EndCombo();
    }
    Section("FRAME RATE");
    Check("VSync", s.vsync);
    Section("QUALITY");
    int render = int(s.render_percent);
    Label("Render scale");
    if (ImGui::SliderInt("##render", &render, 50, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        s.render_percent = uint32_t(render);
        Changed();
    }
    const char* aniso[] = {"As the game asks", "2x", "4x", "8x", "16x"};
    const uint32_t aniso_values[] = {0, 2, 4, 8, 16};
    int a = int(std::find(std::begin(aniso_values), std::end(aniso_values), s.aniso) - std::begin(aniso_values));
    Label("Anisotropic filtering");
    if (ImGui::Combo("##aniso", &a, aniso, 5)) {
        s.aniso = aniso_values[std::clamp(a, 0, 4)];
        Changed();
    }
    Hint("Anisotropic filtering applies to textures the game loads after the change.");
    const char* msaa[] = {"Off", "2x", "4x", "8x"};
    const uint32_t msaa_values[] = {1, 2, 4, 8};
    int m = int(std::find(std::begin(msaa_values), std::end(msaa_values), s.msaa) - std::begin(msaa_values));
    Label("Anti-aliasing (MSAA)");
    if (ImGui::Combo("##msaa", &m, msaa, 4)) {
        s.msaa = msaa_values[std::clamp(m, 0, 3)];
        Changed();
    }
    Hint("Smooths the edges of the 3D scene. The GPU may top out below 8x, then it uses the most it has.");
}

}  // namespace

void SettingsUi(double fps) {
    (void)fps;
    static bool styled = false;
    if (!styled) {
        Style();
        styled = true;
    }
    if (kbm::TakePanelToggle()) Open(!g_open);
    ImGuiIO& io = ImGui::GetIO();
    // while the game has the mouse ImGui leaves it and the cursor alone
    if (g_open) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    SaveSoon(false);
    if (!g_open) return;
    if (!PollCapture() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Open(false);
        return;
    }
    float scale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 2.5f);
    io.FontGlobalScale = scale;
    ImVec2 size{std::min(io.DisplaySize.x * 0.9f, 820.0f * scale), std::min(io.DisplaySize.y * 0.88f, 720.0f * scale)};
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    bool open = true;
    if (ImGui::Begin("SETTINGS", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Mouse & Keyboard")) {
                ImGui::BeginChild("page");
                MouseTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Key Bindings")) {
                ImGui::BeginChild("page");
                BindingsTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controller")) {
                ImGui::BeginChild("page");
                ControllerTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                ImGui::BeginChild("page");
                DisplayTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) Open(false);
}

}  // namespace warriors
