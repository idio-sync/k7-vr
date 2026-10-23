#include "common.h"

namespace k7 {

static bool write_ptr(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool patch_import(HMODULE module, const char* dll, const char* func, void* hook, void** orig) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byname->Name), func) != 0) continue;
            auto slot = reinterpret_cast<void**>(&slots->u1.Function);
            *orig = *slot;
            bool ok = write_ptr(slot, hook);
            logf("import %s!%s %s (was %p)", dll, func, ok ? "hooked" : "PATCH FAILED", *orig);
            return ok;
        }
    }
    logf("import %s!%s not found", dll, func);
    return false;
}

bool patch_vtbl(void* object, int index, void* hook, void** orig) {
    auto vtbl = *reinterpret_cast<void***>(object);
    if (vtbl[index] == hook) return true;
    *orig = vtbl[index];
    return write_ptr(&vtbl[index], hook);
}

}  // namespace k7
