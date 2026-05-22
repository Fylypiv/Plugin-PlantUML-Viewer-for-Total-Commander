#include "WebViewHost.h"
#include "Utils.h"
#include "PlantUmlRunner.h"
#include <shlwapi.h>

using namespace Microsoft::WRL;

const wchar_t* kWndClass = L"PumlWebViewHost";

static void HostNavigateToInitialHtml(Host* host) {
    if (!host || !host->web) return;
    std::wstring html;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        html = host->initialHtml;
    }
    if (!html.empty()) {
        AppendLog(L"HostNavigateToInitialHtml: navigating with HTML length=" + std::to_wstring(html.size()));
        host->web->NavigateToString(html.c_str());
    }
}

static void HostAddRef(Host* host) {
    if (host) host->refs.fetch_add(1, std::memory_order_relaxed);
}

void HostRelease(Host* host) {
    if (!host) return;
    if (host->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete host;
    }
}

bool HostRenderAndReload(Host* host, bool preferSvg, const std::wstring& logContext, const std::wstring& failureDialogMessage, bool showDialogOnFailure) {
    if (!host) return false;

    std::wstring sourcePath;
    RenderBackend renderer = RenderBackend::Java;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        sourcePath = host->sourceFilePath;
        renderer = host->configuredRenderer;
    }

    if (sourcePath.empty()) {
        AppendLog(logContext + L": no source path recorded");
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->lastPreferSvg = preferSvg;
        }
        if (showDialogOnFailure && host->hwnd) {
            MessageBoxW(host->hwnd, L"Unable to render because the original file path is unknown.", L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    AppendLog(logContext + L": reloading file " + sourcePath);
    const std::wstring text = ReadFileUtf16OrAnsi(sourcePath.c_str());
    RenderPipelineResult renderResult = ExecuteRenderBackend(renderer, text, sourcePath, preferSvg);
    std::wstring htmlToNavigate;

    if (renderResult.success) {
        AppendLog(logContext + L": render succeeded");
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->configuredRenderer = renderer;
            host->initialHtml = renderResult.html;
            host->lastPreferSvg = preferSvg;
            host->activeRenderer = renderResult.backend;
            host->firstErrorMessage.clear();
            if (renderResult.backend == RenderBackend::Java) {
                host->lastSvg = renderResult.svg;
                host->lastPng = renderResult.png;
                host->hasRender = preferSvg ? !host->lastSvg.empty() : !host->lastPng.empty();
            }
            else {
                host->lastSvg.clear();
                host->lastPng.clear();
                host->hasRender = false;
            }
            htmlToNavigate = host->initialHtml;
        }
    }
    else {
        std::wstring dialogMessage = failureDialogMessage.empty() ? L"Unable to render the diagram." : failureDialogMessage;
        if (!renderResult.errorMessage.empty()) dialogMessage = renderResult.errorMessage;
        AppendLog(logContext + L": render failed -> " + dialogMessage);
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->initialHtml = BuildErrorHtml(dialogMessage, preferSvg);
            host->lastSvg.clear();
            host->lastPng.clear();
            host->lastPreferSvg = preferSvg;
            host->hasRender = false;
            host->activeRenderer = renderer;
            host->configuredRenderer = renderer;
            host->firstErrorMessage = dialogMessage;
            htmlToNavigate = host->initialHtml;
        }
        if (showDialogOnFailure && host->hwnd) {
            MessageBoxW(host->hwnd, dialogMessage.c_str(), L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        }
    }

    if (host->web && !htmlToNavigate.empty()) {
        host->web->NavigateToString(htmlToNavigate.c_str());
    }
    return renderResult.success;
}

static void HostHandleSaveAs(Host* host) {
    if (!host) return;

    std::wstring svgCopy;
    std::vector<unsigned char> pngCopy;
    std::wstring sourcePath;
    bool preferSvg = true;
    bool hasRender = false;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        hasRender = host->hasRender;
        preferSvg = host->lastPreferSvg;
        svgCopy = host->lastSvg;
        pngCopy = host->lastPng;
        sourcePath = host->sourceFilePath;
    }

    if (!hasRender) {
        MessageBoxW(host->hwnd, L"There is no rendered diagram available to save.", L"PlantUML Viewer", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring defaultExt = preferSvg ? L"svg" : L"png";
    std::wstring suggestedName = L"diagram." + defaultExt;
    if (!sourcePath.empty()) {
        const wchar_t* fileName = PathFindFileNameW(sourcePath.c_str());
        if (fileName && *fileName) {
            suggestedName.assign(fileName);
            size_t dot = suggestedName.find_last_of(L'.');
            if (dot != std::wstring::npos) suggestedName.erase(dot);
            suggestedName += L"." + defaultExt;
        }
    }

    std::wstring fileBuf(MAX_PATH, L'\0');
    lstrcpynW(fileBuf.data(), suggestedName.c_str(), static_cast<int>(fileBuf.size()));

    std::wstring filterSvg = L"Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0\0";
    std::wstring filterPng = L"Portable Network Graphics (*.png)\0*.png\0All Files (*.*)\0*.*\0\0";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host->hwnd;
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
    ofn.lpstrFilter = preferSvg ? filterSvg.c_str() : filterPng.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = defaultExt.c_str();
    ofn.lpstrTitle = L"Save PlantUML Output";

    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring savePath(ofn.lpstrFile);
    bool success = false;
    if (preferSvg) {
        std::string utf8 = ToUtf8(svgCopy);
        if (!svgCopy.empty() && !utf8.empty()) {
            success = WriteBufferToFile(savePath, utf8.data(), utf8.size());
        }
    }
    else {
        success = WriteBufferToFile(savePath, pngCopy.data(), pngCopy.size());
    }

    if (!success) {
        MessageBoxW(host->hwnd, L"Failed to save the file.", L"PlantUML Viewer", MB_OK | MB_ICONERROR);
    }
}

static void HostHandleRefresh(Host* host) {
    if (!host) return;
    bool preferSvg = true;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        preferSvg = host->lastPreferSvg;
    }
    HostRenderAndReload(host, preferSvg, L"HostHandleRefresh", L"Unable to refresh the diagram.", true);
}

static void HostHandleFormatChange(Host* host, bool preferSvg) {
    if (!host) return;
    RenderBackend backend = RenderBackend::Java;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        backend = host->activeRenderer;
        host->lastPreferSvg = preferSvg;
        if (backend == RenderBackend::Web) host->hasRender = false;
    }
    if (backend == RenderBackend::Web) return;
    HostRenderAndReload(host, preferSvg, L"HostHandleFormatChange", L"Unable to render the diagram.", true);
}

static void HostHandleRenderUpdate(Host* host, const std::wstring& format, const std::wstring& svgBase64, const std::wstring& pngBase64) {
    if (!host) return;
    std::vector<unsigned char> svgBytes = Base64Decode(svgBase64);
    std::wstring svgText;
    if (!svgBytes.empty()) {
        std::string utf8(svgBytes.begin(), svgBytes.end());
        svgText = FromUtf8(utf8);
    }
    std::vector<unsigned char> pngBytes = Base64Decode(pngBase64);
    const bool preferSvg = (ToLowerTrim(format) != L"png");
    const bool hasRenderable = !svgText.empty() || !pngBytes.empty();

    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->lastSvg = std::move(svgText);
        host->lastPng = std::move(pngBytes);
        host->lastPreferSvg = preferSvg;
        host->hasRender = hasRenderable;
        if (hasRenderable) host->firstErrorMessage.clear();
    }
}

static void HostHandleRenderFailure(Host* host, const std::wstring& message) {
    if (!host) return;
    std::wstring preservedError;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        if (host->firstErrorMessage.empty() && !message.empty()) host->firstErrorMessage = message;
        preservedError = host->firstErrorMessage;
    }
    std::wstring finalMessage = preservedError.empty() ? message : preservedError;
    if (finalMessage.empty()) finalMessage = L"Unable to render the diagram.";
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->initialHtml = BuildErrorHtml(finalMessage, host->lastPreferSvg);
        host->lastSvg.clear();
        host->lastPng.clear();
        host->hasRender = false;
        host->firstErrorMessage = finalMessage;
    }
    if (host->web && !host->initialHtml.empty()) {
        host->web->NavigateToString(host->initialHtml.c_str());
    }
}

static void HostHandleCopy(Host* host) {
    if (!host) return;
    std::wstring svgCopy;
    std::vector<unsigned char> pngCopy;
    bool preferSvg = true;
    bool hasRender = false;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        hasRender = host->hasRender;
        preferSvg = host->lastPreferSvg;
        svgCopy = host->lastSvg;
        pngCopy = host->lastPng;
    }

    if (!hasRender) {
        MessageBoxW(host->hwnd, L"There is no rendered diagram available to copy.", L"PlantUML Viewer", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!OpenClipboard(host->hwnd)) return;
    EmptyClipboard();
    bool success = false;
    if (preferSvg) {
        if (!svgCopy.empty()) success = ClipboardSetUnicodeText(svgCopy);
    }
    else {
        if (!pngCopy.empty()) {
            std::vector<unsigned char> dib;
            if (CreateDibFromPng(pngCopy, dib)) ClipboardSetBinaryData(CF_DIB, dib.data(), dib.size());
            UINT pngFormat = RegisterClipboardFormatW(L"PNG");
            if (pngFormat != 0) ClipboardSetBinaryData(pngFormat, pngCopy.data(), pngCopy.size());
            success = true;
        }
    }
    CloseClipboard();
    if (!success) MessageBoxW(host->hwnd, L"Failed to copy the diagram to the clipboard.", L"PlantUML Viewer", MB_OK | MB_ICONERROR);
}

typedef HRESULT(STDAPICALLTYPE* PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

static LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE) {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (host && host->ctrl) {
            RECT rc; GetClientRect(h, &rc);
            host->ctrl->put_Bounds(rc);
        }
    }
    if (m == WM_NCDESTROY) {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (host) {
            host->closing.store(true, std::memory_order_release);
            host->hwnd = nullptr;
            if (host->web && host->navCompletedRegistered) {
                host->web->remove_NavigationCompleted(host->navCompletedToken);
                host->navCompletedRegistered = false;
                HostRelease(host);
            }
            if (host->web && host->webMessageRegistered) {
                host->web->remove_WebMessageReceived(host->webMessageToken);
                host->webMessageRegistered = false;
                HostRelease(host);
            }
            if (host->ctrl) host->ctrl->Close();
            if (host->hWvLoader) FreeLibrary(host->hWvLoader);
            host->ctrl.Reset();
            host->web.Reset();
            host->env.Reset();
            HostRelease(host);
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureWndClass() {
    static bool inited = false;
    if (inited) return;
    WNDCLASSW wc{}; wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    inited = true;
}

void InitWebView(struct Host* host) {
    std::wstring loaderPath = GetModuleDir() + L"\\WebView2Loader.dll";
    host->hWvLoader = LoadLibraryW(loaderPath.c_str());
    if (!host->hWvLoader) host->hWvLoader = LoadLibraryW(L"WebView2Loader.dll");
    if (!host->hWvLoader) {
        CreateWindowW(L"STATIC", L"WebView2 Runtime not found.", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, host->hwnd, nullptr, host->hInst, nullptr);
        return;
    }
    auto fn = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(GetProcAddress(host->hWvLoader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!fn) return;

    HostAddRef(host);
    auto envCompleted = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [host](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
            if (!host || host->closing.load(std::memory_order_acquire)) return S_OK;
            if (FAILED(hr) || !env) return S_OK;
            host->env = env;

            HostAddRef(host);
            auto controllerCompleted = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [host](HRESULT hrCtrl, ICoreWebView2Controller* ctrl) -> HRESULT {
                    std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                    if (!host || host->closing.load(std::memory_order_acquire)) return S_OK;
                    if (FAILED(hrCtrl) || !ctrl) return S_OK;
                    host->ctrl = ctrl;
                    host->ctrl->get_CoreWebView2(&host->web);
                    if (!host->web || !host->hwnd) return S_OK;

                    RECT rc; GetClientRect(host->hwnd, &rc);
                    host->ctrl->put_Bounds(rc);

                    HostAddRef(host);
                    auto webMessageHandler = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [host](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            HostAddRef(host);
                            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                            if (!host || host->closing.load(std::memory_order_acquire) || !args) return S_OK;

                            LPWSTR rawJson = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&rawJson)) && rawJson) {
                                std::wstring json(rawJson);
                                CoTaskMemFree(rawJson);
                                const std::wstring type = ToLowerTrim(ExtractJsonStringField(json, L"type"));
                                if (type == L"saveas") HostHandleSaveAs(host);
                                else if (type == L"refresh") HostHandleRefresh(host);
                                else if (type == L"setformat") HostHandleFormatChange(host, ToLowerTrim(ExtractJsonStringField(json, L"format")) != L"png");
                                else if (type == L"copy") HostHandleCopy(host);
                                else if (type == L"rendered") HostHandleRenderUpdate(host, ExtractJsonStringField(json, L"format"), ExtractJsonStringField(json, L"svgBase64"), ExtractJsonStringField(json, L"pngBase64"));
                                else if (type == L"renderfailed") HostHandleRenderFailure(host, ExtractJsonStringField(json, L"message"));
                            }
                            return S_OK;
                        });
                    host->web->add_WebMessageReceived(webMessageHandler.Get(), &host->webMessageToken);
                    host->webMessageRegistered = true;

                    HostAddRef(host);
                    auto navCompletedHandler = Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [host](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            HostAddRef(host);
                            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                            return S_OK;
                        });
                    host->web->add_NavigationCompleted(navCompletedHandler.Get(), &host->navCompletedToken);
                    host->navCompletedRegistered = true;

                    HostNavigateToInitialHtml(host);
                    return S_OK;
                });
            env->CreateCoreWebView2Controller(host->hwnd, controllerCompleted.Get());
            return S_OK;
        });
    fn(nullptr, nullptr, nullptr, envCompleted.Get());
}