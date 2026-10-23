#pragma once
// Shared declarations for the probe DLL.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

struct _XINPUT_STATE;

namespace k7 {

// Paths: the game folder and our k7vr\ folder inside it.
extern std::wstring g_game_dir;
extern std::wstring g_dir;
void init_paths();
std::wstring path_in(const wchar_t* name);  // g_dir + L"\\" + name

void log_open();
void logf(const char* fmt, ...);

// Import and vtable patching.
bool patch_import(HMODULE module, const char* dll, const char* func, void* hook, void** orig);
bool patch_vtbl(void* object, int index, void* hook, void** orig);

// Hook installers (called from DllMain).
void install_d3d_hooks();
void install_compile_hook();
void install_folder_hook();
void install_file_hooks();
void start_audio_mute();

// Memory probing (memscan.cpp), run from the command file on the render thread.
void mem_scanf(float lo, float hi);
void mem_rescanf(float lo, float hi);
void mem_peekf(uintptr_t addr);
void mem_peekd(uintptr_t addr);
void mem_pokef(uintptr_t addr, float value);
void mem_pokehits(float value);
void mem_scanpair(float a, float b, float tol, int window);
void mem_watch(uintptr_t addr, int hits);
void mem_watchhit(size_t index, int hits);
void mem_patch(uintptr_t addr, const std::vector<uint8_t>& bytes);
void scan_projection();  // hooks_d3d.cpp

// DXBC checksum (first 8 bytes of the container's MD5), used to name shaders.
uint64_t dxbc_hash(const void* bytecode, size_t size);

bool save_png(const std::wstring& path, UINT width, UINT height, UINT pitch, const uint8_t* bgra);

// Command file (k7vr\cmd.txt) and scripted pad.
struct Requests {
    bool capture = false;
    bool shot = false;
    bool pair = false;       // save the next two frames (left and right eye in stereo mode)
    float stereo = -1;       // >= 0: new eye separation in GX eye-space units (0 = off)
    bool view = false;       // new free-look angles from the game camera
    float yaw = 0, pitch = 0;
};
Requests control_poll();      // once per frame
bool fake_pad_enabled();
void pad_apply(_XINPUT_STATE* state);  // ORs scripted input into player 1's state

// Scripted keyboard through raw input (hooks_input.cpp).
void install_input_hook();
void set_game_window(HWND hwnd);
void post_key(UINT vk, bool down);
void post_activate();
void post_mouse(USHORT ri_button_flags, LONG dx, LONG dy);
void set_fake_cursor(int x, int y);  // window client coordinates
bool restored_client_size(HWND hwnd, LONG* w, LONG* h);  // false unless hwnd is the minimized game window

}  // namespace k7
