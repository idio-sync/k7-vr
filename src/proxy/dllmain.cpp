#include "common.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        k7::init_paths();
        k7::log_open();
        k7::logf("k7vr probe loaded into %ls", k7::g_game_dir.c_str());
        // The exe's imports are bound before DLL initializers run, so its IAT can be patched here.
        k7::install_folder_hook();
        k7::install_compile_hook();
        k7::install_d3d_hooks();
        k7::install_input_hook();
        k7::install_file_hooks();
        k7::start_audio_mute();
    }
    return TRUE;
}
