// Points the game's Saved Games folder at k7vr\SavedGames when that folder exists, so the test
// copy gets its own killer7.ini and save slots and never touches the real ones (or their Steam
// Cloud sync). A normal install has no such folder and keeps the player's own saves.
#include "common.h"

#include <shlobj.h>

namespace k7 {

// FOLDERID_SavedGames
static const GUID kSavedGames = {0x4c5c32ff, 0xbb9d, 0x43b0, {0xb5, 0xb4, 0x2d, 0x72, 0xe5, 0x4e, 0xaa, 0xa4}};

using SHGetKnownFolderPath_t = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
static SHGetKnownFolderPath_t o_SHGetKnownFolderPath;

static HRESULT WINAPI hk_SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* out) {
    if (IsEqualGUID(id, kSavedGames) && out) {
        std::wstring dir = path_in(L"SavedGames");
        size_t bytes = (dir.size() + 1) * sizeof(wchar_t);
        *out = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (!*out) return E_OUTOFMEMORY;
        memcpy(*out, dir.c_str(), bytes);
        logf("Saved Games redirected to %ls", dir.c_str());
        return S_OK;
    }
    return o_SHGetKnownFolderPath(id, flags, token, out);
}

void install_folder_hook() {
    const DWORD attr = GetFileAttributesW(path_in(L"SavedGames").c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        logf("Saved Games: the game's own (no k7vr\\SavedGames folder)");
        return;
    }
    patch_import(GetModuleHandleW(nullptr), "SHELL32.dll", "SHGetKnownFolderPath",
                 reinterpret_cast<void*>(&hk_SHGetKnownFolderPath), reinterpret_cast<void**>(&o_SHGetKnownFolderPath));
}

}  // namespace k7
