#include "text_capture.h"

#include "imgui_symbols.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace imgui_patch
{
namespace
{
struct NativeImVec2
{
    float x;
    float y;
};

struct NativeImVec4
{
    float x;
    float y;
    float z;
    float w;
};

using RenderTextFn = void(__fastcall*)(
    void* font,
    void* draw_list,
    float size,
    const NativeImVec2& pos,
    unsigned int col,
    const NativeImVec4& clip_rect,
    const char* text_begin,
    const char* text_end,
    float wrap_width,
    int flags);

using CalcTextSizeFn = float* (__fastcall*)(
    void* font,
    float* out,
    float size,
    float max_width,
    float wrap_width,
    const char* text_begin,
    const char* text_end,
    void* remaining);

constexpr std::size_t kAbsoluteJumpSize = 14;
constexpr std::size_t kCalcTextSizePatchSize = 16;
constexpr std::size_t kRenderTextPatchSize = 17;
constexpr std::size_t kMaxPatchSize = 32;
constexpr std::size_t kMaxCapturedTextBytes = 4096;
constexpr std::size_t kMaxQueuedTexts = 512;

struct DetourState
{
    std::uintptr_t target = 0;
    void* trampoline = nullptr;
    std::array<unsigned char, kMaxPatchSize> original_bytes = {};
    std::size_t patch_size = 0;
};

std::mutex g_detour_mutex;
DetourState g_calc_text_size_detour;
DetourState g_render_text_detour;
CalcTextSizeFn g_original_calc_text_size = nullptr;
RenderTextFn g_original_render_text = nullptr;
bool g_capture_installed = false;

std::mutex g_queue_mutex;
std::deque<std::string> g_text_queue;
std::unordered_set<std::string> g_seen_texts;

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

bool render_text_has_expected_prologue(const unsigned char* target)
{
    static constexpr unsigned char kExpectedPrefix[] = {
        0x4c, 0x8b, 0xdc, 0x4d, 0x89, 0x4b, 0x20, 0xf3, 0x0f,
        0x11, 0x54, 0x24, 0x18, 0x49, 0x89, 0x53, 0x10,
    };

    return std::memcmp(target, kExpectedPrefix, sizeof(kExpectedPrefix)) == 0;
}

bool calc_text_size_has_expected_prologue(const unsigned char* target)
{
    static constexpr unsigned char kExpectedPrefix[] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x53,
        0x56, 0x48, 0x81, 0xec,
    };

    return std::memcmp(target, kExpectedPrefix, sizeof(kExpectedPrefix)) == 0;
}

bool read_text_range(const char* text_begin, const char* text_end, std::string* text)
{
    text->clear();

    if (text_begin == nullptr) {
        return false;
    }

    std::size_t length = 0;
    if (text_end != nullptr) {
        const auto begin = reinterpret_cast<std::uintptr_t>(text_begin);
        const auto end = reinterpret_cast<std::uintptr_t>(text_end);
        if (end <= begin || end - begin > kMaxCapturedTextBytes) {
            return false;
        }

        length = static_cast<std::size_t>(end - begin);
    } else {
        while (length < kMaxCapturedTextBytes && text_begin[length] != '\0') {
            ++length;
        }
    }

    text->assign(text_begin, length);
    return true;
}

bool contains_non_ascii_text(const std::string& text)
{
    for (unsigned char byte : text) {
        if (byte >= 0x80) {
            return true;
        }
    }

    return false;
}

void enqueue_rendered_text(const char* text_begin, const char* text_end)
{
    std::string text;
    if (!read_text_range(text_begin, text_end, &text)) {
        return;
    }

    if (text.empty() || !contains_non_ascii_text(text)) {
        return;
    }

    std::unique_lock<std::mutex> lock(g_queue_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    if (g_seen_texts.find(text) != g_seen_texts.end()) {
        return;
    }

    if (g_text_queue.size() >= kMaxQueuedTexts) {
        return;
    }

    g_seen_texts.insert(text);
    g_text_queue.push_back(std::move(text));
}

void __fastcall render_text_hook(
    void* font,
    void* draw_list,
    float size,
    const NativeImVec2& pos,
    unsigned int col,
    const NativeImVec4& clip_rect,
    const char* text_begin,
    const char* text_end,
    float wrap_width,
    int flags)
{
    try {
        enqueue_rendered_text(text_begin, text_end);
    } catch (...) {
    }

    const RenderTextFn original = g_original_render_text;
    if (original != nullptr) {
        original(font, draw_list, size, pos, col, clip_rect, text_begin, text_end, wrap_width, flags);
    }
}

float* __fastcall calc_text_size_hook(
    void* font,
    float* out,
    float size,
    float max_width,
    float wrap_width,
    const char* text_begin,
    const char* text_end,
    void* remaining)
{
    try {
        enqueue_rendered_text(text_begin, text_end);
    } catch (...) {
    }

    const CalcTextSizeFn original = g_original_calc_text_size;
    if (original != nullptr) {
        return original(font, out, size, max_width, wrap_width, text_begin, text_end, remaining);
    }

    return out;
}

bool create_trampoline(DetourState* detour)
{
    if (detour->trampoline != nullptr) {
        return true;
    }

    const std::size_t trampoline_size = detour->patch_size + kAbsoluteJumpSize;
    void* trampoline = VirtualAlloc(nullptr, trampoline_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (trampoline == nullptr) {
        set_last_error(win32_message("VirtualAlloc(trampoline)", GetLastError()));
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
    if (patch_size < kAbsoluteJumpSize || patch_size > kMaxPatchSize) {
        set_last_error("invalid detour patch size");
        return false;
    }

    auto* target = reinterpret_cast<unsigned char*>(target_address);
    if (!validate_prologue(target)) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "resolved %s prologue did not match the expected hook patch bytes", name);
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
        set_last_error(win32_message("VirtualProtect(detour)", GetLastError()));
        return false;
    }

    unsigned char patch[kMaxPatchSize] = {};
    write_absolute_jump(patch, hook);
    std::memset(patch + kAbsoluteJumpSize, 0x90, patch_size - kAbsoluteJumpSize);
    std::memcpy(target, patch, patch_size);

    DWORD ignored = 0;
    VirtualProtect(target, patch_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch_size);
    return true;
}

bool restore_detour(DetourState* detour, const char* name)
{
    if (detour->target == 0) {
        return true;
    }

    auto* target = reinterpret_cast<unsigned char*>(detour->target);
    DWORD old_protect = 0;
    if (!VirtualProtect(target, detour->patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        char operation[128] = {};
        std::snprintf(operation, sizeof(operation), "VirtualProtect(%s restore)", name);
        set_last_error(win32_message(operation, GetLastError()));
        return false;
    }

    std::memcpy(target, detour->original_bytes.data(), detour->patch_size);

    DWORD ignored = 0;
    VirtualProtect(target, detour->patch_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, detour->patch_size);
    detour->target = 0;
    return true;
}

void clear_captured_texts()
{
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_text_queue.clear();
    g_seen_texts.clear();
}
}

bool install_text_capture()
{
    std::lock_guard<std::mutex> lock(g_detour_mutex);

    if (g_capture_installed) {
        return true;
    }

    ResolvedImguiSymbols symbols = {};
    if (!resolve_imgui_symbols(&symbols)) {
        return false;
    }

    if (symbols.calc_text_size == 0) {
        set_last_error("CalcTextSize symbol was not resolved");
        return false;
    }

    if (symbols.render_text == 0) {
        set_last_error("RenderText symbol was not resolved");
        return false;
    }

    clear_captured_texts();

    if (!install_detour(
            &g_calc_text_size_detour,
            symbols.calc_text_size,
            kCalcTextSizePatchSize,
            reinterpret_cast<const void*>(&calc_text_size_hook),
            calc_text_size_has_expected_prologue,
            "CalcTextSize")) {
        return false;
    }

    g_original_calc_text_size = reinterpret_cast<CalcTextSizeFn>(g_calc_text_size_detour.trampoline);

    if (!install_detour(
            &g_render_text_detour,
            symbols.render_text,
            kRenderTextPatchSize,
            reinterpret_cast<const void*>(&render_text_hook),
            render_text_has_expected_prologue,
            "RenderText")) {
        restore_detour(&g_calc_text_size_detour, "CalcTextSize");
        g_original_calc_text_size = nullptr;
        return false;
    }

    g_original_render_text = reinterpret_cast<RenderTextFn>(g_render_text_detour.trampoline);
    g_capture_installed = true;
    return true;
}

bool uninstall_text_capture()
{
    std::lock_guard<std::mutex> lock(g_detour_mutex);

    if (!g_capture_installed) {
        clear_captured_texts();
        return true;
    }

    if (!restore_detour(&g_render_text_detour, "RenderText")) {
        return false;
    }

    if (!restore_detour(&g_calc_text_size_detour, "CalcTextSize")) {
        return false;
    }

    g_original_render_text = nullptr;
    g_original_calc_text_size = nullptr;
    g_capture_installed = false;
    clear_captured_texts();
    return true;
}

bool is_text_capture_installed()
{
    std::lock_guard<std::mutex> lock(g_detour_mutex);
    return g_capture_installed;
}

bool pop_captured_text_batch(std::size_t max_texts, CapturedTextBatch* batch)
{
    if (batch == nullptr || max_texts == 0) {
        set_last_error("invalid captured text batch output");
        return false;
    }

    batch->texts.clear();

    std::lock_guard<std::mutex> lock(g_queue_mutex);
    while (!g_text_queue.empty() && batch->texts.size() < max_texts) {
        batch->texts.push_back(std::move(g_text_queue.front()));
        g_text_queue.pop_front();
    }

    return true;
}
}
