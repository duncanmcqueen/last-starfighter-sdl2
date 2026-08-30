// Exercises the high-score table codec, the built-in MIDI renderer, and the
// diagnostics-menu label lookup.
#include "arcade.h"
#include "music.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // --- High-score table: parse the shipped text layout -------------------
    const fs::path shipped = argc > 1
                                 ? fs::path(argv[1]) / "arrays" /
                                       "StrftrScores.dat"
                                 : fs::path("assets") /
                                       "arrays" / "StrftrScores.dat";
    HighScoreTable table = HighScoreTable::load(shipped);
    const auto& entries = table.entries();
    assert(entries[0].initials == "A R");
    assert(entries[0].score == 1714000);
    assert(entries[9].score == 462000);
    assert(!table.qualifies(0));
    assert(table.qualifies(500000));

    // Insert drops the old tenth place and keeps descending order.
    const std::size_t rank = table.insert("ZQX", 800000);
    assert(rank == 4);
    assert(table.entries()[3].score == 876000);
    assert(table.entries()[4].initials == "ZQX");
    assert(table.entries()[4].score == 800000);
    assert(table.entries()[9].score == 465000);

    // Round-trip through the same CRLF text format.
    const fs::path out = "arcade-test-scores.dat";
    table.save(out);
    HighScoreTable reloaded = HighScoreTable::load(out);
    assert(reloaded.entries()[0].score == 1714000);
    assert(reloaded.entries()[4].initials == "ZQX");
    assert(reloaded.entries()[9].score == 465000);
    std::error_code ec;
    fs::remove(out, ec);

    // Malformed input falls back to an all-zero table rather than throwing.
    const fs::path bad = "arcade-test-bad.dat";
    { std::ofstream junk(bad); junk << "not,a,score,file"; }
    const HighScoreTable fallback = HighScoreTable::load(bad);
    assert(fallback.entries()[0].score == 0);
    fs::remove(bad, ec);

    // --- MIDI renderer: synthesize a tiny valid file -----------------------
    std::vector<std::uint8_t> midi;
    const auto push_be32 = [&midi](std::uint32_t v) {
        midi.push_back(static_cast<std::uint8_t>(v >> 24U));
        midi.push_back(static_cast<std::uint8_t>(v >> 16U));
        midi.push_back(static_cast<std::uint8_t>(v >> 8U));
        midi.push_back(static_cast<std::uint8_t>(v));
    };
    const auto push_be16 = [&midi](std::uint16_t v) {
        midi.push_back(static_cast<std::uint8_t>(v >> 8U));
        midi.push_back(static_cast<std::uint8_t>(v));
    };
    // Header: format 0, one track, 480 ticks per quarter.
    midi.insert(midi.end(), {'M', 'T', 'h', 'd'});
    push_be32(6);
    push_be16(0);
    push_be16(1);
    push_be16(480);
    // Track: note on C4, wait a quarter, note off, end of track.
    std::vector<std::uint8_t> track;
    track.push_back(0x00);
    track.insert(track.end(), {0x90, 60, 100});
    track.push_back(0x83);                    // delta 480 (variable length)
    track.push_back(0x70);
    track.insert(track.end(), {0x80, 60, 0});
    track.push_back(0x00);
    track.insert(track.end(), {0xFF, 0x2F, 0x00});
    midi.insert(midi.end(), {'M', 'T', 'r', 'k'});
    push_be32(static_cast<std::uint32_t>(track.size()));
    midi.insert(midi.end(), track.begin(), track.end());

    const fs::path mid = "arcade-test.mid";
    { std::ofstream out(mid, std::ios::binary);
      out.write(reinterpret_cast<const char*>(midi.data()),
                static_cast<std::streamsize>(midi.size())); }
    const MusicTrack rendered = render_midi_file(mid, 22050, 10.0);
    // One quarter note at default 120 bpm is half a second plus release tail.
    assert(rendered.sample_rate == 22050);
    bool audible = false;
    int peak = 0;
    for (const std::int16_t sample : rendered.samples) {
        const int magnitude = sample < 0 ? -sample : sample;
        peak = peak < magnitude ? magnitude : peak;
        if (magnitude > 200) {
            audible = true;
        }
    }
    assert(audible);
    assert(peak > 500 && peak <= 32767);
    fs::remove(mid, ec);

    // Malformed input throws rather than crashing.
    const fs::path notmidi = "arcade-test-not.mid";
    { std::ofstream junk(notmidi, std::ios::binary); junk << "JUNKJUNK"; }
    bool threw = false;
    try {
        (void)render_midi_file(notmidi, 22050, 1.0);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    fs::remove(notmidi, ec);

    // --- Diagnostics labels -------------------------------------------------
    const std::array<int, 21> values{
        0, 1, 0, 0, 0, 1, 1, 1, 3, 5, 10, 5, 10, 6, 10, 1, 1, 2, 1, 5, 25};
    const std::vector<std::string> rows = diagnostic_rows(values, 8);
    assert(rows.size() == 21);
    assert(rows[8].find("> Shields at start and continue: ") == 0 &&
           rows[8].back() == '3');
    assert(rows[13].find("Close Shots: ") != std::string::npos);
    assert(rows[17].find("Risk DB Malfunction: ") != std::string::npos);
    assert(rows[1].find("Switch 1: ") != std::string::npos);

    return 0;
}
