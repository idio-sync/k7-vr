// Unit tests for src/proxy/ini_bindings (the VR profile's killer7.ini rewrite).
#include <cstdio>
#include <string>

#include "../src/proxy/ini_bindings.h"

static int g_failures;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failures;
    }
}

static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
    // As the game writes it: UTF-8 BOM, CRLF, keyboard fire unbound, exit on Escape.
    const std::string src =
        "\xef\xbb\xbf[Version]\r\nVersion = 12\r\n\r\n[Graphics]\r\nEnableFullScreen = true\r\nWindowSize = 1920,1027\r\n"
        "\r\n[KeyboardAndMouse]\r\nskip_scene = KEY_ESCAPE\r\naccept = KEY_RETURN\r\nfire = KEY_NONE\r\n"
        "exit_game = KEY_ESCAPE\r\nswitch_to_dan = KEY_1\r\n\r\n[Mouse]\r\nfire = MOUSE_LMB\r\n\r\n[Pad]\r\nfire = GAMEPAD_A";
    const std::string out = k7::vr_ini_apply(src);
    check(out.compare(0, 3, "\xef\xbb\xbf") == 0, "BOM kept");
    check(has(out, "\r\nfire = KEY_J\r\n"), "keyboard fire bound to J");
    check(has(out, "exit_game = KEY_F12\r\n"), "exit moved off Escape");
    check(has(out, "accept = KEY_SPACE\r\n"), "accept forced to Space");
    check(has(out, "switch_to_dan = KEY_1\r\n"), "other bindings untouched");
    check(has(out, "EnableFullScreen = false\r\n"), "windowed");
    check(has(out, "WindowSize = 1920,1027\r\n"), "window size untouched");
    check(has(out, "[Mouse]\r\nfire = MOUSE_LMB\r\n"), "mouse section untouched");
    const std::string last = "\r\n[Pad]\r\nfire = GAMEPAD_A";
    check(out.size() >= last.size() && out.compare(out.size() - last.size(), last.size(), last) == 0,
          "pad section and last line kept");
    check(k7::vr_ini_apply(out) == out, "idempotent");
    if (g_failures) return 1;
    std::printf("all ini tests passed\n");
    return 0;
}
