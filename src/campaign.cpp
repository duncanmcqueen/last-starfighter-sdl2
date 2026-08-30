// campaign.cpp implements the combat state machine: the level plan, enemy
// spawning and fire windows, world-motion enemies, the photon-lock and Death
// Blossom systems, scoring, and the between-level recharge. The level-1
// lifecycle lives in practice_stage.cpp.
#include "campaign.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr std::array<int, Campaign::kPlanLength> kPlan{
    1, 2, 3, 4, 3, 5, 6, 5, 6, 11, 7};
constexpr std::array<LevelInfo, 8> kLevels{{
    {1, "Prepare for target", "Light practice", "Starfighter",
     EnemyKind::Scout, 10},
    {2, "Enemy Scout Ships", "Detected over", "Asteroid",
     EnemyKind::Scout, 20},
    {3, "Scout Ships", "Escaping.", "Pursue and Destroy.",
     EnemyKind::Scout, 50},
    // Four lines, not three: "At Frontier." is easy to drop.
    {4, "Command Ship Detected", "At Frontier.", "Intercept Meteors Before",
     EnemyKind::Meteor, 50, "Impacting Our Base."},
    {5, "Enemy Fighters", "Detected in the", "Next Sector",
     EnemyKind::Fighter, 50},
    {6, "Fighter squadrons", "in convoy.", "Pursue and Destroy.",
     EnemyKind::Fighter, 80},
    {11, "Enemy Swarm", "Amassing in the", "Next Sector",
     EnemyKind::Fighter, 80},
    {7, "Ko-Dan Command", "Ship Detected in", "the Next Sector",
     EnemyKind::CommandShip, 1},
}};

float clamp_radius(float value) {
    return std::clamp(value, 7.0F, 82.0F);
}

} // namespace

const std::array<int, Campaign::kPlanLength>& default_campaign_plan() {
    return kPlan;
}

const LevelInfo& campaign_level_info(int level_id) {
    const auto found = std::find_if(
        kLevels.begin(), kLevels.end(), [level_id](const LevelInfo& info) {
            return info.id == level_id;
        });
    return found == kLevels.end() ? kLevels.front() : *found;
}

bool Campaign::roll_accuracy(int setting) {
    // RND(n) returns 0..n inclusive, so RND(n) == 0 is one chance in n+1 and
    // RND(0) == 0 is always true. That is exactly the manual's "setting
    // either to 0 means enemy accuracy 100%".
    if (setting <= 0) {
        return true;
    }
    return next_random() % (static_cast<std::uint32_t>(setting) + 1U) == 0U;
}

std::uint32_t Campaign::next_random() {
    random_state_ = random_state_ * 1664525U + 1013904223U;
    return random_state_;
}

void Campaign::reset(std::uint32_t seed) {
    plan_index_ = 0;
    banked_score_ = 0;
    shields_ = dip_.shields();
    frontier_damage_ = 0;
    laser_energy_ = 100.0F;
    particle_energy_ = 100.0F;
    photon_bolts_ = 20;
    fighter_spread_ = 50;
    random_state_ = seed == 0 ? 1U : seed;
    begin_level(next_random());
}

void Campaign::begin_level(std::uint32_t seed) {
    random_state_ = seed == 0 ? 1U : seed;
    frame_ = 0;
    outcome_ = LevelOutcome::InProgress;
    photon_locked_ = false;
    death_blossom_available_ = level_id() == 11;
    frontier_damage_ = 0;
    missiles_in_flight_ = 0;
    result_class_ = 0;
    enemy_slots_ = 0;
    total_target_count_ = 0;
    spawned_total_ = 0;
    activated_total_ = 0;
    wave_activation_base_ = 0;
    destroyed_total_ = 0;
    escaped_total_ = 0;
    level_score_ = 0;
    for (CampaignEnemy& enemy : enemies_) {
        enemy = CampaignEnemy{};
    }
    for (CampaignProjectile& projectile : projectiles_) {
        projectile = CampaignProjectile{};
    }
    if (level_id() == 1) {
        practice_.reset(next_random());
    } else {
        initialize_generic_level();
    }
}

int Campaign::level_id() const {
    return kPlan[std::min(plan_index_, kPlan.size() - 1)];
}

const LevelInfo& Campaign::level_info() const {
    return campaign_level_info(level_id());
}

void Campaign::initialize_generic_level() {
    const LevelInfo& info = level_info();
    if (info.id == 7) {
        initialize_command_level();
        return;
    }
    // Combat sectors own ten physical records. Their setup routine is called
    // every frame and immediately reuses each record that returned to -1.
    total_target_count_ = info.enemy_count;
    spawn_next_wave();
}

void Campaign::initialize_command_level() {
    // Records 2..10 are initialized against the command ship.
    // These eight triples are the shipped surface offsets; record 10 is a
    // separate 60-point central target.
    static constexpr std::array<std::array<float, 3>, 8> kSurfaceOffsets{{
        {{43.0F, 31.0F, 100.0F}},
        {{-43.0F, 31.0F, 100.0F}},
        {{43.0F, -31.0F, 100.0F}},
        {{-43.0F, -31.0F, 100.0F}},
        {{-32.0F, 46.0F, -10.0F}},
        {{32.0F, 46.0F, -10.0F}},
        {{-297.0F, 0.0F, -113.0F}},
        {{295.0F, 0.0F, -113.0F}},
    }};

    command_phase_ = 8;
    command_phase_started_ = frame_;
    enemy_slots_ = 10; // the command ship plus records 2..10
    total_target_count_ = 9;
    spawned_total_ = 9;

    CampaignEnemy& command = enemies_[0];
    command = CampaignEnemy{};
    command.kind = EnemyKind::CommandShip;
    command.status = EnemyStatus::Active;
    command.world_motion = true;
    command.targetable = false;
    command.can_fire = false;
    command.hit_points = command.max_hit_points = 0;
    command.wx = -10.0F;
    command.wy = -40.0F;
    command.wz = 740.0F;
    command.angle_y = 180.0F;
    command.world_radius = 90.0F;

    for (std::size_t index = 1; index < enemy_slots_; ++index) {
        CampaignEnemy& surface = enemies_[index];
        surface = CampaignEnemy{};
        surface.kind = EnemyKind::CommandShip;
        surface.status = EnemyStatus::Active;
        surface.record_index = index + 1U;
        surface.world_motion = true;
        surface.command_surface = true;
        surface.render_model = false;
        surface.can_fire = false;
        surface.world_radius = 14.0F;
        if (index == 9) {
            surface.hit_points = surface.max_hit_points = 60;
            surface.attack_rate_x = 0.0F;
            surface.attack_rate_y = 0.0F;
            surface.attack_rate_z = -10.0F;
        } else {
            surface.hit_points = surface.max_hit_points = 5;
            surface.attack_rate_x = kSurfaceOffsets[index - 1U][0];
            surface.attack_rate_y = kSurfaceOffsets[index - 1U][1];
            surface.attack_rate_z = kSurfaceOffsets[index - 1U][2];
        }
    }
    activated_total_ = 9;
}

void Campaign::initialize_enemy_record(CampaignEnemy& enemy,
                                       std::size_t index) {
    const LevelInfo& info = level_info();
    enemy = CampaignEnemy{};
    enemy.kind = info.enemy_kind;
    // Hit points: scouts 2, meteors 2, fighters and Ko-Dan escorts 3.
    // Level 11's swarm fighters take one hit.
    if (info.id == 6) {
        enemy.hit_points = enemy.max_hit_points = 1;
    } else if (info.enemy_kind == EnemyKind::Fighter ||
               info.enemy_kind == EnemyKind::CommandShip) {
        enemy.hit_points = enemy.max_hit_points = 3;
    } else {
        enemy.hit_points = enemy.max_hit_points = 2;
    }
    if (info.id == 4) {
        // The bespoke frontier routine uses three hits, unlike the earlier
        // generic meteor routine.
        enemy.hit_points = enemy.max_hit_points = 3;
    }
    if (info.id == 7) {
        if (index == 0) {
            enemy.kind = EnemyKind::CommandShip;
            // The final command-ship target record is 60 points; an earlier
            // 40-point hull was wrong. The game first exposes eight
            // five-point surface targets; those still need distinct
            // collision geometry here.
            enemy.hit_points = enemy.max_hit_points = 60;
        } else {
            enemy.kind = EnemyKind::Fighter;
            enemy.hit_points = enemy.max_hit_points = 3;
        }
    }
    spawn_world_enemy(enemy, index);
}

void Campaign::spawn_next_wave() {
    const int left = total_target_count_ - spawned_total_;
    if (left <= 0) {
        return;
    }
    const int capacity = level_id() == 7 ? 11 : 10;
    wave_activation_base_ = activated_total_;
    if (level_id() == 5 || level_id() == 11) {
        if (wave_activation_base_ < 6) fighter_spread_ = 500;
        else if (wave_activation_base_ < 16) fighter_spread_ = 1000;
        else if (wave_activation_base_ < 30) fighter_spread_ = 1200;
        else fighter_spread_ = 1500;
        // Invisible object 500 is the formation anchor: reset
        // position/rotation, X-rotate by RND(359), choose either a small Y
        // or Z rotation, move forward exactly 2500, then sample its XYZ. Z
        // is roll and therefore does not alter the local-forward vector.
        const float angle_x =
            static_cast<float>(next_random() % 360U) * kPi / 180.0F;
        float angle_y = 0.0F;
        if (next_random() % 2U == 0U) {
            const std::uint32_t range = level_id() == 5 ? 11U : 21U;
            angle_y = static_cast<float>(next_random() % range) * kPi / 180.0F;
        } else {
            const std::uint32_t range = level_id() == 5 ? 11U : 21U;
            (void)(next_random() % range); // Z-roll draw range
        }
        spawn_anchor_x_ = std::sin(angle_y) * std::cos(angle_x) * 2500.0F;
        spawn_anchor_y_ = -std::sin(angle_x) * 2500.0F;
        spawn_anchor_z_ = std::cos(angle_y) * std::cos(angle_x) * 2500.0F;
    } else if (level_id() == 6) {
        fighter_formation_type_ = static_cast<int>(next_random() % 2U);
        if (fighter_formation_type_ == 0) {
            spawn_anchor_x_ = 800.0F + static_cast<float>(next_random() % 1601U);
            spawn_anchor_y_ = 800.0F + static_cast<float>(next_random() % 1601U);
            spawn_anchor_z_ = 2800.0F;
        } else if (next_random() % 2U == 0U) {
            spawn_anchor_x_ = 500.0F + static_cast<float>(next_random() % 1001U);
            spawn_anchor_y_ = 500.0F + static_cast<float>(next_random() % 1001U);
            spawn_anchor_z_ = -2800.0F;
        } else {
            spawn_anchor_x_ = next_random() % 2U == 0U ? -2000.0F : 2000.0F;
            spawn_anchor_y_ = 500.0F + static_cast<float>(next_random() % 1001U);
            spawn_anchor_z_ = -2000.0F;
        }
    }
    const bool first_fill = enemy_slots_ == 0;
    const std::size_t physical_slots = static_cast<std::size_t>(
        std::min(total_target_count_, capacity));
    enemy_slots_ = physical_slots;
    int filled = 0;
    for (std::size_t index = 0;
         index < physical_slots && filled < left; ++index) {
        const EnemyStatus status = enemies_[index].status;
        if (!first_fill && status != EnemyStatus::Destroyed &&
            status != EnemyStatus::Escaped) {
            continue;
        }
        initialize_enemy_record(enemies_[index], index);
        ++filled;
    }
    if (first_fill) {
        for (std::size_t index = physical_slots; index < enemies_.size();
             ++index) {
            enemies_[index] = CampaignEnemy{};
        }
    }
    spawned_total_ += filled;
}

void Campaign::update(std::uint32_t frame) {
    if (complete() || frame <= frame_) {
        return;
    }
    const std::uint32_t elapsed = frame - frame_;
    frame_ = frame;
    // Each weapon holds a 0..100 gauge topped up by its own dipswitch every
    // frame.
    laser_energy_ = std::min(
        100.0F, laser_energy_ + elapsed * dip_.laser_recharge_in_play());
    particle_energy_ = std::min(
        100.0F, particle_energy_ + elapsed * dip_.particle_recharge_in_play());
    // The hull pool regenerates 0.001 per frame (the "partial Gunstar
    // repair"); the discrete shields climb as it accumulates.
    hull_regen_ += 0.001F * static_cast<float>(elapsed);
    while (hull_regen_ >= 1.0F &&
           shields_ < dip_.shields()) {
        hull_regen_ -= 1.0F;
        ++shields_;
    }
    if (hull_regen_ >= 1.0F) {
        hull_regen_ = 1.0F;
    }

    if (level_id() == 1) {
        practice_.update(frame);
        if (practice_.complete()) {
            outcome_ = LevelOutcome::Cleared;
        }
        return;
    }

    for (std::size_t index = 0; index < enemy_slots_; ++index) {
        CampaignEnemy& enemy = enemies_[index];
        if (enemy.status == EnemyStatus::Waiting &&
            frame >= enemy.activation_frame) {
            enemy.status = EnemyStatus::Active;
            ++activated_total_;
        }
        if (enemy.status == EnemyStatus::Active) {
            const EnemyStatus previous = enemy.status;
            update_enemy(enemy);
            count_finished_enemy(enemy, previous);
        }
    }
    // Every sector recalls at frame 1550 - surviving enemies leave and the
    // sector is graded with whatever escaped.
    // Level 4's briefing is "intercept meteors before impacting our
    // base": meteors still adrift at the recall strike the base.
    if (level_id() != 7 && frame_ >= 1550U) {
        for (std::size_t index = 0; index < enemy_slots_; ++index) {
            CampaignEnemy& enemy = enemies_[index];
            if (enemy.status == EnemyStatus::Active ||
                enemy.status == EnemyStatus::Waiting) {
                enemy.status = EnemyStatus::Escaped;
                ++escaped_total_;
                if (enemy.kind == EnemyKind::Meteor) {
                    ++frontier_damage_;
                }
                events_.push_back({Event::Kind::EnemyEscaped, enemy.kind});
            }
        }
        // Targets which had not yet received a physical record also escape
        // at recall; otherwise a partial final wave could never complete.
        if (spawned_total_ < total_target_count_) {
            escaped_total_ += total_target_count_ - spawned_total_;
            spawned_total_ = total_target_count_;
        }
    }
    update_projectiles();
    update_command_phase();
    finish_if_ready();
}

std::vector<Campaign::FlightRow> Campaign::decode_flight_paths(
    const std::vector<std::uint8_t>& data) {
    constexpr std::size_t header = 464;
    constexpr std::size_t row_count = 100;
    constexpr std::size_t column_count = 15;
    constexpr std::size_t cell_bytes = 4;
    if (data.size() < header + row_count * column_count * cell_bytes) {
        throw std::invalid_argument("truncated bomberflightfin.dat");
    }
    std::vector<FlightRow> rows(row_count);
    for (std::size_t row = 0; row < row_count; ++row) {
        for (std::size_t column = 0; column < column_count; ++column) {
            const std::size_t at =
                header + (column * row_count + row) * cell_bytes;
            const std::uint32_t word =
                static_cast<std::uint32_t>(data[at]) |
                (static_cast<std::uint32_t>(data[at + 1]) << 8U) |
                (static_cast<std::uint32_t>(data[at + 2]) << 16U) |
                (static_cast<std::uint32_t>(data[at + 3]) << 24U);
            rows[row].cells[column] =
                static_cast<int>(static_cast<std::int32_t>(word));
        }
    }
    return rows;
}

void Campaign::set_flight_paths(const std::vector<FlightRow>& rows) {
    flight_rows_ = rows;
}

std::vector<Campaign::Event> Campaign::drain_events() {
    std::vector<Event> drained;
    drained.swap(events_);
    return drained;
}

void Campaign::briefing_recharge(int frames) {
    laser_energy_ = std::min(
        100.0F, laser_energy_ +
                    static_cast<float>(frames) *
                        dip_.laser_recharge_between_levels());
    particle_energy_ = std::min(
        100.0F, particle_energy_ +
                    static_cast<float>(frames) *
                        dip_.particle_recharge_between_levels());
}

void Campaign::spawn_world_enemy(CampaignEnemy& enemy, std::size_t index) {
    // lifetime_frames is NOT touched here: it doubles as the level's
    // recycle budget (the wave recall deadline).
    enemy.world_motion = true;
    enemy.record_index = index;
    enemy.status = EnemyStatus::Waiting;
    // Activation thresholds use each phase's random range (also the
    // fire-window rearm range).
    enemy.activation_frame =
        frame_ + static_cast<std::uint32_t>(
                     next_random() %
                     static_cast<std::uint32_t>(rearm_range() + 1));
    enemy.fire_window = fire_window_seed(enemy.kind);
    enemy.fire_window_delay = 0;
    enemy.angle_x = 0.0F;
    enemy.angle_y = 0.0F;
    enemy.angle_z = 0.0F;
    if (enemy.kind == EnemyKind::Scout && !flight_rows_.empty()) {
        // A randomly selected authored row is copied verbatim. A -650
        // coordinate is an intentional off-axis edge spawn.
        const FlightRow& row =
            flight_rows_[next_random() % flight_rows_.size()];
        enemy.wx = static_cast<float>(row.cells[0]);
        enemy.wy = static_cast<float>(row.cells[1]);
        enemy.wz = 1300.0F;
        enemy.angle_x = static_cast<float>(row.cells[2]);
        enemy.angle_y = static_cast<float>(row.cells[3]);
        enemy.angle_z = static_cast<float>(row.cells[4]);
        enemy.rate_x = static_cast<float>(row.cells[5]);
        enemy.attack_rate_x = static_cast<float>(row.cells[6]);
        enemy.rate_y = static_cast<float>(row.cells[7]);
        enemy.attack_rate_y = static_cast<float>(row.cells[8]);
        enemy.rate_z = static_cast<float>(row.cells[9]);
        enemy.attack_rate_z = static_cast<float>(row.cells[10]);
        enemy.attack_far = static_cast<float>(row.cells[11]);
        enemy.attack_near = static_cast<float>(row.cells[12]);
        enemy.flight_type = row.cells[13];
    } else if (enemy.kind == EnemyKind::Meteor) {
        // The meteor sector is a bespoke two-family formation, not the slow
        // generic drift of the other levels. Every rock begins at
        // (0,0,2500). Field 5 is the 0/1/2 movement state: zero remains on
        // the straight path, one changes to two after crossing z=0, and two
        // is the collapsing orbit. Fields 8-14 hold its authored Euler
        // angles and orbit parameters.
        enemy.wx = 0.0F;
        enemy.wy = 0.0F;
        enemy.wz = 2500.0F;
        enemy.flight_type = next_random() % 11U > 3U ? 0 : 1;
        if (index >= 6) {
            enemy.flight_type = 0;
        }
        if (wave_activation_base_ > 30) {
            enemy.flight_type = index >= 3 ? 0 : 1;
        }
        enemy.rate_x =
            1400.0F + static_cast<float>(next_random() % 2801U); // field 8
        enemy.rate_y = enemy.rate_x;                             // field 9
        enemy.rate_z = static_cast<float>((next_random() % 2U) * 2U + 1U);
        if (enemy.flight_type == 0) {
            const bool narrow = index < 6 && next_random() % 2U == 0U;
            const std::uint32_t spread = narrow ? 120U : 1000U;
            const float base = narrow ? 60.0F : 500.0F;
            enemy.attack_rate_x =
                base + static_cast<float>(next_random() % (spread + 1U));
            enemy.attack_rate_y =
                base + static_cast<float>(next_random() % (spread + 1U));
            enemy.attack_rate_z = -3000.0F;
        } else {
            enemy.attack_far = 2200.0F; // field 6, collapsing radius
            // These are ROTATE OBJECT arguments, not a destination.
            enemy.attack_rate_x = 2200.0F * enemy.rate_z;
            enemy.attack_rate_y = enemy.rate_x * enemy.rate_z;
            enemy.attack_rate_z = 0.0F;
        }

        // Activation thresholds are selected per record and phase. The
        // upper four records deliberately lag.
        std::uint32_t delay{};
        if (wave_activation_base_ < 10) {
            delay = index == 0 ? 50U + next_random() % 51U
                    : index >= 6 ? 20U + next_random() % 301U
                                 : next_random() % 61U;
        } else if (wave_activation_base_ < 20) {
            delay = index == 0 ? next_random() % 51U
                    : index >= 6 ? 20U + next_random() % 401U
                                 : next_random() % 151U;
        } else if (wave_activation_base_ < 30) {
            delay = index >= 6 ? 20U + next_random() % 51U
                               : next_random() % 51U;
        } else {
            delay = index == 0 ? next_random() % 51U
                    : index >= 6 ? 150U + next_random() % 121U
                    : enemy.flight_type == 0 ? next_random() % 101U
                                             : next_random() % 31U;
        }
        enemy.activation_frame = frame_ + delay;
    } else if (level_id() == 7) {
        // Ko-Dan escorts are staged around object 1001, rather than around
        // the fighter-sector object-500 anchor. During setup the command
        // ship is at z=2900, so its +50 offset is clamped to 2200 before the
        // per-record -100..-50 displacement.
        float side = 200.0F + static_cast<float>(next_random() % 51U);
        if (next_random() % 2U == 0U) {
            side = -side;
        }
        enemy.wx = side + static_cast<float>(next_random() % 51U) - 100.0F;
        enemy.wy = -40.0F;
        enemy.wz = 2200.0F + static_cast<float>(next_random() % 51U) - 100.0F;
        enemy.angle_x = 100.0F + static_cast<float>(next_random() % 51U);
        enemy.angle_y = 50.0F + static_cast<float>(next_random() % 26U);
        enemy.angle_z = 200.0F + static_cast<float>(next_random() % 101U);
        enemy.rate_x = static_cast<float>(10U + next_random() % 21U) / 60.0F;
        enemy.rate_y = static_cast<float>(10U + next_random() % 21U) / 60.0F;
        enemy.rate_z = static_cast<float>(10U + next_random() % 21U) / 60.0F;
        enemy.attack_far = static_cast<float>((5U + next_random() % 4U) * 90U);
        enemy.attack_near = static_cast<float>((3U + next_random() % 6U) * 5U);
        enemy.flight_type = static_cast<int>(2U + next_random() % 9U);
        enemy.activation_frame =
            frame_ + static_cast<std::uint32_t>(next_random() % 151U);
    } else {
        // Fighter setup: place each record in a cube around the level's
        // moving anchor, point it at the authored positive-coordinate
        // target, and retain both turn triples and the distance band.
        const int half = fighter_spread_ / 2;
        enemy.wx = spawn_anchor_x_ +
                   static_cast<float>(next_random() %
                                      static_cast<std::uint32_t>(fighter_spread_ + 1)) -
                   static_cast<float>(half);
        enemy.wy = spawn_anchor_y_ +
                   static_cast<float>(next_random() %
                                      static_cast<std::uint32_t>(fighter_spread_ + 1)) -
                   static_cast<float>(half);
        enemy.wz = spawn_anchor_z_ +
                   static_cast<float>(next_random() %
                                      static_cast<std::uint32_t>(fighter_spread_ + 1)) -
                   static_cast<float>(half);
        float target_x = 300.0F + static_cast<float>(next_random() % 151U);
        float target_y = 300.0F + static_cast<float>(next_random() % 151U);
        float target_z = level_id() == 6
                             ? 1000.0F + static_cast<float>(next_random() % 101U)
                             : 300.0F + static_cast<float>(next_random() % 151U);
        if (level_id() == 6 && fighter_formation_type_ == 0) {
            enemy.wx = spawn_anchor_x_ -
                       static_cast<float>(400 + static_cast<int>(index) * 40);
            enemy.wy = spawn_anchor_y_ + 400.0F -
                       static_cast<float>(next_random() % 201U);
            target_x = enemy.wx;
            target_y = enemy.wy;
            target_z = enemy.wz + 3000.0F;
        }
        enemy.rate_x = static_cast<float>(10U + next_random() % 21U) / 30.0F;
        enemy.rate_y = static_cast<float>(10U + next_random() % 21U) / 30.0F;
        enemy.rate_z = static_cast<float>(10U + next_random() % 21U) / 30.0F;
        enemy.attack_rate_x = static_cast<float>(5U + next_random() % 11U);
        enemy.attack_rate_y = static_cast<float>(5U + next_random() % 11U);
        enemy.attack_rate_z = static_cast<float>(5U + next_random() % 11U);
        enemy.attack_far = static_cast<float>((4U + next_random() % 6U) * 100U);
        if (level_id() == 6 && fighter_formation_type_ == 0) {
            enemy.attack_far = 1600.0F + static_cast<float>(next_random() % 701U);
        }
        enemy.attack_near = static_cast<float>((3U + next_random() % 6U) * 10U);
        enemy.flight_type = static_cast<int>(next_random() % 11U);
        const float dx = target_x - enemy.wx;
        const float dy = target_y - enemy.wy;
        const float dz = target_z - enemy.wz;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 1.0F) {
            enemy.angle_y = std::atan2(dx, dz) * 180.0F / kPi;
            enemy.angle_x = -std::asin(dy / len) * 180.0F / kPi;
        }
    }
    if (enemy.kind == EnemyKind::CommandShip) {
        // Object 1001 is independent of the speed-15 escort records. It is
        // loaded at (-10,-40,2900), scale 300%, Y rotation 180 degrees,
        // then enters the scripted orbit below.
        enemy.wx = -10.0F;
        enemy.wy = -40.0F;
        enemy.wz = 2900.0F;
        enemy.angle_y = 180.0F;
        enemy.activation_frame = 0;
        enemy.world_radius = 90.0F;
    } else if (enemy.kind == EnemyKind::Meteor) {
        enemy.world_radius = 26.0F;
    } else if (enemy.kind == EnemyKind::Fighter) {
        enemy.world_radius = 34.0F;
    } else {
        enemy.world_radius = 30.0F;
    }
}

void Campaign::update_world_enemy(CampaignEnemy& enemy) {
    if (enemy.kind == EnemyKind::CommandShip) {
        if (enemy.command_surface) {
            const CampaignEnemy& command = enemies_[0];
            enemy.wx = command.wx - enemy.attack_rate_x;
            enemy.wy = command.wy - enemy.attack_rate_y;
            enemy.wz = command.wz - enemy.attack_rate_z;
            project_world_enemy(enemy);
            return;
        }
        const std::uint32_t age = frame_ - command_phase_started_;
        if (command_phase_ == 8) {
            // State 8: the approach orbit.
            const float angle = static_cast<float>(age) * 1.5F * kPi / 180.0F;
            enemy.wx = -std::cos(angle) * 740.0F - 10.0F;
            enemy.wy = -std::cos(angle) * 120.0F - 40.0F;
            enemy.wz = std::sin(angle) * 740.0F;
        } else {
            // State 9: a 500-frame axial attack. It advances from z=740 to
            // 989, then retreats through the player to -260.
            enemy.wx = -10.0F;
            enemy.wy = -40.0F;
            enemy.wz = age < 250U ? 740.0F + static_cast<float>(age)
                                  : 240.0F - static_cast<float>(age);
        }
        project_world_enemy(enemy);
        return;
    }
    if (enemy.kind == EnemyKind::Meteor) {
        if (enemy.flight_type < 2) {
            // Restore fields 12-14 as Euler angles, move 25 units along
            // that heading, then POINT OBJECT at (0,0,frame*3). Pointing
            // happens after movement and fields 12-14 are restored on the
            // next tick, so it affects presentation, not trajectory.
            const float ax = enemy.attack_rate_x * kPi / 180.0F;
            const float ay = enemy.attack_rate_y * kPi / 180.0F;
            enemy.wx += std::sin(ay) * std::cos(ax) * 25.0F;
            enemy.wy -= std::sin(ax) * 25.0F;
            enemy.wz += std::cos(ay) * std::cos(ax) * 25.0F;

            const float target_z = static_cast<float>(frame_) * 3.0F;
            const float dx = -enemy.wx;
            const float dy = -enemy.wy;
            const float dz = target_z - enemy.wz;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length > 0.001F) {
                enemy.angle_y = std::atan2(dx, dz) * 180.0F / kPi;
                enemy.angle_x = -std::asin(dy / length) * 180.0F / kPi;
                enemy.angle_z = 0.0F;
            }

            // Only state one enters the orbit. The crossing test is a
            // strict float<int, so equality with the z=0 plane waits one
            // more tick. The wrap yields 90 or 270.
            if (enemy.wz < 0.0F && enemy.flight_type == 1) {
                enemy.flight_type = 2;
                enemy.attack_far = 2200.0F;
                enemy.rate_y = std::fmod(360.0F + enemy.rate_z * 90.0F,
                                         360.0F);
            }
        } else {
            // Shrink field 6 by ten, evolve field 9 using
            // SQRT(field10^2 + field9^2), derive XYZ, then damp field 8.
            const float combined =
                std::sqrt(enemy.rate_z * enemy.rate_z +
                          enemy.rate_y * enemy.rate_y);
            const float angle = combined * kPi / 180.0F;
            enemy.attack_far -= 10.0F;
            enemy.wx = enemy.attack_far * std::sin(angle);
            enemy.wy = enemy.rate_x * std::sin(angle);
            enemy.wz = enemy.attack_far * std::cos(angle);
            enemy.rate_x *= std::fabs(enemy.rate_x) < 50.0F ? 0.8F : 0.99F;
            enemy.rate_y = combined;
            if (enemy.attack_far <= 30.0F) {
                enemy.status = EnemyStatus::Escaped;
                if (enemy.record_index < 6) {
                    ++frontier_damage_;
                }
                events_.push_back({Event::Kind::EnemyEscaped, enemy.kind});
            }
        }
        project_world_enemy(enemy);
        if (enemy.flight_type < 2 && enemy.wz <= -3000.0F) {
            enemy.status = EnemyStatus::Escaped;
            if (enemy.record_index < 6) {
                ++frontier_damage_;
            }
            events_.push_back({Event::Kind::EnemyEscaped, enemy.kind});
        }
        return;
    }
    // The object moves along its current heading first, then changes the
    // Euler angles for the next frame. Keeping that order matters on the
    // sharply turning authored scout paths.
    // Speed is 15 for every Ko-Dan escort, 25 for the ordinary
    // scout/fighter records. Object 1001 never reaches here.
    const float speed = level_id() == 7 ? 15.0F : 25.0F;
    const float ax = enemy.angle_x * kPi / 180.0F;
    const float ay = enemy.angle_y * kPi / 180.0F;
    const float fx = std::sin(ay) * std::cos(ax);
    const float fy = -std::sin(ax);
    const float fz = std::cos(ay) * std::cos(ax);
    enemy.wx += fx * speed;
    enemy.wy += fy * speed;
    enemy.wz += fz * speed;
    const float distance =
        std::sqrt(enemy.wx * enemy.wx + enemy.wy * enemy.wy +
                  enemy.wz * enemy.wz);
    if (enemy.kind == EnemyKind::Scout || enemy.kind == EnemyKind::Fighter) {
        // Each bomberflightfin.dat row contains two rotation triples and a
        // distance band.  Inside the band, types above one add the attack
        // triple.  Otherwise every non-zero type subtracts the ordinary
        // triple; type zero retains its heading.
        const bool in_attack_band =
            distance < enemy.attack_far && distance > enemy.attack_near;
        if (in_attack_band && enemy.flight_type > 1) {
            enemy.angle_x += enemy.attack_rate_x;
            enemy.angle_y += enemy.attack_rate_y;
            enemy.angle_z += enemy.attack_rate_z;
        } else if (enemy.flight_type > 0) {
            enemy.angle_x -= enemy.rate_x;
            enemy.angle_y -= enemy.rate_y;
            enemy.angle_z -= enemy.rate_z;
        }
    } else {
        enemy.angle_x += enemy.rate_x;
        enemy.angle_y += enemy.rate_y;
        enemy.angle_z += enemy.rate_z;
    }
    const auto wrap_angle = [](float angle) {
        angle = std::fmod(angle, 360.0F);
        return angle < 0.0F ? angle + 360.0F : angle;
    };
    enemy.angle_x = wrap_angle(enemy.angle_x);
    enemy.angle_y = wrap_angle(enemy.angle_y);
    enemy.angle_z = wrap_angle(enemy.angle_z);
    project_world_enemy(enemy);
    enemy.velocity_x = fx * 3.0F;
    enemy.velocity_y = -fy * 3.0F;
    if (distance > 3100.0F) {
        // State 10 ends this record; the continuously called setup routine
        // can reuse this one physical slot on its next pass.
        enemy.status = EnemyStatus::Escaped;
        events_.push_back({Event::Kind::EnemyEscaped, enemy.kind});
    }
}

void Campaign::update_command_phase() {
    if (level_id() != 7 || complete()) {
        return;
    }
    if (command_phase_ == 8) {
        // Records 9 and 10 must both be depleted, then the orbit waits
        // until object 1001 crosses x=-15..0 before state 9.
        if (enemies_[8].status != EnemyStatus::Destroyed ||
            enemies_[9].status != EnemyStatus::Destroyed ||
            enemies_[0].wx < -15.0F || enemies_[0].wx > 0.0F) {
            return;
        }
        command_phase_ = 9;
        command_phase_started_ = frame_;
        for (std::size_t index : {8U, 9U}) {
            CampaignEnemy& target = enemies_[index];
            target.status = EnemyStatus::Active;
            target.hit_points = target.max_hit_points = 10;
        }
        return;
    }
    if (enemies_[9].status == EnemyStatus::Destroyed) {
        outcome_ = LevelOutcome::Victory;
        return;
    }
    if (frame_ - command_phase_started_ >= 500U) {
        command_phase_ = 8;
        command_phase_started_ = frame_;
    }
}

void Campaign::project_world_enemy(CampaignEnemy& enemy) const {
    // Same inverse camera rotations as render_world_model. This keeps the
    // target used by collision/lock/scanners on the rendered mesh as the
    // turret rotates, including the nonlinear perspective near +/-70 yaw.
    const float yaw = view_yaw_ * kPi / 180.0F;
    const float pitch = view_pitch_ * kPi / 180.0F;
    const float x_yaw = enemy.wx * std::cos(yaw) - enemy.wz * std::sin(yaw);
    const float z_yaw = enemy.wx * std::sin(yaw) + enemy.wz * std::cos(yaw);
    const float y_pitch =
        enemy.wy * std::cos(pitch) + z_yaw * std::sin(pitch);
    const float z_pitch =
        -enemy.wy * std::sin(pitch) + z_yaw * std::cos(pitch);
    if (z_pitch >= 1.0F) {
        constexpr float kFocal = 500.0F;
        enemy.x = std::clamp(400.0F + x_yaw * kFocal / z_pitch, -80.0F,
                             880.0F);
        enemy.y = std::clamp(300.0F - y_pitch * kFocal / z_pitch, -60.0F,
                             660.0F);
        enemy.radius = std::clamp(
            enemy.world_radius * kFocal / z_pitch, 6.0F, 82.0F);
    } else {
        enemy.x = -100.0F;
        enemy.y = -100.0F;
        enemy.radius = 6.0F;
    }
}

void Campaign::count_finished_enemy(const CampaignEnemy& enemy,
                                    EnemyStatus previous_status) {
    if (previous_status == EnemyStatus::Destroyed ||
        previous_status == EnemyStatus::Escaped) {
        return;
    }
    if (enemy.status == EnemyStatus::Destroyed) {
        ++destroyed_total_;
    } else if (enemy.status == EnemyStatus::Escaped) {
        ++escaped_total_;
    }
}

void Campaign::update_enemy(CampaignEnemy& enemy) {
    if (enemy.world_motion) {
        update_world_enemy(enemy);
        return;
    }
    const float previous_x = enemy.x;
    const float previous_y = enemy.y;
    const float age = static_cast<float>(frame_ - enemy.activation_frame);
    const float life = static_cast<float>(enemy.lifetime_frames);
    const float progress = std::clamp(age / life, 0.0F, 1.0F);
    const float wave = enemy.phase + age * 0.025F;
    switch (level_id()) {
    case 2:
        enemy.x = 400.0F + std::sin(wave) * (170.0F + 80.0F * progress);
        enemy.y = 275.0F + std::cos(wave * 1.7F) * 125.0F;
        enemy.radius = clamp_radius(12.0F + progress * 55.0F);
        break;
    case 3:
        enemy.x = 400.0F + enemy.lane * 230.0F + std::sin(wave) * 45.0F;
        enemy.y = 280.0F + std::cos(wave * 0.8F) * 130.0F;
        enemy.radius = clamp_radius(62.0F - progress * 50.0F);
        break;
    case 4:
        enemy.x = 400.0F + enemy.lane * 260.0F + std::sin(wave) * 25.0F;
        enemy.y = 270.0F + std::cos(enemy.phase * 2.0F) * 145.0F;
        enemy.radius = clamp_radius(8.0F + progress * 72.0F);
        break;
    case 5:
        enemy.x = 400.0F + std::sin(wave * 1.3F) * 250.0F;
        enemy.y = 275.0F + std::cos(wave * 0.75F) * 140.0F;
        enemy.radius = clamp_radius(14.0F + std::sin(progress * kPi) * 55.0F);
        break;
    case 6:
        enemy.x = 400.0F + enemy.lane * 230.0F + std::sin(wave) * 60.0F;
        enemy.y = 275.0F + std::sin(wave * 0.7F) * 140.0F;
        enemy.radius = clamp_radius(68.0F - progress * 54.0F);
        break;
    case 11:
        enemy.x = 400.0F + std::sin(wave) * (90.0F + 180.0F * progress);
        enemy.y = 275.0F + std::cos(wave * 1.4F) * (70.0F + 90.0F * progress);
        enemy.radius = clamp_radius(12.0F + progress * 24.0F);
        break;
    case 7:
        enemy.x = 400.0F + std::sin(wave * 0.17F) * 155.0F;
        enemy.y = 255.0F + std::cos(wave * 0.11F) * 65.0F;
        enemy.radius = 78.0F;
        break;
    default:
        break;
    }
    enemy.velocity_x = enemy.x - previous_x;
    enemy.velocity_y = enemy.y - previous_y;
    if (frame_ >= enemy.activation_frame + enemy.lifetime_frames) {
        enemy.status = EnemyStatus::Escaped;
        if (enemy.kind == EnemyKind::Meteor) {
            ++frontier_damage_;
        }
    }
}

int Campaign::active_enemy_count() const {
    return static_cast<int>(std::count_if(
        enemies_.begin(),
        enemies_.begin() + static_cast<std::ptrdiff_t>(enemy_slots_),
        [](const CampaignEnemy& enemy) {
            return enemy.status == EnemyStatus::Active;
        }));
}

// The concurrent-missile cap, set by each level routine from the number of
// active enemies.
int Campaign::concurrent_missile_cap() const {
    const int active = wave_activation_base_;
    switch (level_id()) {
    case 4:
        if (active < 5) {
            return 2;
        }
        return active < 8 ? 3 : active < 14 ? 4 : 5;
    case 7:
        // The Ko-Dan sector runs six missiles concurrently and moves
        // everything more slowly (enemy speed 15).
        return 6;
    case 5:
        if (active < 6) {
            return 2;
        }
        return active < 30 ? 3 : 5;
    case 6:
        if (active < 6) return 2;
        return active < 40 ? 3 : 4;
    case 11:
        if (active < 6) return 2;
        return active < 30 ? 3 : 5;
    default:
        if (active < 6) {
            return 2;
        }
        return 3;
    }
}

// Enemy-record field 3, seeded when a record initializes: 1 frame for
// scouts, 2 frames for meteors, fighters, and the Ko-Dan sector's escorts.
int Campaign::fire_window_seed(EnemyKind kind) const {
    return kind == EnemyKind::Scout ? 1 : 2;
}

// The random activation-threshold range each level phase uses when queueing
// a record. It also doubles as the delay before an enemy's fire window
// rearms, standing in for the spawn queue turning a record over.
int Campaign::rearm_range() const {
    const int active = wave_activation_base_;
    switch (level_id()) {
    case 4:
        if (active < 5) {
            return 50;
        }
        return active < 8 ? 100 : active < 14 ? 50 : 150;
    case 7:
        return 150;
    case 5:
        if (active < 6) return 50;
        if (active < 16) return 100;
        return active < 30 ? 350 : 150;
    case 6:
        if (active < 6) {
            return 40;
        }
        return active < 11 ? 30 : active < 40 ? 50 : 10;
    case 11:
        if (active < 6) return 100;
        if (active < 16) return 50;
        return active < 30 ? 30 : 5;
    case 3:
        if (active < 4) return 50;
        if (active < 8) return 100;
        return active < 14 ? 50 : 150;
    default: // level 2
        if (active < 6) {
            return 50;
        }
        return active < 11 ? 100 : 150;
    }
}

void Campaign::schedule_enemy_fire(CampaignEnemy& enemy) {
    // Recruit mode: the enemy lines hold their fire entirely.
    if (mode_ == GameMode::Recruit || !enemy.can_fire) {
        return;
    }
    // Fire attempts are gated on the enemy being inside the attack band
    // (near/far thresholds); approximate with the world Z corridor.
    if (enemy.world_motion && (enemy.wz > 900.0F || enemy.wz < 60.0F)) {
        return;
    }
    // An attempt is only possible while field 3 is positive, and the
    // countdown decrements on every frame inside that window.
    if (enemy.fire_window > 0) {
        --enemy.fire_window;
        // The gate is 5 for scouts while fewer than six enemies are active
        // and 7 otherwise; meteor and fighter call sites use 7 directly.
        const bool scout = enemy.kind == EnemyKind::Scout;
        // The gate is selected with the rest of the phase settings when
        // records are initialized, not recalculated as they activate.
        const int gate = scout && wave_activation_base_ < 6 ? 5 : 7;
        if (missiles_in_flight_ >= concurrent_missile_cap()) {
            return;
        }
        if (!roll_accuracy(gate)) {
            return;
        }
        const auto free = std::find_if(
            projectiles_.begin(), projectiles_.end(),
            [](const CampaignProjectile& projectile) {
                return !projectile.active;
            });
        if (free == projectiles_.end()) {
            return;
        }
        // The in-flight count increments; the missile is created at the
        // firing enemy's position.
        ++missiles_in_flight_;
        events_.push_back({Event::Kind::EnemyFired, enemy.kind});
        free->active = true;
        free->launched_frame = frame_;
        free->world_motion = enemy.world_motion;
        free->source_kind = enemy.kind;
        // The ten preloaded fighter missile slots choose missile 1-2 or
        // 1-6 once at boot. Preserve a stable per-slot mixture.
        free->model_variant = static_cast<std::size_t>(
            std::distance(projectiles_.begin(), free)) & 1U;
        free->start_x = enemy.x;
        free->start_y = enemy.y;
        free->x = enemy.x;
        free->y = enemy.y;
        free->radius = 8.0F;
        // Each missile aims wildly by default, tightens to a near miss when
        // RND(close_shots) is zero, and goes on the player when
        // RND(bullseye) is zero. The bullseye roll is taken second and
        // overrides, so it wins.
        int offset_base = 50;
        int offset_range = 150;
        if (roll_accuracy(dip_.close_shots())) {
            offset_base = 25;
            offset_range = 50;
        }
        free->bullseye = roll_accuracy(dip_.bullseye());
        if (free->bullseye) {
            offset_base = 2;
            offset_range = 4;
        }
        const auto aim_offset = [&]() {
            float value = static_cast<float>(
                offset_base + static_cast<int>(next_random() %
                                               (offset_range + 1U)));
            return next_random() % 2U == 0U ? value : -value;
        };
        free->wx = enemy.wx;
        free->wy = enemy.wy;
        free->wz = enemy.wz;
        const float dx = aim_offset() - free->wx;
        const float dy = aim_offset() - free->wy;
        const float dz = aim_offset() - free->wz;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float speed = level_id() == 7 ? 25.0F : 30.0F;
        free->wvx = dx / distance * speed;
        free->wvy = dy / distance * speed;
        free->wvz = dz / distance * speed;
        enemy.fire_window_delay = static_cast<int>(next_random() %
                                                   static_cast<std::uint32_t>(
                                                       rearm_range() + 1));
        return;
    }
    if (enemy.fire_window_delay > 0) {
        --enemy.fire_window_delay;
        return;
    }
    enemy.fire_window = fire_window_seed(enemy.kind);
}

void Campaign::update_projectiles() {
    if (level_id() == 1) {
        return;
    }
    for (std::size_t index = 0; index < enemy_slots_; ++index) {
        CampaignEnemy& enemy = enemies_[index];
        if (enemy.status == EnemyStatus::Active) {
            schedule_enemy_fire(enemy);
        }
    }
    for (CampaignProjectile& projectile : projectiles_) {
        if (!projectile.active) {
            continue;
        }
        projectile.wx += projectile.wvx;
        projectile.wy += projectile.wvy;
        projectile.wz += projectile.wvz;
        project_world_projectile(projectile);
        const float distance =
            std::sqrt(projectile.wx * projectile.wx +
                      projectile.wy * projectile.wy +
                      projectile.wz * projectile.wz);
        if (projectile.bullseye && distance < 35.0F) {
            // Exactly one shield per bullseye.
            --shields_;
            events_.push_back({Event::Kind::PlayerHit, EnemyKind::Scout});
            projectile.active = false;
            --missiles_in_flight_;
            missiles_in_flight_ = std::max(0, missiles_in_flight_);
        } else if (frame_ - projectile.launched_frame > 20U &&
                   distance > 3000.0F) {
            // Only remove an old missile after it has left the 3000-unit
            // combat sphere.
            projectile.active = false;
            --missiles_in_flight_;
            missiles_in_flight_ = std::max(0, missiles_in_flight_);
        }
    }
    // The player survives at zero and fails on the next hit, so the shipped
    // three shields allow four hits.
    if (shields_ < 0) {
        outcome_ = LevelOutcome::Failed;
    }
}

void Campaign::project_world_projectile(CampaignProjectile& projectile) const {
    CampaignEnemy point;
    point.wx = projectile.wx;
    point.wy = projectile.wy;
    point.wz = projectile.wz;
    point.world_radius = 18.0F;
    project_world_enemy(point);
    projectile.x = point.x;
    projectile.y = point.y;
    projectile.radius = point.radius;
}

bool Campaign::destroy_practice_target(std::size_t index,
                                       std::uint32_t frame) {
    return level_id() == 1 && practice_.destroy(index, frame);
}

ShotResult Campaign::fire_at(Weapon weapon, float aim_x, float aim_y,
                             float view_offset_x, float view_offset_y) {
    ShotResult result;
    if (complete()) {
        return result;
    }
    int damage = 1;
    const bool recruit = mode_ == GameMode::Recruit;
    if (weapon == Weapon::Laser) {
        // One unit of the gauge per shot.
        if (!recruit && laser_energy_ < 1.0F) {
            return result;
        }
        if (!recruit) {
            laser_energy_ -= 1.0F;
        }
    } else if (weapon == Weapon::ParticleBeam) {
        // The particle beam spends its own gauge the same way.
        if (!recruit && particle_energy_ < 1.0F) {
            return result;
        }
        if (!recruit) {
            particle_energy_ -= 1.0F;
        }
        damage = 2;
    } else {
        if (!recruit && photon_bolts_ <= 0) {
            return result;
        }
        if (!recruit) {
            --photon_bolts_;
        }
        damage = 5;
    }
    result.fired = true;
    if (level_id() == 1) {
        return result;
    }

    if (weapon != Weapon::PhotonBolts) {
        for (CampaignProjectile& projectile : projectiles_) {
            if (!projectile.active) {
                continue;
            }
            // Projectiles are already launched from the enemy's camera-space
            // pixel and follow a screen-space path to the reticle centre.
            const float x = projectile.x;
            const float y = projectile.y;
            const float dx = aim_x - x;
            const float dy = aim_y - y;
            const float hit_radius = projectile.radius + 10.0F;
            if (dx * dx + dy * dy <= hit_radius * hit_radius) {
                projectile.active = false;
                --missiles_in_flight_;
                missiles_in_flight_ = std::max(0, missiles_in_flight_);
                result.hit = true;
                result.destroyed = true;
                result.hit_x = x;
                result.hit_y = y;
                return result;
            }
        }
    }

    std::size_t best = enemy_slots_;
    float best_distance = std::numeric_limits<float>::max();
    float lock_distance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < enemy_slots_; ++index) {
        CampaignEnemy& enemy = enemies_[index];
        if (enemy.status != EnemyStatus::Active || !enemy.targetable) {
            continue;
        }
        const float x = enemy.world_motion ? enemy.x : enemy.x + view_offset_x;
        const float y = enemy.world_motion ? enemy.y : enemy.y + view_offset_y;
        const float dx = aim_x - x;
        const float dy = aim_y - y;
        const float distance = dx * dx + dy * dy;
        const float hit_radius = enemy.radius + 12.0F;
        if (distance <= hit_radius * hit_radius && distance < best_distance) {
            best = index;
            best_distance = distance;
        }
        // The photon lock scans active enemies, projects each to the
        // screen, and takes the one nearest the reticle - the running
        // minimum of HALF the pixel distance, first candidate accepted
        // inside 130 (260 full pixels). No world-distance term.
        const float half_dist =
            std::sqrt(dx * dx + dy * dy) * 0.5F;
        if (half_dist < lock_distance) {
            lock_distance = half_dist;
            lock_target_ = index;
        }
    }
    const bool was_locked = photon_locked_;
    photon_locked_ = best != enemy_slots_ && lock_distance < 130.0F;
    if (photon_locked_ && !was_locked) {
        events_.push_back({Event::Kind::PhotonLocked, enemies_[best].kind});
    }
    if (best == enemy_slots_ ||
        (weapon == Weapon::PhotonBolts && !photon_locked_)) {
        return result;
    }
    CampaignEnemy& enemy = enemies_[best];
    result.locked = photon_locked_;
    result.hit = true;
    result.hit_x = enemy.world_motion ? enemy.x : enemy.x + view_offset_x;
    result.hit_y = enemy.world_motion ? enemy.y : enemy.y + view_offset_y;
    result.hit_wx = enemy.wx;
    result.hit_wy = enemy.wy;
    result.hit_wz = enemy.wz;
    enemy.hit_points -= damage;
    if (enemy.hit_points <= 0) {
        enemy.status = EnemyStatus::Destroyed;
        ++destroyed_total_;
        level_score_ += points_for(enemy.kind, best);
        result.destroyed = true;
        events_.push_back({Event::Kind::EnemyDestroyed, enemy.kind});
    }
    update_command_phase();
    finish_if_ready();
    return result;
}

bool Campaign::death_blossom() {
    if (level_id() != 11 || !death_blossom_available_ || complete()) {
        return false;
    }
    death_blossom_available_ = false;
    // The Blossom refills every system - both weapon gauges to 100, a full
    // photon magazine, and one step of hull repair - then rolls RND(4)
    // against dipswitch 17. A match is the malfunction: the refilled gauges
    // drain to 10/10, the photon magazine empties, and the hull repair is
    // taken back away.
    laser_energy_ = 100.0F;
    particle_energy_ = 100.0F;
    photon_bolts_ = 20;
    shields_ = std::min(dip_.shields(), shields_ + 1);
    if (next_random() % 5U == static_cast<std::uint32_t>(dip_.raw(17))) {
        laser_energy_ = 10.0F;
        particle_energy_ = 10.0F;
        photon_bolts_ = 0;
        shields_ -= 1;
        return false;
    }
    for (std::size_t index = 0; index < enemy_slots_; ++index) {
        CampaignEnemy& enemy = enemies_[index];
        if (enemy.status == EnemyStatus::Active ||
            enemy.status == EnemyStatus::Waiting) {
            enemy.status = EnemyStatus::Destroyed;
            ++destroyed_total_;
            level_score_ += points_for(enemy.kind, index);
            events_.push_back({Event::Kind::EnemyDestroyed, enemy.kind});
        }
    }
    finish_if_ready();
    return true;
}

void Campaign::finish_if_ready() {
    if (complete()) {
        return;
    }
    // Frontier damage is counted only by the first six meteor records. The
    // level fails once it exceeds four.
    if (level_id() == 4 && frontier_damage_ >= 5) {
        outcome_ = LevelOutcome::Failed;
        return;
    }
    if (level_id() == 7) {
        update_command_phase();
        return;
    }
    const bool reusable_record = std::any_of(
        enemies_.begin(),
        enemies_.begin() + static_cast<std::ptrdiff_t>(enemy_slots_),
        [](const CampaignEnemy& enemy) {
            return enemy.status == EnemyStatus::Destroyed ||
                   enemy.status == EnemyStatus::Escaped;
        });
    if (spawned_total_ < total_target_count_ && frame_ < 1550U &&
        reusable_record) {
        // Setup runs every frame and refills any individual finished
        // record. A former batch barrier made the next ship wait for the
        // slowest of all ten.
        spawn_next_wave();
    }
    const bool all_finished = std::all_of(
        enemies_.begin(),
        enemies_.begin() + static_cast<std::ptrdiff_t>(enemy_slots_),
        [](const CampaignEnemy& enemy) {
            return enemy.status == EnemyStatus::Destroyed ||
                   enemy.status == EnemyStatus::Escaped;
        });
    if (!all_finished) {
        return;
    }
    if (spawned_total_ < total_target_count_) {
        return;
    }
    // Grade the sector at the shared frame gates. Any escaped target drops
    // the class to "Many targets escaped / No bonus".
    const bool any_escaped = escaped_total_ > 0;
    result_class_ = any_escaped ? 0
                    : frame_ < 1200U ? 3
                                     : frame_ < 1500U ? 2 : 1;
    outcome_ = LevelOutcome::Cleared;
}

// Each kill adds to the score in thousand-point units, keyed by level -
// practice targets 1, scouts 3, meteors 5 for the first six records then
// 3, fighters and Ko-Dan escorts 6.
int Campaign::points_for(EnemyKind kind, std::size_t index) const {
    switch (kind) {
    case EnemyKind::Scout:
        return 3000;
    case EnemyKind::Meteor:
        return index < 6 ? 5000 : 3000;
    case EnemyKind::Fighter:
        return 6000;
    case EnemyKind::CommandShip:
        return command_phase_ == 8 ? 2000 : 0;
    }
    return 0;
}

int Campaign::destroyed_count() const {
    // Level 1 reports through the practice stage; other levels count the
    // enemies destroyed this level.
    if (level_id() == 1) {
        return practice_.destroyed_count();
    }
    return destroyed_total_;
}

int Campaign::remaining_count() const {
    if (level_id() == 1) {
        return practice_.remaining_count();
    }
    // Targets still in play: those spawned but not yet destroyed or escaped.
    return std::max(0, total_target_count_ - destroyed_total_ - escaped_total_);
}

int Campaign::current_level_score() const {
    if (level_id() == 1) {
        return practice_.score();
    }
    return level_score_;
}

int Campaign::score() const {
    // Banked score from cleared levels plus the current level's running score.
    return banked_score_ + current_level_score();
}

bool Campaign::advance_level(std::uint32_t seed) {
    if (!complete() || outcome_ == LevelOutcome::Failed ||
        outcome_ == LevelOutcome::Victory || plan_index_ + 1 >= kPlan.size()) {
        return false;
    }
    banked_score_ += current_level_score();
    ++plan_index_;
    // Between levels, repair one shield, capped at the dipswitch.
    shields_ = std::min(dip_.shields(), shields_ + 1);
    // The between-level recharge is ticked per briefing frame by
    // briefing_recharge() (dipswitches 10/12), a per-frame top-up.
    photon_bolts_ = std::min(20, photon_bolts_ + 5);
    // The result bonus banks with the score.
    banked_score_ += bonus_for_class(result_class_);
    begin_level(seed);
    return true;
}

void Campaign::continue_level(std::uint32_t seed) {
    // The dipswitch is labelled "Shields at start and continue".
    shields_ = dip_.shields();
    laser_energy_ = 100.0F;
    particle_energy_ = 100.0F;
    photon_bolts_ = 20;
    begin_level(seed);
}
