// AudioOutput is a software mixer built on SDL's audio device. WAV sound
// effects are loaded, converted to the device format, and played as looping
// or one-shot channels. Mixing happens in the SDL callback: each stereo block
// is zero-filled then every active channel's samples are summed with its gain
// and clamped to 16-bit range. Channels are identified by a monotonically
// increasing token so playback can be cancelled early.
#include "audio.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

AudioOutput::AudioOutput() {
    SDL_AudioSpec wanted{};
    // Device format: 44.1 kHz, 16-bit signed stereo, 1024-frame buffer.
    // nullptr for the device id means "default device"; bit 0 of the second
    // argument enables capture, which we do not use.
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 2048;
    wanted.callback = &AudioOutput::mix_callback;
    wanted.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, &format_, 0);
    if (device_ == 0) {
        throw std::runtime_error(std::string("cannot open audio: ") +
                                 SDL_GetError());
    }
    SDL_PauseAudioDevice(device_, 0);
}

AudioOutput::~AudioOutput() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
    }
}

SoundEffect AudioOutput::load(const std::filesystem::path& path) const {
    SDL_AudioSpec source{};
    std::uint8_t* source_data = nullptr;
    std::uint32_t source_size = 0;
    if (!SDL_LoadWAV(path.string().c_str(), &source, &source_data,
                     &source_size)) {
        throw std::runtime_error("cannot load " + path.string() + ": " +
                                 SDL_GetError());
    }

    // Convert the WAV to the device format (44.1 kHz 16-bit stereo) so it can
    // be mixed directly. SDL_BuildAudioCVT returns 0 when no conversion is
    // needed (source already matches the device format).
    SDL_AudioCVT conversion{};
    const int needed = SDL_BuildAudioCVT(
        &conversion, source.format, source.channels, source.freq, format_.format,
        format_.channels, format_.freq);
    if (needed < 0) {
        SDL_FreeWAV(source_data);
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<std::uint8_t> converted;
    if (needed == 0) {
        converted.assign(source_data, source_data + source_size);
    } else {
        conversion.len = static_cast<int>(source_size);
        converted.resize(source_size * conversion.len_mult);
        std::memcpy(converted.data(), source_data, source_size);
        conversion.buf = converted.data();
        if (SDL_ConvertAudio(&conversion) != 0) {
            SDL_FreeWAV(source_data);
            throw std::runtime_error(SDL_GetError());
        }
        converted.resize(static_cast<std::size_t>(conversion.len_cvt));
    }
    SDL_FreeWAV(source_data);
    if (converted.size() % sizeof(std::int16_t) != 0) {
        throw std::runtime_error("converted WAV has an invalid sample length");
    }
    SoundEffect result;
    result.samples.resize(converted.size() / sizeof(std::int16_t));
    std::memcpy(result.samples.data(), converted.data(), converted.size());
    return result;
}

std::size_t AudioOutput::play(const SoundEffect& sound) {
    if (sound.samples.empty()) {
        return 0;
    }
    SDL_LockAudioDevice(device_);
    if (channels_.size() >= 16) {
        channels_.erase(channels_.begin());
    }
    const std::size_t token = next_token_++;
    channels_.push_back({&sound, 0, token, 1.0F, false});
    SDL_UnlockAudioDevice(device_);
    return token;
}

void AudioOutput::stop(std::size_t token) {
    if (token == 0) {
        return;
    }
    SDL_LockAudioDevice(device_);
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                   [token](const Channel& channel) {
                                       return channel.token == token;
                                   }),
                    channels_.end());
    SDL_UnlockAudioDevice(device_);
}

bool AudioOutput::is_playing(const SoundEffect& sound) const {
    if (sound.samples.empty()) {
        return false;
    }
    auto* self = const_cast<AudioOutput*>(this);
    SDL_LockAudioDevice(self->device_);
    bool found = false;
    for (const Channel& channel : channels_) {
        if (channel.sound == &sound) {
            found = true;
            break;
        }
    }
    SDL_UnlockAudioDevice(self->device_);
    return found;
}

std::size_t AudioOutput::play_loop(const SoundEffect& sound, float gain) {
    if (sound.samples.empty()) {
        return 0;
    }
    SDL_LockAudioDevice(device_);
    for (Channel& channel : channels_) {
        if (channel.looping && channel.sound == &sound) {
            channel.gain = gain;
            SDL_UnlockAudioDevice(device_);
            return channel.token;
        }
        if (channel.looping && channel.token != 0 &&
            channel.sound->samples.data() == sound.samples.data() &&
            channel.sound->samples.size() == sound.samples.size()) {
            // Same buffer re-loaded into a new SoundEffect instance.
            channel.gain = gain;
            SDL_UnlockAudioDevice(device_);
            return channel.token;
        }
    }
    const std::size_t token = next_token_++;
    channels_.push_back({&sound, 0, token, gain, true});
    SDL_UnlockAudioDevice(device_);
    return token;
}

void AudioOutput::stop_loop(std::size_t token) {
    if (token == 0) {
        return;
    }
    SDL_LockAudioDevice(device_);
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                   [token](const Channel& channel) {
                                       return channel.looping &&
                                              channel.token == token;
                                   }),
                    channels_.end());
    SDL_UnlockAudioDevice(device_);
}

void AudioOutput::stop_all_loops() {
    SDL_LockAudioDevice(device_);
    channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                   [](const Channel& channel) {
                                       return channel.looping;
                                   }),
                    channels_.end());
    SDL_UnlockAudioDevice(device_);
}

void AudioOutput::mix_callback(void* userdata, std::uint8_t* stream,
                               int length) {
    static_cast<AudioOutput*>(userdata)->mix(stream, length);
}

void AudioOutput::mix(std::uint8_t* stream, int length) {
    std::memset(stream, 0, static_cast<std::size_t>(length));
    auto* output = reinterpret_cast<std::int16_t*>(stream);
    // The length is in bytes; convert to the number of interleaved samples.
    // For stereo 16-bit, each frame is 4 bytes (2 samples), so sample_count
    // is the number of individual 16-bit values, not frames.
    const std::size_t sample_count =
        static_cast<std::size_t>(length) / sizeof(std::int16_t);
    for (Channel& channel : channels_) {
        const float gain = channel.gain;
        for (std::size_t index = 0; index < sample_count; ++index) {
            if (channel.cursor >= channel.sound->samples.size()) {
                if (!channel.looping) {
                    break;
                }
                channel.cursor = 0;
            }
            const int mixed = static_cast<int>(output[index]) +
                              static_cast<int>(static_cast<float>(
                                                   channel.sound->samples[
                                                       channel.cursor]) *
                                               gain);
            output[index] = static_cast<std::int16_t>(std::clamp(
                mixed, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            ++channel.cursor;
        }
    }
    channels_.erase(
        std::remove_if(channels_.begin(), channels_.end(), [](const Channel& item) {
            return !item.looping && item.cursor >= item.sound->samples.size();
        }),
        channels_.end());
}
