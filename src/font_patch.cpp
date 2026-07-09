#include "font_patch.h"

#include "clipboard_patch.h"
#include "font_batch.h"
#include "imgui_atlas.h"
#include "input_patch.h"
#include "text_capture.h"

#include <chrono>
#include <exception>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace imgui_patch
{
namespace
{
constexpr std::size_t kMaxCapturedTextsPerBatch = 64;

std::mutex g_capture_mutex;
std::future<PreparedFontBatch> g_capture_future;
bool g_capture_active = false;
bool g_capture_ready = false;
PreparedFontBatch g_capture_batch;

void reset_capture_preparation()
{
    g_capture_active = false;
    g_capture_ready = false;
    g_capture_batch = {};
    g_capture_future = {};
}

std::string join_captured_texts(const std::vector<std::string>& texts)
{
    std::string result;
    for (const std::string& text : texts) {
        if (!result.empty()) {
            result.push_back('\n');
        }

        result += text;
    }

    return result;
}

bool write_status(char* output_buffer, int output_buffer_size, const char* name, const char* value)
{
    if (output_buffer == nullptr || output_buffer_size <= 0) {
        set_last_error("invalid output buffer");
        return false;
    }

    output_buffer[0] = '\0';
    char* cursor = output_buffer;
    int remaining = output_buffer_size;
    return append(&cursor, &remaining, "%s=%s\n", name, value);
}
}
}

extern "C" __declspec(dllexport) const char* ImguiPatch_LastError()
{
    return imgui_patch::g_last_error;
}

extern "C" __declspec(dllexport) int ImguiPatch_ConfigureFonts(char* output_buffer, int output_buffer_size)
{
    using namespace imgui_patch;

    try {
        set_last_error("");

        if (!install_clipboard_patch()) {
            return 0;
        }

        if (!install_input_patch()) {
            return 0;
        }

        PreparedFontBatch batch = prepare_base_font_batch();
        if (!batch.error.empty()) {
            set_last_error(batch.error);
            return 0;
        }

        FontBatchApplyResult apply_result;
        if (!apply_prepared_font_batch(&batch, kDefaultFontSizePixels, &apply_result)) {
            return 0;
        }

        return write_status(
            output_buffer,
            output_buffer_size,
            "configure_status",
            apply_result.atlas_locked ? "atlas_locked" : "applied") ? 1 : 0;
    } catch (const std::exception& error) {
        set_last_error(std::string("native exception: ") + error.what());
        return 0;
    } catch (...) {
        set_last_error("unknown native exception");
        return 0;
    }
}

extern "C" __declspec(dllexport) int ImguiPatch_InstallTextCapture(char* output_buffer, int output_buffer_size)
{
    using namespace imgui_patch;

    try {
        set_last_error("");

        if (!install_text_capture()) {
            return 0;
        }

        return write_status(output_buffer, output_buffer_size, "capture_status", "installed") ? 1 : 0;
    } catch (const std::exception& error) {
        set_last_error(std::string("native exception: ") + error.what());
        return 0;
    } catch (...) {
        set_last_error("unknown native exception");
        return 0;
    }
}

extern "C" __declspec(dllexport) int ImguiPatch_PollTextCapture(char* output_buffer, int output_buffer_size)
{
    using namespace imgui_patch;

    try {
        set_last_error("");

        if (!is_text_capture_installed()) {
            return write_status(output_buffer, output_buffer_size, "capture_status", "not_installed") ? 1 : 0;
        }

        std::lock_guard<std::mutex> lock(g_capture_mutex);
        if (g_capture_active && !g_capture_ready) {
            if (!g_capture_future.valid()) {
                set_last_error("text capture font preparation future is invalid");
                reset_capture_preparation();
                return 0;
            }

            if (g_capture_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return write_status(output_buffer, output_buffer_size, "capture_status", "pending") ? 1 : 0;
            }

            g_capture_batch = g_capture_future.get();
            g_capture_ready = true;
        }

        if (g_capture_active && g_capture_ready) {
            if (!g_capture_batch.error.empty()) {
                set_last_error(g_capture_batch.error);
                reset_capture_preparation();
                return 0;
            }

            FontBatchApplyResult apply_result;
            if (!apply_prepared_font_batch(&g_capture_batch, 0.0f, &apply_result)) {
                return 0;
            }

            if (apply_result.atlas_locked) {
                return write_status(output_buffer, output_buffer_size, "capture_status", "atlas_locked") ? 1 : 0;
            }

            reset_capture_preparation();
            return write_status(output_buffer, output_buffer_size, "capture_status", "applied") ? 1 : 0;
        }

        CapturedTextBatch captured;
        if (!pop_captured_text_batch(kMaxCapturedTextsPerBatch, &captured)) {
            return 0;
        }

        if (captured.texts.empty()) {
            return write_status(output_buffer, output_buffer_size, "capture_status", "idle") ? 1 : 0;
        }

        const std::string text = join_captured_texts(captured.texts);
        g_capture_batch = {};
        g_capture_ready = false;
        g_capture_active = true;
        g_capture_future = std::async(std::launch::async, [text]() {
            return prepare_font_batch_for_text(text);
        });

        return write_status(output_buffer, output_buffer_size, "capture_status", "preparing") ? 1 : 0;
    } catch (const std::exception& error) {
        set_last_error(std::string("native exception: ") + error.what());
        return 0;
    } catch (...) {
        set_last_error("unknown native exception");
        return 0;
    }
}

extern "C" __declspec(dllexport) int ImguiPatch_UninstallTextCapture(char* output_buffer, int output_buffer_size)
{
    using namespace imgui_patch;

    try {
        set_last_error("");

        {
            std::lock_guard<std::mutex> lock(g_capture_mutex);
            reset_capture_preparation();
        }

        if (!uninstall_text_capture()) {
            return 0;
        }

        return write_status(output_buffer, output_buffer_size, "capture_status", "uninstalled") ? 1 : 0;
    } catch (const std::exception& error) {
        set_last_error(std::string("native exception: ") + error.what());
        return 0;
    } catch (...) {
        set_last_error("unknown native exception");
        return 0;
    }
}

extern "C" __declspec(dllexport) int ImguiPatch_WantsTextInput()
{
    using namespace imgui_patch;

    try {
        set_last_error("");

        std::uintptr_t context = 0;
        if (!locate_imgui_context(&context)) {
            return 0;
        }

        unsigned char wants_text_input = 0;
        if (!read_value(context + kContextWantTextInputOffset, &wants_text_input)) {
            set_last_error("ImGuiIO.WantTextInput offset is not readable");
            return 0;
        }

        return wants_text_input != 0 ? 1 : 0;
    } catch (const std::exception& error) {
        set_last_error(std::string("native exception: ") + error.what());
        return 0;
    } catch (...) {
        set_last_error("unknown native exception");
        return 0;
    }
}
