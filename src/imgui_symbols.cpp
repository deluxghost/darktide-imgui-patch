#include "imgui_symbols.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace imgui_patch
{
namespace
{
constexpr char kDearImguiPrefix[] = "Dear ImGui ";

struct ImageSection
{
    const unsigned char* begin = nullptr;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    DWORD characteristics = 0;
    char name[9] = {};
};

struct PatternByte
{
    unsigned char value;
    bool wildcard;
};

std::mutex g_symbol_mutex;
bool g_symbols_resolved = false;
ResolvedImguiSymbols g_symbols;

bool section_name_equals(const ImageSection& section, const char* name)
{
    return std::strncmp(section.name, name, sizeof(section.name)) == 0;
}

bool address_in_section(const ImageSection& section, std::uintptr_t address)
{
    return address >= section.address && address < section.address + section.size;
}

bool address_in_any_writable_section(const std::vector<ImageSection>& sections, std::uintptr_t address)
{
    for (const ImageSection& section : sections) {
        if ((section.characteristics & IMAGE_SCN_MEM_WRITE) != 0 && address_in_section(section, address)) {
            return true;
        }
    }

    return false;
}

bool get_image_sections(std::uintptr_t module_base, std::vector<ImageSection>* sections)
{
    if (module_base == 0 || sections == nullptr) {
        set_last_error("invalid module base");
        return false;
    }

    const auto dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        set_last_error("Darktide module has invalid DOS header");
        return false;
    }

    const auto nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(module_base + static_cast<std::uintptr_t>(dos_header->e_lfanew));
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        set_last_error("Darktide module has invalid NT header");
        return false;
    }

    const auto image_end = module_base + nt_headers->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section_header = IMAGE_FIRST_SECTION(nt_headers);

    sections->clear();
    sections->reserve(nt_headers->FileHeader.NumberOfSections);

    for (WORD index = 0; index < nt_headers->FileHeader.NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& native_section = section_header[index];
        const auto section_address = module_base + native_section.VirtualAddress;
        std::size_t section_size = std::max<std::size_t>(native_section.Misc.VirtualSize, native_section.SizeOfRawData);
        if (section_address >= image_end || section_size == 0) {
            continue;
        }

        if (section_address + section_size > image_end) {
            section_size = image_end - section_address;
        }

        ImageSection section = {};
        section.begin = reinterpret_cast<const unsigned char*>(section_address);
        section.address = section_address;
        section.size = section_size;
        section.characteristics = native_section.Characteristics;
        std::memcpy(section.name, native_section.Name, IMAGE_SIZEOF_SHORT_NAME);
        section.name[IMAGE_SIZEOF_SHORT_NAME] = '\0';
        sections->push_back(section);
    }

    return true;
}

bool find_section(const std::vector<ImageSection>& sections, const char* name, ImageSection* section)
{
    for (const ImageSection& candidate : sections) {
        if (section_name_equals(candidate, name)) {
            *section = candidate;
            return true;
        }
    }

    return false;
}

bool read_unaligned_i32(const unsigned char* address, std::int32_t* value)
{
    if (address == nullptr || value == nullptr) {
        return false;
    }

    std::memcpy(value, address, sizeof(*value));
    return true;
}

bool read_unaligned_pointer(const unsigned char* address, std::uintptr_t* value)
{
    if (address == nullptr || value == nullptr) {
        return false;
    }

    std::memcpy(value, address, sizeof(*value));
    return true;
}

bool resolve_rip_relative_target(const unsigned char* instruction, std::uintptr_t instruction_address, std::uintptr_t* target)
{
    if (instruction == nullptr || target == nullptr) {
        return false;
    }

    if (!((instruction[0] == 0x48 || instruction[0] == 0x4c) &&
          (instruction[1] == 0x8b || instruction[1] == 0x8d || instruction[1] == 0x89) &&
          ((instruction[2] & 0xc7) == 0x05))) {
        return false;
    }

    std::int32_t displacement = 0;
    if (!read_unaligned_i32(instruction + 3, &displacement)) {
        return false;
    }

    *target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction_address + 7) + displacement);
    return true;
}

void find_rip_relative_references(
    const ImageSection& text_section,
    std::uintptr_t target,
    std::vector<std::uintptr_t>* references)
{
    references->clear();

    if (text_section.size < 7) {
        return;
    }

    for (std::size_t offset = 0; offset <= text_section.size - 7; ++offset) {
        const auto instruction = text_section.begin + offset;
        const auto instruction_address = text_section.address + offset;

        std::uintptr_t resolved_target = 0;
        if (!resolve_rip_relative_target(instruction, instruction_address, &resolved_target)) {
            continue;
        }

        if (resolved_target == target) {
            references->push_back(instruction_address);
        }
    }
}

void find_ascii_prefix_in_section(const ImageSection& section, const char* prefix, std::vector<std::uintptr_t>* addresses)
{
    addresses->clear();

    const std::size_t prefix_length = std::strlen(prefix);
    if (prefix_length == 0 || section.size < prefix_length) {
        return;
    }

    for (std::size_t offset = 0; offset <= section.size - prefix_length; ++offset) {
        if (std::memcmp(section.begin + offset, prefix, prefix_length) != 0) {
            continue;
        }

        addresses->push_back(section.address + offset);
    }
}

void find_pointer_slots_to(
    const std::vector<ImageSection>& sections,
    std::uintptr_t target,
    std::vector<std::uintptr_t>* slots)
{
    slots->clear();

    for (const ImageSection& section : sections) {
        if (!section_name_equals(section, ".rdata") && !section_name_equals(section, ".data")) {
            continue;
        }

        if (section.size < sizeof(std::uintptr_t)) {
            continue;
        }

        for (std::size_t offset = 0; offset <= section.size - sizeof(std::uintptr_t); offset += sizeof(std::uintptr_t)) {
            std::uintptr_t value = 0;
            if (!read_unaligned_pointer(section.begin + offset, &value)) {
                continue;
            }

            if (value == target) {
                slots->push_back(section.address + offset);
            }
        }
    }
}

bool looks_like_msvc_function_start(const unsigned char* bytes, std::size_t available)
{
    if (bytes == nullptr || available < 16) {
        return false;
    }

    if (available >= 31 &&
        bytes[0] == 0x48 && bytes[1] == 0x89 && bytes[2] == 0x5c && bytes[3] == 0x24 &&
        bytes[5] == 0x48 && bytes[6] == 0x89 && bytes[7] == 0x74 && bytes[8] == 0x24 &&
        bytes[10] == 0x48 && bytes[11] == 0x89 && bytes[12] == 0x7c && bytes[13] == 0x24 &&
        bytes[15] == 0x55) {
        return true;
    }

    if (bytes[0] == 0x40 && bytes[1] == 0x53) {
        return true;
    }

    return false;
}

bool find_enclosing_function_start(const ImageSection& text_section, std::uintptr_t reference, std::uintptr_t* function_start)
{
    if (!address_in_section(text_section, reference) || function_start == nullptr) {
        return false;
    }

    const std::size_t reference_offset = reference - text_section.address;
    const std::size_t min_offset = reference_offset > 0x600 ? reference_offset - 0x600 : 0;

    for (std::size_t offset = reference_offset; offset >= min_offset; --offset) {
        if (looks_like_msvc_function_start(text_section.begin + offset, text_section.size - offset)) {
            *function_start = text_section.address + offset;
            return true;
        }

        if (offset == 0) {
            break;
        }
    }

    return false;
}

struct TargetReferenceCount
{
    std::uintptr_t target = 0;
    std::uintptr_t first_reference = 0;
    int count = 0;
};

void add_target_reference(std::vector<TargetReferenceCount>* counts, std::uintptr_t target, std::uintptr_t reference)
{
    for (TargetReferenceCount& count : *counts) {
        if (count.target == target) {
            ++count.count;
            return;
        }
    }

    TargetReferenceCount count = {};
    count.target = target;
    count.first_reference = reference;
    count.count = 1;
    counts->push_back(count);
}

bool find_repeated_writable_rip_target(
    const std::vector<ImageSection>& sections,
    const ImageSection& text_section,
    std::uintptr_t range_start,
    std::uintptr_t range_end,
    std::uintptr_t* target)
{
    if (!address_in_section(text_section, range_start) || !address_in_section(text_section, range_end - 1) || target == nullptr) {
        return false;
    }

    const std::size_t start_offset = range_start - text_section.address;
    const std::size_t end_offset = range_end - text_section.address;
    std::vector<TargetReferenceCount> counts;

    if (end_offset < start_offset + 7) {
        return false;
    }

    for (std::size_t offset = start_offset; offset <= end_offset - 7; ++offset) {
        const auto instruction = text_section.begin + offset;
        const auto instruction_address = text_section.address + offset;

        std::uintptr_t resolved_target = 0;
        if (!resolve_rip_relative_target(instruction, instruction_address, &resolved_target)) {
            continue;
        }

        if (!address_in_any_writable_section(sections, resolved_target)) {
            continue;
        }

        add_target_reference(&counts, resolved_target, instruction_address);
    }

    std::uintptr_t best_target = 0;
    int candidate_count = 0;
    int best_count = 0;

    for (const TargetReferenceCount& count : counts) {
        if (count.count < 2) {
            continue;
        }

        ++candidate_count;
        if (count.count > best_count) {
            best_count = count.count;
            best_target = count.target;
        }
    }

    if (candidate_count != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "GImGui storage scan found %d candidates", candidate_count);
        set_last_error(message);
        return false;
    }

    *target = best_target;
    return true;
}

bool matches_pattern(const unsigned char* bytes, const PatternByte* pattern, std::size_t pattern_size)
{
    for (std::size_t index = 0; index < pattern_size; ++index) {
        if (!pattern[index].wildcard && bytes[index] != pattern[index].value) {
            return false;
        }
    }

    return true;
}

bool contains_bytes(const unsigned char* bytes, std::size_t size, const unsigned char* needle, std::size_t needle_size)
{
    if (bytes == nullptr || needle == nullptr || needle_size == 0 || size < needle_size) {
        return false;
    }

    for (std::size_t offset = 0; offset <= size - needle_size; ++offset) {
        if (std::memcmp(bytes + offset, needle, needle_size) == 0) {
            return true;
        }
    }

    return false;
}

struct PlatformCallbackAssignment
{
    std::uintptr_t instruction = 0;
    std::uintptr_t function = 0;
    std::uint32_t slot_offset = 0;
};

bool read_platform_callback_assignment(
    const ImageSection& text_section,
    const unsigned char* instruction,
    std::uintptr_t instruction_address,
    PlatformCallbackAssignment* assignment)
{
    if (instruction == nullptr || assignment == nullptr) {
        return false;
    }

    if (!(instruction[0] == 0x48 && instruction[1] == 0x8d && instruction[2] == 0x05 &&
          instruction[7] == 0x48 && instruction[8] == 0x89 && instruction[9] == 0x83)) {
        return false;
    }

    std::int32_t function_displacement = 0;
    std::int32_t slot_offset = 0;
    if (!read_unaligned_i32(instruction + 3, &function_displacement) ||
        !read_unaligned_i32(instruction + 10, &slot_offset)) {
        return false;
    }

    if (slot_offset <= 0 || static_cast<std::uintptr_t>(slot_offset) >= kContextScanBytes ||
        (slot_offset % static_cast<std::int32_t>(sizeof(std::uintptr_t))) != 0) {
        return false;
    }

    const auto function = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction_address + 7) + function_displacement);
    if (!address_in_section(text_section, function)) {
        return false;
    }

    assignment->instruction = instruction_address;
    assignment->function = function;
    assignment->slot_offset = static_cast<std::uint32_t>(slot_offset);
    return true;
}

bool find_platform_clipboard_offsets(
    const ImageSection& text_section,
    std::uintptr_t version_reference,
    std::uintptr_t* get_clipboard_offset,
    std::uintptr_t* set_clipboard_offset)
{
    if (get_clipboard_offset == nullptr || set_clipboard_offset == nullptr ||
        !address_in_section(text_section, version_reference)) {
        return false;
    }

    const std::size_t reference_offset = version_reference - text_section.address;
    const std::size_t scan_end = std::min<std::size_t>(text_section.size, reference_offset + 0x200);
    std::vector<PlatformCallbackAssignment> assignments;

    if (scan_end < reference_offset + 14) {
        set_last_error("CreateContext scan range is too small for ImGui platform callback slots");
        return false;
    }

    for (std::size_t offset = reference_offset; offset <= scan_end - 14; ++offset) {
        PlatformCallbackAssignment assignment = {};
        if (read_platform_callback_assignment(
                text_section,
                text_section.begin + offset,
                text_section.address + offset,
                &assignment)) {
            assignments.push_back(assignment);
        }
    }

    std::uintptr_t resolved_get_offset = 0;
    std::uintptr_t resolved_set_offset = 0;
    int candidate_count = 0;

    for (std::size_t index = 0; index + 3 < assignments.size(); ++index) {
        const PlatformCallbackAssignment& first = assignments[index];
        const PlatformCallbackAssignment& second = assignments[index + 1];
        const PlatformCallbackAssignment& third = assignments[index + 2];
        const PlatformCallbackAssignment& fourth = assignments[index + 3];

        if (second.slot_offset != first.slot_offset + sizeof(std::uintptr_t) ||
            third.slot_offset != first.slot_offset + sizeof(std::uintptr_t) * 3 ||
            fourth.slot_offset != first.slot_offset + sizeof(std::uintptr_t) * 5) {
            continue;
        }

        if (fourth.instruction - first.instruction > 0x80) {
            continue;
        }

        ++candidate_count;
        resolved_get_offset = first.slot_offset;
        resolved_set_offset = second.slot_offset;
    }

    if (candidate_count != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "ImGui platform callback slot scan found %d candidates", candidate_count);
        set_last_error(message);
        return false;
    }

    *get_clipboard_offset = resolved_get_offset;
    *set_clipboard_offset = resolved_set_offset;
    return true;
}

bool find_add_font_symbol(const ImageSection& text_section, std::uintptr_t* add_font)
{
    static constexpr PatternByte kAddFontPattern[] = {
        {0x40, false}, {0x53, false}, {0x56, false}, {0x57, false}, {0x41, false}, {0x55, false}, {0x41, false}, {0x56, false},
        {0x48, false}, {0x83, false}, {0xec, false}, {0x20, false}, {0x48, false}, {0x83, false}, {0xb9, false}, {0xb0, false},
        {0x02, false}, {0x00, false}, {0x00, false}, {0x00, false}, {0x4c, false}, {0x8b, false}, {0xf2, false}, {0x48, false},
        {0x89, false}, {0x6c, false}, {0x24, false}, {0x50, false}, {0x48, false}, {0x8b, false}, {0xf9, false}, {0x75, false},
        {0x05, false}, {0xe8, false}, {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true}, {0x45, false}, {0x33, false},
        {0xed, false}, {0x4c, false}, {0x89, false}, {0x64, false}, {0x24, false}, {0x58, false}, {0x4c, false}, {0x89, false},
        {0x7c, false}, {0x24, false}, {0x60, false}, {0x41, false}, {0x8d, false}, {0x6d, false}, {0x08, false}, {0x45, false},
        {0x38, false}, {0x6e, false}, {0x35, false}, {0x0f, false}, {0x85, false}, {0x00, true}, {0x00, true}, {0x00, true},
        {0x00, true}, {0x8d, false}, {0x4d, false}, {0x48, false}, {0xff, false}, {0x15, false}, {0x00, true}, {0x00, true},
        {0x00, true}, {0x00, true},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kAddFontPattern) / sizeof(kAddFontPattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for AddFont scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (matches_pattern(text_section.begin + offset, kAddFontPattern, pattern_size)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "AddFont signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *add_font = hits[0];
    return true;
}

bool build_atlas_candidate_has_expected_body(const ImageSection& text_section, std::size_t offset)
{
    const std::size_t remaining = text_section.size - offset;
    const std::size_t scan_size = std::min<std::size_t>(remaining, 0x500);
    const unsigned char* bytes = text_section.begin + offset;

    static constexpr unsigned char kStoreRendererHasTextures[] = {0x44, 0x88, 0x41, 0x51};
    static constexpr unsigned char kTexIsBuiltAddress[] = {0x48, 0x8d, 0x59, 0x52};
    static constexpr unsigned char kAtlasBuilderCheck[] = {0x48, 0x83, 0xb9, 0xb0, 0x02, 0x00, 0x00, 0x00};
    static constexpr unsigned char kSetTexIsBuilt[] = {0xc6, 0x03, 0x01};
    static constexpr unsigned char kStoreFrameCount[] = {0x89, 0xb5, 0xa4, 0x00, 0x00, 0x00};

    return contains_bytes(bytes, scan_size, kStoreRendererHasTextures, sizeof(kStoreRendererHasTextures)) &&
        contains_bytes(bytes, scan_size, kTexIsBuiltAddress, sizeof(kTexIsBuiltAddress)) &&
        contains_bytes(bytes, scan_size, kAtlasBuilderCheck, sizeof(kAtlasBuilderCheck)) &&
        contains_bytes(bytes, scan_size, kSetTexIsBuilt, sizeof(kSetTexIsBuilt)) &&
        contains_bytes(bytes, scan_size, kStoreFrameCount, sizeof(kStoreFrameCount));
}

bool find_build_atlas_symbol(const ImageSection& text_section, std::uintptr_t* build_atlas)
{
    static constexpr PatternByte kBuildAtlasPattern[] = {
        {0x40, false}, {0x53, false}, {0x55, false}, {0x56, false}, {0x57, false}, {0x41, false}, {0x54, false},
        {0x48, false}, {0x83, false}, {0xec, false}, {0x20, false}, {0x44, false}, {0x88, false}, {0x41, false},
        {0x51, false}, {0x8b, false}, {0xf2, false}, {0x48, false}, {0x8d, false}, {0x59, false}, {0x52, false},
        {0x48, false}, {0x8b, false}, {0xf9, false}, {0x45, false}, {0x84, false}, {0xc0, false}, {0x74, false},
        {0x00, true}, {0x48, false}, {0x83, false}, {0xb9, false}, {0xb0, false}, {0x02, false}, {0x00, false},
        {0x00, false}, {0x00, false}, {0xc6, false}, {0x03, false}, {0x01, false},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kBuildAtlasPattern) / sizeof(kBuildAtlasPattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for ImFontAtlas build/update scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (!matches_pattern(text_section.begin + offset, kBuildAtlasPattern, pattern_size)) {
            continue;
        }

        if (build_atlas_candidate_has_expected_body(text_section, offset)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "ImFontAtlas build/update signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *build_atlas = hits[0];
    return true;
}

bool render_text_candidate_has_expected_body(const ImageSection& text_section, std::size_t offset)
{
    const std::size_t remaining = text_section.size - offset;
    const std::size_t scan_size = std::min<std::size_t>(remaining, 0x900);
    const unsigned char* bytes = text_section.begin + offset;

    static constexpr unsigned char kLargeTextLimit[] = {0x48, 0x3d, 0x10, 0x27, 0x00, 0x00};
    static constexpr unsigned char kUtf8AsciiLimit8[] = {0x83, 0xf9, 0x80};
    static constexpr unsigned char kUtf8AsciiLimit32[] = {0x81, 0xf9, 0x80, 0x00, 0x00, 0x00};
    static constexpr unsigned char kNewLineCheck[] = {0x83, 0xf9, 0x0a};
    static constexpr unsigned char kCarriageReturnCheck[] = {0x83, 0xf9, 0x0d};

    return contains_bytes(bytes, scan_size, kLargeTextLimit, sizeof(kLargeTextLimit)) &&
        (contains_bytes(bytes, scan_size, kUtf8AsciiLimit8, sizeof(kUtf8AsciiLimit8)) ||
            contains_bytes(bytes, scan_size, kUtf8AsciiLimit32, sizeof(kUtf8AsciiLimit32))) &&
        contains_bytes(bytes, scan_size, kNewLineCheck, sizeof(kNewLineCheck)) &&
        contains_bytes(bytes, scan_size, kCarriageReturnCheck, sizeof(kCarriageReturnCheck));
}

bool find_render_text_symbol(const ImageSection& text_section, std::uintptr_t* render_text)
{
    static constexpr PatternByte kRenderTextPattern[] = {
        {0x4c, false}, {0x8b, false}, {0xdc, false}, {0x4d, false}, {0x89, false}, {0x4b, false}, {0x20, false},
        {0xf3, false}, {0x0f, false}, {0x11, false}, {0x54, false}, {0x24, false}, {0x18, false}, {0x49, false},
        {0x89, false}, {0x53, false}, {0x10, false}, {0x49, false}, {0x89, false}, {0x4b, false}, {0x08, false},
        {0x56, false}, {0x41, false}, {0x54, false}, {0x41, false}, {0x57, false}, {0x48, false}, {0x81, false},
        {0xec, false}, {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kRenderTextPattern) / sizeof(kRenderTextPattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for RenderText scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (!matches_pattern(text_section.begin + offset, kRenderTextPattern, pattern_size)) {
            continue;
        }

        if (render_text_candidate_has_expected_body(text_section, offset)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "RenderText signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *render_text = hits[0];
    return true;
}

bool calc_text_size_candidate_has_expected_body(const ImageSection& text_section, std::size_t offset)
{
    const std::size_t remaining = text_section.size - offset;
    const std::size_t scan_size = std::min<std::size_t>(remaining, 0x500);
    const unsigned char* bytes = text_section.begin + offset;

    static constexpr unsigned char kNullTextEndStrlenLoop[] = {
        0x48, 0x85, 0xff, 0x75, 0x18, 0x48, 0xc7, 0xc7,
        0xff, 0xff, 0xff, 0xff, 0x0f, 0x1f, 0x44, 0x00,
        0x00, 0x48, 0xff, 0xc7, 0x80, 0x3c, 0x3b, 0x00,
        0x75, 0xf7, 0x48, 0x03, 0xfb,
    };
    static constexpr unsigned char kUtf8AsciiLimit32[] = {0x81, 0xfa, 0x80, 0x00, 0x00, 0x00};
    static constexpr unsigned char kNewLineCheck[] = {0x83, 0xfa, 0x0a};
    static constexpr unsigned char kCarriageReturnCheck[] = {0x83, 0xfa, 0x0d};

    return contains_bytes(bytes, scan_size, kNullTextEndStrlenLoop, sizeof(kNullTextEndStrlenLoop)) &&
        contains_bytes(bytes, scan_size, kUtf8AsciiLimit32, sizeof(kUtf8AsciiLimit32)) &&
        contains_bytes(bytes, scan_size, kNewLineCheck, sizeof(kNewLineCheck)) &&
        contains_bytes(bytes, scan_size, kCarriageReturnCheck, sizeof(kCarriageReturnCheck));
}

bool find_calc_text_size_symbol(const ImageSection& text_section, std::uintptr_t* calc_text_size)
{
    static constexpr PatternByte kCalcTextSizePattern[] = {
        {0x48, false}, {0x8b, false}, {0xc4, false}, {0x48, false}, {0x89, false}, {0x48, false}, {0x08, false},
        {0x53, false}, {0x56, false}, {0x48, false}, {0x81, false}, {0xec, false}, {0x00, true}, {0x00, true},
        {0x00, true}, {0x00, true}, {0x48, false}, {0x8b, false}, {0x9c, false}, {0x24, false}, {0x00, true},
        {0x00, true}, {0x00, true}, {0x00, true}, {0x48, false}, {0x8b, false}, {0xf2, false}, {0x48, false},
        {0x89, false}, {0x68, false}, {0x10, false}, {0x48, false}, {0x8b, false}, {0xe9, false},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kCalcTextSizePattern) / sizeof(kCalcTextSizePattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for CalcTextSize scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (!matches_pattern(text_section.begin + offset, kCalcTextSizePattern, pattern_size)) {
            continue;
        }

        if (calc_text_size_candidate_has_expected_body(text_section, offset)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "CalcTextSize signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *calc_text_size = hits[0];
    return true;
}

bool add_input_character_candidate_has_expected_body(const ImageSection& text_section, std::size_t offset)
{
    const std::size_t remaining = text_section.size - offset;
    const std::size_t scan_size = std::min<std::size_t>(remaining, 0x100);
    const unsigned char* bytes = text_section.begin + offset;

    static constexpr unsigned char kAppAcceptingEventsCheck[] = {
        0x80, 0xb9, 0xe5, 0x0b, 0x00, 0x00, 0x00,
    };
    static constexpr unsigned char kInputEventsQueueLoad[] = {
        0x48, 0x8b, 0x99, 0xe0, 0x00, 0x00, 0x00,
    };
    static constexpr unsigned char kTextEventType[] = {
        0xc7, 0x44, 0x24, 0x20, 0x06, 0x00, 0x00, 0x00,
    };
    static constexpr unsigned char kKeyboardEventSource[] = {
        0xc7, 0x44, 0x24, 0x24, 0x02, 0x00, 0x00, 0x00,
    };

    return contains_bytes(bytes, scan_size, kAppAcceptingEventsCheck, sizeof(kAppAcceptingEventsCheck)) &&
        contains_bytes(bytes, scan_size, kInputEventsQueueLoad, sizeof(kInputEventsQueueLoad)) &&
        contains_bytes(bytes, scan_size, kTextEventType, sizeof(kTextEventType)) &&
        contains_bytes(bytes, scan_size, kKeyboardEventSource, sizeof(kKeyboardEventSource));
}

bool find_add_input_character_symbol(const ImageSection& text_section, std::uintptr_t* add_input_character)
{
    static constexpr PatternByte kAddInputCharacterPattern[] = {
        {0x85, false}, {0xd2, false}, {0x0f, false}, {0x84, false},
        {0x00, true}, {0x00, true}, {0x00, true}, {0x00, true},
        {0x53, false}, {0x48, false}, {0x83, false}, {0xec, false}, {0x40, false},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kAddInputCharacterPattern) / sizeof(kAddInputCharacterPattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for AddInputCharacter scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (!matches_pattern(text_section.begin + offset, kAddInputCharacterPattern, pattern_size)) {
            continue;
        }

        if (add_input_character_candidate_has_expected_body(text_section, offset)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "AddInputCharacter signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *add_input_character = hits[0];
    return true;
}

bool win32_message_handler_candidate_has_expected_body(const ImageSection& text_section, std::size_t offset)
{
    const std::size_t remaining = text_section.size - offset;
    const std::size_t scan_size = std::min<std::size_t>(remaining, 0xa20);
    const unsigned char* bytes = text_section.begin + offset;

    static constexpr unsigned char kBackendDataLoad[] = {
        0x48, 0x8b, 0xb3, 0xa0, 0x00, 0x00, 0x00,
    };
    static constexpr unsigned char kMessageLimitCheck[] = {
        0x81, 0xfa, 0x00, 0x02, 0x00, 0x00,
    };
    static constexpr unsigned char kKeyboardCodePageLoad[] = {
        0x8b, 0x4e, 0x2c,
    };
    static constexpr unsigned char kMbcToWideCharSetup[] = {
        0x44, 0x8b, 0xc8, 0x8b, 0xd0, 0x66, 0x89, 0x7d, 0x7f,
    };
    static constexpr unsigned char kInputEventsQueueLoadBody[] = {
        0x48, 0x8b, 0x8b, 0xe0, 0x00, 0x00, 0x00,
    };

    return contains_bytes(bytes, scan_size, kBackendDataLoad, sizeof(kBackendDataLoad)) &&
        contains_bytes(bytes, scan_size, kMessageLimitCheck, sizeof(kMessageLimitCheck)) &&
        contains_bytes(bytes, scan_size, kKeyboardCodePageLoad, sizeof(kKeyboardCodePageLoad)) &&
        contains_bytes(bytes, scan_size, kMbcToWideCharSetup, sizeof(kMbcToWideCharSetup)) &&
        contains_bytes(bytes, scan_size, kInputEventsQueueLoadBody, sizeof(kInputEventsQueueLoadBody));
}

bool find_win32_message_handler_symbol(const ImageSection& text_section, std::uintptr_t* win32_message_handler)
{
    static constexpr PatternByte kWin32MessageHandlerPattern[] = {
        {0x48, false}, {0x8b, false}, {0xc4, false}, {0x48, false}, {0x89, false},
        {0x58, false}, {0x08, false}, {0x48, false}, {0x89, false}, {0x70, false},
        {0x10, false}, {0x48, false}, {0x89, false}, {0x78, false}, {0x20, false},
        {0x4c, false}, {0x89, false}, {0x40, false}, {0x18, false}, {0x55, false},
        {0x41, false}, {0x54, false}, {0x41, false}, {0x55, false}, {0x41, false},
        {0x56, false}, {0x41, false}, {0x57, false}, {0x48, false}, {0x8d, false},
        {0x68, false}, {0xa9, false}, {0x48, false}, {0x81, false}, {0xec, false},
        {0xa0, false}, {0x00, false}, {0x00, false}, {0x00, false},
    };

    std::vector<std::uintptr_t> hits;
    const std::size_t pattern_size = sizeof(kWin32MessageHandlerPattern) / sizeof(kWin32MessageHandlerPattern[0]);

    if (text_section.size < pattern_size) {
        set_last_error("Darktide .text section is too small for Win32 message handler scan");
        return false;
    }

    for (std::size_t offset = 0; offset <= text_section.size - pattern_size; ++offset) {
        if (!matches_pattern(text_section.begin + offset, kWin32MessageHandlerPattern, pattern_size)) {
            continue;
        }

        if (win32_message_handler_candidate_has_expected_body(text_section, offset)) {
            hits.push_back(text_section.address + offset);
        }
    }

    if (hits.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "Win32 message handler signature scan found %zu candidates", hits.size());
        set_last_error(message);
        return false;
    }

    *win32_message_handler = hits[0];
    return true;
}

bool resolve_imgui_symbols_uncached(ResolvedImguiSymbols* symbols)
{
    if (symbols == nullptr) {
        set_last_error("invalid ImGui symbol output");
        return false;
    }

    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        set_last_error("GetModuleHandleW(NULL) failed");
        return false;
    }

    const auto module_base = reinterpret_cast<std::uintptr_t>(module);

    std::vector<ImageSection> sections;
    if (!get_image_sections(module_base, &sections)) {
        return false;
    }

    ImageSection text_section = {};
    ImageSection rdata_section = {};
    if (!find_section(sections, ".text", &text_section)) {
        set_last_error("Darktide .text section not found");
        return false;
    }

    if (!find_section(sections, ".rdata", &rdata_section)) {
        set_last_error("Darktide .rdata section not found");
        return false;
    }

    std::vector<std::uintptr_t> version_strings;
    find_ascii_prefix_in_section(rdata_section, kDearImguiPrefix, &version_strings);
    if (version_strings.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "Dear ImGui version string scan found %zu candidates", version_strings.size());
        set_last_error(message);
        return false;
    }

    std::vector<std::uintptr_t> version_pointer_slots;
    find_pointer_slots_to(sections, version_strings[0], &version_pointer_slots);
    if (version_pointer_slots.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "Dear ImGui version pointer slot scan found %zu candidates", version_pointer_slots.size());
        set_last_error(message);
        return false;
    }

    std::vector<std::uintptr_t> version_pointer_references;
    find_rip_relative_references(text_section, version_pointer_slots[0], &version_pointer_references);
    if (version_pointer_references.size() != 1) {
        char message[256] = {};
        std::snprintf(message, sizeof(message), "Dear ImGui version pointer xref scan found %zu candidates", version_pointer_references.size());
        set_last_error(message);
        return false;
    }

    std::uintptr_t create_context = 0;
    if (!find_enclosing_function_start(text_section, version_pointer_references[0], &create_context)) {
        set_last_error("failed to find Dear ImGui CreateContext function start");
        return false;
    }

    const std::uintptr_t g_scan_start = create_context;
    std::uintptr_t g_scan_end = version_pointer_references[0] + 0x100;
    const std::uintptr_t text_end = text_section.address + text_section.size;
    if (g_scan_end > text_end) {
        g_scan_end = text_end;
    }

    std::uintptr_t g_imgui_storage = 0;
    if (!find_repeated_writable_rip_target(sections, text_section, g_scan_start, g_scan_end, &g_imgui_storage)) {
        return false;
    }

    std::uintptr_t platform_get_clipboard_text_offset = 0;
    std::uintptr_t platform_set_clipboard_text_offset = 0;
    if (!find_platform_clipboard_offsets(
            text_section,
            version_pointer_references[0],
            &platform_get_clipboard_text_offset,
            &platform_set_clipboard_text_offset)) {
        return false;
    }

    std::uintptr_t add_font = 0;
    if (!find_add_font_symbol(text_section, &add_font)) {
        return false;
    }

    std::uintptr_t build_atlas = 0;
    if (!find_build_atlas_symbol(text_section, &build_atlas)) {
        return false;
    }

    std::uintptr_t calc_text_size = 0;
    if (!find_calc_text_size_symbol(text_section, &calc_text_size)) {
        return false;
    }

    std::uintptr_t render_text = 0;
    if (!find_render_text_symbol(text_section, &render_text)) {
        return false;
    }

    std::uintptr_t add_input_character = 0;
    if (!find_add_input_character_symbol(text_section, &add_input_character)) {
        return false;
    }

    std::uintptr_t win32_message_handler = 0;
    if (!find_win32_message_handler_symbol(text_section, &win32_message_handler)) {
        return false;
    }

    symbols->module_base = module_base;
    symbols->version_string = version_strings[0];
    symbols->version_pointer_slot = version_pointer_slots[0];
    symbols->create_context = create_context;
    symbols->g_imgui_storage = g_imgui_storage;
    symbols->add_font = add_font;
    symbols->build_atlas = build_atlas;
    symbols->calc_text_size = calc_text_size;
    symbols->render_text = render_text;
    symbols->add_input_character = add_input_character;
    symbols->win32_message_handler = win32_message_handler;
    symbols->platform_get_clipboard_text_offset = platform_get_clipboard_text_offset;
    symbols->platform_set_clipboard_text_offset = platform_set_clipboard_text_offset;
    return true;
}
}

bool resolve_imgui_symbols(ResolvedImguiSymbols* symbols)
{
    std::lock_guard<std::mutex> guard(g_symbol_mutex);

    if (!g_symbols_resolved) {
        ResolvedImguiSymbols resolved = {};
        if (!resolve_imgui_symbols_uncached(&resolved)) {
            return false;
        }

        g_symbols = resolved;
        g_symbols_resolved = true;
    }

    if (symbols != nullptr) {
        *symbols = g_symbols;
    }

    return true;
}
}
