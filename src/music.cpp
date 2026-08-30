// Renders a standard MIDI file to PCM using a small built-in wavetable voice.
// The MIDI container is parsed by hand: the header gives the track count and
// ticks-per-quarter-note, note-on/note-off events become absolute-tick ranges,
// and SMF tempo metatags (type 0x51) convert ticks to seconds. Each note is
// summed into the stereo buffer with a per-note envelope and then the whole
// mix is normalized to just under full scale, emulating the arcade
// synthesizer's limiter so dense chords do not hard-clip.
#include "music.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {

// A fully-resolved note: absolute start/end times in seconds, its frequency
// in Hz, normalized velocity (0..1), and whether it is a percussion hit.
struct NoteEvent {
    double start = 0.0;
    double end = 0.0;
    double frequency = 440.0;
    float velocity = 1.0F;
    bool percussion = false;
};

// Reads a big-endian 32-bit value from a MIDI file. MIDI stores all multi-byte
// integers big-endian, unlike the little-endian asset arrays elsewhere.
std::uint32_t read_be32(const std::vector<std::uint8_t>& data,
                        std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

std::uint16_t read_be16(const std::vector<std::uint8_t>& data,
                        std::size_t offset) {
    return static_cast<std::uint16_t>((data[offset] << 8U) | data[offset + 1]);
}

std::uint32_t read_varlen(const std::vector<std::uint8_t>& data,
                          std::size_t& cursor) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t byte = data.at(cursor++);
        value = (value << 7U) | static_cast<std::uint32_t>(byte & 0x7FU);
        if ((byte & 0x80U) == 0U) {
            break;
        }
    }
    return value;
}

// Deterministic pseudo-noise for percussion voices.
float noise_sample(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return static_cast<float>(static_cast<int>(state >> 8U) % 20001) / 10000.0F -
           1.0F;
}

} // namespace

MusicTrack render_midi_file(const std::filesystem::path& path, int sample_rate,
                            double max_seconds) {
    // Read the whole file into memory; MIDI tracks are small and this keeps
    // the parser simple.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (data.size() < 14 || std::memcmp(data.data(), "MThd", 4) != 0) {
        throw std::runtime_error(path.string() + " is not a MIDI file");
    }
    const std::uint32_t header_length = read_be32(data, 4);
    const std::uint16_t track_count = read_be16(data, 10);
    const std::uint16_t division = read_be16(data, 12);
    if (division == 0 || (division & 0x8000U) != 0U) {
        throw std::runtime_error("unsupported MIDI time division");
    }
    const double ticks_per_quarter = static_cast<double>(division);

    // Collect absolute-tick note events per track while recording tempo
    // changes so ticks can be converted to seconds afterwards.
    struct RawNote {
        std::uint64_t start_tick = 0;
        std::uint64_t end_tick = 0;
        int channel = 0;
        int pitch = 0;
        int velocity = 0;
    };
    std::vector<RawNote> notes;
    std::vector<std::pair<std::uint64_t, int>> tempos; // tick -> us/quarter
    tempos.emplace_back(0, 500000);

    std::size_t cursor = 8 + header_length;
    for (int track = 0; track < track_count &&
                        cursor + 8 <= data.size();
         ++track) {
        if (std::memcmp(data.data() + cursor, "MTrk", 4) != 0) {
            break;
        }
        const std::uint32_t length = read_be32(data, cursor + 4);
        const std::size_t end = cursor + 8 + length;
        std::size_t p = cursor + 8;
        std::uint64_t tick = 0;
        std::uint8_t running_status = 0;
        struct OpenNote {
            int channel;
            int pitch;
            std::uint64_t start_tick;
            int velocity;
        };
        std::vector<OpenNote> open;
        while (p < end) {
            tick += read_varlen(data, p);
            std::uint8_t status = data.at(p);
            if ((status & 0x80U) != 0U) {
                ++p;
                running_status = status;
            } else {
                status = running_status;
                if (status == 0U) {
                    // Stray data byte after running status was cancelled;
                    // the track cannot be resynchronized reliably.
                    break;
                }
            }
            if (status == 0xFFU) {
                const std::uint8_t type = data.at(p++);
                const std::uint32_t size = read_varlen(data, p);
                if (type == 0x51U && size == 3 && p + 2 < end) {
                    const int usec = (data[p] << 16U) | (data[p + 1] << 8U) |
                                     data[p + 2];
                    tempos.emplace_back(tick, usec);
                } else if (type == 0x2FU) {
                    break;
                }
                p += size;
                // Meta and sysex events cancel running status.
                running_status = 0;
            } else if (status == 0xF0U || status == 0xF7U) {
                p += read_varlen(data, p);
                running_status = 0;
            } else {
                const std::uint8_t kind = status & 0xF0U;
                const int channel = status & 0x0FU;
                switch (kind) {
                case 0x90U:
                case 0x80U: {
                    const int pitch = data.at(p++);
                    const int velocity = data.at(p++);
                    if (kind == 0x90U && velocity > 0) {
                        open.push_back({channel, pitch, tick, velocity});
                    } else {
                        for (std::size_t i = open.size(); i-- > 0;) {
                            if (open[i].channel == channel &&
                                open[i].pitch == pitch) {
                                notes.push_back({open[i].start_tick, tick,
                                                 channel, pitch,
                                                 open[i].velocity});
                                open.erase(open.begin() +
                                           static_cast<std::ptrdiff_t>(i));
                                break;
                            }
                        }
                    }
                    break;
                }
                case 0xC0U:
                case 0xD0U:
                    ++p;
                    break;
                default:
                    p += 2;
                    break;
                }
            }
        }
        // Unterminated notes still sound until the loop point.
        for (const OpenNote& note : open) {
            notes.push_back({note.start_tick, tick + 1, note.channel,
                             note.pitch, note.velocity});
        }
        cursor = end;
    }
    if (notes.empty()) {
        throw std::runtime_error("MIDI file contains no notes");
    }

    std::sort(tempos.begin(), tempos.end());
    auto tick_to_seconds = [&](std::uint64_t tick) {
        double seconds = 0.0;
        std::uint64_t previous_tick = 0;
        double us_per_quarter = 500000.0;
        for (const auto& change : tempos) {
            if (change.first >= tick) {
                break;
            }
            seconds += static_cast<double>(change.first - previous_tick) *
                       us_per_quarter / ticks_per_quarter / 1000000.0;
            previous_tick = change.first;
            us_per_quarter = static_cast<double>(change.second);
        }
        seconds += static_cast<double>(tick - previous_tick) * us_per_quarter /
                   ticks_per_quarter / 1000000.0;
        return seconds;
    };

    std::vector<NoteEvent> events;
    events.reserve(notes.size());
    double sequence_end = 0.0;
    for (const RawNote& note : notes) {
        NoteEvent event;
        event.start = tick_to_seconds(note.start_tick);
        event.end = std::max(tick_to_seconds(note.end_tick), event.start + 0.05);
        event.frequency =
            440.0 * std::pow(2.0, (static_cast<double>(note.pitch) - 69.0) / 12.0);
        event.velocity = static_cast<float>(note.velocity) / 127.0F;
        event.percussion = note.channel == 9;
        sequence_end = std::max(sequence_end, event.end);
        events.push_back(event);
    }

    MusicTrack result;
    result.sample_rate = sample_rate;
    const double render_seconds = std::min(sequence_end, max_seconds);
    result.samples.assign(
        static_cast<std::size_t>(render_seconds * sample_rate) + 1U, 0);

    // Voice budget: notes are summed in file order and clamped, so even a
    // dense arrangement stays within real-time mixing cost when played back.
    std::uint32_t noise_state = 1234567U;
    const double release_seconds = 0.06;
    for (const NoteEvent& event : events) {
        const std::size_t start_index = static_cast<std::size_t>(
            event.start * sample_rate);
        const std::size_t end_index = std::min(
            static_cast<std::size_t>(event.end * sample_rate),
            result.samples.size());
        const float gain = 0.22F * event.velocity;
        for (std::size_t i = start_index; i < end_index; ++i) {
            const double t = static_cast<double>(i - start_index) /
                             static_cast<double>(sample_rate);
            const double duration =
                static_cast<double>(end_index - start_index) / sample_rate;
            double envelope = 1.0;
            if (t < 0.008) {
                envelope = t / 0.008;
            } else if (!event.percussion) {
                envelope = 0.55 + 0.45 * std::exp(-t * 3.0);
            } else {
                envelope = std::exp(-t * 18.0);
            }
            if (duration - t < release_seconds) {
                envelope *= (duration - t) / release_seconds;
            }
            double wave;
            if (event.percussion) {
                wave = noise_sample(noise_state) * envelope;
            } else {
                wave = 0.62 * std::sin(6.283185307179586 * event.frequency * t) +
                       0.18 * std::sin(6.283185307179586 * 2.0 *
                                       event.frequency * t) +
                       0.08 * std::sin(6.283185307179586 * 3.0 *
                                       event.frequency * t);
                wave *= envelope;
            }
            const double mixed = static_cast<double>(result.samples[i]) +
                                 wave * gain * 32767.0;
            result.samples[i] = static_cast<std::int16_t>(
                std::clamp(mixed, -32768.0, 32767.0));
        }
    }

    // Arcade synthesizers sum many voices into a limiter; emulate that by
    // normalizing the finished mix to just under full scale so dense chords
    // never hard-clip.
    std::int16_t peak = 0;
    for (const std::int16_t sample : result.samples) {
        const std::int16_t magnitude =
            sample < 0 ? static_cast<std::int16_t>(-sample) : sample;
        peak = std::max(peak, magnitude);
    }
    if (peak > 0 && peak != 0) {
        constexpr double kTargetPeak = 26500.0; // ~-1.8 dBFS
        const double scale = kTargetPeak / static_cast<double>(peak);
        if (scale < 1.0) {
            for (std::int16_t& sample : result.samples) {
                sample = static_cast<std::int16_t>(
                    std::lround(static_cast<double>(sample) * scale));
            }
        }
    }
    return result;
}
