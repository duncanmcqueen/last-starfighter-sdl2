// Exercises the campaign state machine: the level plan, flight-path loading,
// weapons and scoring, a full playthrough to the command ship, recruit mode,
// and the lethal shield check.
#include "campaign.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

int main() {
    const auto& plan = default_campaign_plan();
    const std::array<int, Campaign::kPlanLength> expected{
        1, 2, 3, 4, 3, 5, 6, 5, 6, 11, 7};
    assert(plan == expected);
    assert(campaign_level_info(4).enemy_count == 50);
    assert(campaign_level_info(2).enemy_count == 20);
    assert(campaign_level_info(3).enemy_count == 50);
    assert(campaign_level_info(5).enemy_count == 50);
    assert(campaign_level_info(6).enemy_count == 80);
    assert(campaign_level_info(11).enemy_count == 80);
    assert(campaign_level_info(7).enemy_kind == EnemyKind::CommandShip);

    // The 100x15 array is saved column-major. Exercise a nonzero row
    // and signed values so a row-major or unsigned regression is obvious.
    {
        constexpr std::size_t header = 464;
        constexpr std::size_t rows = 100;
        std::vector<std::uint8_t> bytes(header + rows * 15U * 4U);
        const auto put = [&](std::size_t row, std::size_t column,
                             std::int32_t value) {
            const std::size_t at = header + (column * rows + row) * 4U;
            const std::uint32_t word = static_cast<std::uint32_t>(value);
            for (unsigned byte = 0; byte < 4; ++byte) {
                bytes[at + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
            }
        };
        put(37, 0, -400);
        put(37, 1, -650);
        put(37, 13, 6);
        const auto decoded = Campaign::decode_flight_paths(bytes);
        assert(decoded.size() == 100);
        assert(decoded[37].cells[0] == -400);
        assert(decoded[37].cells[1] == -650);
        assert(decoded[37].cells[13] == 6);
        assert(decoded[0].cells[0] == 0);
    }

    // The shipped dipswitches are the difficulty. arrays/diagnostic.dat gives
    // three shields, Close Shots 6 and Bullseye 10, and both weapon gauges
    // recharge at 0.05 per frame against a cost of one unit per shot.
    const DipSwitches shipped;
    assert(shipped.shields() == 3);
    assert(shipped.close_shots() == 6);
    assert(shipped.bullseye() == 10);
    assert(shipped.coin_to_start() == 1);
    assert(shipped.death_blossom_risk_percent() == 40);
    assert(shipped.laser_recharge_in_play() > 0.049F &&
           shipped.laser_recharge_in_play() < 0.051F);
    assert(shipped.laser_recharge_between_levels() >
           shipped.laser_recharge_in_play());
    {
        DipSwitches edited = shipped;
        edited.values[8] = 7;
        edited.values[20] = 31;
        const std::filesystem::path path = "campaign-test-diagnostic.dat";
        assert(save_dipswitches(path, edited));
        const DipSwitches reloaded = load_dipswitch_file(path, shipped);
        assert(reloaded.values == edited.values);
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    Campaign campaign;
    campaign.reset(1234);
    assert(campaign.level_id() == 1);
    assert(campaign.shields() == shipped.shields());
    assert(campaign.max_shields() == 3);
    campaign.update(PracticeStage::kRecallFrame);
    assert(campaign.complete());
    assert(campaign.advance_level(55));
    assert(campaign.level_id() == 2);
    // 018393 iterates the ten scout records (1..10).  The old port created
    // twelve, which made the first real combat sector denser than the game.
    assert(campaign.enemy_slots() == 10);
    assert(campaign.total_target_count() == 20);
    assert(campaign.remaining_count() == 20);

    // One authored row also pins movement order: heading 90 moves +X first,
    // then the ordinary Y rate is subtracted for the following frame.
    {
        Campaign path_run;
        Campaign::FlightRow row;
        row.cells[0] = 100;
        row.cells[1] = 50;
        row.cells[3] = 90;
        row.cells[7] = 5;
        row.cells[13] = 1;
        path_run.set_flight_paths({row});
        path_run.reset(12);
        path_run.update(PracticeStage::kRecallFrame);
        assert(path_run.advance_level(34));
        const std::uint32_t activation =
            std::max<std::uint32_t>(1U, path_run.enemies()[0].activation_frame);
        path_run.update(activation);
        const CampaignEnemy& moved = path_run.enemies()[0];
        assert(std::fabs(moved.wx - 125.0F) < 0.01F);
        assert(std::fabs(moved.wy - 50.0F) < 0.01F);
        assert(std::fabs(moved.wz - 1300.0F) < 0.01F);
        assert(std::fabs(moved.angle_y - 85.0F) < 0.01F);
    }

    // Ten records are storage, not the level quota. Straight outbound paths
    // finish the first batch and expose a second one; counters remain
    // cumulative even though those same records are overwritten.
    {
        Campaign waves;
        Campaign::FlightRow outbound;
        outbound.cells[2] = 0;
        outbound.cells[3] = 0;
        waves.set_flight_paths({outbound});
        waves.reset(91);
        waves.update(PracticeStage::kRecallFrame);
        assert(waves.advance_level(92));
        for (std::uint32_t frame = 1; frame < 500 && !waves.complete(); ++frame) {
            waves.update(frame);
        }
        assert(waves.complete());
        assert(waves.enemy_slots() == 10);
        assert(waves.activated_count() == 20);
        assert(waves.destroyed_count() == 0);
        assert(waves.remaining_count() == 0);
    }

    // Activation thresholds are random per the phase range, so
    // advance until the first scout is actually on screen.
    std::uint32_t warm = 1;
    for (; warm <= 300U; ++warm) {
        campaign.update(warm);
        if (campaign.enemies()[0].status == EnemyStatus::Active) {
            break;
        }
    }
    assert(warm <= 300U);
    const CampaignEnemy& scout = campaign.enemies()[0];
    assert(scout.status == EnemyStatus::Active);
    const float laser_before = campaign.laser_energy();
    const ShotResult shot = campaign.fire_at(Weapon::Laser, scout.x, scout.y,
                                             0.0F, 0.0F);
    assert(shot.fired);
    assert(shot.hit);
    assert(campaign.laser_energy() < laser_before);
    // One unit of the 0..100 gauge per shot, not the four an earlier
    // revision used to invent.
    assert(laser_before - campaign.laser_energy() > 0.9F);
    assert(laser_before - campaign.laser_energy() < 1.1F);
    const ShotResult finishing_shot = campaign.fire_at(
        Weapon::Laser, scout.x, scout.y, 0.0F, 0.0F);
    assert(finishing_shot.destroyed);
    campaign.update(warm + 1U);
    // The game's setup runs every frame: one finished physical record is
    // reused
    // immediately instead of waiting for the other nine to finish.
    assert(campaign.enemies()[0].status != EnemyStatus::Destroyed);
    campaign.update(warm + 10U);
    assert(campaign.laser_energy() > laser_before - 2.1F);
    // Scout kills add 3,000 points each, and the photon
    // lock takes the active enemy nearest the reticle
    // inside 260 pixels - firing straight at one always locks.
    {
        Campaign points_run;
        points_run.reset(4321);
        while (points_run.level_id() == 1) {
            points_run.update(PracticeStage::kRecallFrame);
            if (!points_run.advance_level(3)) {
                break;
            }
        }
        std::uint32_t warm2 = 1;
        for (; warm2 <= 300U; ++warm2) {
            points_run.update(warm2);
            bool any_active = false;
            for (std::size_t index = 0; index < points_run.enemy_slots();
                 ++index) {
                if (points_run.enemies()[index].status ==
                    EnemyStatus::Active) {
                    any_active = true;
                    break;
                }
            }
            if (any_active) {
                break;
            }
        }
        assert(warm2 <= 300U);
        for (std::size_t index = 0; index < points_run.enemy_slots();
             ++index) {
            const CampaignEnemy& enemy = points_run.enemies()[index];
            if (enemy.status != EnemyStatus::Active) {
                continue;
            }
            const ShotResult photon = points_run.fire_at(
                Weapon::PhotonBolts, enemy.x, enemy.y, 0.0F, 0.0F);
            assert(photon.locked);
            while (enemy.status == EnemyStatus::Active &&
                   points_run.laser_energy() >= 1.0F) {
                points_run.fire_at(Weapon::Laser, enemy.x, enemy.y, 0.0F,
                                   0.0F);
            }
            assert(points_run.current_level_score() == 3000);
            break;
        }
    }

    bool intercepted_missile = false;
    for (std::uint32_t frame = warm + 11U;
         frame < warm + 1100U && !intercepted_missile; ++frame) {
        campaign.update(frame);
        for (const CampaignProjectile& projectile : campaign.projectiles()) {
            if (!projectile.active) {
                continue;
            }
            const ShotResult intercept = campaign.fire_at(
                Weapon::Laser, projectile.x, projectile.y, 0.0F, 0.0F);
            assert(intercept.fired);
            assert(intercept.hit);
            assert(intercept.destroyed);
            intercepted_missile = true;
            break;
        }
    }
    assert(intercepted_missile);

    Campaign full_run;
    DipSwitches no_risk;
    no_risk.values[17] = 5;  // RND(4) never matches: no DB malfunction
    full_run.set_dipswitches(no_risk);
    full_run.reset(9001);
    full_run.update(PracticeStage::kRecallFrame);
    assert(full_run.advance_level(1));
    std::uint32_t seed = 2;
    while (true) {
        std::uint32_t level_start_frame = 1;
        if (full_run.level_id() == 4) {
            assert(full_run.total_target_count() == 50);
            assert(full_run.enemies()[0].max_hit_points == 3);
            // The Euler angles live in the flight fields; they are not a
            // destination vector. Pin both the state-1 initializer and the
            // movement-before-pointing order.
            const CampaignEnemy initial = full_run.enemies()[0];
            assert(initial.flight_type == 0 || initial.flight_type == 1);
            if (initial.flight_type == 1) {
                assert(std::fabs(initial.attack_rate_x -
                                 2200.0F * initial.rate_z) < 0.01F);
                assert(std::fabs(initial.attack_rate_y -
                                 initial.rate_x * initial.rate_z) < 0.01F);
                assert(std::fabs(initial.attack_rate_z) < 0.01F);
            }
            const std::uint32_t activation =
                std::max<std::uint32_t>(1U, initial.activation_frame);
            const float ax = initial.attack_rate_x * 3.14159265358979323846F /
                             180.0F;
            const float ay = initial.attack_rate_y * 3.14159265358979323846F /
                             180.0F;
            full_run.update(activation);
            const CampaignEnemy& moved = full_run.enemies()[0];
            assert(std::fabs(moved.wx -
                             std::sin(ay) * std::cos(ax) * 25.0F) < 0.02F);
            assert(std::fabs(moved.wy + std::sin(ax) * 25.0F) < 0.02F);
            assert(std::fabs(moved.wz -
                             (2500.0F + std::cos(ay) * std::cos(ax) * 25.0F)) <
                   0.02F);
            level_start_frame = activation + 1U;
        } else if (full_run.level_id() == 5) {
            assert(full_run.total_target_count() == 50);
            assert(full_run.enemies()[0].max_hit_points == 3);
        } else if (full_run.level_id() == 6) {
            assert(full_run.total_target_count() == 80);
            assert(full_run.enemies()[0].max_hit_points == 1);
        } else if (full_run.level_id() == 11) {
            assert(full_run.total_target_count() == 80);
            assert(full_run.enemies()[0].max_hit_points == 3);
        } else if (full_run.level_id() == 7) {
            // The command ship is visual geometry plus collision records
            // 2..10; it is not an eleventh shootable fighter-style hull.
            assert(full_run.enemy_slots() == 10);
            assert(full_run.total_target_count() == 9);
            assert(full_run.enemies()[0].kind == EnemyKind::CommandShip);
            assert(!full_run.enemies()[0].targetable);
            assert(full_run.enemies()[0].render_model);
            assert(full_run.enemies()[1].command_surface);
            assert(!full_run.enemies()[1].render_model);
            assert(full_run.enemies()[1].max_hit_points == 5);
            assert(full_run.enemies()[9].max_hit_points == 60);
            assert(full_run.command_phase() == 8);
            assert(full_run.command_phase_age() == 0U);
            // The command ship is object 1001, not a speed-15 fighter. Pin
            // its exact state-8 orbit at frame 60: angle 90 degrees.
            full_run.update(60);
            assert(full_run.command_phase_age() == 60U);
            assert(std::fabs(full_run.enemies()[0].wx + 10.0F) < 0.01F);
            assert(std::fabs(full_run.enemies()[0].wy + 40.0F) < 0.01F);
            assert(std::fabs(full_run.enemies()[0].wz - 740.0F) < 0.01F);
        }
        // Death Blossom carries a malfunction risk, so the test
        // attempts it without asserting success; the swarm either falls or
        // the level times out on escapes.
        if (full_run.level_id() == 11) {
            // Dipswitch 17 = 5 can never match RND(4), so this Blossom is
            // guaranteed to engage and refill every system.
            assert(full_run.death_blossom());
            assert(full_run.laser_energy() == 100.0F);
            assert(full_run.photon_bolts() == 20);
        }
        for (std::uint32_t frame = level_start_frame;
             frame < 6000 && !full_run.complete(); ++frame) {
            full_run.update(frame);
            // Missile interception first (they cost shields), then the
            // best available weapon against enemies: photon bolts when
            // locked, particle beam, laser as the fallback.
            bool fired = false;
            for (const CampaignProjectile& projectile :
                 full_run.projectiles()) {
                if (projectile.active &&
                    full_run.laser_energy() >= 1.0F) {
                    const ShotResult intercept = full_run.fire_at(
                        Weapon::Laser, projectile.x, projectile.y, 0.0F,
                        0.0F);
                    assert(intercept.hit);
                    fired = true;
                    break;
                }
            }
            if (fired) {
                continue;
            }
            for (std::size_t index = 0; index < full_run.enemy_slots();
                 ++index) {
                const CampaignEnemy& enemy = full_run.enemies()[index];
                if (enemy.status != EnemyStatus::Active) {
                    continue;
                }
                if (full_run.photon_bolts() > 0) {
                    full_run.fire_at(Weapon::PhotonBolts, enemy.x, enemy.y,
                                     0.0F, 0.0F);
                } else if (full_run.particle_energy() >= 2.0F) {
                    full_run.fire_at(Weapon::ParticleBeam, enemy.x, enemy.y,
                                     0.0F, 0.0F);
                } else if (full_run.laser_energy() >= 1.0F) {
                    full_run.fire_at(Weapon::Laser, enemy.x, enemy.y, 0.0F,
                                     0.0F);
                }
                break;
            }
        }
        assert(full_run.complete());
        assert(full_run.outcome() != LevelOutcome::Failed);
        if (full_run.outcome() == LevelOutcome::Victory) {
            break;
        }
        assert(full_run.advance_level(seed++));
    }
    assert(full_run.level_id() == 7);
    assert(full_run.score() > 0);
    // Recruit mode: enemies never launch, and weapons fire without spending
    // their gauges or ammunition.
    {
        Campaign recruit;
        recruit.set_mode(GameMode::Recruit);
        assert(recruit.mode() == GameMode::Recruit);
        recruit.reset(4242);
        while (recruit.level_id() == 1) {
            recruit.update(PracticeStage::kRecallFrame);
            if (!recruit.advance_level(5)) {
                break;
            }
        }
        assert(recruit.level_id() != 1);
        // Rapid fire: the gauges never move no matter how often the player
        // pulls the trigger.
        for (int burst = 0; burst < 50; ++burst) {
            const ShotResult spray =
                recruit.fire_at(Weapon::Laser, -1000.0F, -1000.0F, 0.0F, 0.0F);
            assert(spray.fired);
            assert(recruit.laser_energy() == 100.0F);
        }
        for (int burst = 0; burst < 50; ++burst) {
            const ShotResult spray = recruit.fire_at(Weapon::ParticleBeam,
                                                     -1000.0F, -1000.0F, 0.0F,
                                                     0.0F);
            assert(spray.fired);
            assert(recruit.particle_energy() == 100.0F);
        }
        for (int burst = 0; burst < 50; ++burst) {
            const ShotResult spray = recruit.fire_at(Weapon::PhotonBolts,
                                                     -1000.0F, -1000.0F, 0.0F,
                                                     0.0F);
            assert(spray.fired);
            assert(recruit.photon_bolts() == 20);
        }
        int launches = 0;
        bool was[Campaign::kMaxProjectiles] = {};
        for (std::uint32_t frame = 1; frame <= 900U; ++frame) {
            recruit.update(frame);
            const auto& projectiles = recruit.projectiles();
            for (std::size_t index = 0; index < projectiles.size(); ++index) {
                if (projectiles[index].active && !was[index]) {
                    ++launches;
                }
                was[index] = projectiles[index].active;
            }
            if (recruit.complete()) {
                break;
            }
        }
        assert(launches == 0);
        assert(recruit.shields() ==
               std::min(3, recruit.max_shields()));
    }

    // The player must survive exactly max_shields hits and fail on the next
    // one: one is subtracted per hit and the player fails below zero.
    {
        Campaign lethal;
        DipSwitches always_hit;
        always_hit.values[8] = 2;   // shields
        always_hit.values[13] = 0;  // Close Shots 0
        always_hit.values[14] = 0;  // Bullseye 0 == every missile hits
        lethal.set_dipswitches(always_hit);
        lethal.reset(99);
        assert(lethal.advance_level(7) || lethal.level_id() == 1);
        while (lethal.level_id() == 1) {
            lethal.update(PracticeStage::kRecallFrame);
            if (!lethal.advance_level(7)) {
                break;
            }
        }
        assert(lethal.shields() == std::min(2, lethal.max_shields()));
        int seen_low = lethal.shields();
        for (std::uint32_t frame = 1; frame < 4000; ++frame) {
            lethal.update(frame);
            seen_low = std::min(seen_low, lethal.shields());
            if (lethal.outcome() == LevelOutcome::Failed) {
                break;
            }
        }
        assert(lethal.outcome() == LevelOutcome::Failed);
        assert(seen_low < 0);
    }
}
