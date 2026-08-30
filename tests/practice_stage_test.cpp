// Exercises the level-1 practice lifecycle: staged activation, destruction,
// the three clear bonuses, and the recall timeout.
#include "practice_stage.h"

#include <cassert>
#include <cstddef>

int main() {
    PracticeStage stage;
    stage.reset(1234);
    assert(stage.remaining_count() == 10);
    stage.update(49);
    assert(stage.targets()[0].status == PracticeTargetStatus::Waiting);
    stage.update(50);
    assert(stage.targets()[0].status == PracticeTargetStatus::Active);
    assert(stage.targets()[1].status == PracticeTargetStatus::Waiting);
    stage.update(500);
    for (const PracticeTarget& target : stage.targets()) {
        assert(target.status == PracticeTargetStatus::Active);
        assert(target.angular_step == 1 || target.angular_step == 3);
        assert(target.vertical_seed >= 50 && target.vertical_seed <= 150);
    }

    for (std::size_t index = 0; index < PracticeStage::kTargetCount; ++index) {
        assert(stage.destroy(index, 1100));
        assert(!stage.destroy(index, 1100));
    }
    assert(stage.complete());
    assert(stage.result() == PracticeResult::RecordTime);
    assert(stage.score() == 110000);

    stage.reset(99);
    stage.update(500);
    for (std::size_t index = 0; index < PracticeStage::kTargetCount; ++index) {
        assert(stage.destroy(index, 1300));
    }
    assert(stage.result() == PracticeResult::TimeToSpare);
    assert(stage.bonus() == 50000);

    stage.reset(77);
    stage.update(500);
    for (std::size_t index = 0; index < PracticeStage::kTargetCount; ++index) {
        assert(stage.destroy(index, 1510));
    }
    assert(stage.result() == PracticeResult::Cleared);
    assert(stage.bonus() == 25000);

    stage.reset(55);
    stage.update(PracticeStage::kRecallFrame);
    assert(stage.result() == PracticeResult::RemainingTargets);
    assert(stage.remaining_count() == 10);
    for (const PracticeTarget& target : stage.targets()) {
        assert(target.status == PracticeTargetStatus::Recalled);
    }
}
