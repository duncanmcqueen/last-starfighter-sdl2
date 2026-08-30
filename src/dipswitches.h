#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

// The release has no difficulty levels. Difficulty is a bank of arcade
// dipswitches reached by pressing 0 during attract, stored in
// arrays/diagnostic.dat as 21 int32 values and read at word offset 000130.
// Element 0 is unused padding.
//
// The accessors below are the entries whose meaning was confirmed by matching
// each global against the label the diagnostics menu prints for it. The
// remaining entries are toggles (music, attract sound, high-score reset, free
// play, Death Blossom enable, between-level text, continue enable, mouse
 // control) whose individual indices are NOT yet confirmed, so they are only
 // reachable through raw().
struct DipSwitches {
    // Defaults are the values both arrays/diagnostic.dat and
    // arrays/diagnostic orig.dat ship with.
    std::array<int, 21> values{0, 1, 0, 0, 0, 1, 1, 1,  3, 5, 10,
                               5, 10, 6, 10, 1, 1, 2, 1, 5, 25};

    int raw(std::size_t index) const {
        return index < values.size() ? values[index] : 0;
    }

    // "Coin to start: " / "Coin to continue: "
    int coin_to_start() const { return raw(6); }
    int coin_to_continue() const { return raw(7); }

    // "Shields at start and continue: ". This is a count of discrete shields,
    // not a percentage: the game subtracts exactly one per hit and fails
    // the player once the counter drops below zero, so the shipped 3 gives
    // four hits.
    int shields() const { return raw(8); }

    // "Recharge in play: " and "Recharge between levels: ", stored as
    // hundredths. Both weapons hold a 0..100 gauge and each shot costs one
    // unit, so the shipped 0.05 per frame is one sustained shot every twenty
    // frames.
    float laser_recharge_in_play() const {
        return static_cast<float>(raw(9)) / 100.0F;
    }
    float laser_recharge_between_levels() const {
        return static_cast<float>(raw(10)) / 100.0F;
    }
    float particle_recharge_in_play() const {
        return static_cast<float>(raw(11)) / 100.0F;
    }
    float particle_recharge_between_levels() const {
        return static_cast<float>(raw(12)) / 100.0F;
    }

    // "Close Shots: " and "Bullseye: ". Each launched enemy missile rolls
    // RND(n) == 0 against these, so the chance is one in n+1 and a setting of
    // zero means always, which is what the manual means by "0 means enemy
    // accuracy 100%. Lower is deadlier."
    int close_shots() const { return raw(13); }
    int bullseye() const { return raw(14); }

    // "Risk DB Malfunction: ", printed by the menu as (4 - value) * 20.
    int death_blossom_risk_percent() const { return (4 - raw(17)) * 20; }

    // Turret feel, from the menu's "Max Speed: " and the tenths value beside
    // it.
    float turret_acceleration() const {
        return static_cast<float>(raw(19)) / 10.0F;
    }
    int turret_max_speed() const { return raw(20); }
};

// Reads arrays/diagnostic.dat. A missing or malformed file leaves the
// shipped defaults in place rather than failing, so deleting the file
// restores the factory difficulty.
DipSwitches load_dipswitches(const std::filesystem::path& assets);

// Reads an exact diagnostics file, using fallback if it is absent/malformed.
DipSwitches load_dipswitch_file(const std::filesystem::path& path,
                                const DipSwitches& fallback = DipSwitches{});

// Writes the exact 21-element little-endian array payload. The path is
// explicit so operator edits persist outside the immutable release tree.
bool save_dipswitches(const std::filesystem::path& path,
                      const DipSwitches& switches);
