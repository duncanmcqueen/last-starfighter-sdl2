#pragma once

#include "practice_stage.h"

#include "dipswitches.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class Weapon { Laser, ParticleBeam, PhotonBolts };
enum class EnemyKind { Scout, Meteor, Fighter, CommandShip };
enum class EnemyStatus { Waiting, Active, Destroyed, Escaped };
enum class LevelOutcome { InProgress, Cleared, Failed, Victory };

// Arcade plays the dipswitch difficulty exactly. Recruit is the practice
// mode: enemies hold their fire entirely and every weapon fires without
// spending its gauge or ammunition.
enum class GameMode { Arcade, Recruit };

struct LevelInfo {
    int id{};
    const char* line1{};
    const char* line2{};
    const char* line3{};
    EnemyKind enemy_kind{};
    int enemy_count{};
    // Level 4 is the only briefing with FOUR lines; every other level
    // leaves this null. Declared last so the other entries' initialisers
    // stay unchanged.
    const char* line4{};
};

struct CampaignEnemy {
    EnemyKind kind = EnemyKind::Scout;
    EnemyStatus status = EnemyStatus::Waiting;
    std::size_t record_index{};
    std::uint32_t activation_frame{};
    std::uint32_t lifetime_frames{};
    float x = 400.0F;
    float y = 280.0F;
    float radius = 20.0F;
    float velocity_x{};
    float velocity_y{};
    float phase{};
    float lane{};
    int hit_points = 1;
    int max_hit_points = 1;
    // Enemy-record field 3: a small countdown armed whenever the record
    // initializes (scouts 1 frame, meteors/fighters 2 frames). A shot
    // attempt is only possible while it is positive; fire_window_delay
    // schedules its next rearming.
    int fire_window{};
    int fire_window_delay{};
    // ---- world motion model (levels 2-7 and 11) -------------------------
    // Enemies live in the shared 3D world: straight-line flight at constant
    // speed with continuous rotation rates, spawned far away and projected
    // onto the screen by the camera at the origin.
    bool world_motion = false;
    float wx{};
    float wy{};
    float wz{};
    float wvx{};
    float wvy{};
    float wvz{};
    float angle_x{};
    float angle_y{};
    float angle_z{};
    float rate_x{};
    float rate_y{};
    float rate_z{};
    float attack_rate_x{};
    float attack_rate_y{};
    float attack_rate_z{};
    float attack_far{};
    float attack_near{};
    int flight_type{};
    float world_radius = 30.0F;
    // Ko-Dan object 1001 is visual geometry, while records 2..10 are
    // invisible collision targets positioned against its limbs.
    bool targetable = true;
    bool render_model = true;
    bool can_fire = true;
    bool command_surface = false;
};

struct CampaignProjectile {
    bool active{};
    std::uint32_t launched_frame{};
    std::uint32_t lifetime_frames{};
    float start_x{};
    float start_y{};
    float x{};
    float y{};
    float radius = 8.0F;
    bool world_motion{};
    float wx{};
    float wy{};
    float wz{};
    float wvx{};
    float wvy{};
    float wvz{};
    EnemyKind source_kind = EnemyKind::Scout;
    std::size_t model_variant{};
    // Each launched missile rolls its accuracy class once. Only a bullseye
    // costs a shield.
    bool bullseye{};
};

struct ShotResult {
    bool fired{};
    bool locked{};
    bool hit{};
    bool destroyed{};
    float hit_x{};
    float hit_y{};
    float hit_wx{};
    float hit_wy{};
    float hit_wz{};
};

class Campaign final {
  public:
    static constexpr std::size_t kMaxEnemies = 64;
    static constexpr std::size_t kMaxProjectiles = 24;
    static constexpr std::size_t kPlanLength = 11;

    // Difficulty comes entirely from the arcade dipswitches; call this before
    // reset() so the starting shield count and recharge rates take effect.
    void set_dipswitches(const DipSwitches& switches) { dip_ = switches; }
    const DipSwitches& dipswitches() const { return dip_; }

    // Selects Arcade (default, faithful) or Recruit (enemies hold fire,
    // weapons rapid-fire). Call before reset().
    void set_mode(GameMode mode) { mode_ = mode; }
    GameMode mode() const { return mode_; }
    void set_view_angles(float yaw_degrees, float pitch_degrees) {
        view_yaw_ = yaw_degrees;
        view_pitch_ = pitch_degrees;
    }

    // One row of arrays/bomberflightfin.dat (15 int32 cells; the 464-byte
    // array header is stripped by the caller). Scouts spawn from these
    // precomputed flight paths.
    struct FlightRow {
        std::array<int, 15> cells{};
    };
    static std::vector<FlightRow> decode_flight_paths(
        const std::vector<std::uint8_t>& data);
    void set_flight_paths(const std::vector<FlightRow>& rows);

    // Combat events accumulated during update()/fire_at() for the audio
    // layer; drain returns them and clears the queue.
    struct Event {
        enum class Kind { EnemyFired, PlayerHit, EnemyDestroyed, EnemyEscaped, PhotonLocked };
        Kind kind{};
        EnemyKind enemy{};
    };
    std::vector<Event> drain_events();

    // The between-level recharge rates tick every frame of the briefing
    // (dipswitches 10/12) rather than a lump sum being applied.
    void briefing_recharge(int frames);

    // Result classification: every level grades
    // completion at the same frame gates - all destroyed before frame 1200
    // is class 3 (Outstanding, 100,000), before 1500 class 2 (Sharp
    // Shooting, 50,000), later class 1 (Good work, 25,000), and any
    // escaped targets leave class 0 (Many targets escaped, no bonus).
    int result_class() const { return result_class_; }
    static int bonus_for_class(int result_class) {
        switch (result_class) {
        case 3: return 100000;
        case 2: return 50000;
        case 1: return 25000;
        default: return 0;
        }
    }

    void reset(std::uint32_t seed);
    void begin_level(std::uint32_t seed);
    void update(std::uint32_t frame);
    bool destroy_practice_target(std::size_t index, std::uint32_t frame);
    ShotResult fire_at(Weapon weapon, float aim_x, float aim_y,
                       float view_offset_x, float view_offset_y);
    bool death_blossom();
    bool advance_level(std::uint32_t seed);
    void continue_level(std::uint32_t seed);

    int level_id() const;
    const LevelInfo& level_info() const;
    std::size_t plan_index() const { return plan_index_; }
    const PracticeStage& practice() const { return practice_; }
    const std::array<CampaignEnemy, kMaxEnemies>& enemies() const {
        return enemies_;
    }
    std::size_t enemy_slots() const { return enemy_slots_; }
    int total_target_count() const { return total_target_count_; }
    int activated_count() const { return activated_total_; }
    const std::array<CampaignProjectile, kMaxProjectiles>& projectiles() const {
        return projectiles_;
    }
    LevelOutcome outcome() const { return outcome_; }
    bool complete() const { return outcome_ != LevelOutcome::InProgress; }
    int destroyed_count() const;
    int remaining_count() const;
    int score() const;
    int current_level_score() const;
    // 0..dipswitches().shields(); the player fails once this drops below zero.
    int shields() const { return shields_; }
    int max_shields() const { return dip_.shields(); }
    float hull_regen() const { return hull_regen_; }
    int frontier_damage() const { return frontier_damage_; }
    float laser_energy() const { return laser_energy_; }
    float particle_energy() const { return particle_energy_; }
    int photon_bolts() const { return photon_bolts_; }
    bool photon_locked() const { return photon_locked_; }
    bool death_blossom_available() const { return death_blossom_available_; }
    std::uint32_t frame() const { return frame_; }
    int command_phase() const { return command_phase_; }
    std::uint32_t command_phase_age() const {
        return frame_ - command_phase_started_;
    }

  private:
    std::uint32_t next_random();
    bool roll_accuracy(int setting);
    void initialize_generic_level();
    void initialize_command_level();
    void spawn_next_wave();
    void initialize_enemy_record(CampaignEnemy& enemy, std::size_t index);
    void count_finished_enemy(const CampaignEnemy& enemy,
                              EnemyStatus previous_status);
    void update_enemy(CampaignEnemy& enemy);
    void schedule_enemy_fire(CampaignEnemy& enemy);
    void update_projectiles();
    void finish_if_ready();
    int points_for(EnemyKind kind, std::size_t index) const;
    int active_enemy_count() const;
    void spawn_world_enemy(CampaignEnemy& enemy, std::size_t index);
    void update_world_enemy(CampaignEnemy& enemy);
    void update_command_phase();
    void project_world_enemy(CampaignEnemy& enemy) const;
    void project_world_projectile(CampaignProjectile& projectile) const;
    int concurrent_missile_cap() const;
    int fire_window_seed(EnemyKind kind) const;
    int rearm_range() const;

    std::array<CampaignEnemy, kMaxEnemies> enemies_{};
    std::vector<FlightRow> flight_rows_;
    std::vector<Event> events_;
    float hull_regen_ = 0.0F;
    std::size_t lock_target_ = 0;
    int result_class_{};
    std::array<CampaignProjectile, kMaxProjectiles> projectiles_{};
    PracticeStage practice_;
    GameMode mode_ = GameMode::Arcade;
    std::size_t enemy_slots_{};
    int total_target_count_{};
    int spawned_total_{};
    int activated_total_{};
    // Sampled by each setup routine before it initializes a batch. The
    // selected cap/ranges remain fixed for that entire batch.
    int wave_activation_base_{};
    int destroyed_total_{};
    int escaped_total_{};
    int level_score_{};
    float spawn_anchor_x_{};
    float spawn_anchor_y_{};
    float spawn_anchor_z_{};
    int fighter_spread_ = 50;
    int fighter_formation_type_{};
    int command_phase_ = 8;
    std::uint32_t command_phase_started_{};
    float view_yaw_{};
    float view_pitch_{};
    std::size_t plan_index_{};
    std::uint32_t random_state_ = 1;
    std::uint32_t frame_{};
    int banked_score_{};
    DipSwitches dip_{};
    int shields_ = 3;
    int frontier_damage_{};
    float laser_energy_ = 100.0F;
    float particle_energy_ = 100.0F;
    int photon_bolts_ = 20;
    bool photon_locked_{};
    bool death_blossom_available_{};
    // How many enemy missiles are in flight. Launches increment it and
    // removals release it again; a launch is only allowed while it is below
    // the per-phase cap.
    int missiles_in_flight_{};
    LevelOutcome outcome_ = LevelOutcome::InProgress;
};

const std::array<int, Campaign::kPlanLength>& default_campaign_plan();
const LevelInfo& campaign_level_info(int level_id);
