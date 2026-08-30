// PracticeStage models the level-1 lifecycle: a batch of targets
// appears on screen and the player must destroy them all before a recall
// timer fires. It is a self-contained, deterministic simulation driven by a
// frame counter so arcade cabinet replays and unit tests are stable across
// architectures. Scoring rewards clearing every target, with faster clears
// worth more (see bonus()).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Lifecycle of a single practice target.
enum class PracticeTargetStatus { Waiting, Active, Destroyed, Recalled };
enum class PracticeResult {
    InProgress,
    RemainingTargets,
    Cleared,
    TimeToSpare,
    RecordTime
};

struct PracticeTarget {
    // Lifecycle state of this target within the practice round.
    PracticeTargetStatus status = PracticeTargetStatus::Waiting;
    // Frame at which this target becomes active; targets activate in spawn
    // order, one every 50 frames (see PracticeStage::reset).
    std::uint32_t activation_frame{};
    // Initial angular position in degrees; the target rotates by angular_step
    // degrees per frame.
    float phase_degrees{};
    // Per-target rotation step in degrees per frame, derived from a
    // deterministic pseudo-random value so replays are stable.
    int angular_step{};
    // Seed for the vertical oscillation of this target's motion.
    int vertical_seed{};
};

class PracticeStage final {
  public:
    // Number of targets in a practice round.
    static constexpr std::size_t kTargetCount = 10;
    // Frame at which any not-yet-destroyed target is recalled, ending the
    // round with no bonus (RemainingTargets).
    static constexpr std::uint32_t kRecallFrame = 1550;
    // Frame threshold for a TimeToSpare bonus: clear all targets before this.
    static constexpr std::uint32_t kTimeToSpareFrame = 1500;
    // Frame threshold for a RecordTime bonus: clear all targets before this.
    static constexpr std::uint32_t kRecordFrame = 1200;

    void reset(std::uint32_t random_seed);
    void update(std::uint32_t frame);
    bool destroy(std::size_t target_index, std::uint32_t frame);

    const std::array<PracticeTarget, kTargetCount>& targets() const {
        return targets_;
    }
    int destroyed_count() const;
    int remaining_count() const;
    bool complete() const { return result_ != PracticeResult::InProgress; }
    PracticeResult result() const { return result_; }
    int bonus() const;
    int score() const { return destroyed_count() * 1000 + bonus(); }
    std::uint32_t completion_frame() const { return completion_frame_; }

  private:
    std::uint32_t next_random();
    void classify_clear(std::uint32_t frame);

    std::array<PracticeTarget, kTargetCount> targets_{};
    std::uint32_t random_state_ = 1;
    std::uint32_t completion_frame_{};
    PracticeResult result_ = PracticeResult::InProgress;
};

