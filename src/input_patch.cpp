#include "input_patch.h"

#include "imgui_symbols.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "imm32.lib")

namespace imgui_patch
{
namespace
{
constexpr std::size_t kAbsoluteJumpSize = 14;
constexpr std::size_t kMaxDetourPatchSize = 32;
constexpr std::size_t kMsgHandlerPatchSize = 19;

using AddInputCharFn = void(__fastcall*)(void* io, unsigned int character);
using Win32MsgHandlerFn = LRESULT(__fastcall*)(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* io);

struct DetourState
{
    std::uintptr_t target = 0;
    void* trampoline = nullptr;
    std::array<unsigned char, kMaxDetourPatchSize> original_bytes = {};
    std::size_t patch_size = 0;
};

std::mutex g_input_mutex;
DetourState g_msg_handler_detour;
Win32MsgHandlerFn g_original_msg_handler = nullptr;
AddInputCharFn g_add_input_char = nullptr;
bool g_input_patch_installed = false;
thread_local WPARAM g_pending_surrogate = 0;

bool win32_message_handler_has_expected_prologue(const unsigned char* target)
{
    static constexpr unsigned char kExpectedPrefix[] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70,
        0x10, 0x48, 0x89, 0x78, 0x20, 0x4c, 0x89, 0x40, 0x18,
    };

    return std::memcmp(target, kExpectedPrefix, sizeof(kExpectedPrefix)) == 0;
}

void write_absolute_jump(unsigned char* target, const void* destination)
{
    target[0] = 0xff;
    target[1] = 0x25;
    target[2] = 0x00;
    target[3] = 0x00;
    target[4] = 0x00;
    target[5] = 0x00;

    const auto destination_address = reinterpret_cast<std::uintptr_t>(destination);
    std::memcpy(target + 6, &destination_address, sizeof(destination_address));
}

bool create_trampoline(DetourState* detour)
{
    if (detour->trampoline != nullptr) {
        return true;
    }

    const std::size_t trampoline_size = detour->patch_size + kAbsoluteJumpSize;
    void* trampoline = VirtualAlloc(nullptr, trampoline_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (trampoline == nullptr) {
        set_last_error(win32_message("VirtualAlloc(input trampoline)", GetLastError()));
        return false;
    }

    auto* trampoline_bytes = reinterpret_cast<unsigned char*>(trampoline);
    std::memcpy(trampoline_bytes, reinterpret_cast<const void*>(detour->target), detour->patch_size);
    write_absolute_jump(trampoline_bytes + detour->patch_size, reinterpret_cast<const void*>(detour->target + detour->patch_size));
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_size);

    detour->trampoline = trampoline;
    return true;
}

bool install_detour(
    DetourState* detour,
    std::uintptr_t target_address,
    std::size_t patch_size,
    const void* hook,
    bool (*validate_prologue)(const unsigned char*),
    const char* name)
{
    if (patch_size < kAbsoluteJumpSize || patch_size > kMaxDetourPatchSize) {
        set_last_error("invalid input detour patch size");
        return false;
    }

    auto* target = reinterpret_cast<unsigned char*>(target_address);
    if (!validate_prologue(target)) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "resolved %s prologue did not match", name);
        set_last_error(message);
        return false;
    }

    detour->target = target_address;
    detour->patch_size = patch_size;
    std::memcpy(detour->original_bytes.data(), target, patch_size);

    if (!create_trampoline(detour)) {
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(target, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        set_last_error(win32_message("VirtualProtect(input detour)", GetLastError()));
        return false;
    }

    unsigned char patch[kMaxDetourPatchSize] = {};
    write_absolute_jump(patch, hook);
    std::memset(patch + kAbsoluteJumpSize, 0x90, patch_size - kAbsoluteJumpSize);
    std::memcpy(target, patch, patch_size);

    DWORD ignored = 0;
    VirtualProtect(target, patch_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch_size);
    return true;
}

bool is_ime_open(HWND hwnd)
{
    const HIMC himc = ImmGetContext(hwnd);
    if (himc == nullptr) {
        return false;
    }

    const bool open = ImmGetOpenStatus(himc) != FALSE;
    ImmReleaseContext(hwnd, himc);
    return open;
}

LRESULT __fastcall win32_message_handler_hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* io)
{
    if (message == WM_IME_CHAR || message == WM_CHAR) {
        if (is_ime_open(hwnd)) {
            if (message == WM_IME_CHAR) {
                if (wparam >= 0xD800 && wparam <= 0xDBFF) {
                    g_pending_surrogate = wparam;
                } else if (wparam >= 0xDC00 && wparam <= 0xDFFF && g_pending_surrogate != 0) {
                    const unsigned int codepoint =
                        0x10000 + ((g_pending_surrogate - 0xD800) << 10) + (wparam - 0xDC00);
                    g_pending_surrogate = 0;
                    if (g_add_input_char != nullptr) {
                        g_add_input_char(io, codepoint);
                    }
                } else {
                    g_pending_surrogate = 0;
                    if (wparam > 0 && wparam < 0xD800 && g_add_input_char != nullptr) {
                        g_add_input_char(io, static_cast<unsigned int>(wparam));
                    }
                }
            }

            return 1;
        }

        if (message == WM_CHAR && wparam > 0 && wparam < 0xD800 && g_add_input_char != nullptr) {
            g_add_input_char(io, static_cast<unsigned int>(wparam));
        }

        return 0;
    }

    const Win32MsgHandlerFn original = g_original_msg_handler;
    if (original == nullptr) {
        return 0;
    }

    return original(hwnd, message, wparam, lparam, io);
}
}

bool install_input_patch()
{
    std::lock_guard<std::mutex> lock(g_input_mutex);

    if (g_input_patch_installed) {
        return true;
    }

    ResolvedImguiSymbols symbols = {};
    if (!resolve_imgui_symbols(&symbols)) {
        return false;
    }

    if (symbols.add_input_character == 0) {
        set_last_error("AddInputCharacter symbol was not resolved");
        return false;
    }

    if (symbols.win32_message_handler == 0) {
        set_last_error("Win32 message handler symbol was not resolved");
        return false;
    }

    g_add_input_char = reinterpret_cast<AddInputCharFn>(symbols.add_input_character);

    if (!install_detour(
            &g_msg_handler_detour,
            symbols.win32_message_handler,
            kMsgHandlerPatchSize,
            reinterpret_cast<const void*>(&win32_message_handler_hook),
            win32_message_handler_has_expected_prologue,
            "Win32 message handler")) {
        g_add_input_char = nullptr;
        return false;
    }

    g_original_msg_handler = reinterpret_cast<Win32MsgHandlerFn>(g_msg_handler_detour.trampoline);
    g_input_patch_installed = true;
    return true;
}
}
