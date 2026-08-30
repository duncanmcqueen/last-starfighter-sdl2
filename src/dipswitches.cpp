// dipswitches.cpp implements the codec for the arcade difficulty settings.
// Difficulty is a bank of 21 int32 values stored in arrays/diagnostic.dat as
// little-endian 32-bit words. The decode is explicit little-endian so the
// file reads identically on a big-endian Raspberry Pi build. A missing or
// malformed file falls back to the shipped defaults rather than failing, so an
// operator who deletes the file gets the factory difficulty back.
#include "dipswitches.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

DipSwitches load_dipswitch_file(const std::filesystem::path& path,
                                const DipSwitches& fallback) {
    DipSwitches switches = fallback;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return switches;
    }
    std::vector<unsigned char> bytes(switches.values.size() * 4U);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        return switches;
    }
    for (std::size_t index = 0; index < switches.values.size(); ++index) {
        const std::size_t at = index * 4U;
        // Explicit little-endian decode; the asset must read the same way on
        // a big-endian Raspberry Pi build.
        const auto decoded = static_cast<std::uint32_t>(bytes[at]) |
                             (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
                             (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) |
                             (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
        switches.values[index] = static_cast<std::int32_t>(decoded);
    }
    return switches;
}

DipSwitches load_dipswitches(const std::filesystem::path& assets) {
    return load_dipswitch_file(assets / "arrays" / "diagnostic.dat");
}

bool save_dipswitches(const std::filesystem::path& path,
                      const DipSwitches& switches) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    for (const int value : switches.values) {
        const std::uint32_t word = static_cast<std::uint32_t>(value);
        const std::array<unsigned char, 4> bytes{
            static_cast<unsigned char>(word),
            static_cast<unsigned char>(word >> 8U),
            static_cast<unsigned char>(word >> 16U),
            static_cast<unsigned char>(word >> 24U)};
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(output);
}
