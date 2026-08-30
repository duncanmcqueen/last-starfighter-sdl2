// music.cpp renders the shipped Strftr.mid attract music to PCM without
// requiring a MIDI synthesizer library. No recorded audio of this theme is
// available, so this is a small built-in wavetable voice: a soft organ
// timbre for melody voices and short noise bursts for percussion (MIDI
// channel 10). Tempo changes and note timing are honored so the music plays
// at the correct speed. The result is a single interleaved stereo sample
// buffer ready for the audio mixer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// A rendered music loop: monophonic samples at the render rate, meant to be
// duplicated into a stereo looping channel.
struct MusicTrack {
    std::vector<std::int16_t> samples;
    int sample_rate = 22050;
};

// Renders a standard MIDI file to PCM using a small built-in wavetable
// voice (soft organ timbre; channel 10 renders short noise bursts for
// percussion). This is an approximation of the theme's sound - no recording
// of it exists - but it keeps the attract music playing without adding a
// platform dependency.
// Throws std::runtime_error on malformed input.
// sample_rate is the output rate (the mixer runs at this rate); max_seconds
// caps the rendered length so a very long track cannot exhaust memory.
MusicTrack render_midi_file(const std::filesystem::path& path,
                            int sample_rate = 22050,
                            double max_seconds = 150.0);
