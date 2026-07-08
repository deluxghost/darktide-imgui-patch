#include "imgui_patch_common.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <climits>

namespace imgui_patch
{
char g_last_error[4096] = {};

void set_last_error(const char* message)
{
    if (message == nullptr) {
        message = "";
    }

    std::snprintf(g_last_error, sizeof(g_last_error), "%s", message);
}

void set_last_error(const std::string& message)
{
    set_last_error(message.c_str());
}

bool is_readable_protect(DWORD protect)
{
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    switch (protect & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool is_readable(const void* address, std::size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + size;
    if (end < start) {
        return false;
    }

    auto current = start;
    while (current < end) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0) {
            return false;
        }
        if (info.State != MEM_COMMIT || !is_readable_protect(info.Protect)) {
            return false;
        }

        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= current) {
            return false;
        }

        current = region_end;
    }

    return true;
}

bool read_c_string(std::uintptr_t address, char* buffer, std::size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    for (std::size_t index = 0; index + 1 < buffer_size; ++index) {
        char ch = '\0';
        if (!read_value(address + index, &ch)) {
            buffer[index] = '\0';
            return false;
        }

        buffer[index] = ch;
        if (ch == '\0') {
            return true;
        }
    }

    buffer[buffer_size - 1] = '\0';
    return true;
}

bool append(char** cursor, int* remaining, const char* format, ...)
{
    if (*remaining <= 0) {
        set_last_error("output buffer is full");
        return false;
    }

    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(*cursor, static_cast<std::size_t>(*remaining), format, args);
    va_end(args);

    if (written < 0 || written >= *remaining) {
        set_last_error("output buffer is too small");
        return false;
    }

    *cursor += written;
    *remaining -= written;
    return true;
}

std::string hresult_message(const char* operation, HRESULT result)
{
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed with HRESULT 0x%08X", operation, static_cast<unsigned int>(result));
    return buffer;
}

std::string win32_message(const char* operation, DWORD error)
{
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed with Win32 error %lu", operation, static_cast<unsigned long>(error));
    return buffer;
}

bool contains_non_ascii(const char* value)
{
    if (value == nullptr) {
        return false;
    }

    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(value); *cursor != 0; ++cursor) {
        if (*cursor >= 0x80) {
            return true;
        }
    }

    return false;
}

bool utf16_from_utf8(const char* utf8_text, std::wstring* utf16_text, std::string* error)
{
    if (utf16_text == nullptr || error == nullptr) {
        return false;
    }

    utf16_text->clear();
    if (utf8_text == nullptr || utf8_text[0] == '\0') {
        return true;
    }

    const int utf8_length = static_cast<int>(std::strlen(utf8_text));
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text, utf8_length, nullptr, 0);
    if (size <= 0) {
        *error = win32_message("MultiByteToWideChar(size)", GetLastError());
        return false;
    }

    utf16_text->resize(static_cast<std::size_t>(size));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_text, utf8_length, utf16_text->data(), size) <= 0) {
        *error = win32_message("MultiByteToWideChar(convert)", GetLastError());
        utf16_text->clear();
        return false;
    }

    return true;
}

std::string utf8_from_wide(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) <= 0) {
        return {};
    }

    return result;
}

std::wstring font_key(const FontFileRef& font)
{
    std::wstring result = font.path;
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    result.push_back(L'#');
    result += std::to_wstring(font.face_index);
    return result;
}

std::uint64_t fnv1a_wide(const std::wstring& value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : value) {
        const auto code = static_cast<std::uint32_t>(ch);
        hash ^= static_cast<unsigned char>(code & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<unsigned char>((code >> 8) & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<unsigned char>((code >> 16) & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<unsigned char>((code >> 24) & 0xffu);
        hash *= 1099511628211ull;
    }

    return hash;
}

std::string font_source_name(const std::wstring& key)
{
    char name[kNativeImFontConfigNameSize] = {};
    std::snprintf(name, sizeof(name), "ImguiPatch_%016llx", static_cast<unsigned long long>(fnv1a_wide(key)));
    return name;
}

bool read_file_bytes(const std::wstring& path, std::vector<unsigned char>* bytes, std::string* error)
{
    bytes->clear();

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = win32_message("CreateFileW(font)", GetLastError());
        return false;
    }

    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(file, &file_size)) {
        const DWORD last_error = GetLastError();
        CloseHandle(file);
        *error = win32_message("GetFileSizeEx(font)", last_error);
        return false;
    }

    if (file_size.QuadPart <= 100 || file_size.QuadPart > INT_MAX) {
        CloseHandle(file);
        *error = "Font file size is outside the supported range";
        return false;
    }

    bytes->resize(static_cast<std::size_t>(file_size.QuadPart));
    unsigned char* cursor = bytes->data();
    std::size_t remaining = bytes->size();
    while (remaining > 0) {
        const DWORD chunk_size = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
        DWORD bytes_read = 0;
        if (!ReadFile(file, cursor, chunk_size, &bytes_read, nullptr)) {
            const DWORD last_error = GetLastError();
            CloseHandle(file);
            bytes->clear();
            *error = win32_message("ReadFile(font)", last_error);
            return false;
        }
        if (bytes_read == 0) {
            CloseHandle(file);
            bytes->clear();
            *error = "ReadFile(font) reached EOF before reading the full file";
            return false;
        }

        cursor += bytes_read;
        remaining -= bytes_read;
    }

    CloseHandle(file);
    return true;
}

bool is_probably_pointer(std::uintptr_t value)
{
    if (value < 0x10000) {
        return false;
    }

    if ((value >> 48) != 0 && (value >> 48) != 0x7fff) {
        return false;
    }

    return true;
}
}
