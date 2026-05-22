#pragma once

#include <windows.h>
#include <string>
#include <vector>

enum class RenderBackend {
    Java,
    Web,
};

const wchar_t* RenderBackendName(RenderBackend backend);

// Глобальні налаштування, які потрібні Тотал Коммандеру
extern std::wstring g_prefer;
extern std::string  g_detectA;

struct RenderPipelineResult {
    bool success = false;
    RenderBackend backend = RenderBackend::Java;
    std::wstring html;
    std::wstring svg;
    std::vector<unsigned char> png;
    std::wstring errorMessage;
};

// Основні функції рендерингу
void LoadConfigIfNeeded();
RenderBackend GetConfiguredRenderer();
std::wstring GetConfiguredRendererName();

RenderPipelineResult ExecuteRenderBackend(RenderBackend backend,
    const std::wstring& text,
    const std::wstring& sourcePath,
    bool preferSvg);

std::wstring BuildErrorHtml(const std::wstring& message, bool preferSvg);
bool CreateDibFromPng(const std::vector<unsigned char>& png, std::vector<unsigned char>& outDib);