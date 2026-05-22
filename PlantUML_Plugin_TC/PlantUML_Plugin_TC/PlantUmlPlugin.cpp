// PlantUML WebView Lister
#include <windows.h>
#include "WebViewHost.h"
#include "Utils.h"
#include "PlantUmlRunner.h"

// ---------------------- WLX exports ----------------------
extern "C" {

    __declspec(dllexport) int __stdcall ListGetDetectString(char* DetectString, int maxlen) {
        LoadConfigIfNeeded();
        lstrcpynA(DetectString, g_detectA.c_str(), maxlen);
        return 0;
    }

    __declspec(dllexport) HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int /*ShowFlags*/) {
        LoadConfigIfNeeded();
        AppendLog(L"ListLoadW: start for file " + std::wstring(FileToLoad ? FileToLoad : L"<null>"));
        EnsureWndClass();

        auto* host = new Host();
        host->hInst = GetModuleHandleW(nullptr);
        host->hwnd = CreateWindowExW(0, kWndClass, L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
            ParentWin, nullptr, host->hInst, nullptr);
        if (!host->hwnd) {
            AppendLog(L"ListLoadW: CreateWindowExW failed");
            HostRelease(host);
            return nullptr;
        }
        SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, (LONG_PTR)host);

        const bool preferSvg = (ToLowerTrim(g_prefer) == L"svg");
        RenderBackend renderer = GetConfiguredRenderer();

        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->sourceFilePath = FileToLoad ? std::wstring(FileToLoad) : std::wstring();
            host->configuredRenderer = renderer;
            host->activeRenderer = renderer;
            host->lastPreferSvg = preferSvg;
            host->firstErrorMessage.clear();
            host->lastSvg.clear();
            host->lastPng.clear();
            host->hasRender = false;
        }

        HostRenderAndReload(host, preferSvg, L"ListLoadW", L"Unable to render the diagram. Check the log.", false);
        InitWebView(host);

        return host->hwnd;
    }

    __declspec(dllexport) int __stdcall ListSendCommand(HWND /*ListWin*/, int /*Command*/, int /*Parameter*/) {
        return 0;
    }

    __declspec(dllexport) void __stdcall ListCloseWindow(HWND ListWin) {
        AppendLog(L"ListCloseWindow: destroying window");
        DestroyWindow(ListWin);
    }

} // extern "C"