#pragma once

#include <windows.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "WebView2.h"
#include "PlantUmlRunner.h"

extern const wchar_t* kWndClass;

struct Host {
    std::atomic<long> refs{ 1 };
    std::atomic<bool> closing{ false };

    std::mutex stateMutex;

    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    HMODULE   hWvLoader = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  ctrl;
    Microsoft::WRL::ComPtr<ICoreWebView2>            web;
    EventRegistrationToken           navCompletedToken{};
    EventRegistrationToken           webMessageToken{};
    bool                             navCompletedRegistered = false;
    bool                             webMessageRegistered = false;

    std::wstring initialHtml;
    std::wstring sourceFilePath;
    std::wstring lastSvg;
    std::vector<unsigned char> lastPng;
    bool lastPreferSvg = true;
    bool hasRender = false;
    RenderBackend configuredRenderer = RenderBackend::Java;
    RenderBackend activeRenderer = RenderBackend::Java;
    std::wstring firstErrorMessage;
};

// Публічні функції для роботи з вікном
void HostRelease(Host* host);
bool HostRenderAndReload(Host* host, bool preferSvg, const std::wstring& logContext, const std::wstring& failureDialogMessage, bool showDialogOnFailure);
void EnsureWndClass();
void InitWebView(Host* host);