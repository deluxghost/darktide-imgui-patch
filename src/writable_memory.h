#pragma once

#include "imgui_patch_common.h"

namespace imgui_patch
{
bool is_writable_memory(std::uintptr_t address, std::size_t size);

template <typename T>
bool write_value(std::uintptr_t address, const T& value)
{
    if (!is_writable_memory(address, sizeof(T))) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    return true;
}
}
