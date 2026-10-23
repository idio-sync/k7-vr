// Command file: tools write k7vr\cmd.txt (atomically, via rename) and the probe reads it at
// the next frame boundary. One command per line:
//   capture                 next frame's draw list + PNG (also F10)
//   shot                    PNG of the next frame
//   stereo SEP              alternate eyes every frame, SEP apart in GX eye-space units (0 = off)
//   view YAW PITCH          free look from the game camera, degrees (+yaw right, +pitch up)
//   pair                    PNGs of the next two frames, named by eye (pair_NNN_L/R.png)
//   key NAME [ms]           tap a keyboard key through raw input (default 120 ms), then 120 ms gap
//   keydown NAME / keyup NAME   press or release a key and move on (hold across other steps)
//   dumpf ADDR N [TAG] / dumpptr PTR OFF N TAG   log N floats (memscan.cpp)
//   holdkey NAME ms         hold a key
//   click [ms] / rclick [ms] left / right mouse button through raw input
//   look dx dy [steps] [ms] relative mouse motion: `steps` moves of (dx,dy) spread over ms
//   press A[+B...] [ms]     tap pad buttons (default 120 ms), then release for 120 ms
//   hold A[+B...] ms        hold pad buttons
//   stick lx ly ms          left stick, -1..1
//   rstick rx ry ms         right stick, -1..1
//   trig l r ms             triggers, 0..1
//   wait ms                 idle
//   scanf LO HI / rescanf LO HI / peekf ADDR / pokef ADDR V / pokehits V   memory probing (memscan.cpp)
//   activate                tell the game its window is active (it ignores input otherwise)
//   mouse x y               where the game sees the cursor, in window client pixels (default: off-window)
//   log text                marker in k7vr.log
// Steps (all but log/activate) run in order, each for its duration; shot/capture take no time. Pad input is ORed into player 1's pad; keys
// are posted to the game window as WM_INPUT (see hooks_input.cpp), so it needn't have focus.
#include "common.h"

#include <xinput.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace k7 {

struct Step {
    WORD buttons = 0;
    SHORT lx = 0, ly = 0, rx = 0, ry = 0;
    BYTE lt = 0, rt = 0;
    UINT vk = 0;
    int mouse = 0;  // 1 left, 2 right button
    LONG dx = 0, dy = 0;  // relative mouse motion, posted when the step starts
    DWORD ms = 0;
    bool shot = false;
    bool capture = false;
    bool pair = false;
    float stereo = -1;
    bool view = false;
    float yaw = 0, pitch = 0;
    std::function<void()> action;  // run when the step starts (memory commands)
};

static std::mutex g_mx;
static std::deque<Step> g_steps;
static bool g_running;
static Step g_cur;
static ULONGLONG g_cur_end;
static DWORD g_seq;
static Requests g_pending;  // shot/capture steps that started since the last control_poll

static WORD parse_buttons(const std::string& list) {
    static const struct {
        const char* name;
        WORD bit;
    } kNames[] = {
        {"UP", XINPUT_GAMEPAD_DPAD_UP},       {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
        {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT},   {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"START", XINPUT_GAMEPAD_START},      {"BACK", XINPUT_GAMEPAD_BACK},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB},    {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"A", XINPUT_GAMEPAD_A},              {"B", XINPUT_GAMEPAD_B},
        {"X", XINPUT_GAMEPAD_X},              {"Y", XINPUT_GAMEPAD_Y},
    };
    WORD bits = 0;
    std::stringstream ss(list);
    std::string name;
    while (std::getline(ss, name, '+')) {
        bool found = false;
        for (auto& n : kNames)
            if (_stricmp(n.name, name.c_str()) == 0) {
                bits |= n.bit;
                found = true;
            }
        if (!found) logf("cmd: unknown button '%s'", name.c_str());
    }
    return bits;
}

// Names as in killer7.ini's [KeyboardAndMouse] (without KEY_), or a single letter/digit.
static UINT parse_key(const std::string& name) {
    static const struct {
        const char* name;
        UINT vk;
    } kKeys[] = {
        {"SPACE", VK_SPACE},   {"ESCAPE", VK_ESCAPE}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN},
        {"TAB", VK_TAB},       {"LSHIFT", VK_LSHIFT}, {"LCONTROL", VK_LCONTROL}, {"UP", VK_UP},
        {"DOWN", VK_DOWN},     {"LEFT", VK_LEFT},     {"RIGHT", VK_RIGHT},   {"BACK", VK_BACK},
        {"F1", VK_F1},         {"F2", VK_F2},
    };
    for (auto& k : kKeys)
        if (_stricmp(k.name, name.c_str()) == 0) return k.vk;
    if (name.size() == 1 && isalnum(static_cast<unsigned char>(name[0])))
        return UINT(toupper(static_cast<unsigned char>(name[0])));
    logf("cmd: unknown key '%s'", name.c_str());
    return 0;
}

// Hex address, or exe+RVA (relative to the game exe's load address).
static uintptr_t parse_addr(const std::string& text) {
    if (_strnicmp(text.c_str(), "exe+", 4) == 0)
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + uintptr_t(strtoul(text.c_str() + 4, nullptr, 16));
    return uintptr_t(strtoul(text.c_str(), nullptr, 16));
}

static SHORT axis(double v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return SHORT(v * 32767);
}

static void run_line(const std::string& line) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd.empty() || cmd[0] == '#') return;
    std::lock_guard lock(g_mx);
    if (cmd == "capture") {
        g_steps.push_back(Step{.capture = true});
    } else if (cmd == "shot") {
        g_steps.push_back(Step{.shot = true});
    } else if (cmd == "pair") {
        g_steps.push_back(Step{.pair = true});
    } else if (cmd == "view") {
        float yaw = 0, pitch = 0;
        ss >> yaw >> pitch;
        g_steps.push_back(Step{.view = true, .yaw = yaw, .pitch = pitch});
    } else if (cmd == "stereo") {
        float sep = 0;
        ss >> sep;
        g_steps.push_back(Step{.stereo = sep < 0 ? 0 : sep});
    } else if (cmd == "key" || cmd == "holdkey") {
        std::string name;
        DWORD ms = 120;
        ss >> name >> ms;
        Step s;
        s.vk = parse_key(name);
        s.ms = ms;
        g_steps.push_back(s);
        if (cmd == "key") g_steps.push_back(Step{.ms = 120});
    } else if (cmd == "click" || cmd == "rclick") {
        DWORD ms = 120;
        ss >> ms;
        g_steps.push_back(Step{.mouse = cmd == "click" ? 1 : 2, .ms = ms});
        g_steps.push_back(Step{.ms = 120});
    } else if (cmd == "look") {
        LONG dx = 0, dy = 0;
        int steps = 1;
        DWORD ms = 0;
        ss >> dx >> dy >> steps >> ms;
        if (steps < 1) steps = 1;
        for (int i = 0; i < steps; ++i) g_steps.push_back(Step{.dx = dx, .dy = dy, .ms = ms / DWORD(steps)});
    } else if (cmd == "press" || cmd == "hold") {
        std::string buttons;
        DWORD ms = 120;
        ss >> buttons >> ms;
        Step s;
        s.buttons = parse_buttons(buttons);
        s.ms = ms;
        g_steps.push_back(s);
        if (cmd == "press") g_steps.push_back(Step{.ms = 120});
    } else if (cmd == "stick" || cmd == "rstick") {
        double x = 0, y = 0;
        DWORD ms = 0;
        ss >> x >> y >> ms;
        Step s;
        (cmd == "stick" ? s.lx : s.rx) = axis(x);
        (cmd == "stick" ? s.ly : s.ry) = axis(y);
        s.ms = ms;
        g_steps.push_back(s);
    } else if (cmd == "trig") {
        double l = 0, r = 0;
        DWORD ms = 0;
        ss >> l >> r >> ms;
        Step s;
        s.lt = BYTE(l * 255);
        s.rt = BYTE(r * 255);
        s.ms = ms;
        g_steps.push_back(s);
    } else if (cmd == "wait") {
        DWORD ms = 0;
        ss >> ms;
        g_steps.push_back(Step{.ms = ms});
    } else if (cmd == "scanf" || cmd == "rescanf") {
        float lo = 0, hi = 0;
        ss >> lo >> hi;
        bool rescan = cmd == "rescanf";
        g_steps.push_back(Step{.action = [=] { (rescan ? mem_rescanf : mem_scanf)(lo, hi); }});
    } else if (cmd == "dumpf") {
        std::string addr, tag;
        int count = 64;
        ss >> addr >> count >> tag;
        uintptr_t a = parse_addr(addr);
        g_steps.push_back(Step{.action = [=] { mem_dumpf(a, count, tag.empty() ? addr : tag); }});
    } else if (cmd == "dumpptr") {
        // dumpptr PTR OFF COUNT TAG: dump COUNT floats at *(PTR) + OFF (hex), resolved when the step runs
        std::string addr, tag;
        unsigned offset = 0;
        int count = 64;
        ss >> addr >> std::hex >> offset >> std::dec >> count >> tag;
        uintptr_t a = parse_addr(addr);
        g_steps.push_back(Step{.action = [=] { mem_dumpf(*reinterpret_cast<uintptr_t*>(a) + offset, count, tag); }});
    } else if (cmd == "keydown" || cmd == "keyup") {
        std::string name;
        ss >> name;
        UINT vk = parse_key(name);
        bool down = cmd == "keydown";
        g_steps.push_back(Step{.action = [=] { post_key(vk, down); }});
    } else if (cmd == "patch") {
        std::string addr, hex;
        ss >> addr >> hex;
        uintptr_t a = parse_addr(addr);
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(uint8_t(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        g_steps.push_back(Step{.action = [=] { mem_patch(a, bytes); }});
    } else if (cmd == "peekd") {
        std::string addr;
        ss >> addr;
        uintptr_t a = parse_addr(addr);
        g_steps.push_back(Step{.action = [=] { mem_peekd(a); }});
    } else if (cmd == "watchptr") {
        std::string addr;
        unsigned offset = 0;
        int hits = 8;
        ss >> addr >> std::hex >> offset >> std::dec >> hits;
        uintptr_t a = parse_addr(addr);
        g_steps.push_back(Step{.action = [=] { mem_watch(*reinterpret_cast<uintptr_t*>(a) + offset, hits); }});
    } else if (cmd == "watchhit") {
        size_t index = 0;
        int hits = 8;
        ss >> index >> hits;
        g_steps.push_back(Step{.action = [=] { mem_watchhit(index, hits); }});
    } else if (cmd == "fov_inflate") {
        float deg = 0;
        ss >> deg;
        g_steps.push_back(Step{.action = [=] { set_fov_inflate(deg); }});
    } else if (cmd == "scanproj") {
        g_steps.push_back(Step{.action = [] { scan_projection(); }});
    } else if (cmd == "scanpair") {
        float a = 0, b = 0, tol = 0.001f;
        int window = 64;
        ss >> a >> b >> tol >> window;
        g_steps.push_back(Step{.action = [=] { mem_scanpair(a, b, tol, window); }});
    } else if (cmd == "watch") {
        std::string addr;
        int hits = 8;
        ss >> addr >> hits;
        uintptr_t a = parse_addr(addr);
        g_steps.push_back(Step{.action = [=] { mem_watch(a, hits); }});
    } else if (cmd == "peekf" || cmd == "pokef" || cmd == "pokehits") {
        std::string addr;
        float v = 0;
        if (cmd != "pokehits") ss >> addr;
        ss >> v;
        uintptr_t a = parse_addr(addr);
        if (cmd == "peekf")
            g_steps.push_back(Step{.action = [=] { mem_peekf(a); }});
        else if (cmd == "pokef")
            g_steps.push_back(Step{.action = [=] { mem_pokef(a, v); }});
        else
            g_steps.push_back(Step{.action = [=] { mem_pokehits(v); }});
    } else if (cmd == "activate") {
        post_activate();
    } else if (cmd == "mouse") {
        int x = 0, y = 0;
        ss >> x >> y;
        set_fake_cursor(x, y);
    } else if (cmd == "log") {
        std::string rest;
        std::getline(ss, rest);
        logf("cmd log:%s", rest.c_str());
        return;
    } else {
        logf("cmd: unknown '%s'", line.c_str());
        return;
    }
    logf("cmd: %s", line.c_str());
}

// Advances the step queue; called every frame (and from the pad hook, whichever comes first).
static void advance_locked() {
    ULONGLONG now = GetTickCount64();
    if (g_running && now >= g_cur_end) {
        if (g_cur.vk) post_key(g_cur.vk, false);
        if (g_cur.mouse) post_mouse(g_cur.mouse == 1 ? RI_MOUSE_LEFT_BUTTON_UP : RI_MOUSE_RIGHT_BUTTON_UP, 0, 0);
        g_running = false;
        g_cur = Step{};
        ++g_seq;
    }
    if (!g_running && !g_steps.empty()) {
        g_cur = g_steps.front();
        g_steps.pop_front();
        g_cur_end = now + g_cur.ms;
        g_running = true;
        ++g_seq;
        if (g_cur.vk) post_key(g_cur.vk, true);
        if (g_cur.mouse) post_mouse(g_cur.mouse == 1 ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_RIGHT_BUTTON_DOWN, 0, 0);
        if (g_cur.action) g_cur.action();
        if (g_cur.dx || g_cur.dy) post_mouse(0, g_cur.dx, g_cur.dy);
        g_pending.shot |= g_cur.shot;
        g_pending.capture |= g_cur.capture;
        g_pending.pair |= g_cur.pair;
        if (g_cur.stereo >= 0) g_pending.stereo = g_cur.stereo;
        if (g_cur.view) {
            g_pending.view = true;
            g_pending.yaw = g_cur.yaw;
            g_pending.pitch = g_cur.pitch;
        }
    }
}

Requests control_poll() {
    Requests req;
    std::wstring path = path_in(L"cmd.txt");
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
            DeleteFileW(path.c_str());
            std::stringstream ss(text);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                run_line(line);
            }
        }
    }
    std::lock_guard lock(g_mx);
    advance_locked();
    req = g_pending;
    g_pending = Requests{};
    return req;
}

bool fake_pad_enabled() {
    static int enabled = -1;
    if (enabled < 0) enabled = GetPrivateProfileIntW(L"probe", L"fake_pad", 1, path_in(L"k7vr.ini").c_str());
    return enabled != 0;
}

void pad_apply(XINPUT_STATE* state) {
    std::lock_guard lock(g_mx);
    advance_locked();
    auto& pad = state->Gamepad;
    pad.wButtons |= g_cur.buttons;
    if (g_cur.lx || g_cur.ly) {
        pad.sThumbLX = g_cur.lx;
        pad.sThumbLY = g_cur.ly;
    }
    if (g_cur.rx || g_cur.ry) {
        pad.sThumbRX = g_cur.rx;
        pad.sThumbRY = g_cur.ry;
    }
    if (g_cur.lt) pad.bLeftTrigger = g_cur.lt;
    if (g_cur.rt) pad.bRightTrigger = g_cur.rt;
    state->dwPacketNumber += g_seq << 20;
}

}  // namespace k7
