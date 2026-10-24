#include "app/app.h"
#include "game/kbm.h"

namespace warriors {
void StartApuClock();
void SettingsUi(double fps);  // menu.cpp
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"The Warriors",             // window and splash
    .short_title = L"The Warriors",       // message boxes
    .name = L"Warriors",                  // window classes, crash dumps, handover event
    .log = "warriors",                    // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Warriors"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                        // game settings kept in the settings file
    .load = warriors::kbm::Load,          // reads more from the settings file's toml::table
    .save = warriors::kbm::Save,          // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = warriors::StartApuClock,    // image in memory, before its entry point runs
    .window = warriors::kbm::Window,      // the game window is up
    .message = warriors::kbm::Message,    // window messages first; true takes one
    .ui = warriors::SettingsUi,           // ImGui drawing each frame
    .pad = warriors::kbm::Pad,            // changes a polled pad
    .rumble = warriors::kbm::Rumble,      // changes a rumble before it is set
};
