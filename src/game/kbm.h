#pragma once
// Keyboard and mouse, and the controller's settings (kbm.cpp). The keys press the console pad's controls on the
// keyboard's port; the mouse turns the game's camera itself (camera.cpp) and stands in for the right stick only where
// no camera reads it.
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/input.h"

namespace warriors::kbm {

// what a key can be bound to: the console pad's controls in Pad order, then the sticks' directions and the walk key
enum Control : int {
    kA, kB, kX, kY, kBlack, kWhite, kLT, kRT,                        // Pad::analog
    kUp, kDown, kLeft, kRight, kStart, kBack, kLThumb, kRThumb,      // Pad::buttons bits
    kMoveUp, kMoveDown, kMoveLeft, kMoveRight, kWalk,                // left stick
    kLookUp, kLookDown, kLookLeft, kLookRight,                       // right stick
    kRage,                                                           // both triggers
    kControls
};

struct Action {
    const char* id;        // key in the settings file's [keys]
    const char* label;     // the settings panel's row
    const char* defaults;  // key names comma separated, up to two
};
const Action& ActionInfo(int control);
std::string BindingText(int control, int slot);  // the slot's key as the panel shows it, empty for none
void Bind(int control, int slot, int vk);  // vk 0 clears; a key another action has moves here
void ResetBindings();
int PressedKey();  // a bindable key or mouse button down now, 0 for none (the panel's capture)

// a pad control to press in place of each physical one (identity by default), for every controller
int PadMap(int control);  // control < kUp + 8
void SetPadMap(int control, int to);
const char* PadControlName(int control);

struct Settings {
    bool enabled = true;           // keyboard and mouse on port 1
    float mouse_sensitivity = 1.0f;
    bool invert_mouse_y = false;
    float mouse_smoothing = 0.0f;  // 0 none, up to 0.9: share of the last frame's turn kept
    bool menu_mouse = true;        // in menus the buttons press A and B and the wheel steps
    // the controller
    float stick_camera_speed = 1.0f;
    bool invert_stick_x = false, invert_stick_y = false;
    bool vibration = true;
    float vibration_strength = 1.0f;
    float stick_deadzone = 0.24f;  // the game's own, of full tilt
};
Settings& Get();

// host hooks (game_info.cpp)
void Load(const void* table);
void Save(void* table);
void Window(void* hwnd);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Pad(int port, input::Pad& pad);
void Rumble(int port, uint16_t& left, uint16_t& right);

// the settings panel holds the input: the cursor is free and nothing reaches the game
void SetPanelOpen(bool open);
bool PanelOpen();
bool TakePanelToggle();  // F10 went down since the last call
bool Captured();         // the game has the mouse: cursor hidden and kept in the window

// The camera (camera.cpp): raw mouse counts since the last call, turned into radians by the settings, positive x
// turns right and positive y looks up. Each call also notes the game is in play with a camera the mouse turns.
bool TakeLook(float& yaw, float& pitch);
// stick: the camera reads the right stick now; while it does not (the command menu, a fixed camera) the mouse is the
// right stick
void CameraSeen(bool stick);
bool InPlay();  // a camera took the mouse within the last moments
// The world's update ran (camera.cpp hooks 0x16D8D0): the game is in the world, not a front end menu
void WorldSeen();
// In the world with no camera the mouse turns (the lock pick, a car stereo): the mouse holds both sticks where it
// pushes them, as a stick stays where it is held
bool MouseStick();
// the rumble the game asked of the keyboard's port, 0 to 1, shown while MouseStick for want of a controller to feel
float RumbleLevel();
bool KeyboardActive();  // the keyboard or mouse was used after the controller's last new press
bool TestMode();        // XBE_KBM_TEST: driven by a script, diagnostics logged

// the game's button prompts (prompts.cpp): keys, while the keyboard and mouse were used last or no controller is
// plugged in, and the short name of the key bound to a control, empty for none
bool PromptKeys();
std::string PromptKey(int control);
// the title screen's "press any key" is up: any key or mouse button presses Start
void TitlePrompt();

}  // namespace warriors::kbm
