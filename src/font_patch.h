#pragma once

extern "C" __declspec(dllexport) int ImguiPatch_ConfigureFonts(char* output_buffer, int output_buffer_size);
extern "C" __declspec(dllexport) int ImguiPatch_InstallTextCapture(char* output_buffer, int output_buffer_size);
extern "C" __declspec(dllexport) int ImguiPatch_PollTextCapture(char* output_buffer, int output_buffer_size);
extern "C" __declspec(dllexport) int ImguiPatch_UninstallTextCapture(char* output_buffer, int output_buffer_size);
extern "C" __declspec(dllexport) int ImguiPatch_WantsTextInput();
extern "C" __declspec(dllexport) const char* ImguiPatch_LastError();
