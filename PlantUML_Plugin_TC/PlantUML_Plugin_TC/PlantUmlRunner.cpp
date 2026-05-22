#include "PlantUmlRunner.h"
#include "Utils.h"
#include "HtmlTemplates.h"
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl.h>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;

// ---------------------- Çì³íí³ êîíô³ãóðàö³¿ ----------------------
std::wstring g_prefer = L"svg";
std::wstring g_rendererSetting = L"java";
std::string  g_detectA = R"(EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML")";

static std::wstring g_jarPath;
static std::wstring g_javaPath;
static DWORD        g_jarTimeoutMs = 8000;
static bool         g_cfgLoaded = false;

const wchar_t* RenderBackendName(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::Java: return L"java";
    case RenderBackend::Web:  return L"web";
    }
    return L"unknown";
}

bool CreateDibFromPng(const std::vector<unsigned char>& png, std::vector<unsigned char>& outDib) {
    outDib.clear();
    if (png.empty()) return false;

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = false;
    if (hrInit == RPC_E_CHANGED_MODE) {
        hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hrInit == RPC_E_CHANGED_MODE) hrInit = S_OK;
        else if (SUCCEEDED(hrInit)) needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    }
    else if (SUCCEEDED(hrInit)) {
        needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    }
    if (FAILED(hrInit)) return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) { if (needUninit) CoUninitialize(); return false; }

    IStream* rawStream = SHCreateMemStream(png.data(), static_cast<UINT>(png.size()));
    if (!rawStream) { if (needUninit) CoUninitialize(); return false; }
    ComPtr<IStream> stream; stream.Attach(rawStream);

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) { if (needUninit) CoUninitialize(); return false; }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) { if (needUninit) CoUninitialize(); return false; }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) { if (needUninit) CoUninitialize(); return false; }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { if (needUninit) CoUninitialize(); return false; }

    UINT width = 0, height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) { if (needUninit) CoUninitialize(); return false; }

    const UINT stride = width * 4;
    const UINT imageSize = stride * height;
    outDib.resize(sizeof(BITMAPV5HEADER) + imageSize);
    auto* header = reinterpret_cast<BITMAPV5HEADER*>(outDib.data());
    ZeroMemory(header, sizeof(BITMAPV5HEADER));
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = static_cast<LONG>(width);
    header->bV5Height = -static_cast<LONG>(height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5SizeImage = imageSize;

    BYTE* pixels = outDib.data() + sizeof(BITMAPV5HEADER);
    hr = converter->CopyPixels(nullptr, stride, imageSize, pixels);

    if (needUninit) CoUninitialize();
    if (FAILED(hr)) { outDib.clear(); return false; }
    return true;
}

static bool TryAutoDetectPlantUmlJar(std::wstring& outPath) {
    const std::wstring dir = GetModuleDir();
    const std::wstring exact = dir + L"\\plantuml.jar";
    if (FileExistsW(exact)) { outPath = exact; return true; }
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\plantuml*.jar";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                outPath = dir + L"\\" + fd.cFileName;
                FindClose(hFind);
                return true;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return false;
}

static RenderBackend ParseRendererSettingValue(const std::wstring& rendererText, RenderBackend fallback) {
    std::wstring token = ToLowerTrim(rendererText);
    if (token.empty()) return fallback;
    size_t comma = token.find(L',');
    if (comma != std::wstring::npos) token = ToLowerTrim(token.substr(0, comma));
    if (token == L"web") return RenderBackend::Web;
    if (token == L"java") return RenderBackend::Java;
    return fallback;
}

RenderBackend GetConfiguredRenderer() {
    return ParseRendererSettingValue(g_rendererSetting, RenderBackend::Java);
}

std::wstring GetConfiguredRendererName() {
    return std::wstring(RenderBackendName(GetConfiguredRenderer()));
}

void LoadConfigIfNeeded() {

    g_cfgLoaded = false;

    const std::wstring moduleDir = GetModuleDir();
    const std::wstring ini = moduleDir + L"\\plantuml_settings.ini";
    wchar_t buf[2048];

    if (GetPrivateProfileStringW(L"render", L"prefer", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_prefer = buf;

    RenderBackend rendererChoice = GetConfiguredRenderer();
    if (GetPrivateProfileStringW(L"render", L"renderer", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        rendererChoice = ParseRendererSettingValue(buf, rendererChoice);
    }
    else if (GetPrivateProfileStringW(L"render", L"pipeline", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        rendererChoice = ParseRendererSettingValue(buf, rendererChoice);
    }
    g_rendererSetting = RenderBackendName(rendererChoice);

    if (GetPrivateProfileStringW(L"detect", L"string", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(need ? need - 1 : 0, '\0');
        if (need > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need - 1, nullptr, nullptr);
        if (!utf8.empty()) g_detectA = utf8;
    }

    if (GetPrivateProfileStringW(L"plantuml", L"jar", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_jarPath = buf;
        if (PathIsRelativeW(g_jarPath.c_str())) g_jarPath = moduleDir + L"\\" + g_jarPath;
    }
    if (GetPrivateProfileStringW(L"plantuml", L"java", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_javaPath = buf;
        if (PathIsRelativeW(g_javaPath.c_str())) g_javaPath = moduleDir + L"\\" + g_javaPath;
    }
    DWORD tmo = GetPrivateProfileIntW(L"plantuml", L"timeout_ms", 0, ini.c_str());
    if (tmo > 0) g_jarTimeoutMs = tmo;

    int logEnabled = GetPrivateProfileIntW(L"debug", L"log_enabled", 1, ini.c_str());
    g_logEnabled = (logEnabled != 0);

    if (GetPrivateProfileStringW(L"debug", L"log", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_logPath = buf;
        if (PathIsRelativeW(g_logPath.c_str())) g_logPath = moduleDir + L"\\" + g_logPath;
    }

    if (g_logEnabled) {
        if (g_logPath.empty()) g_logPath = moduleDir + L"\\plantumlwebview.log";
    }
    else { g_logPath.clear(); }

    bool needDetectJar = g_jarPath.empty();
    if (!g_jarPath.empty() && !FileExistsW(g_jarPath)) {
        AppendLog(L"LoadConfig: configured jar not found at " + g_jarPath + L". Attempting auto-detect.");
        needDetectJar = true;
    }
    if (needDetectJar) {
        std::wstring detected;
        if (TryAutoDetectPlantUmlJar(detected)) g_jarPath.swap(detected);
    }

    std::wstringstream cfg;
    cfg << L"Config loaded. prefer=" << g_prefer
        << L", renderer=" << GetConfiguredRendererName()
        << L", jar=" << (g_jarPath.empty() ? L"<auto>" : g_jarPath)
        << L", java=" << (g_javaPath.empty() ? L"<auto>" : g_javaPath)
        << L", timeoutMs=" << g_jarTimeoutMs
        << L", logEnabled=" << (g_logEnabled ? L"1" : L"0")
        << L", log=" << (g_logPath.empty() ? L"<disabled>" : g_logPath);
    AppendLog(cfg.str());
}

static bool FindJavaExecutable(std::wstring& outPath) {
    if (!g_javaPath.empty() && FileExistsW(g_javaPath)) { outPath = g_javaPath; return true; }
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"java.exe", nullptr, MAX_PATH, found, nullptr)) { outPath = found; return true; }
    if (SearchPathW(nullptr, L"javaw.exe", nullptr, MAX_PATH, found, nullptr)) { outPath = found; return true; }
    return false;
}

static bool RunPlantUmlJar(const std::wstring& umlTextW, bool preferSvg, std::wstring& outSvg, std::vector<unsigned char>& outPng) {
    AppendLog(L"RunPlantUmlJar: start");
    if (g_jarPath.empty()) { AppendLog(L"RunPlantUmlJar: jar path is empty"); return false; }
    if (!FileExistsW(g_jarPath)) { AppendLog(L"RunPlantUmlJar: jar not found at " + g_jarPath); return false; }
    std::wstring javaExe;
    if (!FindJavaExecutable(javaExe)) { AppendLog(L"RunPlantUmlJar: Java executable not found"); return false; }

    std::wstring fmt = preferSvg ? L"-tsvg" : L"-tpng";
    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hInR = nullptr, hInW = nullptr;
    HANDLE hOutR = nullptr, hOutW = nullptr;

    if (!CreatePipe(&hInR, &hInW, &sa, 0)) return false;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) { CloseHandle(hInR); CloseHandle(hInW); return false; }

    SetHandleInformation(hInR, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hOutW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hInW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);

    std::wstringstream cmd;
    cmd << L"\"" << javaExe << L"\" -Dfile.encoding=UTF-8 -Djava.awt.headless=true -jar \"" << g_jarPath << L"\" -pipe " << fmt;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hInR;
    si.hStdOutput = hOutW;
    si.hStdError = hOutW;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd.str();
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOutW); CloseHandle(hInR);
    if (!ok) { CloseHandle(hInW); CloseHandle(hOutR); return false; }

    std::string umlUtf8 = ToUtf8(umlTextW);
    DWORD written = 0;
    if (!umlUtf8.empty()) WriteFile(hInW, umlUtf8.data(), (DWORD)umlUtf8.size(), &written, nullptr);
    CloseHandle(hInW);

    std::vector<unsigned char> buffer;
    buffer.reserve(64 * 1024);
    unsigned char tmp[16 * 1024];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) || got == 0) break;
        buffer.insert(buffer.end(), tmp, tmp + got);
        if (buffer.size() > (50 * 1024 * 1024)) break;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            while (ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) && got) {
                buffer.insert(buffer.end(), tmp, tmp + got);
            }
            break;
        }
    }
    CloseHandle(hOutR);

    DWORD wr = WaitForSingleObject(pi.hProcess, g_jarTimeoutMs);
    if (wr == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    DWORD exitCode = 1; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if (buffer.empty()) return false;

    if (preferSvg) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buffer.data(), (int)buffer.size(), nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring svg(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)buffer.data(), (int)buffer.size(), svg.data(), wlen);
        outSvg.swap(svg);
    }
    else {
        outPng.swap(buffer);
    }
    return true;
}

static const std::wstring& PlantumlEncoderScript() {
    static const std::wstring script = []() {
        std::string combined;
        combined.reserve(30000);
        combined.append(kPlantumlEncoderScriptPart1);
        combined.append(kPlantumlEncoderScriptPart2);
        combined.append(kPlantumlEncoderScriptPart3);
        combined.append(kPlantumlEncoderScriptPart4);
        combined.append(kPlantumlEncoderScriptPart5);
        combined.append(kPlantumlEncoderScriptPart6);
        combined.append(kPlantumlEncoderScriptPart7);
        combined.append(kPlantumlEncoderScriptPart8);
        combined.append(kPlantumlEncoderScriptPart9);
        combined.append(kPlantumlEncoderScriptPart10);
        combined.append(kPlantumlEncoderScriptPart11);
        combined.append(kPlantumlEncoderScriptPart12);
        combined.append(kPlantumlEncoderScriptPart13);
        combined.append(kPlantumlEncoderScriptPart14);
        combined.append(kPlantumlEncoderScriptPart15);
        return FromUtf8(combined);
        }();
    return script;
}

static std::wstring BuildShellHtmlWithBody(const std::wstring& body, bool preferSvg) {
    std::wstring html = LR"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="X-UA-Compatible" content="IE=edge"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>PlantUML Viewer</title>
    <style>
    :root { color-scheme: light dark; }
    html, body { margin: 0; padding: 0; min-height: 100vh; }
    body { background: canvas; color: CanvasText; font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
    #toolbar { position: fixed; top: 8px; left: 8px; display: flex; gap: 6px; z-index: 10; background: color-mix(in oklab, Canvas 85%, transparent); padding: 6px; border-radius: 8px; backdrop-filter: blur(4px); box-shadow: 0 2px 8px rgba(0,0,0,0.15); }
    #toolbar button, #toolbar select { padding: 6px 10px; border-radius: 6px; border: 1px solid color-mix(in oklab, Canvas 70%, CanvasText 30%); background: color-mix(in oklab, Canvas 92%, CanvasText 8%); color: inherit; font: inherit; cursor: pointer; }
    #toolbar button:hover, #toolbar select:hover { background: color-mix(in oklab, Canvas 88%, CanvasText 12%); }
    #toolbar button:disabled, #toolbar select:disabled { opacity: 0.6; cursor: not-allowed; }
    #root { padding: 60px 20px 20px 20px; text-align: center; }
    img, svg { display: inline-block; max-width: none !important; max-height: none !important; }
    .err { padding: 12px 14px; border-radius: 10px; background: color-mix(in oklab, Canvas 85%, red 15%); display: inline-block; }
  </style>
</head>
<body data-format="{{FORMAT}}">
  <div id="toolbar">
    <button id="btn-refresh" type="button">Refresh</button>
    <button id="btn-save" type="button">Save as...</button>
    <select id="format-select">
      <option value="svg">SVG</option>
      <option value="png">PNG</option>
    </select>
    <button id="btn-copy" type="button">Copy to clipboard</button>
  </div>
  <div id="root">
    {{BODY}}
  </div>
  <script>
    const hookButton = (btn, messageType) => {
      if (!btn) {
        return;
      }
      const update = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        btn.disabled = !connected;
        if (!connected) {
          btn.title = 'Available inside Total Commander';
          window.setTimeout(update, 1000);
        } else {
          btn.removeAttribute('title');
        }
      };
      update();
      btn.addEventListener('click', () => {
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: messageType });
        }
      });
    };
    hookButton(document.getElementById('btn-refresh'), 'refresh');
    hookButton(document.getElementById('btn-save'), 'saveAs');
    const select = document.getElementById('format-select');
    if (select) {
      const setDisabled = (disabled) => {
        if (disabled) {
          select.setAttribute('disabled', 'disabled');
        } else {
          select.removeAttribute('disabled');
        }
      };
      const update = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        setDisabled(!connected);
        if (!connected) {
          select.title = 'Available inside Total Commander';
          window.setTimeout(update, 1000);
        } else {
          select.removeAttribute('title');
        }
      };
      const initial = document.body?.dataset?.format;
      if (initial) {
        select.value = initial;
      }
      select.addEventListener('change', () => {
        if (document.body && document.body.dataset) {
          document.body.dataset.format = select.value;
        }
        if (typeof updateCopyState === 'function') {
          updateCopyState();
        }
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: 'setFormat', format: select.value });
        }
      });
      update();
    }
    const copyButton = document.getElementById('btn-copy');
    const copyWithWebApi = async () => {
      if (!navigator.clipboard) {
        return false;
      }
      const svg = document.querySelector('svg');
      if (svg) {
        const s = new XMLSerializer().serializeToString(svg);
        await navigator.clipboard.writeText(s);
        return true;
      }
      const img = document.querySelector('img');
      if (img) {
        if (!window.ClipboardItem) {
          return false;
        }
        const c = document.createElement('canvas');
        c.width = img.naturalWidth;
        c.height = img.naturalHeight;
        const g = c.getContext('2d');
        g.drawImage(img, 0, 0);
        const blob = await new Promise(r => c.toBlob(r, 'image/png'));
        await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
        return true;
      }
      return false;
    };
    const triggerCopy = async () => {
      try {
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: 'copy' });
        } else {
          await copyWithWebApi();
        }
      } catch (e) {}
    };
    let updateCopyState = null;
    if (copyButton) {
      updateCopyState = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        const format = document.body?.dataset?.format || 'svg';
        const clipboardItemAvailable = typeof window.ClipboardItem !== 'undefined';
        const webApiAvailable = !!navigator.clipboard && (format !== 'png' || clipboardItemAvailable);
        if (connected) {
          copyButton.disabled = false;
          copyButton.removeAttribute('title');
        } else if (webApiAvailable) {
          copyButton.disabled = false;
          copyButton.title = 'Host unavailable – using browser clipboard';
          window.setTimeout(updateCopyState, 1000);
        } else {
          copyButton.disabled = true;
          copyButton.title = format === 'png' && !clipboardItemAvailable
            ? 'Clipboard image support is unavailable'
            : 'Clipboard access is unavailable';
          window.setTimeout(updateCopyState, 1000);
        }
      };
      updateCopyState();
      copyButton.addEventListener('click', triggerCopy);
    }
    // Ctrl+C copies SVG or PNG
    document.addEventListener('keydown', async ev => {
      if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
        ev.preventDefault();
        await triggerCopy();
      }
    });
  </script>
</body>
</html>)HTML";
    ReplaceAll(html, L"{{BODY}}", body);
    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? L"svg" : L"png");
    ReplaceAll(html, L"{{PLANTUML_ENCODER}}", PlantumlEncoderScript());
    return html;
}

std::wstring BuildErrorHtml(const std::wstring& message, bool preferSvg) {
    std::wstring safe = message;
    ReplaceAll(safe, L"<", L"&lt;");
    ReplaceAll(safe, L">", L"&gt;");
    return BuildShellHtmlWithBody(L"<div class='err'>" + safe + L"</div>", preferSvg);
}

static bool BuildHtmlFromJavaRender(const std::wstring& umlText, bool preferSvg, std::wstring& outHtml, std::wstring* outSvg, std::vector<unsigned char>* outPng, std::wstring* outErrorMessage) {
    std::wstring svgOut;
    std::vector<unsigned char> pngOut;
    if (!RunPlantUmlJar(umlText, preferSvg, svgOut, pngOut)) {
        if (outErrorMessage) *outErrorMessage = L"Local Java/JAR rendering failed. Check Java installation and plantuml.jar path in the INI file.";
        return false;
    }

    if (preferSvg) {
        outHtml = BuildShellHtmlWithBody(svgOut, true);
    }
    else {
        const std::string b64 = Base64(pngOut);
        std::wstring body = L"<img alt=\"diagram\" src=\"data:image/png;base64,";
        body += FromUtf8(b64);
        body += L"\"/>";
        outHtml = BuildShellHtmlWithBody(body, false);
    }
    if (outSvg) *outSvg = std::move(svgOut);
    if (outPng) *outPng = std::move(pngOut);
    if (outErrorMessage) *outErrorMessage = std::wstring();
    return true;
}

static bool BuildHtmlFromWebRender(const std::wstring& umlText, const std::wstring& sourcePath, bool preferSvg, std::wstring& outHtml, std::wstring* outErrorMessage) {
    const std::wstring escaped = HtmlEscape(umlText);
    std::wstring sourceName = ExtractFileStem(sourcePath);
    if (sourceName.empty()) sourceName = L"plantuml-diagram";
    const std::wstring safeSourceName = HtmlAttributeEscape(sourceName);

    std::wstring html(kWebShellPart1);
    html.append(kWebShellPart2);
    html.append(kWebShellPart3);
    html.append(kWebShellPart4);

    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? L"svg" : L"png");
    ReplaceAll(html, L"{{SOURCE_NAME}}", safeSourceName);
    ReplaceAll(html, L"{{PLANTUML_ENCODER}}", PlantumlEncoderScript());
    ReplaceAll(html, L"{{PLANTUML_SOURCE}}", escaped);

    outHtml.swap(html);
    if (outErrorMessage) *outErrorMessage = std::wstring();
    return true;
}

RenderPipelineResult ExecuteRenderBackend(RenderBackend backend, const std::wstring& text, const std::wstring& sourcePath, bool preferSvg) {
    RenderPipelineResult result;
    result.backend = backend;

    if (backend == RenderBackend::Java) {
        std::wstring html;
        std::wstring svg;
        std::vector<unsigned char> png;
        std::wstring error;
        if (BuildHtmlFromJavaRender(text, preferSvg, html, &svg, &png, &error)) {
            result.success = true;
            result.html = std::move(html);
            result.svg = std::move(svg);
            result.png = std::move(png);
            return result;
        }
        result.errorMessage = !error.empty() ? error : std::wstring(L"Local Java rendering failed.");
        return result;
    }

    if (backend == RenderBackend::Web) {
        std::wstring html;
        std::wstring error;
        if (BuildHtmlFromWebRender(text, sourcePath, preferSvg, html, &error)) {
            result.success = true;
            result.html = std::move(html);
            if (!error.empty()) result.errorMessage = error;
            return result;
        }
        result.errorMessage = !error.empty() ? error : std::wstring(L"PlantUML web rendering failed.");
        return result;
    }

    result.errorMessage = std::wstring(L"Unsupported renderer selected.");
    return result;
}