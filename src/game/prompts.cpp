// The game's button prompts as keys, while the keyboard and mouse are in use.
// Tags: the text parser 0xE2580 turns a button tag (<S>, <L2>, ...: the PS2's names, which the Xbox build draws as
// the Xbox pad's buttons) into one character of the font's button icons and writes it with sprintf(word, "%c",
// glyph), then measures and draws the word as a run of text in the current font. After the sprintf the word gets the
// name of the key bound to the pad control the tag stands for, so "Press <S> for a light attack" reads "Press [LMB]".
// The game's controller configurations draw some tags as other buttons ([[0x5B9F78]+0x11C], 0 the default); a tag
// is changed only when its icon is the default configuration's, so another configuration keeps the pad's icons.
// Words: the Xbox text also names the pad's parts in words ("click the left thumbstick", "PRESS START", "pull and
// release both triggers"). The text object's draw (0xE3B50) and measure (0xE3CD0) word wrap and parse the string at
// +0x3C (+0x40 the one they put back, +0x34 one shown instead); for the call those are copies with the words as keys,
// so the wrap and the layout see the words that are drawn. The front end's menus draw and measure whole runs of text
// with 0x96660 and 0x964A0, where the run is the copy. English only.
// Both read the bindings and which was used last, the keys or a controller, each time the text is drawn.
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/log.h"
#include "game/kbm.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

namespace kbm = warriors::kbm;

constexpr int kAnyGlyph = -1;
struct Tag {
    uint32_t index;  // in the tag table at 0x45A610, 61 bytes an entry
    int glyph;       // the icon the default configuration draws, kAnyGlyph for the animated stick
    int control;
};
// the Xbox build's input conversion (0x6FC00) gives the PS2 names: cross A, circle B, square X, triangle Y, R1 black,
// L3 white, L2 the left trigger, L1 the right, R2 the left thumb, R3 the right
constexpr Tag kTags[] = {
    {33, 0x9F, kbm::kX},      {34, 0x9D, kbm::kB},      {35, 0x96, kbm::kY},       {37, 0x9E, kbm::kA},
    {38, 0x97, kbm::kStart},  {39, 0x93, kbm::kBack},   {40, 0x9C, kbm::kBlack},   {41, 0x94, kbm::kLThumb},
    {42, 0x92, kbm::kRThumb}, {43, 0xA0, kbm::kRT},     {44, 0x95, kbm::kLT},      {45, 0x91, kbm::kWhite},
    {46, 0x9B, kbm::kUp},     {47, 0x99, kbm::kDown},   {48, 0x9A, kbm::kLeft},    {49, 0x98, kbm::kRight},
    // <SDD> <SDL> <SDR> <SDU>: the left stick pushed one way, animated
    {52, kAnyGlyph, kbm::kMoveDown}, {53, kAnyGlyph, kbm::kMoveLeft}, {54, kAnyGlyph, kbm::kMoveRight},
    {55, kAnyGlyph, kbm::kMoveUp},
};

constexpr uint32_t kFont = 0x13C;  // the text's font while a word is drawn; -1 the text's own
constexpr size_t kMaxName = 24;

}  // namespace

// 0xE2F05: sprintf's arguments still pushed, word at [esp], the glyph at [esp+8]; edi the tag, bl set inside
// <BGFONT>, where the icon is drawn in font 0xB on a circle
void PromptGlyph(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!kbm::PromptKeys()) return;
    uint32_t word = R32(ctx.esp), glyph = R32(ctx.esp + 8) & 0xFF;
    const Tag* tag = nullptr;
    for (const Tag& t : kTags)
        if (t.index == ctx.edi) tag = &t;
    if (!tag || (tag->glyph != kAnyGlyph && uint32_t(tag->glyph) != glyph)) return;
    std::string key = kbm::PromptKey(tag->control);
    if (key.empty() || key.size() > kMaxName) return;
    std::string text = "[" + key + "]";
    std::memcpy(HostPtr(word), text.c_str(), text.size() + 1);
    // the icon font has no letters: the name is drawn in the text's font
    if (ctx.ebx & 0xFF) W32(ctx.esi + kFont, 0xFFFFFFFF);
}

// The words
namespace {

constexpr uint32_t kText = 0x3C, kTextBack = 0x40;
// the string the parser draws instead of +0x3C when the text is not word wrapped (+0x157 clear), 0 for none
constexpr uint32_t kShown = 0x34;
constexpr size_t kMaxText = 4096;
constexpr uint32_t kWhole = 0xFFFFFFFF;  // a run's length: to the end of the string

std::string Key(int control) {
    std::string k = kbm::PromptKey(control);
    return k.empty() ? std::string() : "[" + k + "]";
}

// the move keys as one, [W/A/S/D]
std::string MoveKeys() {
    std::string keys;
    for (int c : {kbm::kMoveUp, kbm::kMoveLeft, kbm::kMoveDown, kbm::kMoveRight}) {
        std::string k = kbm::PromptKey(c);
        if (k.empty()) return {};
        keys += (keys.empty() ? "" : "/") + k;
    }
    return "[" + keys + "]";
}

void Replace(std::string& s, std::string_view from, const std::string& to) {
    if (to.empty()) return;  // nothing bound: the pad's words stay
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

bool Has(const std::string& s, std::string_view what) { return s.find(what) != std::string::npos; }

bool At(const std::string& s, size_t at, std::string_view what) { return s.compare(at, what.size(), what) == 0; }

// Rage is both triggers at once: "pull and release both " is followed by "triggers" or "right and left trigger",
// each its own string, maybe across a line break. All of it becomes the rage key.
void Rage(std::string& s) {
    std::string rage = Key(kbm::kRage);
    if (rage.empty()) return;
    for (size_t both = s.find("both "); both != std::string::npos; both = s.find("both ", both + 1)) {
        size_t at = both + 5;
        while (at < s.size() && (s[at] == ' ' || At(s, at, "<CR>"))) at += s[at] == ' ' ? 1 : 4;
        size_t end = std::string::npos;
        for (std::string_view t : {"right and left triggers", "right and left trigger", "the triggers", "triggers"})
            if (At(s, at, t)) {
                end = at + t.size();
                break;
            }
        if (end == std::string::npos) continue;
        size_t start = both;
        for (std::string_view verb : {"pull and release ", "Pull and release ", "pull ", "Pull ", "press ", "Press "})
            if (both >= verb.size() && At(s, both - verb.size(), verb)) {
                start = both - verb.size();
                break;
            }
        std::string to = start == both ? rage : (s[start] == 'P' ? "Press " : "press ") + rage;
        s.replace(start, end - start, to);
        both = start;
    }
}

// the text with the pad's words as keys; title: it is the title screen's prompt
std::string Rewrite(std::string s, bool& title) {
    title = false;
    // player 2 and joining are a controller's
    bool other = Has(s, "Player 2") || Has(s, "TO JOIN") || Has(s, "PLAYER 2");
    if (!other) {
        for (std::string_view press : {"PRESS THE START BUTTON", "PRESS START", "Press Start"}) {
            if (!Has(s, press)) continue;
            title = true;
            Replace(s, press, press[1] == 'R' ? "PRESS ANY KEY" : "Press any key");
        }
        Replace(s, "the START button", Key(kbm::kStart));
        Replace(s, "press START", "press " + Key(kbm::kStart));
        Replace(s, "the BACK button", Key(kbm::kBack));
    }
    // the gang's commands
    Replace(s, "Click the left thumbstick", "Press " + Key(kbm::kLThumb));
    Replace(s, "click the left thumbstick", "press " + Key(kbm::kLThumb));
    Replace(s, "Click to bring", Key(kbm::kLThumb) + " brings");
    Replace(s, "with the right thumbstick", "with the mouse");
    Replace(s, "the right thumbstick", "the mouse");
    Replace(s, "right thumbstick", "mouse");
    Replace(s, "the left thumbstick", MoveKeys());
    Replace(s, "left thumbstick", MoveKeys());
    // the triggers
    Rage(s);
    Replace(s, "right and left triggers", Key(kbm::kRage));
    Replace(s, "right and left trigger", Key(kbm::kRage));
    Replace(s, "the right trigger", Key(kbm::kRT));
    Replace(s, "the left trigger", Key(kbm::kLT));
    Replace(s, "right trigger", Key(kbm::kRT));
    Replace(s, "left trigger", Key(kbm::kLT));
    // a trigger is pulled, a key pressed; the tag after it becomes the key
    for (auto [from, to] : {std::pair{"Pull and hold down <", "Hold <"}, {"pull and hold down <", "hold <"},
                            {"Pull and hold <", "Hold <"}, {"pull and hold <", "hold <"},
                            {"Pull and release <", "Press <"}, {"pull and release <", "press <"},
                            {"Pull <", "Press <"}, {"pull <", "press <"}})
        Replace(s, from, to);
    return s;
}

std::mutex g_lock;
std::unordered_map<std::string, uint32_t> g_copies;  // rewritten text -> its copy in guest memory

// text (a whole string, or a run of len characters) with keys for the words, in guest memory: 0 when nothing changes
uint32_t Copy(uint32_t va, uint32_t len, uint32_t* out_len = nullptr) {
    std::string text = ReadCString(va, kMaxText);
    if (len != kWhole && len < text.size()) text.resize(len);
    if (text.empty()) return 0;
    bool title;
    std::string out = Rewrite(text, title);
    if (title) kbm::TitlePrompt();
    if (out == text) return 0;
    if (out_len) *out_len = uint32_t(out.size());
    std::lock_guard lock(g_lock);
    uint32_t& copy = g_copies[out];
    if (!copy) {
        copy = mem::AllocSystem(uint32_t(out.size() + 1));
        if (copy) std::memcpy(HostPtr(copy), out.c_str(), out.size() + 1);
    }
    return copy;
}

// XBE_KBM_TEST: each text drawn, once
void LogText(const char* where, uint32_t text, uint32_t len = kWhole) {
    if (!kbm::TestMode() || !text) return;
    std::string t = ReadCString(text, 200);
    if (len != kWhole && len < t.size()) t.resize(len);
    static std::mutex lock;
    static std::unordered_map<std::string, bool> seen;
    std::lock_guard l(lock);
    if (seen.emplace(std::string(where) + t, true).second) LOGR("text %s: %s", where, t.c_str());
}

// runs a text object's draw or measure with copies in place of its strings
template <void (*Original)(X86Context&, uint8_t*)>
void WithKeys(X86Context& ctx, uint8_t* base) {
    uint32_t obj = ctx.ecx;
    constexpr uint32_t kFields[] = {kText, kTextBack, kShown};
    uint32_t was[3];
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        was[i] = R32(obj + kFields[i]);
        LogText("object", was[i]);
        if (!was[i] || !kbm::PromptKeys()) continue;
        // +0x40 is +0x3C's to put back after a word wrap: the same copy
        uint32_t copy = i == 1 && was[1] == was[0] ? R32(obj + kText) : Copy(was[i], kWhole);
        if (!copy || copy == was[i]) continue;
        W32(obj + kFields[i], copy);
        changed = true;
    }
    Original(ctx, base);
    if (!changed) return;
    for (int i = 0; i < 3; i++) W32(obj + kFields[i], was[i]);
}

// a run of text drawn or measured whoever asks, the front end's menus too: its string and length (kWhole for all)
template <void (*Original)(X86Context&, uint8_t*), uint32_t kString, uint32_t kLength>
void RunWithKeys(X86Context& ctx, uint8_t* base) {
    uint32_t esp = ctx.esp, str = R32(esp + kString), len = R32(esp + kLength), out_len = 0;
    LogText("run", str, len);
    uint32_t copy = str && kbm::PromptKeys() ? Copy(str, len, &out_len) : 0;
    if (!copy) return Original(ctx, base);
    W32(esp + kString, copy);
    if (len != kWhole) W32(esp + kLength, out_len);
    Original(ctx, base);
    // cdecl: the arguments are the caller's, put back as they were
    W32(esp + kString, str);
    W32(esp + kLength, len);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_000E3B50);
X86_FUNC(sub_000E3B50) { WithKeys<__imp__sub_000E3B50>(ctx, base); }  // draw
X86_EXTERN_FUNC(__imp__sub_000E3CD0);
X86_FUNC(sub_000E3CD0) { WithKeys<__imp__sub_000E3CD0>(ctx, base); }  // measure
// (?, string, ..., length as argument 11) and (string, length, ...), cdecl
X86_EXTERN_FUNC(__imp__sub_00096660);
X86_FUNC(sub_00096660) { RunWithKeys<__imp__sub_00096660, 8, 0x2C>(ctx, base); }
X86_EXTERN_FUNC(__imp__sub_000964A0);
X86_FUNC(sub_000964A0) { RunWithKeys<__imp__sub_000964A0, 4, 8>(ctx, base); }
