#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <sstream>
#include <string>
inline uintptr_t UniquePattern(HMODULE module, const char* text) {
    std::vector<int> pattern; std::istringstream input(text); std::string token;
    while (input >> token) pattern.push_back(token == "?" ? -1 : std::stoi(token, nullptr, 16));
    auto base = reinterpret_cast<uintptr_t>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt); uintptr_t found = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto bytes = reinterpret_cast<unsigned char*>(base + section[s].VirtualAddress);
        size_t size = section[s].Misc.VirtualSize;
        for (size_t i = 0; i + pattern.size() <= size; ++i) {
            if (pattern[0] >= 0 && bytes[i] != pattern[0]) continue;
            size_t j = 1;
            for (; j < pattern.size(); ++j) if (pattern[j] >= 0 && bytes[i+j] != pattern[j]) break;
            if (j == pattern.size()) { if (found) return 0; found = reinterpret_cast<uintptr_t>(bytes+i); }
        }
    }
    return found;
}
inline uintptr_t RipTarget(uintptr_t instruction) {
    return instruction + 7 + *reinterpret_cast<int32_t*>(instruction + 3);
}
inline constexpr const char* kSetModelPattern = "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 ? 48 8B 01 FF 50 ? 48 8B 54 24 ? 48 8B CB E8 ? ? ? ? 48 83 C4 ? 5B C3";
inline constexpr const char* kEntityPattern = "48 89 0D ? ? ? ? E9 ? ? ? ? CC";
inline constexpr const char* kControllerPattern = "48 8B 05 ? ? ? ? 41 89 BE";
inline constexpr const char* kButterflyModel = "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl";
