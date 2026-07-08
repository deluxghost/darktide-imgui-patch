#include "writable_memory.h"

namespace imgui_patch
{
namespace
{
bool is_writable_protect(DWORD protect)
{
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    switch (protect & 0xff) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}
}

bool is_writable_memory(std::uintptr_t address, std::size_t size)
{
    if (address == 0 || size == 0) {
        return false;
    }

    const auto end = address + size;
    if (end < address) {
        return false;
    }

    auto current = address;
    while (current < end) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0) {
            return false;
        }
        if (info.State != MEM_COMMIT || !is_writable_protect(info.Protect)) {
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
}
