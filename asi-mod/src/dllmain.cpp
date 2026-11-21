#include "dx11_hook.h"
#include "paws_common.h"

#include <filesystem>

namespace {

DWORD WINAPI InitThread(LPVOID moduleHandle) {
    auto hMod = reinterpret_cast<HMODULE>(moduleHandle);

    // Place paws_gtav.log next to paws_gtav.asi (inside GTA V directory)
    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(hMod, modPath, MAX_PATH);
    std::filesystem::path modDir = std::filesystem::path(modPath).parent_path();
    std::filesystem::path logPath = modDir / L"paws_gtav.log";

    paws::Logger::Instance().Init(logPath.c_str());
    paws::Dx11Hook::SetModuleDirectory(modDir.wstring());
    PAWS_LOG("paws_gtav.asi loaded (Story Mode ANR FG + FSR plugin).");
    PAWS_LOG("Hotkeys: F8 = Cycle Normal / Depth Buffer / Motion Vectors | F9 = Toggle ANR FG.");

    // Wait briefly until d3d11.dll is loaded by GTA5.exe
    for (int i = 0; i < 100 && GetModuleHandleW(L"d3d11.dll") == nullptr; ++i) {
        Sleep(100);
    }

    if (!paws::Dx11Hook::Install()) {
        PAWS_LOG("ERROR: Dx11Hook::Install() failed.");
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID /*lpReserved*/) {
    switch (ulReasonForCall) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);
            HANDLE hThread = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            paws::Dx11Hook::Uninstall();
            paws::Logger::Instance().Shutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
