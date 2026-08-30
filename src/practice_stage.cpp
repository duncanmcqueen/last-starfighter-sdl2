#include "practice_stage.h"

#include <algorithm>

std::uint32_t PracticeStage::next_random() {
    // A tiny deterministic generator keeps tests and cabinet replays stable on
    // every architecture. Only the inclusive integer ranges matter.
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}

void PracticeStage::reset(std::uint32_t random_seed) {
    random_state_ = random_seed == 0 ? 1U : random_seed;
    completion_frame_ = 0;
    result_ = PracticeResult::InProgress;
    for (std::size_t index = 0; index < targets_.size(); ++index) {
        PracticeTarget& target = targets_[index];
        target.status = PracticeTargetStatus::Waiting;
        target.activation_frame =
            static_cast<std::uint32_t>(index + 1) * 50U;
        target.phase_degrees = static_cast<float>((index + 1) * 36U);
        // Angular step: 1 or 3 degrees per frame (1 + 2 * RND(1)).
        target.angular_step = 1 + 2 * static_cast<int>(next_random() % 2U);
        target.vertical_seed = 50 + static_cast<int>(next_random() % 101U);
    }
}

void PracticeStage::update(std::uint32_t frame) {
    if (complete()) {
        return;
    }
    for (PracticeTarget& target : targets_) {
        if (target.status == PracticeTargetStatus::Waiting &&
            frame >= target.activation_frame) {
            target.status = PracticeTargetStatus::Active;
        }
    }
    if (frame >= kRecallFrame) {
        for (PracticeTarget& target : targets_) {
            if (target.status == PracticeTargetStatus::Waiting ||
                target.status == PracticeTargetStatus::Active) {
                target.status = PracticeTargetStatus::Recalled;
            }
        }
        completion_frame_ = frame;
        result_ = PracticeResult::RemainingTargets;
    }
}

bool PracticeStage::destroy(std::size_t target_index, std::uint32_t frame) {
    if (complete() || target_index >= targets_.size() ||
        targets_[target_index].status != PracticeTargetStatus::Active) {
        return false;
    }
    targets_[target_index].status = PracticeTargetStatus::Destroyed;
    if (destroyed_count() == static_cast<int>(kTargetCount)) {
        classify_clear(frame);
    }
    return true;
}

int PracticeStage::destroyed_count() const {
    return static_cast<int>(std::count_if(
        targets_.begin(), targets_.end(), [](const PracticeTarget& target) {
            return target.status == PracticeTargetStatus::Destroyed;
        }));
}

int PracticeStage::remaining_count() const {
    return static_cast<int>(kTargetCount) - destroyed_count();
}

void PracticeStage::classify_clear(std::uint32_t frame) {
    completion_frame_ = frame;
    if (frame < kRecordFrame) {
        result_ = PracticeResult::RecordTime;
    } else if (frame < kTimeToSpareFrame) {
        result_ = PracticeResult::TimeToSpare;
    } else {
        result_ = PracticeResult::Cleared;
    }
}

int PracticeStage::bonus() const {
    switch (result_) {
    case PracticeResult::Cleared:
        return 25000;
    case PracticeResult::TimeToSpare:
        return 50000;
    case PracticeResult::RecordTime:
        return 100000;
    case PracticeResult::InProgress:
    case PracticeResult::RemainingTargets:
        return 0;
    }
    return 0;
}
