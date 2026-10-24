// Keyboard and mouse on the keyboard's port (port 1), and the controller's settings.
// Keys and mouse buttons come from the window's messages, the mouse's movement as raw input counts, which the camera
// turns into angles (camera.cpp): the game never sees the mouse as a stick there. Each key presses the console pad
// control it is bound to, merged with a controller on the same port.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "game/kbm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace warriors::kbm {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

// The wheel binds as two keys, codes Windows leaves unassigned. A notch holds what is bound to it for one tap, long
// enough for the game's 30 Hz reads to see it.
constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;
constexpr uint64_t kWheelTapMs = 80;
// a camera that took the mouse this recently is play; past it the game is in a menu, a movie or loading
constexpr uint64_t kPlayMs = 250;
// counts a camera did not take for this long are stale (a menu, a load) and are dropped, not turned all at once
constexpr uint64_t kStaleMs = 150;
// radians a count at sensitivity 1: 800 counts (an inch at 800 DPI) turn about 100 degrees
constexpr float kRadiansPerCount = 0.0022f;
// the mouse as the right stick where no camera reads it: counts a second for full tilt, over the last window
constexpr float kStickCountsPerSecond = 1500.0f;
constexpr uint64_t kStickWindowMs = 40;
// the walk key's tilt, past the game's 0.24 dead zone: a walk
constexpr float kWalkTilt = 0.55f;
constexpr float kStickMax = 32767.0f;

constexpr UINT kRecapture = WM_APP + 40;
// XBE_KBM_TEST: for driving the game from a script, posted keys count with the window behind others, the cursor is
// left alone, and this message adds mouse counts (wParam x, lParam y, signed)
constexpr UINT kTestMouse = WM_APP + 41;
const bool g_test = std::getenv("XBE_KBM_TEST") != nullptr;

Settings g_settings;

constexpr Action kActions[kControls] = {
    // what each does in play, from the game's own help text; A selects and B backs out in menus
    {"a", "Heavy attack, charge (A)", "Mouse2,Enter"},
    {"b", "Grab, throw (B)", "E,Backspace"},
    {"x", "Light attack (X)", "Mouse1"},
    {"y", "Action: jump, climb, mug, steal (Y)", "Space"},
    {"black", "Block (Black)", "F"},
    {"white", "White", "R"},
    {"left_trigger", "Sprint, let go of a target (LT)", "LShift"},
    {"right_trigger", "Target, reverse a grapple (RT)", "Q"},
    {"dpad_up", "D-pad up", "Up,1"},
    {"dpad_down", "D-pad down", "Down,2"},
    {"dpad_left", "D-pad left", "Left,3"},
    {"dpad_right", "D-pad right", "Right,4"},
    {"start", "Start: pause", "Escape"},
    {"back", "Back", "Tab"},
    {"left_thumb", "Gang commands (left stick click)", "C"},
    {"right_thumb", "Right stick click", "Mouse3,V"},
    {"forward", "Move forward", "W"},
    {"backward", "Move back", "S"},
    {"left", "Move left", "A"},
    {"right", "Move right", "D"},
    {"walk", "Walk", "LAlt"},
    {"look_up", "Look up", ""},
    {"look_down", "Look down", ""},
    {"look_left", "Look left", ""},
    {"look_right", "Look right", ""},
    {"rage", "Rage (both triggers)", "Z"},
};

constexpr const char* kPadNames[16] = {"A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger",
                                       "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "Back",
                                       "Left stick click", "Right stick click"};

struct Name {
    int vk;
    const char* name;   // in the settings file
    const char* label;  // in the panel, nullptr for the name
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "Mouse1", "Left mouse"}, {VK_RBUTTON, "Mouse2", "Right mouse"},
    {VK_MBUTTON, "Mouse3", "Middle mouse"}, {VK_XBUTTON1, "Mouse4", "Mouse 4"}, {VK_XBUTTON2, "Mouse5", "Mouse 5"},
    {kWheelUp, "WheelUp", "Wheel up"}, {kWheelDown, "WheelDown", "Wheel down"},
    {VK_SPACE, "Space", nullptr}, {VK_RETURN, "Enter", nullptr}, {VK_ESCAPE, "Escape", nullptr},
    {VK_TAB, "Tab", nullptr}, {VK_BACK, "Backspace", nullptr}, {VK_CAPITAL, "CapsLock", "Caps Lock"},
    {VK_LSHIFT, "LShift", "Left Shift"}, {VK_RSHIFT, "RShift", "Right Shift"},
    {VK_LCONTROL, "LCtrl", "Left Ctrl"}, {VK_RCONTROL, "RCtrl", "Right Ctrl"},
    {VK_LMENU, "LAlt", "Left Alt"}, {VK_RMENU, "RAlt", "Right Alt"},
    {VK_UP, "Up", "Up arrow"}, {VK_DOWN, "Down", "Down arrow"}, {VK_LEFT, "Left", "Left arrow"},
    {VK_RIGHT, "Right", "Right arrow"}, {VK_INSERT, "Insert", nullptr}, {VK_DELETE, "Delete", nullptr},
    {VK_HOME, "Home", nullptr}, {VK_END, "End", nullptr}, {VK_PRIOR, "PageUp", "Page Up"},
    {VK_NEXT, "PageDown", "Page Down"},
    {VK_OEM_MINUS, "Minus", "-"}, {VK_OEM_PLUS, "Equals", "="}, {VK_OEM_4, "LBracket", "["},
    {VK_OEM_6, "RBracket", "]"}, {VK_OEM_1, "Semicolon", ";"}, {VK_OEM_7, "Quote", "'"},
    {VK_OEM_COMMA, "Comma", ","}, {VK_OEM_PERIOD, "Period", "."}, {VK_OEM_2, "Slash", "/"},
    {VK_OEM_5, "Backslash", "\\"}, {VK_OEM_3, "Grave", "`"},
    {VK_NUMPAD0, "Num0", "Num 0"}, {VK_NUMPAD1, "Num1", "Num 1"}, {VK_NUMPAD2, "Num2", "Num 2"},
    {VK_NUMPAD3, "Num3", "Num 3"}, {VK_NUMPAD4, "Num4", "Num 4"}, {VK_NUMPAD5, "Num5", "Num 5"},
    {VK_NUMPAD6, "Num6", "Num 6"}, {VK_NUMPAD7, "Num7", "Num 7"}, {VK_NUMPAD8, "Num8", "Num 8"},
    {VK_NUMPAD9, "Num9", "Num 9"}, {VK_ADD, "NumPlus", "Num +"}, {VK_SUBTRACT, "NumMinus", "Num -"},
    {VK_MULTIPLY, "NumStar", "Num *"}, {VK_DIVIDE, "NumSlash", "Num /"}, {VK_DECIMAL, "NumDot", "Num ."},
    {VK_F1, "F1", nullptr}, {VK_F2, "F2", nullptr}, {VK_F4, "F4", nullptr}, {VK_F5, "F5", nullptr},
    {VK_F6, "F6", nullptr}, {VK_F7, "F7", nullptr}, {VK_F8, "F8", nullptr}, {VK_F9, "F9", nullptr},
    {VK_F11, "F11", nullptr}, {VK_F12, "F12", nullptr},
};
// F3 dumps the threads (the host) and F10 opens the settings panel: neither binds

int VkOf(std::string_view name) {
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    for (const Name& n : kNames)
        if (name == n.name) return n.vk;
    return 0;
}

std::string NameOf(int vk, bool label) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    for (const Name& n : kNames)
        if (n.vk == vk) return label && n.label ? n.label : n.name;
    return {};
}

bool Bindable(int vk) { return !NameOf(vk, false).empty(); }

std::array<std::array<std::atomic<int>, 2>, kControls> g_bind;
std::array<std::atomic<int>, 16> g_pad_map;

// keys and mouse buttons held, from the window's messages
std::array<std::atomic<bool>, 256> g_down;
// held when the panel closed (Escape, the click on Close): they press nothing until let go
std::array<std::atomic<bool>, 256> g_suppress;
std::array<std::atomic<uint64_t>, 2> g_wheel_until;  // up, down

std::atomic<bool> g_panel{false};
std::atomic<bool> g_toggle{false};
std::atomic<bool> g_captured{false};
std::atomic<uint64_t> g_camera_ms{0};
std::atomic<bool> g_stick_camera{false};
std::atomic<uint64_t> g_kbm_ms{0}, g_pad_ms{0};
std::atomic<uint64_t> g_pad_seen_ms{0};  // a controller was plugged in, on any port
std::atomic<uint64_t> g_title_ms{0};     // the title screen's prompt was drawn
constexpr uint64_t kTitleMs = 500;
// with no controller seen for this long the prompts show keys whatever was used last
constexpr uint64_t kNoPadMs = 2000;

std::mutex g_mouse_lock;
long g_dx = 0, g_dy = 0;  // counts not taken by a camera yet
uint64_t g_last_take = 0;
float g_smooth_yaw = 0, g_smooth_pitch = 0;
struct Move {
    uint64_t ms;
    long dx, dy;
};
std::array<Move, 64> g_moves{};  // the latest counts, for the mouse as a stick
size_t g_move_next = 0;

HWND g_hwnd = nullptr;

void ResetKeys() {
    for (auto& d : g_down) d = false;
}

void SetDown(int vk, bool down) {
    if (vk <= 0 || vk >= 256) return;
    g_down[vk] = down;
    if (!down) g_suppress[vk] = false;
}

bool Down(int vk) {
    if (vk == kWheelUp || vk == kWheelDown) return NowMs() < g_wheel_until[vk - kWheelUp];
    return vk > 0 && vk < 256 && g_down[vk] && !g_suppress[vk];
}

bool Held(int control) {
    return Down(g_bind[control][0]) || Down(g_bind[control][1]);
}

bool IsMouse(int vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2; }

// left and right of Shift, Ctrl and Alt, which WM_KEYDOWN reports as one
int KeyVk(WPARAM wp, LPARAM lp) {
    UINT scan = (lp >> 16) & 0xFF;
    bool extended = (lp >> 24) & 1;
    switch (wp) {
        case VK_SHIFT: return int(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
        default: return int(wp);
    }
}

void NoteKbm() { g_kbm_ms = NowMs(); }

bool Foreground() { return g_hwnd && (g_test || (GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd))); }

// the cursor is hidden and kept inside the window while the game has the mouse
void UpdateCapture() {
    bool want = g_settings.enabled && !g_panel && Foreground();
    if (g_test) {
        g_captured = want;
        return;
    }
    if (want) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        POINT a{r.left, r.top}, b{r.right, r.bottom};
        ClientToScreen(g_hwnd, &a);
        ClientToScreen(g_hwnd, &b);
        RECT clip{a.x, a.y, b.x, b.y};
        RECT now;
        if (!GetClipCursor(&now) || !EqualRect(&now, &clip)) ClipCursor(&clip);
        if (!g_captured) SetCursor(nullptr);
    } else if (g_captured) {
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (want != g_captured) {
        LOGI("kbm: mouse %s", want ? "captured" : "released");
        std::lock_guard lock(g_mouse_lock);
        g_dx = g_dy = 0;
    }
    g_captured = want;
}

void AddCounts(long dx, long dy) {
    if (!g_captured || (!dx && !dy)) return;
    NoteKbm();
    std::lock_guard lock(g_mouse_lock);
    g_dx += dx;
    g_dy += dy;
    g_moves[g_move_next++ % g_moves.size()] = {NowMs(), dx, dy};
}

void OnRawInput(HRAWINPUT handle) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(handle, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return;  // remote desktop and pen tablets: no counts to turn by
    AddCounts(m.lLastX, m.lLastY);
}

// the mouse's speed over the last moments, counts a second
void MouseSpeed(float& vx, float& vy) {
    std::lock_guard lock(g_mouse_lock);
    uint64_t now = NowMs();
    long sx = 0, sy = 0;
    for (const Move& m : g_moves)
        if (m.ms && now - m.ms <= kStickWindowMs) sx += m.dx, sy += m.dy;
    vx = sx * (1000.0f / kStickWindowMs);
    vy = sy * (1000.0f / kStickWindowMs);
}

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * kStickMax); }

// a stick's tilt with the player's dead zone in place of the game's 0.24: past the player's, the tilt starts where
// the game's starts counting
void Deadzone(int16_t& x, int16_t& y, float zone) {
    constexpr float kGame = 0.24f;
    float fx = x / kStickMax, fy = y / kStickMax, m = std::sqrt(fx * fx + fy * fy);
    if (std::fabs(zone - kGame) < 0.005f || m <= 0.0f) return;
    float out = m <= zone ? 0.0f : std::min(1.0f, kGame + (m - zone) / (1.0f - zone) * (1.0f - kGame));
    x = Axis(fx / m * out);
    y = Axis(fy / m * out);
}

// a physical control's state, to move it to another
struct Control16 {
    bool on;
    uint8_t analog;
};
Control16 ReadControl(const input::Pad& p, int c) {
    if (c < 8) return {p.analog[c] != 0, p.analog[c]};
    bool on = p.buttons & (1u << (c - 8));
    return {on, uint8_t(on ? 0xFF : 0)};
}

void RemapPad(input::Pad& p) {
    bool identity = true;
    for (int c = 0; c < 16; c++) identity &= g_pad_map[c] == c;
    if (identity) return;
    input::Pad out = p;
    out.buttons = 0;
    std::fill(std::begin(out.analog), std::end(out.analog), uint8_t(0));
    for (int c = 0; c < 16; c++) {
        Control16 v = ReadControl(p, c);
        if (!v.on) continue;
        int to = std::clamp(g_pad_map[c].load(), 0, 15);
        if (to < 8) out.analog[to] = std::max(out.analog[to], v.analog);
        else out.buttons |= uint16_t(1u << (to - 8));
    }
    p = out;
}

void Press(input::Pad& p, int control) {
    if (control < 8) p.analog[control] = 0xFF;
    else if (control < 16) p.buttons |= uint16_t(1u << (control - 8));
}

// the stick that is pushed further wins, so a controller and the keys can share the port
void MergeStick(int16_t& x, int16_t& y, float kx, float ky) {
    float px = x / kStickMax, py = y / kStickMax;
    if (kx * kx + ky * ky > px * px + py * py) x = Axis(kx), y = Axis(ky);
}

void Keys(input::Pad& p) {
    bool play = InPlay();
    bool menu_mouse = g_settings.menu_mouse && !play;
    if (Held(kRage)) Press(p, kLT), Press(p, kRT);
    if (NowMs() - g_title_ms < kTitleMs && PressedKey()) Press(p, kStart);
    for (int c = 0; c < 16; c++) {
        for (int slot = 0; slot < 2; slot++) {
            int vk = g_bind[c][slot];
            // in menus the mouse buttons press A and B, not what they press in play
            if (menu_mouse && IsMouse(vk)) continue;
            if (Down(vk)) Press(p, c);
        }
    }
    if (menu_mouse) {
        if (Down(VK_LBUTTON)) Press(p, kA);
        if (Down(VK_RBUTTON)) Press(p, kB);
        if (Down(kWheelUp)) Press(p, kUp);
        if (Down(kWheelDown)) Press(p, kDown);
    }
    float mx = float(Held(kMoveRight)) - float(Held(kMoveLeft));
    float my = float(Held(kMoveUp)) - float(Held(kMoveDown));
    if (mx || my) {
        float tilt = Held(kWalk) ? kWalkTilt : 1.0f, len = std::sqrt(mx * mx + my * my);
        MergeStick(p.lx, p.ly, mx / len * tilt, my / len * tilt);
    }
    float lx = float(Held(kLookRight)) - float(Held(kLookLeft));
    float ly = float(Held(kLookUp)) - float(Held(kLookDown));
    // the mouse is the right stick only where no camera turns by it (the command menu, a menu that reads the stick)
    if ((!play || !g_stick_camera) && g_captured) {
        float vx, vy;
        MouseSpeed(vx, vy);
        lx += vx / kStickCountsPerSecond;
        ly -= vy / kStickCountsPerSecond;
    }
    if (lx || ly) MergeStick(p.rx, p.ry, std::clamp(lx, -1.0f, 1.0f), std::clamp(ly, -1.0f, 1.0f));
}

std::vector<int> ParseKeys(std::string_view list) {
    std::vector<int> vks;
    while (!list.empty()) {
        size_t comma = list.find(',');
        std::string_view name = list.substr(0, comma);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (int vk = VkOf(name)) vks.push_back(vk);
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return vks;
}

void SetDefaults(int c) {
    std::vector<int> vks = ParseKeys(kActions[c].defaults);
    g_bind[c][0] = vks.size() > 0 ? vks[0] : 0;
    g_bind[c][1] = vks.size() > 1 ? vks[1] : 0;
}

// a slider's value as the settings file keeps it, 0.24 not 0.23999999463558197
double Round(float v) { return std::round(double(v) * 100.0) / 100.0; }

struct Init {
    Init() {
        for (int c = 0; c < kControls; c++) SetDefaults(c);
        for (int c = 0; c < 16; c++) g_pad_map[c] = c;
    }
} g_init;

}  // namespace

const Action& ActionInfo(int control) { return kActions[control]; }

std::string BindingText(int control, int slot) { return NameOf(g_bind[control][slot], true); }

void Bind(int control, int slot, int vk) {
    if (vk && !Bindable(vk)) return;
    // a key is on one action at a time
    for (int c = 0; vk && c < kControls; c++)
        for (int s = 0; s < 2; s++)
            if (g_bind[c][s] == vk) g_bind[c][s] = 0;
    g_bind[control][slot] = vk;
}

void ResetBindings() {
    for (int c = 0; c < kControls; c++) SetDefaults(c);
}

int PressedKey() {
    for (int vk = 1; vk < 256; vk++)
        if (g_down[vk] && Bindable(vk)) return vk;
    for (int vk : {kWheelUp, kWheelDown})
        if (Down(vk)) return vk;
    return 0;
}

int PadMap(int control) { return g_pad_map[control]; }
void SetPadMap(int control, int to) { g_pad_map[control] = std::clamp(to, 0, 15); }
const char* PadControlName(int control) { return kPadNames[std::clamp(control, 0, 15)]; }

Settings& Get() { return g_settings; }

void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Settings& s = g_settings;
    if (const toml::table* m = t["mouse"].as_table()) {
        s.enabled = (*m)["keyboard_and_mouse"].value_or(s.enabled);
        s.mouse_sensitivity = std::clamp((*m)["sensitivity"].value_or(double(s.mouse_sensitivity)), 0.05, 10.0);
        s.invert_mouse_y = (*m)["invert_y"].value_or(s.invert_mouse_y);
        s.mouse_smoothing = std::clamp((*m)["smoothing"].value_or(double(s.mouse_smoothing)), 0.0, 0.9);
        s.menu_mouse = (*m)["menu_buttons"].value_or(s.menu_mouse);
    }
    if (const toml::table* k = t["keys"].as_table()) {
        for (int c = 0; c < kControls; c++) {
            std::optional<std::string> v = (*k)[kActions[c].id].value<std::string>();
            if (!v) continue;
            std::vector<int> vks = ParseKeys(*v);
            g_bind[c][0] = vks.size() > 0 ? vks[0] : 0;
            g_bind[c][1] = vks.size() > 1 ? vks[1] : 0;
        }
    }
    if (const toml::table* p = t["controller"].as_table()) {
        s.stick_camera_speed = std::clamp((*p)["camera_speed"].value_or(double(s.stick_camera_speed)), 0.25, 3.0);
        s.invert_stick_x = (*p)["invert_x"].value_or(s.invert_stick_x);
        s.invert_stick_y = (*p)["invert_y"].value_or(s.invert_stick_y);
        s.vibration = (*p)["vibration"].value_or(s.vibration);
        s.vibration_strength = std::clamp((*p)["vibration_strength"].value_or(double(s.vibration_strength)), 0.0, 1.0);
        s.stick_deadzone = std::clamp((*p)["deadzone"].value_or(double(s.stick_deadzone)), 0.05, 0.5);
        if (const toml::table* map = (*p)["buttons"].as_table()) {
            for (int c = 0; c < 16; c++) {
                std::optional<std::string> v = (*map)[kActions[c].id].value<std::string>();
                for (int to = 0; v && to < 16; to++)
                    if (*v == kActions[to].id) g_pad_map[c] = to;
            }
        }
    }
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Settings& s = g_settings;
    toml::table m;
    m.insert("keyboard_and_mouse", s.enabled);
    m.insert("sensitivity", Round(s.mouse_sensitivity));
    m.insert("invert_y", s.invert_mouse_y);
    m.insert("smoothing", Round(s.mouse_smoothing));
    m.insert("menu_buttons", s.menu_mouse);
    t.insert_or_assign("mouse", std::move(m));
    toml::table k;
    for (int c = 0; c < kControls; c++) {
        std::string v = NameOf(g_bind[c][0], false);
        if (std::string alt = NameOf(g_bind[c][1], false); !alt.empty()) v += (v.empty() ? "" : ",") + alt;
        k.insert(kActions[c].id, v);
    }
    t.insert_or_assign("keys", std::move(k));
    toml::table p;
    p.insert("camera_speed", Round(s.stick_camera_speed));
    p.insert("invert_x", s.invert_stick_x);
    p.insert("invert_y", s.invert_stick_y);
    p.insert("vibration", s.vibration);
    p.insert("vibration_strength", Round(s.vibration_strength));
    p.insert("deadzone", Round(s.stick_deadzone));
    toml::table map;
    for (int c = 0; c < 16; c++) map.insert(kActions[c].id, kActions[g_pad_map[c]].id);
    p.insert("buttons", std::move(map));
    t.insert_or_assign("controller", std::move(p));
}

void Window(void* hwnd) {
    g_hwnd = HWND(hwnd);
    RAWINPUTDEVICE dev{0x01, 0x02, 0, g_hwnd};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&dev, 1, sizeof(dev))) LOGE("kbm: raw mouse input failed: %lu", GetLastError());
    LOGI("kbm: keyboard and mouse %s, sensitivity %g", g_settings.enabled ? "on" : "off", g_settings.mouse_sensitivity);
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    (void)hwnd;
    auto button = [](int vk, bool down) {
        SetDown(vk, down);
        if (down) NoteKbm();
        return false;
    };
    switch (msg) {
        case kTestMouse:
            if (!g_test) return false;
            AddCounts(long(intptr_t(wp)), long(lp));
            *result = 0;
            return true;
        case WM_INPUT:
            OnRawInput(HRAWINPUT(lp));
            return false;  // DefWindowProc frees the input
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (wp == VK_F10) {
                if (!(lp & (1 << 30))) g_toggle = true;
                *result = 0;
                return true;
            }
            SetDown(KeyVk(WPARAM(wp), LPARAM(lp)), true);
            NoteKbm();
            // Alt and F10 would open the window's system menu, which stops the game until it closes; Alt+F4 still
            // closes and Alt+Enter is the host's
            if (msg == WM_SYSKEYDOWN && wp != VK_F4 && wp != VK_RETURN) {
                *result = 0;
                return true;
            }
            return false;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            SetDown(KeyVk(WPARAM(wp), LPARAM(lp)), false);
            // Windows sends one key up for both Shift keys held together
            if (wp == VK_SHIFT) SetDown(VK_LSHIFT, false), SetDown(VK_RSHIFT, false);
            if (msg == WM_SYSKEYUP) {
                *result = 0;
                return true;
            }
            return false;
        }
        case WM_SYSCHAR:
            *result = 0;
            return true;  // no beep for Alt combinations
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                *result = 0;
                return true;
            }
            return false;
        case WM_LBUTTONDOWN: return button(VK_LBUTTON, true);
        case WM_LBUTTONUP: return button(VK_LBUTTON, false);
        case WM_RBUTTONDOWN: return button(VK_RBUTTON, true);
        case WM_RBUTTONUP: return button(VK_RBUTTON, false);
        case WM_MBUTTONDOWN: return button(VK_MBUTTON, true);
        case WM_MBUTTONUP: return button(VK_MBUTTON, false);
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            button(HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, msg == WM_XBUTTONDOWN);
            *result = TRUE;
            return true;
        case WM_MOUSEWHEEL: {
            int notch = GET_WHEEL_DELTA_WPARAM(wp);
            if (notch) g_wheel_until[notch > 0 ? 0 : 1] = NowMs() + kWheelTapMs;
            NoteKbm();
            return false;
        }
        case WM_SETCURSOR:
            if (g_captured && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                *result = TRUE;
                return true;
            }
            return false;
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_KILLFOCUS:
            if (msg == WM_KILLFOCUS || LOWORD(wp) == WA_INACTIVE || (msg == WM_ACTIVATEAPP && !wp)) ResetKeys();
            [[fallthrough]];
        case WM_SETFOCUS:
        case WM_SIZE:
        case WM_MOVE:
        case WM_EXITSIZEMOVE:
        case kRecapture:
            // the window may come to the front after this message: look again once it has
            UpdateCapture();
            if (msg != kRecapture) PostMessageW(g_hwnd, kRecapture, 0, 0);
            if (msg == kRecapture) {
                *result = 0;
                return true;
            }
            return false;
    }
    return false;
}

void Pad(int port, input::Pad& p) {
    if (p.connected) {
        g_pad_seen_ms = NowMs();
        RemapPad(p);
        Deadzone(p.lx, p.ly, g_settings.stick_deadzone);
        Deadzone(p.rx, p.ry, g_settings.stick_deadzone);
        if (g_settings.invert_stick_x) p.rx = int16_t(-1 - p.rx);
        if (g_settings.invert_stick_y) p.ry = int16_t(-1 - p.ry);
        if (port == 0) {
            static std::atomic<uint16_t> last{0};
            uint16_t held = p.buttons;
            for (int i = 0; i < 8; i++) held |= p.analog[i] ? uint16_t(0x100u << i) : 0;
            if (held & ~last.exchange(held)) g_pad_ms = NowMs();
        }
    }
    if (port == 0 && g_settings.enabled) {
        p.connected = true;
        if (Foreground()) Keys(p);
    }
    if (g_panel) {
        // the panel has the input; the port stays connected
        bool connected = p.connected;
        p = {};
        p.connected = connected;
    }
}

void Rumble(int port, uint16_t& left, uint16_t& right) {
    (void)port;
    float k = g_settings.vibration ? g_settings.vibration_strength : 0.0f;
    left = uint16_t(left * k);
    right = uint16_t(right * k);
}

void SetPanelOpen(bool open) {
    g_panel = open;
    if (open) ResetKeys();
    else
        for (int vk = 0; vk < 256; vk++) g_suppress[vk] = g_down[vk].load();
    if (g_hwnd) PostMessageW(g_hwnd, kRecapture, 0, 0);
}
bool PanelOpen() { return g_panel; }
bool TakePanelToggle() { return g_toggle.exchange(false); }
bool Captured() { return g_captured; }

bool TakeLook(float& yaw, float& pitch) {
    long dx, dy;
    uint64_t now = NowMs();
    {
        std::lock_guard lock(g_mouse_lock);
        dx = g_dx, dy = g_dy;
        g_dx = g_dy = 0;
        bool stale = now - g_last_take > kStaleMs;
        g_last_take = now;
        if (stale) dx = dy = 0;
    }
    float k = kRadiansPerCount * g_settings.mouse_sensitivity;
    float y = dx * k, p = (g_settings.invert_mouse_y ? dy : -dy) * k;
    float keep = g_settings.mouse_smoothing;
    if (keep > 0.0f) {
        y = g_smooth_yaw = g_smooth_yaw * keep + y * (1.0f - keep);
        p = g_smooth_pitch = g_smooth_pitch * keep + p * (1.0f - keep);
        if (std::fabs(y) < 1e-5f) y = g_smooth_yaw = 0;
        if (std::fabs(p) < 1e-5f) p = g_smooth_pitch = 0;
    }
    yaw = y, pitch = p;
    return y != 0.0f || p != 0.0f;
}

void CameraSeen(bool stick) {
    g_camera_ms = NowMs();
    g_stick_camera = stick;
}
bool InPlay() { return NowMs() - g_camera_ms < kPlayMs; }
bool KeyboardActive() { return g_kbm_ms > g_pad_ms; }
bool TestMode() { return g_test; }

void TitlePrompt() { g_title_ms = NowMs(); }

bool PromptKeys() { return g_settings.enabled && (KeyboardActive() || NowMs() - g_pad_seen_ms > kNoPadMs); }

std::string PromptKey(int control) {
    int vk = g_bind[control][0] ? g_bind[control][0].load() : g_bind[control][1].load();
    switch (vk) {
        case 0: return {};
        case VK_LBUTTON: return "LMB";
        case VK_RBUTTON: return "RMB";
        case VK_MBUTTON: return "MMB";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_LSHIFT: case VK_RSHIFT: return "Shift";
        case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
        case VK_LMENU: case VK_RMENU: return "Alt";
        case VK_ESCAPE: return "Esc";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        default: return NameOf(vk, true);
    }
}

}  // namespace warriors::kbm
