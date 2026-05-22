#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Глобальні змінні для логування
extern std::wstring g_logPath;
extern bool         g_logEnabled;

// Функції логування
void AppendLog(const std::wstring& message);

// Робота з буфером обміну
bool ClipboardSetUnicodeText(const std::wstring& text);
bool ClipboardSetBinaryData(UINT format, const void* data, size_t size);

// Допоміжні функції (файли та шляхи)
std::wstring GetModuleDir();
bool FileExistsW(const std::wstring& p);
std::wstring ReadFileUtf16OrAnsi(const wchar_t* path);
bool WriteBufferToFile(const std::wstring& path, const void* data, size_t size);
std::wstring ExtractFileStem(const std::wstring& path);

// Робота з текстом (рядками)
void ReplaceAll(std::wstring& inout, const std::wstring& from, const std::wstring& to);
std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field);
std::wstring FromUtf8(const std::string& s);
std::string ToUtf8(const std::wstring& w);
std::wstring ToLowerTrim(const std::wstring& in);
std::wstring HtmlEscape(const std::wstring& text);
std::wstring HtmlAttributeEscape(const std::wstring& text);

// Кодування Base64
std::string Base64(const std::vector<unsigned char>& in);
std::vector<unsigned char> Base64Decode(const std::wstring& in);