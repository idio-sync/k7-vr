#include "ini_bindings.h"

namespace k7 {

namespace {

// [KeyboardAndMouse] bindings the controllers rely on (see input.cpp).
const struct {
    const char* action;
    const char* key;
} kBindings[] = {
    {"skip_scene", "KEY_ESCAPE"}, {"accept", "KEY_SPACE"},     {"decline", "KEY_ESCAPE"},
    {"menu_up", "KEY_W"},         {"menu_down", "KEY_S"},      {"menu_left", "KEY_A"},
    {"menu_right", "KEY_D"},      {"walk", "KEY_W"},           {"turn", "KEY_S"},
    {"enter_battle_mode", "KEY_SPACE"}, {"reload", "KEY_R"},   {"use_special", "KEY_Q"},
    {"open_map", "KEY_M"},        {"open_menu", "KEY_TAB"},    {"aim_left", "KEY_LEFT"},
    {"aim_down", "KEY_DOWN"},     {"aim_up", "KEY_UP"},        {"aim_right", "KEY_RIGHT"},
    {"fire", "KEY_J"},            {"scan_enemies", "KEY_LSHIFT"}, {"exit_game", "KEY_F12"},
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

bool starts_with_bom(const std::string& s) {
    return s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xef && static_cast<unsigned char>(s[1]) == 0xbb &&
           static_cast<unsigned char>(s[2]) == 0xbf;
}

}  // namespace

std::string vr_ini_apply(const std::string& src) {
    std::string out, section;
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t end = src.find('\n', pos);
        const size_t next = end == std::string::npos ? src.size() : end + 1;
        std::string line = src.substr(pos, next - pos);
        pos = next;
        const bool bom = starts_with_bom(line);
        const bool has_nl = !line.empty() && line.back() == '\n';
        const bool crlf = has_nl && line.size() >= 2 && line[line.size() - 2] == '\r';
        const std::string body = trim(bom ? line.substr(3) : line);
        if (!body.empty() && body[0] == '[') {
            section = trim(body.substr(0, body.find(']') + 1));
        } else if (const size_t eq = body.find('='); eq != std::string::npos) {
            const std::string name = trim(body.substr(0, eq));
            const char* value = nullptr;
            if (section == "[KeyboardAndMouse]") {
                for (const auto& b : kBindings)
                    if (name == b.action) value = b.key;
            } else if (section == "[Graphics]" && name == "EnableFullScreen") {
                value = "false";
            }
            if (value)
                line = (bom ? line.substr(0, 3) : std::string()) + name + " = " + value +
                       (crlf ? "\r\n" : has_nl ? "\n" : "");
        }
        out += line;
    }
    return out;
}

}  // namespace k7
