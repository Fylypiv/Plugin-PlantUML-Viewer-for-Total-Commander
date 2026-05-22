#include "Utils.h"
#include <shlwapi.h>
#include <algorithm>
#include <cwchar>
#include <mutex>

#pragma comment(lib, "shlwapi.lib")

// Ініціалізація глобальних змінних логування
std::wstring g_logPath;
bool         g_logEnabled = true;
static std::mutex g_logMutex;
static bool       g_logSessionStarted = false;

static std::wstring FormatTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void AppendLog(const std::wstring& message) {
    if (!g_logEnabled || g_logPath.empty()) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!g_logSessionStarted) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
            std::string sep = "\r\n";
            DWORD written = 0;
            WriteFile(h, sep.c_str(), (DWORD)sep.size(), &written, nullptr);
        }
        std::wstring header = FormatTimestamp() + L"--- PlantUML WebView session start ---\r\n";
        std::string headerUtf8 = ToUtf8(header);
        if (!headerUtf8.empty()) {
            DWORD written = 0;
            WriteFile(h, headerUtf8.data(), (DWORD)headerUtf8.size(), &written, nullptr);
        }
        g_logSessionStarted = true;
    }
    std::wstring line = FormatTimestamp() + message + L"\r\n";
    std::string utf8 = ToUtf8(line);
    if (!utf8.empty()) {
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
    CloseHandle(h);
}

bool ClipboardSetUnicodeText(const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return false;
    void* ptr = GlobalLock(mem);
    if (!ptr) { GlobalFree(mem); return false; }
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); return false; }
    return true;
}

bool ClipboardSetBinaryData(UINT format, const void* data, size_t size) {
    if (!data || !size) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    void* ptr = GlobalLock(mem);
    if (!ptr) { GlobalFree(mem); return false; }
    memcpy(ptr, data, size);
    GlobalUnlock(mem);
    if (!SetClipboardData(format, mem)) { GlobalFree(mem); return false; }
    return true;
}

std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH]{}; HMODULE hm{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&GetModuleDir, &hm);
    GetModuleFileNameW(hm, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void ReplaceAll(std::wstring& inout, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = inout.find(from, pos)) != std::wstring::npos) {
        inout.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field) {
    if (json.empty() || field.empty()) return std::wstring();
    const std::wstring needle = L"\"" + field + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return std::wstring();
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return std::wstring();
    size_t quote = json.find_first_of(L"\"'", pos + 1);
    if (quote == std::wstring::npos) return std::wstring();
    const wchar_t delimiter = json[quote];
    size_t end = json.find(delimiter, quote + 1);
    if (end == std::wstring::npos) return std::wstring();
    return json.substr(quote + 1, end - quote - 1);
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ReadFileUtf16OrAnsi(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AppendLog(L"ReadFileUtf16OrAnsi: failed to open file " + std::wstring(path ? path : L"<null>") +
            L" (error=" + std::to_wstring(GetLastError()) + L")");
        return L"";
    }
    DWORD size = GetFileSize(h, nullptr);
    std::string bytes; bytes.resize(size ? size : 0);
    DWORD read = 0;
    if (size && (!ReadFile(h, bytes.data(), size, &read, nullptr) || read != size)) {
        AppendLog(L"ReadFileUtf16OrAnsi: short read for file " + std::wstring(path ? path : L"<null>"));
    }
    CloseHandle(h);

    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        std::wstring w((wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2); return w;
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + 3, (int)bytes.size() - 3, nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + 3, (int)bytes.size() - 3, w.data(), wlen);
        return w;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);

    if (wlen > 0) {
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), wlen);
        return w;
    }

    wlen = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w(wlen, L'\0');
    if (wlen > 0) {
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), w.data(), wlen);
    }
    return w;
}

bool WriteBufferToFile(const std::wstring& path, const void* data, size_t size) {
    if (size > MAXDWORD) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(h);
    return (ok && written == size);
}

std::wstring ToLowerTrim(const std::wstring& in) {
    std::wstring s = in;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](wchar_t c) { return !iswspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t c) { return !iswspace(c); }).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::wstring HtmlEscape(const std::wstring& text) {
    std::wstring out = text;
    ReplaceAll(out, L"&", L"&amp;");
    ReplaceAll(out, L"<", L"&lt;");
    ReplaceAll(out, L">", L"&gt;");
    ReplaceAll(out, L"\"", L"&quot;");
    ReplaceAll(out, L"'", L"&#39;");
    return out;
}

std::wstring HtmlAttributeEscape(const std::wstring& text) {
    return HtmlEscape(text);
}

std::wstring ExtractFileStem(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    const wchar_t* fileName = PathFindFileNameW(path.c_str());
    if (!fileName || !*fileName) return std::wstring();
    std::wstring stem(fileName);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    return stem;
}

std::string Base64(const std::vector<unsigned char>& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0, n = in.size();
    out.reserve(((n + 2) / 3) * 4);
    while (i + 2 < n) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == n) {
        unsigned v = (in[i] << 16);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (i + 2 == n) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

static int Base64DecodeChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return int(c - L'A');
    if (c >= L'a' && c <= L'z') return int(c - L'a') + 26;
    if (c >= L'0' && c <= L'9') return int(c - L'0') + 52;
    if (c == L'+') return 62;
    if (c == L'/') return 63;
    if (c == L'=') return -1;
    return -2;
}

std::vector<unsigned char> Base64Decode(const std::wstring& in) {
    std::vector<unsigned char> out;
    if (in.empty()) return out;
    unsigned int buffer = 0;
    int bitsCollected = 0;
    bool hitPadding = false;
    for (wchar_t wc : in) {
        int decoded = Base64DecodeChar(wc);
        if (decoded < 0) {
            if (decoded == -1) {
                if (bitsCollected == 18) {
                    out.push_back(static_cast<unsigned char>((buffer >> 10) & 0xFF));
                    out.push_back(static_cast<unsigned char>((buffer >> 2) & 0xFF));
                }
                else if (bitsCollected == 12) {
                    out.push_back(static_cast<unsigned char>((buffer >> 4) & 0xFF));
                }
                hitPadding = true;
                break;
            }
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned int>(decoded);
        bitsCollected += 6;
        if (bitsCollected >= 24) {
            out.push_back(static_cast<unsigned char>((buffer >> 16) & 0xFF));
            out.push_back(static_cast<unsigned char>((buffer >> 8) & 0xFF));
            out.push_back(static_cast<unsigned char>(buffer & 0xFF));
            buffer = 0;
            bitsCollected = 0;
        }
    }
    if (!hitPadding) {
        if (bitsCollected == 18) {
            out.push_back(static_cast<unsigned char>((buffer >> 10) & 0xFF));
            out.push_back(static_cast<unsigned char>((buffer >> 2) & 0xFF));
        }
        else if (bitsCollected == 12) {
            out.push_back(static_cast<unsigned char>((buffer >> 4) & 0xFF));
        }
    }
    return out;
}