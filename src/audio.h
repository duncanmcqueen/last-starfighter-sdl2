// audio.h declares the SDL-based audio mixer. WAV sound effects are loaded,
// converted to the device format, and played as one-shot or looping channels.
// is_playing() tests whether a given sound is currently sounding, the way
// the turret-servo and target-light effects gate on it. Mixing happens in
// the SDL callback at the device rate.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <vector>

// An in-memory sound effect: interleaved 16-bit stereo samples.
struct SoundEffect {
    std::vector<std::int16_t> samples;
};

class AudioOutput final {
  public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    SoundEffect load(const std::filesystem::path& path) const;

    // One-shot playback. Returns a token that stop() can cancel early -
    // needed for long stings (the game-over theme) that must not keep
    // playing after the scene moves on.
    std::size_t play(const SoundEffect& sound);
    void stop(std::size_t token);

    // Several effects only start when their own sound is not already
    // sounding: the turret servo and the level-1 target lights. This is that
    // test, by sound rather than by token, because the caller asks about a
    // sound slot.
    bool is_playing(const SoundEffect& sound) const;

    // Looping channels (engine hum, Death Blossom whine, attract music).
    // Each call returns a token usable with stop_loop(); starting the same
    // sound twice returns the same token instead of stacking copies.
    std::size_t play_loop(const SoundEffect& sound, float gain = 1.0F);
    void stop_loop(std::size_t token);
    void stop_all_loops();

  private:
    struct Channel {
        const SoundEffect* sound{};
        std::size_t cursor{};
        std::size_t token = 0; // non-zero marks a looping channel
        float gain = 1.0F;
        bool looping = false;
    };

    static void mix_callback(void* userdata, std::uint8_t* stream, int length);
    void mix(std::uint8_t* stream, int length);

    SDL_AudioDeviceID device_{};
    SDL_AudioSpec format_{};
    std::vector<Channel> channels_;
    std::size_t next_token_ = 1;
};
