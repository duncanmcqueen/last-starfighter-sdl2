#include <SDL.h>

#include "arcade.h"
#include "audio.h"
#include "campaign.h"
#include "dipswitches.h"
#include "font.h"
#include "model_3ds.h"
#include "music.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __HAIKU__
#include <Application.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>
#include <MediaDefs.h>
#include <MediaAddOn.h>
#include <Window.h>
#include <kernel/image.h>
#include <libgen.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

struct DisplayMode {
    std::uint32_t width = 800;
    std::uint32_t height = 600;
    std::uint32_t depth = 16;
};

std::uint32_t read_le32(const std::array<unsigned char, 16>& bytes,
                        std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

DisplayMode load_display_mode(const fs::path& assets) {
    const fs::path path = assets / "arrays" / "displaymode.dat";
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 16> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        throw std::runtime_error("cannot read " + path.string());
    }

    // Element zero of the saved array is unused; the useful values are 1..3.
    DisplayMode mode{read_le32(bytes, 4), read_le32(bytes, 8),
                     read_le32(bytes, 12)};
    if (mode.width < 320 || mode.width > 7680 || mode.height < 200 ||
        mode.height > 4320) {
        throw std::runtime_error("invalid display mode in " + path.string());
    }
    return mode;
}

std::uint32_t load_sync_rate(const fs::path& assets) {
    const fs::path path = assets / "arrays" / "syncrate.dat";
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 8> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const std::uint32_t decoded = static_cast<std::uint32_t>(bytes[4]) |
                                  (static_cast<std::uint32_t>(bytes[5]) << 8U) |
                                  (static_cast<std::uint32_t>(bytes[6]) << 16U) |
                                  (static_cast<std::uint32_t>(bytes[7]) << 24U);
    return decoded >= 10 && decoded <= 240 ? decoded : 30;
}

// Text effects choose a fresh ink once per game tick, not once per host
// display refresh, so a 60/120/144 Hz desktop neither speeds the effect up
// nor consumes gameplay's random-number stream. The exact RND seed is not
// known, so this reproduces the channel ranges and per-tick cadence
// deterministically instead.
std::uint8_t ink_channel(std::uint32_t tick, std::uint32_t salt,
                         std::uint32_t range = 256U,
                         std::uint32_t minimum = 0U) {
    std::uint32_t value = tick + 0x9e3779b9U * (salt + 1U);
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<std::uint8_t>(minimum + value % range);
}

SDL_Color random_ink(std::uint32_t tick, std::uint32_t salt) {
    return SDL_Color{ink_channel(tick, salt), ink_channel(tick, salt + 1U),
                     ink_channel(tick, salt + 2U), 255};
}

SDL_Color bright_random_ink(std::uint32_t tick, std::uint32_t salt) {
    // Each channel covers the inclusive 100..255 range (RND(155)+100).
    return SDL_Color{ink_channel(tick, salt, 156U, 100U),
                     ink_channel(tick, salt + 1U, 156U, 100U),
                     ink_channel(tick, salt + 2U, 156U, 100U), 255};
}

SDL_Color victory_ink(std::uint32_t tick) {
    // RGB(255, RND(100), RND(100)+50).
    return SDL_Color{255, ink_channel(tick, 73U, 101U),
                     ink_channel(tick, 74U, 101U, 50U), 255};
}

std::array<ModelWorldTransform, 49> load_frontier_layout(
    const fs::path& assets) {
    const fs::path path = assets / "arrays" / "frontier.dat";
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 50U * 6U * sizeof(float)> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const auto value = [&](std::size_t row, std::size_t column) {
        const std::size_t offset = (column * 50U + row) * sizeof(float);
        const std::uint32_t bits =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
        float result{};
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    };
    std::array<ModelWorldTransform, 49> result{};
    for (std::size_t row = 1; row < 50U; ++row) {
        ModelWorldTransform& transform = result[row - 1U];
        transform.x = value(row, 1U);
        transform.y = value(row, 2U);
        transform.z = value(row, 3U);
        const float scale = value(row, 5U) / 100.0F;
        transform.scale_x = transform.scale_y = transform.scale_z = scale;
    }
    return result;
}

Model3ds command_breakup_model(const Model3ds& source,
                               std::uint32_t breakup_frame) {
    Model3ds result = source;
    if (source.limbs.size() != source.vertices.size() ||
        breakup_frame == 0U) {
        return result;
    }

    constexpr std::size_t kLimbCount = 47U;
    constexpr float kDeg = 3.14159265358979323846F / 180.0F;
    std::array<ModelVertex, kLimbCount + 1U> centers{};
    std::array<std::size_t, kLimbCount + 1U> counts{};
    for (std::size_t index = 0; index < source.vertices.size(); ++index) {
        const std::size_t limb = source.limbs[index];
        if (limb == 0U || limb > kLimbCount) {
            continue;
        }
        centers[limb].x += source.vertices[index].x;
        centers[limb].y += source.vertices[index].y;
        centers[limb].z += source.vertices[index].z;
        ++counts[limb];
    }
    for (std::size_t limb = 1U; limb <= kLimbCount; ++limb) {
        if (counts[limb] == 0U) {
            continue;
        }
        const float inverse = 1.0F / static_cast<float>(counts[limb]);
        centers[limb].x *= inverse;
        centers[limb].y *= inverse;
        centers[limb].z *= inverse;
    }

    // Each of the 47 limbs receives a boot-random direction
    // in 0..180 (the rear half adds 180), a speed in 6..18 multiplied by
    // 1.5, and per-frame XYZ limb rotation. A stable hash supplies values in
    // those exact ranges so screenshots are deterministic while retaining
    // the randomized breakup pattern.
    const float frame = static_cast<float>(breakup_frame);
    for (std::size_t index = 0; index < source.vertices.size(); ++index) {
        const std::size_t limb = source.limbs[index];
        if (limb == 0U || limb > kLimbCount) {
            continue;
        }
        float direction = static_cast<float>((limb * 137U + 53U) % 181U);
        if (limb > 21U) {
            direction += 180.0F;
        }
        const float speed =
            1.5F * static_cast<float>(6U + (limb * 73U + 11U) % 13U);
        const float ax = (frame - 3.0F) * kDeg;
        const float ay = (frame * 0.5F) * kDeg;
        const float az = (frame - speed * 0.25F) * kDeg;
        const float sx = std::sin(ax);
        const float cx = std::cos(ax);
        const float sy = std::sin(ay);
        const float cy = std::cos(ay);
        const float sz = std::sin(az);
        const float cz = std::cos(az);
        const ModelVertex center = centers[limb];
        const ModelVertex local{source.vertices[index].x - center.x,
                                source.vertices[index].y - center.y,
                                source.vertices[index].z - center.z};
        const float rx = local.x * cz - local.y * sz;
        const float ry = local.x * sz + local.y * cz;
        const float yx = ry * cx - local.z * sx;
        const float zx = ry * sx + local.z * cx;
        const float xy = rx * cy + zx * sy;
        const float zy = -rx * sy + zx * cy;
        const float radians = direction * kDeg;
        result.vertices[index] = {
            center.x + xy + std::sin(radians) * frame * speed,
            center.y + yx + std::cos(radians) * frame * speed,
            center.z + zy};
    }
    return result;
}

#ifdef __HAIKU__
// Haiku's media_addon_server crashes (null-deref in AudioMixer::Connected)
// when a BSoundPlayer connects and no default audio output is registered.
// The BSoundPlayer constructor then hangs the calling thread. This helper
// loads the system audio adapter and sets it as the default output before
// SDL gets a chance to create its BSoundPlayer.
static void haiku_ensure_audio_output() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (roster == nullptr) return;
    media_node output;
    if (roster->GetAudioOutput(&output) == B_OK && output.node != 0)
        return; // already have a default output

    const char* paths[] = {
        "/boot/system/add-ons/media/hmulti_audio.media_addon",
        "/boot/system/add-ons/media/cortex_audioadapter.media_addon",
    };
    for (const char* path : paths) {
        image_id image = load_add_on(path);
        if (image == B_APP_IMAGE) continue;
        BMediaAddOn* addon = new BMediaAddOn(image);
        const char* fail = nullptr;
        if (addon->InitCheck(&fail) == B_OK) {
            int32 flavors = addon->CountFlavors();
            for (int32 i = 0; i < flavors; ++i) {
                const flavor_info* info = nullptr;
                if (addon->GetFlavorAt(i, &info) != B_OK || info == nullptr)
                    continue;
                BMediaNode* node = nullptr;
                status_t err = B_OK;
                node = addon->InstantiateNodeFor(info, nullptr, &err);
                if (node == nullptr || err != B_OK) {
                    delete (flavor_info*)info;
                    continue;
                }
                if (roster->SetAudioOutput(node->Node()) == B_OK) {
                    node->Release();
                    delete (flavor_info*)info;
                    delete addon;
                    unload_add_on(image);
                    return;
                }
                node->Release();
                delete (flavor_info*)info;
            }
        }
        delete addon;
        unload_add_on(image);
    }
}

#endif // __HAIKU__

struct Sdl final {
    Sdl() {
        // Haiku's SDL video driver owns the event loop (and posts the window
        // close button as the quit event through it), so the events subsystem
        // is redundant there. The joystick subsystem only exists on X11/DirectX
        // in the shipped SDL build - querying it on Haiku fails the whole init.
        const std::uint32_t flags =
#ifdef __HAIKU__
            SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS
#else
            SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
            SDL_INIT_JOYSTICK
#endif
            ;
#ifdef __HAIKU__
        // SDL_Init installs SIGINT/SIGTERM handlers that convert those
        // signals into SDL_QUIT. On Haiku a spurious one arrives during
        // startup and closes the window at once, so keep SDL out of it; the
        // game already quits via the window close button and Esc/Q.
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        // Must run before SDL_Init: the Haiku audio driver creates a
        // BSoundPlayer which hangs/crashes the mixer if no default output.
        haiku_ensure_audio_output();
#endif
        if (SDL_Init(flags) != 0) {
            throw std::runtime_error(SDL_GetError());
        }
    }
    ~Sdl() { SDL_Quit(); }
    Sdl(const Sdl&) = delete;
    Sdl& operator=(const Sdl&) = delete;
};

enum class Scene {
    Boot,
    Attract,
    Intro,
    Briefing,
    Playing,
    LevelResult,
    GameOver,
    Victory,
    Diagnostics
};

struct CombatState {
    float reticle_x = 400.0F;
    float reticle_y = 300.0F;
    // Absolute camera angles in degrees. The two angular velocities
    // accumulate directly into the yaw/pitch state, clamping yaw to
    // +/-70 degrees and leaving pitch free to wrap.
    float turret_yaw = 0.0F;
    float turret_pitch = 0.0F;
    float turret_vel_yaw = 0.0F;
    float turret_vel_pitch = 0.0F;
    std::uint32_t started_at = 0;
    std::uint32_t result_started_at = 0;
    std::uint32_t laser_until = 0;
    std::uint32_t explosion_until = 0;
    std::uint32_t damage_until = 0;
    std::uint32_t command_explosion_until = 0;
    std::uint32_t hit_flash_until = 0;
    bool hit_flash_white = false;
    std::uint32_t death_blossom_until = 0;
    bool intro_blast_played = false;
    bool briefing_voice_played = false;
    bool briefing_hud_sound_played = false;
    float explosion_x = 400.0F;
    float explosion_y = 300.0F;
    float command_explosion_x = 400.0F;
    float command_explosion_y = 300.0F;
    float command_explosion_wx = 0.0F;
    float command_explosion_wy = 0.0F;
    float command_explosion_wz = 300.0F;
    // Which of the ten Explodeset variants this burst uses. Chosen per
    // explosion: RND(2)+base 449 -> the white sets 8-10
    // for light targets and the tutorial, RND(4)/RND(6)+base 400 -> the
    // orange sets for everything else. See the loader comment.
    std::size_t explosion_set = 0;
    Campaign campaign;
    Weapon weapon = Weapon::Laser;
    // Briefing frames 110/175 switch object 2 to image 2 (inner HUD) for
    // levels 2, 4 and 7. Every other level leaves image 1
    // (the full outer HUD) on the same plane.
    bool hud_outer_visible = true;
};

bool level_uses_outer_hud(int level_id) {
    return level_id != 2 && level_id != 4 && level_id != 7;
}

float horizontal_view_offset(float yaw_degrees) {
    constexpr float kDegrees = 3.14159265358979323846F / 180.0F;
    return -500.0F * std::tan(yaw_degrees * kDegrees);
}

float vertical_view_offset(float pitch_degrees) {
    constexpr float kDegrees = 3.14159265358979323846F / 180.0F;
    return 500.0F * std::tan(pitch_degrees * kDegrees);
}

const char* scene_name(Scene scene) {
    switch (scene) {
    case Scene::Boot:
        return "BOOT - arcade self-test";
    case Scene::Attract:
        return "ATTRACT - 5 inserts coin; Space/Left Alt starts; 0 diagnostics";
    case Scene::Intro:
        return "YOU HAVE BEEN RECRUITED";
    case Scene::Briefing:
        return "SECTOR BRIEFING";
    case Scene::Playing:
        return "TARGET PRACTICE - J/K/I/L or mouse aim; Space fires; A/W/S/D turret";
    case Scene::LevelResult:
        return "SECTOR RESULTS";
    case Scene::GameOver:
        return "GAME OVER";
    case Scene::Victory:
        return "STAR LEAGUE VICTORY";
    case Scene::Diagnostics:
        return "DIAGNOSTICS - UP/DOWN select, LEFT/RIGHT edit, 0 saves+exits";
    }
    return "STARFIGHTER";
}

SDL_Texture* load_bmp_texture(SDL_Renderer* renderer, const fs::path& path,
                              bool black_is_transparent = false) {
    SDL_Surface* surface = SDL_LoadBMP(path.string().c_str());
    if (!surface) {
        throw std::runtime_error("cannot load " + path.string() + ": " +
                                 SDL_GetError());
    }
    if (black_is_transparent &&
        SDL_SetColorKey(surface, SDL_TRUE,
                        SDL_MapRGB(surface->format, 0, 0, 0)) != 0) {
        const std::string error = SDL_GetError();
        SDL_FreeSurface(surface);
        throw std::runtime_error(error);
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    return texture;
}

// The intro's textured objects all use single-colour bitmaps, so one flat
// colour reproduces the shipped texture exactly.
// images/landing.bmp and images/pad.bmp are both 5x5 solid fills.
constexpr SDL_Color kLandingTexture{172, 156, 98, 255};
constexpr SDL_Color kPadTexture{123, 74, 49, 255};
// Object 11 is a flat plane carrying images/starfield2.bmp. It is the only
// textured surface in the intro whose texture is not a single colour, so it
// is drawn as a projected quad rather than through the depth raster.
void draw_textured_plane(SDL_Renderer* renderer, SDL_Texture* texture,
                         float plane_width, float plane_height,
                         const ModelWorldTransform& transform,
                         const ModelCamera& camera, float scroll_v) {
    const float half_w = plane_width * 0.5F;
    const float half_h = plane_height * 0.5F;
    const std::array<std::array<float, 4>, 4> corners{{
        {-half_w, half_h, 0.0F, 0.0F},
        {half_w, half_h, 1.0F, 0.0F},
        {half_w, -half_h, 1.0F, 1.0F},
        {-half_w, -half_h, 0.0F, 1.0F},
    }};
    std::array<SDL_Vertex, 4> quad{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        float x = 0.0F;
        float y = 0.0F;
        if (!project_world_point(transform, camera, corners[index][0],
                                 corners[index][1], 0.0F, 800, 600, x, y)) {
            return;
        }
        quad[index] = {{x, y},
                       {255, 255, 255, 255},
                       {corners[index][2], corners[index][3] + scroll_v}};
    }
    const std::array<int, 6> order{0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, texture, quad.data(),
                       static_cast<int>(quad.size()), order.data(),
                       static_cast<int>(order.size()));
}

constexpr float kPi = 3.14159265358979323846F;

// The HUD's red label/value ink, sampled from the reference captures.
constexpr SDL_Color kHudLabelRed{225, 30, 25, 255};

// ---- HUD instrument primitives ---------------------------------------
//
// Geometry here is measured from 800x600 screenshots of the release.

// Both HUD bitmaps are drawn scaled to 720x540, centred. Measured
// from the blue compass circle, which is 285 px wide in the 760x570 bitmap
// and 270 px wide on screen: 270/285 = 0.9474, and 760*0.9474 = 720.
constexpr SDL_Rect kHudRect{40, 30, 720, 540};
// Sphere centres and radius measured from the inner-only frame (frame 04),
// where no chrome shares the green channel and the blobs are unambiguous.
// The two scanner meshes are ghosted 3D objects, staged at
//   left  (-3.15,  1.90, 6.24)
//   right (+3.15,  1.90, 6.24)
// and scaled 3.5, 3.7, 3.5 (per cent of a radius-30 mesh).
// At z=6.24 with the 500 px focal length one world unit is 80.13 px, so the
// centres project to (147.6, 147.8) and (652.4, 147.8) and the 1.05-unit
// radius to 84.1 px. An earlier revision eyeballed these from a capture as
// (142,147)/(658,146) r85 - close, but off.
constexpr float kScannerRadius = 84.1F;
constexpr float kLeftScannerX = 147.6F;
constexpr float kLeftScannerY = 147.8F;
constexpr float kRightScannerX = 652.4F;
constexpr float kRightScannerY = 147.8F;
// The lower gauges and the LOCK ON banner are
// plain objects too, all ghosted, all at z=6.24 (80.13 px per world unit):
//   661 HULL TEMP   plain 2.55x2.55 at (-3.24, -1.91, 6.24), image 484
//   662 TARGET TEMP plain 2.55x2.55 at ( 3.26, -1.91, 6.24), image 485
//   663 LOCK ON     plain 0.86x0.43 at ( 0.012, 2.373, 6.24), image 489
// Projected: centres (140.4, 453.0), (661.2, 453.0) and (401.0, 109.9);
// the temp gauges are 204 px square and the banner 69x34.
constexpr float kLowerScannerY = 453.0F;
constexpr float kHullTempX = 140.4F;
constexpr float kTargetTempX = 661.2F;
constexpr int kTempGaugePx = 204;

// A latitude/longitude wireframe globe, the form both top-corner scanners
// take. `yaw`/`pitch` spin the grid so the instrument reads
// as a rotating sphere rather than a static doily.
void draw_wire_sphere(SDL_Renderer* renderer, float cx, float cy, float radius,
                      float yaw, float pitch, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const float cy_p = std::cos(pitch);
    const float sy_p = std::sin(pitch);
    const auto project = [&](float lat, float lon, float& sx, float& sy,
                             float& depth) {
        const float x = std::cos(lat) * std::cos(lon + yaw);
        const float y = std::sin(lat);
        const float z = std::cos(lat) * std::sin(lon + yaw);
        const float ry = y * cy_p - z * sy_p;
        const float rz = y * sy_p + z * cy_p;
        sx = cx + x * radius;
        sy = cy + ry * radius;
        depth = rz;
    };
    constexpr int kSteps = 24;
    // four latitude rings
    for (int ring = 1; ring <= 3; ++ring) {
        const float lat = -kPi * 0.5F + kPi * static_cast<float>(ring) / 4.0F;
        float px = 0.0F, py = 0.0F, pd = 0.0F;
        for (int step = 0; step <= kSteps; ++step) {
            const float lon = 2.0F * kPi * static_cast<float>(step) /
                              static_cast<float>(kSteps);
            float sx = 0.0F, sy = 0.0F, depth = 0.0F;
            project(lat, lon, sx, sy, depth);
            if (step > 0 && (depth > 0.0F || pd > 0.0F)) {
                SDL_RenderDrawLine(renderer, static_cast<int>(px),
                                   static_cast<int>(py), static_cast<int>(sx),
                                   static_cast<int>(sy));
            }
            px = sx;
            py = sy;
            pd = depth;
        }
    }
    // six longitude meridians
    for (int meridian = 0; meridian < 6; ++meridian) {
        const float lon = kPi * static_cast<float>(meridian) / 6.0F;
        float px = 0.0F, py = 0.0F, pd = 0.0F;
        for (int step = 0; step <= kSteps; ++step) {
            const float lat = -kPi * 0.5F + kPi * static_cast<float>(step) /
                                                static_cast<float>(kSteps);
            float sx = 0.0F, sy = 0.0F, depth = 0.0F;
            project(lat, lon, sx, sy, depth);
            if (step > 0 && (depth > 0.0F || pd > 0.0F)) {
                SDL_RenderDrawLine(renderer, static_cast<int>(px),
                                   static_cast<int>(py), static_cast<int>(sx),
                                   static_cast<int>(sy));
            }
            px = sx;
            py = sy;
            pd = depth;
        }
    }
}

// A filled arc band. The HULL TEMP and TARGET TEMP gauges are
// arcs that follow their circle's inner ring and fill from the bottom, not
// the bars an earlier revision drew.
void draw_arc_band(SDL_Renderer* renderer, float cx, float cy, float inner,
                   float outer, float start_angle, float sweep,
                   SDL_Color color) {
    if (sweep <= 0.0F) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int steps = std::max(2, static_cast<int>(sweep * 64.0F / kPi));
    for (int step = 0; step < steps; ++step) {
        const float a0 =
            start_angle + sweep * static_cast<float>(step) /
                              static_cast<float>(steps);
        const float a1 =
            start_angle + sweep * static_cast<float>(step + 1) /
                              static_cast<float>(steps);
        const std::array<SDL_Vertex, 4> quad{{
            {{cx + std::cos(a0) * inner, cy + std::sin(a0) * inner}, color, {0, 0}},
            {{cx + std::cos(a0) * outer, cy + std::sin(a0) * outer}, color, {0, 0}},
            {{cx + std::cos(a1) * outer, cy + std::sin(a1) * outer}, color, {0, 0}},
            {{cx + std::cos(a1) * inner, cy + std::sin(a1) * inner}, color, {0, 0}},
        }};
        const std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(renderer, nullptr, quad.data(),
                           static_cast<int>(quad.size()), order.data(),
                           static_cast<int>(order.size()));
    }
}

ModelWorldTransform practice_target_transform(const PracticeTarget& target,
                                              std::uint32_t frame) {
    constexpr float kRadiansPerDegree = 0.01745329251994329577F;
    const std::uint32_t age =
        frame >= target.activation_frame ? frame - target.activation_frame + 1U
                                         : 0U;
    // Advance the phase by twice the angular step each frame, grow the orbit
    // radius by ten per frame to 100, then position the target from its
    // actual world-space orbit.
    float orbit_radius =
        static_cast<float>(std::min<std::uint32_t>(100U, age * 10U));
    if (frame > 1530U) {
        orbit_radius = std::max(
            0.0F, orbit_radius - 15.0F * static_cast<float>(frame - 1530U));
    }
    const float angle_degrees =
        target.phase_degrees +
        static_cast<float>(2U * age *
                           static_cast<std::uint32_t>(target.angular_step));
    const float angle = angle_degrees * kRadiansPerDegree;
    ModelWorldTransform transform;
    transform.x = orbit_radius * std::cos(angle);
    transform.y =
        (orbit_radius + 20.0F) *
            std::sin(angle + target.phase_degrees * kRadiansPerDegree) +
        (static_cast<float>(target.vertical_seed) -
         orbit_radius * static_cast<float>(target.angular_step));
    transform.z = orbit_radius * std::sin(angle);
    transform.scale_x = transform.scale_y = transform.scale_z = 0.5F;
    transform.rotation_y = angle * 3.0F;
    return transform;
}

void target_position(const CombatState& combat, const PracticeTarget& target,
                     const Model3ds& model, std::uint32_t frame, float& x,
                     float& y, float& radius) {
    const ModelWorldTransform transform = practice_target_transform(target, frame);
    ModelCamera camera;
    constexpr float kRadiansPerDegree = 0.01745329251994329577F;
    camera.rotation_x = combat.turret_pitch * kRadiansPerDegree;
    camera.rotation_y = combat.turret_yaw * kRadiansPerDegree;
    float edge_x{};
    float edge_y{};
    if (!project_world_point(transform, camera, 0.0F, 0.0F, 0.0F, 800, 600,
                             x, y) ||
        !project_world_point(transform, camera, model.radius, 0.0F, 0.0F, 800,
                             600, edge_x, edge_y)) {
        x = -100.0F;
        y = -100.0F;
        radius = 1.0F;
        return;
    }
    radius = std::max(1.0F, std::hypot(edge_x - x, edge_y - y));
}

int run(const fs::path& assets, bool smoke_test, bool start_fullscreen,
        bool cabinet_mode, bool model_gallery, bool intro_test,
        std::uint32_t intro_preview_ms, const fs::path& screenshot_path,
        GameMode game_mode, std::uint32_t attract_preview_ms,
        std::uint32_t boot_preview_ms,
        std::uint32_t briefing_preview_ms, bool practice_preview,
        bool spanish_voice) {
    if (!fs::is_directory(assets / "Images") ||
        !fs::is_directory(assets / "Models") ||
        !fs::is_directory(assets / "Sounds")) {
        throw std::runtime_error("--assets must name the Starfighter asset directory");
    }

    const DisplayMode mode = load_display_mode(assets);
    const std::uint32_t sync_rate = load_sync_rate(assets);
    // Difficulty lives entirely in the shipped arcade dipswitches. The
    // diagnostics screen edits a working copy and applies it on exit.
    const DipSwitches shipped_switches = load_dipswitches(assets);
    const fs::path diagnostic_file = fs::path("starfighter-diagnostic.dat");
    DipSwitches active_switches = fs::exists(diagnostic_file)
                                      ? load_dipswitch_file(diagnostic_file,
                                                            shipped_switches)
                                      : shipped_switches;
    // The high-score table ships as CRLF text; new entries are mirrored to
    // starfighter-scores.dat beside the working directory so the supplied
    // release tree stays untouched.
    // arrays/bomberflightfin.dat: 464-byte header, then a column-major
    // 100x15 array of little-endian int32 cells.
    std::vector<Campaign::FlightRow> flight_rows;
    {
        std::ifstream in(assets / "arrays" / "bomberflightfin.dat",
                         std::ios::binary);
        if (in) {
            std::vector<std::uint8_t> data(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
            flight_rows = Campaign::decode_flight_paths(data);
        }
    }
    HighScoreTable high_scores =
        HighScoreTable::load(assets / "arrays" / "StrftrScores.dat");
    const fs::path score_file = fs::path("starfighter-scores.dat");
    if (fs::exists(score_file)) {
        high_scores = HighScoreTable::load(score_file);
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    Sdl sdl;
#ifdef __HAIKU__
    // SDL's Haiku driver chdir()s to the executable's directory during
    // SDL_Init when it believes the app was launched from the GUI (TERM
    // unset, as when Tracker launches it). The font directory is compiled as
    // a path relative to the working directory, so restore the directory the
    // caller chose. assets is absolute, so the game data is unaffected.
    std::error_code cwd_error;
    std::filesystem::current_path(
        std::filesystem::absolute(assets).parent_path(), cwd_error);
#endif

    std::uint32_t window_flags =
        SDL_WINDOW_RESIZABLE |
        (start_fullscreen
             ? static_cast<std::uint32_t>(SDL_WINDOW_FULLSCREEN_DESKTOP)
             : 0U);
#ifdef __HAIKU__
    // The dummy video driver (used for headless tests) rejects
    // SDL_WINDOW_RESIZABLE and SDL_WINDOW_FULLSCREEN_DESKTOP.
    if (SDL_getenv("SDL_VIDEODRIVER") &&
        std::string(SDL_getenv("SDL_VIDEODRIVER")) == "dummy") {
        window_flags = 0;
    }
#endif
    SDL_Window* window = SDL_CreateWindow(
        "STARFIGHTER", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(mode.width), static_cast<int>(mode.height),
        window_flags);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_ShowWindow(window);
#ifdef __HAIKU__
    // On Haiku, a window launched from a terminal may not receive keyboard
    // focus automatically. Walk the BApplication's windows and activate the
    // one SDL created.
    if (be_app) {
        BWindow* target = nullptr;
        for (int32 i = be_app->CountWindows() - 1; i >= 0; --i) {
            BWindow* w = be_app->WindowAt(i);
            if (w && w->Title() && strcmp(w->Title(), "STARFIGHTER") == 0) {
                target = w;
                break;
            }
        }
        if (!target && be_app->CountWindows() > 0) {
            target = be_app->WindowAt(be_app->CountWindows() - 1);
        }
        if (target) {
            target->Activate(true);
        }
    }
#endif

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }
    if (SDL_RenderSetLogicalSize(renderer, 800, 600) != 0) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RenderSetIntegerScale(renderer, SDL_FALSE);
    SDL_ShowCursor(cabinet_mode ? SDL_DISABLE : SDL_ENABLE);

    // The cabinet has two independent analog controls: turret and reticle.
    // SDL also exposes common dual-stick controllers as one joystick, so the
    // first device's axes 2/3 serve as the reticle when no second device is
    // present. Either controller's buttons fire, matching the tutorial card.
    std::vector<SDL_Joystick*> joysticks;
    for (int index = 0; index < std::min(2, SDL_NumJoysticks()); ++index) {
        if (SDL_Joystick* joystick = SDL_JoystickOpen(index)) {
            joysticks.push_back(joystick);
        }
    }

    SDL_Texture* stars =
        load_bmp_texture(renderer, assets / "Images" / "starfield2.bmp");
    SDL_Texture* emblem = load_bmp_texture(
        renderer, assets / "Images" / "emblem.bmp", true);
    // There are TWO HUD bitmaps and the game switches between them.
    // `inner hud new.bmp` carries the blue
    // compass, the red weapon labels and the green HITS/SCORE/GROUP text;
    // `outer hud new.bmp` is a superset that adds the green chrome, the four
    // corner circles, the top ship scale and the LOCK ON legend. Captures
    // show the game running in an inner-only state and an inner+outer state.
    // The texture switches at briefing frames 110/175:
    // levels 2, 4 and 7 use the inner image, all others the outer.
    SDL_Texture* hud = load_bmp_texture(
        renderer, assets / "Images" / "outer hud new.bmp", true);
    SDL_Texture* hud_inner = load_bmp_texture(
        renderer, assets / "Images" / "inner hud new.bmp", true);
    // Arcade boot sequence, all shipped as ready-made 800x600 frames:
    // bootup.bmp is the monitor alignment grid, bootup 1-6 animate a 3x3
    // hardware self-test with one cell highlighted at a time, bootup 7 is the
    // all-OK grid and bootup 8 the "INITIAL TESTS INDICATE / UNIT OK" card.
    SDL_Texture* boot_grid = load_bmp_texture(
        renderer, assets / "Images" / "bootup.bmp");
    std::array<SDL_Texture*, 8> boot_frames{};
    for (std::size_t index = 0; index < boot_frames.size(); ++index) {
        boot_frames[index] = load_bmp_texture(
            renderer,
            assets / "Images" /
                ("bootup " + std::to_string(index + 1) + ".BMP"));
    }
    // The two corner scanners are shipped MESHES, not something to draw by
    // hand: "L sphere fin 4.3ds" is the globe with the Gunstar inside it
    // (mesh07), "R sphere fin 5.3ds" the denser object/bearing globe. Their
    // 3DS materials already carry the captured colours - mat1 yellow for the
    // Gunstar, material02 green for the cage, material01 red for the bars -
    // so they are rendered with a white tint and colour themselves.
    const Model3ds left_scanner_model =
        load_3ds_model(assets / "Models" / "L sphere fin 4.3ds");
    const Model3ds right_scanner_model =
        load_3ds_model(assets / "Models" / "R sphere fin 5.3ds",
                       {23, 24, 25, 26, 27, 28, 29, 30, 31, 32});
    const Model3ds star_sphere_model =
        load_3ds_model(assets / "Models" / "3dstarscape.3ds");
    // ptemp/entemp are the real 213x213 temperature gauges, four stages each
    // (4 = cool, label only; 1 = full ring, and ptemp 1 adds DANGER). They
    // carry their own HULL TEMP / TARGET TEMP labels, which is why neither
    // string appears in the HUD bitmaps.
    std::array<SDL_Texture*, 4> hull_temp_frames{};
    std::array<SDL_Texture*, 4> target_temp_frames{};
    for (std::size_t index = 0; index < 4; ++index) {
        hull_temp_frames[index] = load_bmp_texture(
            renderer,
            assets / "Images" / ("ptemp " + std::to_string(index + 1) + ".bmp"),
            true);
        target_temp_frames[index] = load_bmp_texture(
            renderer,
            assets / "Images" / ("entemp " + std::to_string(index + 1) + ".bmp"),
            true);
        SDL_SetTextureBlendMode(hull_temp_frames[index], SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(target_temp_frames[index], SDL_BLENDMODE_BLEND);
    }
    SDL_Texture* lock_on_image = load_bmp_texture(
        renderer, assets / "Images" / "lockon fin.bmp", true);
    SDL_SetTextureBlendMode(lock_on_image, SDL_BLENDMODE_BLEND);
    SDL_Texture* reticle = load_bmp_texture(
        renderer, assets / "Images" / "reticle inner new.bmp", true);
    // death gauges 2.bmp is a plain 9.05x0.86
    // object at z=6.26, i.e. a 723x69 px bar that SLIDES across mid-screen -
    // not anything in the lower-right circle, which is the entemp gauge.
    // That bar is not implemented yet, so the image is not loaded.
    // TEN explosion sets of seven frames, not one. Boot loads set 1
    // alongside sets 2-10. Each set is a COMPLETE explosion - measured, every
    // set runs from a mean debris radius of ~31 px at frame 1 to ~120 px at
    // frame 7 - so the ten are variants, not stages of one long burst.
    //
    // The variant is chosen as `RND(n) * 7 + base`:
    //   base 400, RND(4)  -> sets 1-5   (orange)
    //   base 400, RND(6)  -> sets 1-7   (orange)
    //   base 449, RND(2)  -> sets 8-10  (white), including the tutorial
    // An earlier revision used set 1 for every kill in the game.
    std::array<std::array<SDL_Texture*, 7>, 10> explosion_sets{};
    for (std::size_t set = 0; set < explosion_sets.size(); ++set) {
        for (std::size_t index = 0; index < 7; ++index) {
            explosion_sets[set][index] = load_bmp_texture(
                renderer,
                assets / "Images" /
                    ("Explodeset " + std::to_string(set + 1) + "-" +
                     std::to_string(index + 1) + ".bmp"),
                true);
            SDL_SetTextureBlendMode(explosion_sets[set][index],
                                    SDL_BLENDMODE_BLEND);
        }
    }
    std::array<SDL_Texture*, 3> particle_frames{};
    for (std::size_t index = 0; index < particle_frames.size(); ++index) {
        particle_frames[index] = load_bmp_texture(
            renderer, assets / "Images" /
                          ("partbeam" + std::to_string(index + 1) + ".bmp"),
            true);
        SDL_SetTextureBlendMode(particle_frames[index], SDL_BLENDMODE_BLEND);
    }
    std::array<SDL_Texture*, 20> command_explosion_frames{};
    for (std::size_t index = 0; index < command_explosion_frames.size();
         ++index) {
        command_explosion_frames[index] = load_bmp_texture(
            renderer, assets / "Images" /
                          ("Explodecomm 1-" + std::to_string(index + 1) +
                           ".bmp"),
            true);
        SDL_SetTextureBlendMode(command_explosion_frames[index],
                                SDL_BLENDMODE_BLEND);
    }
    std::array<SDL_Texture*, 4> flash_red{};
    std::array<SDL_Texture*, 4> flash_white{};
    for (std::size_t index = 0; index < flash_red.size(); ++index) {
        flash_red[index] = load_bmp_texture(
            renderer, assets / "Images" /
                          ("flashred " + std::to_string(index + 1) + ".bmp"));
        flash_white[index] = load_bmp_texture(
            renderer, assets / "Images" /
                          ("flashwhite " + std::to_string(index + 1) + ".bmp"));
    }
    SDL_SetTextureBlendMode(emblem, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(hud, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(hud_inner, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(reticle, SDL_BLENDMODE_BLEND);
    std::array<SDL_Texture*, 100> gauge_frames{};
    for (std::size_t index = 0; index < gauge_frames.size(); ++index) {
        gauge_frames[index] = load_bmp_texture(
            renderer,
            assets / "Images" / ("newgauge" + std::to_string(index) + ".bmp"),
            true);
        SDL_SetTextureBlendMode(gauge_frames[index], SDL_BLENDMODE_BLEND);
    }
    // Title-sequence meshes. Boot stages both: LOGO.3ds and CENTAURI2.3ds,
    // each scaled 100/100/10 (the extruded lettering is flattened to a thin
    // slab) and placed at 0,0,120 in front of the origin camera, both hidden
    // until the attract script shows them. At z=120 with the 500 px focal
    // length the logo's 82.8-unit half-width projects to ~345 px, which is
    // the near-full-width title the captures show.
    const Model3ds title_logo_model =
        load_3ds_model(assets / "Models" / "LOGO.3ds");
    const Model3ds centauri_model =
        load_3ds_model(assets / "Models" / "CENTAURI2.3ds");
    const Model3ds light_target =
        load_3ds_model(assets / "Models" / "lighttarget.3ds");
    const Model3ds scout_model =
        load_3ds_model(assets / "Models" / "xurian ship 4.3DS");
    const Model3ds scout_flame_model =
        load_3ds_model(assets / "Models" / "xurian flames 6.3ds");
    const Model3ds meteor_model =
        load_3ds_model(assets / "Models" / "meteor 3d 1.3ds");
    const Model3ds frontier_model =
        load_3ds_model(assets / "Models" / "frontier prim 2.3ds");
    const Model3ds laser_bore_model =
        load_3ds_model(assets / "Models" / "laser bore 2.3ds");
    const Model3ds briefing_asteroid_model =
        load_3ds_model(assets / "Models" / "asteroid4.3ds");
    const std::array<ModelWorldTransform, 49> frontier_layout =
        load_frontier_layout(assets);
    const Model3ds fighter_model =
        load_3ds_model(assets / "Models" / "kd fighter 6.3ds");
    const Model3ds fighter_flame_model =
        load_3ds_model(assets / "Models" / "fighter flame 1.3ds");
    const Model3ds command_ship_model =
        load_3ds_model(assets / "Models" / "command ship 2.3ds");
    const Model3ds command_ship_combat_model =
        load_3ds_model(assets / "Models" / "command ship 2.3ds",
                       {15, 23, 24, 25, 26, 27, 28, 32});
    const Model3ds command_ship_breakup_model =
        load_3ds_model(assets / "Models" / "comm ship exp.3ds");
    const Model3ds weapon_switch_model =
        load_3ds_model(assets / "Models" / "weapons switches.3ds");
    const Model3ds missile_model =
        load_3ds_model(assets / "Models" / "missile 1-2.3ds");
    const Model3ds missile_model_alt =
        load_3ds_model(assets / "Models" / "missile 1-6.3ds");
    const Model3ds scout_missile_model =
        load_3ds_model(assets / "Models" / "missile 2-3.3ds");
    const Model3ds command_missile_model =
        load_3ds_model(assets / "Models" / "comm miss 3.3ds");
    const Model3ds command_missile_flame_model =
        load_3ds_model(assets / "Models" / "comm miss flame.3ds");
    const Model3ds gunstar_model =
        load_x_model(assets / "Models" / "gunstar3.x");
    const Model3ds launch_pad_model =
        load_3ds_model(assets / "Models" / "pad.3ds");
    const Model3ds landing_model =
        load_3ds_model(assets / "Models" / "landing5.3ds");
    // The Gunstar's hull material names images/gunstar.bmp; its black canopy
    // slot and orange plating come from that texture, not from a flat colour.
    const TextureImage gunstar_texture =
        load_texture_image(assets / "Images" / "gunstar.bmp");
    const Model3ds rylos_background_model =
        load_3ds_model(assets / "Models" / "bkgd.3ds");
    const Model3ds gunstar_flame_short =
        load_x_model(assets / "Models" / "flames.X");
    const Model3ds gunstar_flame_long =
        load_x_model(assets / "Models" / "flamel.X");
    auto hud_font = std::make_unique<GameFont>(
        renderer, assets / "FONTS- INSTALL ME" / "laststar.TTF", 17);
    auto message_font = std::make_unique<GameFont>(
        renderer, assets / "FONTS- INSTALL ME" / "laststar.TTF", 22);
    // The nominal briefing size is 0.02625*800 = 21, but the legacy rasterizer
    // produced glyphs about 15 percent narrower than FreeType at that nominal
    // size.  Nineteen pixels matches the measured 203-pixel width of
    // "PREPARE FOR TARGET" in the reference capture.
    auto briefing_font = std::make_unique<GameFont>(
        renderer, assets / "FONTS- INSTALL ME" / "laststar.TTF", 19);
    // The intro text uses the "last starfighter" face at 0.075 * display
    // width, which is 60 for this release's 800x600 displaymode.dat.
    auto intro_font = std::make_unique<GameFont>(
        renderer, assets / "FONTS- INSTALL ME" / "laststar.TTF",
        static_cast<int>(0.075F * 800.0F));
    // The attract fly-by changes Usuzi to 15%, 10%, and 25% of the relevant
    // display dimension. The face is shipped inside
    // USUZI.ZIP and extracted by CMake into the build tree.
    const fs::path usuzi_font = fs::path(STARFIGHTER_FONT_DIR) / "USUZI.TTF";
    // FreeType's pixel-em metrics are slightly smaller than the legacy
    // rasterizer's. These sizes reproduce the measured reference glyph
    // bounds while retaining the 120/80/150 proportions.
    auto pitch_large_font =
        std::make_unique<GameFont>(renderer, usuzi_font, 126);
    auto pitch_medium_font =
        std::make_unique<GameFont>(renderer, usuzi_font, 84);
    auto pitch_you_font =
        std::make_unique<GameFont>(renderer, usuzi_font, 158);
    auto pitch_title_font = std::make_unique<GameFont>(
        renderer, assets / "FONTS- INSTALL ME" / "STARF7.TTF", 124);
    auto intro_raster = std::make_unique<DepthRaster>(renderer, 800, 600);
    // Creating textures can invalidate a renderer's in-progress back buffer on
    // some SDL software drivers. Build the small glyph atlas before frame one.
    hud_font->width("0123456789STARFIGHTER");
    message_font->width("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -");
    intro_font->width("You have been recruited by the star league to defend "
                      "the frontier against xur and the ko-dan armada.");
    pitch_title_font->width("STARFIGHTER");
    pitch_large_font->width("Do you have what it takes?");
    pitch_medium_font->width(
        "Can you defeat Xur and the Ko-Dan armada? The fate of the galaxy "
        "depends on...");
    pitch_you_font->width("YOU");

    std::unique_ptr<AudioOutput> audio;
    SoundEffect laser_sound;
    SoundEffect particle_sound;
    SoundEffect photon_sound;
    SoundEffect explosion_sound;
    SoundEffect recruitment_sound;
    SoundEffect rocket_sound;
    SoundEffect briefing_voice_target;
    SoundEffect briefing_voice_frontier;
    SoundEffect briefing_voice_squadron;
    SoundEffect briefing_voice_closing;
    SoundEffect briefing_alarm_1;
    SoundEffect briefing_alarm_2;
    SoundEffect briefing_alarm_3;
    SoundEffect hiscore_music;
    SoundEffect enemy_missile_sound;
    SoundEffect enemy_missile2_sound;
    SoundEffect player_hit_sound;
    SoundEffect player_hit2_sound;
    SoundEffect lockon_sound;
    SoundEffect explosion2_sound;
    SoundEffect explosion3_sound;
    SoundEffect meteor_explode_sound;
    SoundEffect meteor_explode2_sound;
    SoundEffect cs_explode_sound;
    SoundEffect defeat_sound;
    SoundEffect record_breaker_sound;
    std::array<SoundEffect, 4> flyby_sounds{};
    SoundEffect engine_loop_sound;
    SoundEffect db_loop_sound;
    // The turret servo is three sounds, not one:
    //   turret3  played on movement start, only if the previous
    //            movement sound is no longer playing
    //   turret4  LOOPED while moving, stopped on rest
    //   turret5  played once as the turret comes to rest
    SoundEffect turret_start_sound;
    SoundEffect turret_move_sound;
    SoundEffect turret_stop_sound;
    std::size_t turret_loop_token = 0;
    bool turret_was_moving = false;
    SoundEffect target_lights_sound;   // level 1 target lights
    SoundEffect ship_pass_sound;       // enemy pass
    SoundEffect victory_voice_sound;   // victory voice
    SoundEffect attract_greeting_sound; // attract greeting
    SoundEffect attract_greeting_alt;  // the coin-flip alternate greeting
    SoundEffect attract_music;
    SoundEffect screen_on_sound;      // screen-on one-shot
    SoundEffect hud_tone_sound;       // HUD tone
    SoundEffect hud_tone2_sound;      // second HUD tone
    SoundEffect killzone_sound;       // kill zone
    SoundEffect welcome_rylos_sound;  // Rylos welcome
    bool music_ready = false;
    try {
        const fs::path voice_dir =
            assets / "Sounds" / (spanish_voice ? "Spanish" : "");
        const auto sound_path = [&](const char* name, bool voice = false) {
            return voice && spanish_voice ? voice_dir / name
                                         : assets / "Sounds" / name;
        };
        audio = std::make_unique<AudioOutput>();
        laser_sound = audio->load(assets / "Sounds" / "Laser1.wav");
        particle_sound = audio->load(assets / "Sounds" / "PBeam.wav");
        photon_sound = audio->load(assets / "Sounds" / "pmissile2.wav");
        explosion_sound = audio->load(assets / "Sounds" / "explosion1.wav");
        recruitment_sound = audio->load(sound_path("lstsrftr greet.wav", true));
        rocket_sound =
            audio->load(assets / "Sounds" / "rocket blast3.wav");
        attract_greeting_sound =
            audio->load(sound_path("greetings_starfighter.wav", true));
        // The alternate greeting is chosen by a coin flip at boot.
        attract_greeting_alt = audio->load(sound_path("maytheluck.wav", true));
        turret_start_sound = audio->load(assets / "Sounds" / "turret3.wav");
        turret_move_sound = audio->load(assets / "Sounds" / "turret4.wav");
        turret_stop_sound = audio->load(assets / "Sounds" / "turret5.wav");
        target_lights_sound =
            audio->load(assets / "Sounds" / "targetlights.wav");
        ship_pass_sound = audio->load(assets / "Sounds" / "ship1.wav");
        victory_voice_sound = audio->load(
            sound_path("goodshowstarfighterwaytogo.wav", true));
        // Briefing voices: level 1 plays
        // the target-practice briefing, level 4 the frontier warning,
        // fighter levels 5/6 flip a coin between two squadron calls, and
        // every other level (2, 3, 7, 11) gets a random alarm.
        briefing_voice_target =
            audio->load(sound_path("preparefortargetwithmusic.wav", true));
        // frontier.wav has no localized counterpart in the shipped pack.
        briefing_voice_frontier = audio->load(sound_path("frontier.wav"));
        briefing_voice_squadron = audio->load(
            sound_path("enemysquadroninsector3.wav", true));
        briefing_voice_closing = audio->load(
            sound_path("enemyinsectors367closingfast.wav", true));
        briefing_alarm_1 = audio->load(assets / "Sounds" / "alarm1.wav");
        briefing_alarm_2 = audio->load(assets / "Sounds" / "alarm2.wav");
        briefing_alarm_3 = audio->load(assets / "Sounds" / "alarm3.wav");
        enemy_missile_sound =
            audio->load(assets / "Sounds" / "emissile.wav");
        enemy_missile2_sound =
            audio->load(assets / "Sounds" / "emissile2.wav");
        player_hit_sound = audio->load(assets / "Sounds" / "phit1.wav");
        player_hit2_sound = audio->load(assets / "Sounds" / "phit2.wav");
        lockon_sound = audio->load(assets / "Sounds" / "lockon2.wav");
        explosion2_sound = audio->load(assets / "Sounds" / "explosion2.wav");
        explosion3_sound = audio->load(assets / "Sounds" / "explosion3.wav");
        meteor_explode_sound =
            audio->load(assets / "Sounds" / "meteorexplode.wav");
        meteor_explode2_sound =
            audio->load(assets / "Sounds" / "meteorexplode2.wav");
        cs_explode_sound = audio->load(assets / "Sounds" / "cs explode.wav");
        defeat_sound = audio->load(sound_path("defeat.wav", true));
        record_breaker_sound =
            audio->load(assets / "Sounds" / "record breaker.wav");
        flyby_sounds[0] = audio->load(assets / "Sounds" / "ship1.wav");
        flyby_sounds[1] = audio->load(assets / "Sounds" / "ship2.wav");
        flyby_sounds[2] = audio->load(assets / "Sounds" / "ship3.wav");
        flyby_sounds[3] = audio->load(assets / "Sounds" / "ship4.wav");
        hiscore_music = audio->load(assets / "Sounds" / "hiscoremusic1.wav");
        engine_loop_sound = audio->load(assets / "Sounds" / "engineloop.wav");
        db_loop_sound = audio->load(assets / "Sounds" / "dbloop.wav");
        screen_on_sound = audio->load(assets / "Sounds" / "screenon2.wav");
        hud_tone_sound = audio->load(assets / "Sounds" / "tone.wav");
        hud_tone2_sound = audio->load(assets / "Sounds" / "tone2.wav");
        killzone_sound = audio->load(assets / "Sounds" / "killzone.wav");
        welcome_rylos_sound =
            audio->load(sound_path("welcometorylos.wav", true));
        // Attract music override chain: a user-supplied attract-music.wav or
        // .mid (working directory first, then the asset Sounds directory)
        // replaces the shipped Strftr music.mid, which renders once through
        // the built-in wavetable voice.
        const std::initializer_list<std::pair<fs::path, bool>> overrides{
            {fs::path("attract-music.wav"), false},
            {assets / "Sounds" / "attract-music.wav", false},
            {fs::path("attract-music.mid"), true},
            {assets / "Sounds" / "attract-music.mid", true},
        };
        for (const auto& [candidate, is_midi] : overrides) {
            if (!fs::exists(candidate)) {
                continue;
            }
            if (is_midi) {
                const MusicTrack track = render_midi_file(candidate, 22050,
                                                          150.0);
                attract_music.samples.reserve(track.samples.size() * 2U);
                for (const std::int16_t sample : track.samples) {
                    attract_music.samples.push_back(sample);
                    attract_music.samples.push_back(sample);
                }
            } else {
                attract_music = audio->load(candidate);
            }
            music_ready = !attract_music.samples.empty();
            break;
        }
        if (!music_ready) {
            const MusicTrack track =
                render_midi_file(assets / "Sounds" / "Strftr music.mid",
                                 22050, 150.0);
            attract_music.samples.reserve(track.samples.size() * 2U);
            for (const std::int16_t sample : track.samples) {
                attract_music.samples.push_back(sample);
                attract_music.samples.push_back(sample);
            }
            music_ready = !attract_music.samples.empty();
        }
    } catch (const std::exception& error) {
        std::cerr << "starfighter: audio disabled: " << error.what()
                  << '\n';
    }

    // The game does NOT start in attract: it runs an arcade boot
    // sequence first, captured from a cold start.
    Scene scene = Scene::Boot;
    CombatState combat;
    int credits = 0;
    // Coin prices come from the dipswitches and follow diagnostics edits.
    int coin_price_start = std::max(0, active_switches.coin_to_start());
    int coin_price_continue = std::max(0, active_switches.coin_to_continue());
    // Cabinet meta-state: attract cycle clock, looping-channel tokens, the
    // high-score initials entry, and the diagnostics cursor.
    std::uint32_t attract_entered_at = SDL_GetTicks();
    std::uint32_t boot_started_at = SDL_GetTicks();
    bool attract_greeting_played = false;
    bool attract_welcome_played = false;
    std::size_t engine_loop_token = 0;
    std::size_t db_loop_token = 0;
    std::size_t music_loop_token = 0;
    std::size_t hiscore_token = 0;
    std::size_t greeting_token = 0;
    bool entering_initials = false;
    int pending_high_score = 0;
    std::string initials;
    std::size_t diag_selected = 6;
    Scene previous_scene = Scene::Attract;
    bool running = true;
    unsigned frames = 0;
    const std::uint32_t frame_ms = 1000U / sync_rate;
    const std::uint32_t smoke_frame_offset = smoke_test ? 500U : 0U;
    auto stage_frame = [&](std::uint32_t now) {
        return smoke_frame_offset + static_cast<std::uint32_t>(
                                        (static_cast<std::uint64_t>(
                                             now - combat.started_at) *
                                         sync_rate) /
                                        1000U);
    };
    auto reset_combat = [&]() {
        combat = CombatState{};
        combat.started_at = SDL_GetTicks();
        // set_dipswitches must precede reset(): the starting shield count and
        // both recharge rates are read from it.
        combat.campaign.set_dipswitches(active_switches);
        combat.campaign.set_mode(game_mode);
        combat.campaign.set_flight_paths(flight_rows);
        combat.campaign.reset(combat.started_at);
        combat.hud_outer_visible =
            level_uses_outer_hud(combat.campaign.level_id());
    };
    if (model_gallery) {
        // the gallery is its own diagnostic screen, not a game scene
        scene = Scene::Attract;
    }
    if (smoke_test) {
        reset_combat();
        if (briefing_preview_ms > 0U) {
            // Freeze the shared HUD/briefing renderer at an exact source
            // frame for side-by-side comparison with the reference captures.
            combat.result_started_at = SDL_GetTicks() - briefing_preview_ms;
            combat.briefing_voice_played = briefing_preview_ms >= 5000U;
            scene = Scene::Briefing;
        } else if (boot_preview_ms > 0U) {
            // Boot diagnostic: freeze the boot sequence at the requested age.
            boot_started_at = SDL_GetTicks() - boot_preview_ms;
            scene = Scene::Boot;
        } else if (attract_preview_ms > 0U) {
            // Attract diagnostic: freeze the cycle at the requested age so
            // the tutorial/showcase phases can be screenshot-verified.
            attract_entered_at = SDL_GetTicks() - attract_preview_ms;
            scene = Scene::Attract;
        } else if (intro_test) {
            combat.result_started_at = SDL_GetTicks();
            scene = Scene::Intro;
        } else if (practice_preview) {
            scene = Scene::Playing;
        } else {
            // Exercise a non-practice combat level and its larger model in CI.
            combat.campaign.update(PracticeStage::kRecallFrame);
            combat.campaign.advance_level(combat.started_at + 1U);
            combat.started_at = SDL_GetTicks();
            scene = Scene::Playing;
        }
    }
    auto fire_weapon = [&](std::uint32_t now) {
        if (scene != Scene::Playing || now < combat.laser_until) {
            return;
        }
        const std::uint32_t frame = stage_frame(now);
        combat.campaign.update(frame);
        const float view_x = horizontal_view_offset(combat.turret_yaw);
        const float view_y = vertical_view_offset(combat.turret_pitch);
        const ShotResult shot = combat.campaign.fire_at(
            combat.weapon, combat.reticle_x, combat.reticle_y, view_x, view_y);
        if (!shot.fired) {
            return;
        }
        combat.laser_until = now + 95U;
        if (audio) {
            if (combat.weapon == Weapon::ParticleBeam) {
                audio->play(particle_sound);
            } else if (combat.weapon == Weapon::PhotonBolts) {
                audio->play(photon_sound);
            } else {
                audio->play(laser_sound);
            }
        }
        if (combat.campaign.level_id() != 1) {
            if (shot.hit) {
                combat.explosion_until = now + 360U;
                combat.explosion_x = shot.hit_x;
                combat.explosion_y = shot.hit_y;
                // orange family, base 400. The wider RND(6) band belongs to
                // the fighter and Ko-Dan levels; scouts and
                // meteors use RND(4).
                const int level = combat.campaign.level_id();
                const int span =
                    (level == 5 || level == 6 || level == 7 || level == 11)
                        ? 7
                        : 5;
                combat.explosion_set =
                    static_cast<std::size_t>(std::rand() % span);
                combat.hit_flash_until = now + 4U * 1000U / 30U;
                combat.hit_flash_white = shot.destroyed;
                if (level == 7 && shot.destroyed &&
                    combat.campaign.outcome() == LevelOutcome::Victory) {
                    combat.command_explosion_until = now + 20U * 1000U / 30U;
                    combat.command_explosion_x = shot.hit_x;
                    combat.command_explosion_y = shot.hit_y;
                    combat.command_explosion_wx = shot.hit_wx;
                    combat.command_explosion_wy = shot.hit_wy;
                    combat.command_explosion_wz = shot.hit_wz;
                    if (audio) {
                        audio->play(cs_explode_sound);
                    }
                }
                if (shot.destroyed && audio) {
                    audio->play(explosion_sound);
                }
            }
            return;
        }
        std::size_t best_target = PracticeStage::kTargetCount;
        float best_distance_squared = 0.0F;
        float best_x = 0.0F;
        float best_y = 0.0F;
        for (std::size_t index = 0;
             index < combat.campaign.practice().targets().size(); ++index) {
            const PracticeTarget& target =
                combat.campaign.practice().targets()[index];
            if (target.status != PracticeTargetStatus::Active) {
                continue;
            }
            float target_x{};
            float target_y{};
            float target_radius{};
            target_position(combat, target, light_target, frame, target_x, target_y,
                            target_radius);
            const float dx = combat.reticle_x - target_x;
            const float dy = combat.reticle_y - target_y;
            const float distance_squared = dx * dx + dy * dy;
            const float hit_radius = target_radius + 12.0F;
            if (distance_squared <= hit_radius * hit_radius &&
                (best_target == PracticeStage::kTargetCount ||
                 distance_squared < best_distance_squared)) {
                best_target = index;
                best_distance_squared = distance_squared;
                best_x = target_x;
                best_y = target_y;
            }
        }
        if (best_target != PracticeStage::kTargetCount &&
            combat.campaign.destroy_practice_target(best_target, frame)) {
            combat.explosion_until = now + 360U;
            combat.explosion_x = best_x;
            combat.explosion_y = best_y;
            // light targets use the WHITE family: RND(2)*7 + 449
            combat.explosion_set = 7U + static_cast<std::size_t>(
                                            std::rand() % 3);
            if (audio) {
                audio->play(explosion_sound);
            }
        }
    };
    auto cycle_weapon = [&]() {
        switch (combat.weapon) {
        case Weapon::Laser:
            combat.weapon = Weapon::ParticleBeam;
            break;
        case Weapon::ParticleBeam:
            combat.weapon = Weapon::PhotonBolts;
            break;
        case Weapon::PhotonBolts:
            combat.weapon = Weapon::Laser;
            break;
        }
    };
    auto start_game = [&]() {
        if (scene == Scene::Attract && credits >= coin_price_start) {
            credits -= coin_price_start;
            reset_combat();
            combat.result_started_at = SDL_GetTicks();
            scene = Scene::Intro;
            if (audio) {
                audio->play(recruitment_sound);
            }
            return true;
        }
        return false;
    };
    auto continue_game = [&](std::uint32_t now) {
        if (scene == Scene::GameOver && !entering_initials &&
            credits >= coin_price_continue) {
            credits -= coin_price_continue;
            combat.campaign.continue_level(now);
            combat.hud_outer_visible =
                level_uses_outer_hud(combat.campaign.level_id());
            combat.started_at = now;
            combat.result_started_at = now;
            combat.briefing_voice_played = false;
            combat.briefing_hud_sound_played = false;
            scene = Scene::Briefing;
            return true;
        }
        return false;
    };
    auto begin_sector = [&](std::uint32_t now) {
        if (scene != Scene::Briefing) {
            return false;
        }
        combat.started_at = now;
        scene = Scene::Playing;
        // The level-1 target lights, gated on is_playing() so
        // they never stack.
        if (audio && combat.campaign.level_id() == 1 &&
            !audio->is_playing(target_lights_sound)) {
            audio->play(target_lights_sound);
        }
        if (audio) {
            if (combat.campaign.level_id() == 7) {
                // The level-7 lead-in: play the killzone tone as the
                // target orbit begins.
                audio->play(killzone_sound);
            }
        }
        return true;
    };
    auto finish_intro = [&](std::uint32_t now) {
        if (scene != Scene::Intro) {
            return false;
        }
        combat.result_started_at = now;
        combat.briefing_voice_played = false;
        combat.briefing_hud_sound_played = false;
        scene = Scene::Briefing;
        return true;
    };
    auto play_briefing_voice = [&]() {
        if (!audio || combat.briefing_voice_played) {
            return;
        }
        combat.briefing_voice_played = true;
        // This selector runs exactly at briefing frame 150.
        const int level = combat.campaign.level_id();
        if (level == 1) {
            audio->play(briefing_voice_target);
        } else if (level == 4) {
            audio->play(briefing_voice_frontier);
        } else if (level == 5 || level == 6) {
            audio->play(std::rand() % 2 == 0 ? briefing_voice_squadron
                                              : briefing_voice_closing);
        } else {
            switch (std::rand() % 3) {
            case 0: audio->play(briefing_alarm_1); break;
            case 1: audio->play(briefing_alarm_2); break;
            default: audio->play(briefing_alarm_3); break;
            }
        }
    };
    auto activate_death_blossom = [&](std::uint32_t now) {
        if (scene == Scene::Playing && combat.campaign.death_blossom()) {
            combat.death_blossom_until = now + 1800U;
            if (audio) {
                if (db_loop_token == 0) {
                    db_loop_token = audio->play_loop(db_loop_sound, 0.7F);
                }
                audio->play(explosion_sound);
            }
        }
    };
    auto enter_attract = [&]() {
        scene = Scene::Attract;
        entering_initials = false;
        attract_entered_at = SDL_GetTicks();
        attract_greeting_played = false;
        attract_welcome_played = false;
    };
    // Arcade flow: a finished game that places in the table goes straight to
    // initials entry before attract returns.
    auto begin_result_scene = [&](Scene result) {
        combat.result_started_at = SDL_GetTicks();
        scene = result;
        pending_high_score = 0;
        entering_initials = false;
        if (result == Scene::Victory && audio) {
            // Victory voice: "goodshowstarfighterwaytogo".
            audio->play(victory_voice_sound);
        }
        if (result == Scene::GameOver || result == Scene::Victory) {
            const int final_score = combat.campaign.score();
            if (final_score > 0 && high_scores.qualifies(final_score)) {
                pending_high_score = final_score;
                entering_initials = true;
                initials = "AAA";
                SDL_StartTextInput();
                if (audio) {
                    audio->stop(hiscore_token);
                    hiscore_token = 0;
                    audio->play(record_breaker_sound);
                }
            }
        }
    };
    auto commit_high_score = [&]() {
        if (!entering_initials) {
            return;
        }
        const std::size_t rank =
            high_scores.insert(initials, pending_high_score);
        (void)rank;
        high_scores.save(score_file);
        entering_initials = false;
        SDL_StopTextInput();
        enter_attract();
    };
    // The cabinet boots straight into attract: start the music loop for the
    // opening cycle.
    if (audio && music_ready) {
        music_loop_token = audio->play_loop(attract_music, 0.8F);
    }
    if (audio) {
        // The cabinet screen-on one-shot precedes the main loop.
        audio->play(screen_on_sound);
    }
    while (running) {
        const std::uint32_t frame_start = SDL_GetTicks();
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_MOUSEMOTION &&
                       scene == Scene::Playing) {
                float logical_x{};
                float logical_y{};
                SDL_RenderWindowToLogical(renderer, event.motion.x,
                                          event.motion.y, &logical_x,
                                          &logical_y);
                combat.reticle_x = std::clamp(logical_x, 75.0F, 725.0F);
                combat.reticle_y = std::clamp(logical_y, 70.0F, 520.0F);
            } else if (event.type == SDL_JOYBUTTONDOWN) {
                if (!start_game() && !finish_intro(frame_start) &&
                    !continue_game(frame_start) &&
                    !begin_sector(frame_start)) {
                    fire_weapon(frame_start);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                if (!start_game() && !finish_intro(frame_start) &&
                    !continue_game(frame_start) &&
                    !begin_sector(frame_start)) {
                    fire_weapon(frame_start);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_RIGHT &&
                       scene == Scene::Playing) {
                cycle_weapon();
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_MIDDLE) {
                activate_death_blossom(frame_start);
            } else if (event.type == SDL_TEXTINPUT && entering_initials) {
                for (const char* c = event.text.text; *c != '\0'; ++c) {
                    const char ch = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(*c)));
                    if (initials.size() < 3 &&
                        ((ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == ' ')) {
                        initials.push_back(ch);
                    }
                }
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    running = false;
                    break;
                case SDLK_F11: {
                    const std::uint32_t flags = SDL_GetWindowFlags(window);
                    const bool fullscreen =
                        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(
                        window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                case SDLK_5:
                    ++credits;
                    break;
                case SDLK_0:
                    if (scene == Scene::Diagnostics) {
                        // Leaving diagnostics applies the edited bank.
                        combat.campaign.set_dipswitches(active_switches);
                        if (!save_dipswitches(diagnostic_file,
                                             active_switches)) {
                            std::cerr << "warning: cannot save "
                                      << diagnostic_file << '\n';
                        }
                        coin_price_start =
                            std::max(0, active_switches.coin_to_start());
                        coin_price_continue =
                            std::max(0, active_switches.coin_to_continue());
                        scene = Scene::Attract;
                    } else if (scene == Scene::Attract) {
                        scene = Scene::Diagnostics;
                    }
                    break;
                case SDLK_UP:
                    if (scene == Scene::Diagnostics) {
                        diag_selected = diag_selected <= 1
                                            ? 20
                                            : (diag_selected == 0 ? 20
                                                                  : diag_selected - 1);
                    }
                    break;
                case SDLK_DOWN:
                    if (scene == Scene::Diagnostics) {
                        diag_selected = diag_selected >= 20 ? 1 : diag_selected + 1;
                    }
                    break;
                case SDLK_LEFT:
                    if (scene == Scene::Diagnostics) {
                        active_switches.values[diag_selected] =
                            std::max(0, active_switches.values[diag_selected] - 1);
                    }
                    break;
                case SDLK_RIGHT:
                    if (scene == Scene::Diagnostics) {
                        active_switches.values[diag_selected] =
                            std::min(255, active_switches.values[diag_selected] + 1);
                    }
                    break;
                case SDLK_SPACE:
                case SDLK_LALT:
                    if (!start_game() && !finish_intro(frame_start) &&
                        !continue_game(frame_start) &&
                        !begin_sector(frame_start)) {
                        fire_weapon(frame_start);
                    }
                    break;
                case SDLK_BACKSPACE:
                    if (entering_initials && !initials.empty()) {
                        initials.pop_back();
                    }
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    if (entering_initials) {
                        commit_high_score();
                    } else if (scene == Scene::Playing) {
                        cycle_weapon();
                    }
                    break;
                case SDLK_3:
                case SDLK_b:
                    activate_death_blossom(frame_start);
                    break;
                case SDLK_1:
                    if (!start_game() && scene == Scene::Playing) {
                        cycle_weapon();
                    }
                    break;
                case SDLK_m:
                    combat.weapon = Weapon::Laser;
                    break;
                case SDLK_COMMA:
                    combat.weapon = Weapon::ParticleBeam;
                    break;
                case SDLK_PERIOD:
                    combat.weapon = Weapon::PhotonBolts;
                    break;
                default:
                    break;
                }
            }
        }

        if (scene == Scene::Playing) {
            const std::uint8_t* keys = SDL_GetKeyboardState(nullptr);
            const float reticle_speed = 245.0F / static_cast<float>(sync_rate);
            // Acceleration is turret_acceleration / 10 (0.5 degrees/frame),
            // maximum velocity is turret_max_speed / 10 (2.5 degrees/frame),
            // and an idle axis brakes toward zero by exactly one
            // degree/frame. There is no exponential damping.
            const float turret_accel = active_switches.turret_acceleration();
            const float turret_max =
                static_cast<float>(active_switches.turret_max_speed()) / 10.0F;
            const float yaw_dir =
                (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT] ? -1.0F
                                                                 : 0.0F) +
                (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT] ? 1.0F
                                                                  : 0.0F);
            const float pitch_dir =
                (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP] ? -1.0F
                                                               : 0.0F) +
                (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN] ? 1.0F
                                                                 : 0.0F);
            const auto analog_axis = [](SDL_Joystick* joystick, int axis) {
                if (!joystick || SDL_JoystickNumAxes(joystick) <= axis) {
                    return 0.0F;
                }
                const Sint16 raw = SDL_JoystickGetAxis(joystick, axis);
                constexpr float deadzone = 8000.0F;
                if (std::fabs(static_cast<float>(raw)) < deadzone) {
                    return 0.0F;
                }
                return std::clamp(static_cast<float>(raw) / 32767.0F,
                                  -1.0F, 1.0F);
            };
            const float joy_yaw = joysticks.empty()
                                      ? 0.0F
                                      : analog_axis(joysticks[0], 0);
            const float joy_pitch = joysticks.empty()
                                        ? 0.0F
                                        : analog_axis(joysticks[0], 1);
            const float effective_yaw = std::clamp(yaw_dir + joy_yaw,
                                                   -1.0F, 1.0F);
            const float effective_pitch = std::clamp(pitch_dir + joy_pitch,
                                                     -1.0F, 1.0F);
            combat.turret_vel_yaw += effective_yaw * turret_accel;
            combat.turret_vel_pitch += effective_pitch * turret_accel;
            const auto brake = [](float velocity) {
                if (velocity > 0.0F) {
                    return std::max(0.0F, velocity - 1.0F);
                }
                if (velocity < 0.0F) {
                    return std::min(0.0F, velocity + 1.0F);
                }
                return 0.0F;
            };
            if (effective_yaw == 0.0F) {
                combat.turret_vel_yaw = brake(combat.turret_vel_yaw);
            }
            if (effective_pitch == 0.0F) {
                combat.turret_vel_pitch = brake(combat.turret_vel_pitch);
            }
            combat.turret_vel_yaw =
                std::clamp(combat.turret_vel_yaw, -turret_max, turret_max);
            combat.turret_vel_pitch =
                std::clamp(combat.turret_vel_pitch, -turret_max, turret_max);
            // The turret servo. Three sounds:
            //   turret3 starts on movement, gated on the start sound not
            //     already sounding, so a held key does not retrigger it;
            //   turret4 LOOPS while moving and is
            //     stopped when the turret rests;
            //   turret5 fires once as it comes to rest,
            //     itself gated on the other two not already sounding.
            // The rest test is `abs(vel) == 1` on integer
            // velocities; here the equivalent is a small epsilon on the
            // float servo.
            if (audio) {
                const bool moving =
                    std::fabs(combat.turret_vel_yaw) > 0.02F ||
                    std::fabs(combat.turret_vel_pitch) > 0.02F;
                if (moving && !turret_was_moving) {
                    if (!audio->is_playing(turret_start_sound)) {
                        audio->play(turret_start_sound);
                    }
                    turret_loop_token = audio->play_loop(turret_move_sound);
                } else if (!moving && turret_was_moving) {
                    audio->stop_loop(turret_loop_token);
                    turret_loop_token = 0;
                    if (!audio->is_playing(turret_start_sound) &&
                        !audio->is_playing(turret_stop_sound)) {
                        audio->play(turret_stop_sound);
                    }
                }
                turret_was_moving = moving;
            }
            if (keys[SDL_SCANCODE_J]) {
                combat.reticle_x -= reticle_speed;
            }
            if (keys[SDL_SCANCODE_L]) {
                combat.reticle_x += reticle_speed;
            }
            if (keys[SDL_SCANCODE_I]) {
                combat.reticle_y -= reticle_speed;
            }
            if (keys[SDL_SCANCODE_K]) {
                combat.reticle_y += reticle_speed;
            }
            SDL_Joystick* reticle_joystick =
                joysticks.size() > 1 ? joysticks[1]
                                     : (joysticks.empty() ? nullptr
                                                          : joysticks[0]);
            const int reticle_axis_x = joysticks.size() > 1 ? 0 : 2;
            const int reticle_axis_y = joysticks.size() > 1 ? 1 : 3;
            combat.reticle_x += analog_axis(reticle_joystick, reticle_axis_x) *
                                reticle_speed;
            combat.reticle_y += analog_axis(reticle_joystick, reticle_axis_y) *
                                reticle_speed;
            combat.turret_yaw += combat.turret_vel_yaw;
            combat.turret_pitch += combat.turret_vel_pitch;
            combat.reticle_x = std::clamp(combat.reticle_x, 75.0F, 725.0F);
            combat.reticle_y = std::clamp(combat.reticle_y, 70.0F, 520.0F);
            combat.turret_yaw = std::clamp(combat.turret_yaw, -70.0F, 70.0F);
            combat.turret_pitch = std::remainder(combat.turret_pitch, 360.0F);
            combat.campaign.set_view_angles(combat.turret_yaw,
                                            combat.turret_pitch);
            const std::uint32_t mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
            if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_LALT] ||
                (mouse_buttons & SDL_BUTTON_LMASK) != 0U) {
                fire_weapon(frame_start);
            }
            const int previous_shields = combat.campaign.shields();
            std::size_t previous_active = 0;
            for (std::size_t index = 0;
                 index < combat.campaign.enemy_slots(); ++index) {
                if (combat.campaign.enemies()[index].status ==
                    EnemyStatus::Active) {
                    ++previous_active;
                }
            }
            combat.campaign.update(stage_frame(frame_start));
            for (const Campaign::Event& combat_event :
                 combat.campaign.drain_events()) {
                if (!audio) {
                    continue;
                }
                switch (combat_event.kind) {
                case Campaign::Event::Kind::EnemyFired:
                    audio->play(combat_event.enemy == EnemyKind::Scout
                                    ? enemy_missile2_sound
                                    : enemy_missile_sound);
                    break;
                case Campaign::Event::Kind::PlayerHit:
                    audio->play(std::rand() % 2 == 0 ? player_hit_sound
                                                      : player_hit2_sound);
                    break;
                case Campaign::Event::Kind::EnemyDestroyed:
                    // fire_weapon handles ordinary aimed kills. This covers
                    // the Death Blossom's event-driven multi-kills.
                    if (frame_start < combat.death_blossom_until) {
                        audio->play(explosion2_sound);
                    }
                    break;
                case Campaign::Event::Kind::PhotonLocked:
                    audio->play(lockon_sound);
                    break;
                case Campaign::Event::Kind::EnemyEscaped:
                    break;
                }
            }
            // The enemy pass plays only when NONE of the four ship-pass
            // sounds is already sounding, so the four
            // share a single voice and never stack. The other three
            // ship sounds are loaded and gated on but never played in this build.
            if (audio && combat.campaign.level_id() != 1) {
                std::size_t now_active = 0;
                for (std::size_t index = 0;
                     index < combat.campaign.enemy_slots(); ++index) {
                    if (combat.campaign.enemies()[index].status ==
                        EnemyStatus::Active) {
                        ++now_active;
                    }
                }
                if (now_active > previous_active &&
                    !audio->is_playing(ship_pass_sound)) {
                    audio->play(ship_pass_sound);
                }
            }
            if (combat.campaign.shields() < previous_shields) {
                combat.damage_until = frame_start + 180U;
                combat.hit_flash_until = frame_start + 4U * 1000U / 30U;
                combat.hit_flash_white = false;
            }
            if (combat.campaign.complete() &&
                frame_start >= combat.death_blossom_until &&
                frame_start >= combat.command_explosion_until) {
                combat.result_started_at = frame_start;
                scene = Scene::LevelResult;
            }
        } else if (scene == Scene::Intro) {
            const std::uint32_t intro_age =
                frame_start - combat.result_started_at + intro_preview_ms;
            // Source frame 250 fires rocket blast3.wav and
            // the sequence runs out after frame 433, both at the
            // shipped 30 Hz sync rate measured from source frame -30.
            constexpr std::uint32_t kLaunchAtMs = 9300U;
            constexpr std::uint32_t kIntroDurationMs = 15400U;
            if (!combat.intro_blast_played && intro_age >= kLaunchAtMs) {
                combat.intro_blast_played = true;
                if (audio) {
                    audio->play(rocket_sound);
                }
            }
            if (intro_age >= kIntroDurationMs) {
                finish_intro(frame_start);
            }
        } else if (scene == Scene::Briefing) {
            const std::uint32_t briefing_age =
                frame_start - combat.result_started_at;
            if (!combat.briefing_voice_played &&
                briefing_age >= 150U * 1000U / 30U) {
                play_briefing_voice();
            }
            if (!combat.briefing_hud_sound_played &&
                briefing_age >= 110U * 1000U / 30U) {
                combat.briefing_hud_sound_played = true;
                if (audio) {
                    const int level = combat.campaign.level_id();
                    audio->play(level == 2 || level == 4 || level == 7
                                    ? hud_tone2_sound
                                    : hud_tone_sound);
                }
            }
            if (briefing_age >= 200U * 1000U / 30U) {
                begin_sector(frame_start);
            }
        } else if (scene == Scene::LevelResult &&
                   frame_start - combat.result_started_at >= 4500U) {
            if (combat.campaign.outcome() == LevelOutcome::Victory) {
                begin_result_scene(Scene::Victory);
            } else if (combat.campaign.outcome() == LevelOutcome::Failed) {
                begin_result_scene(Scene::GameOver);
            } else if (combat.campaign.advance_level(frame_start)) {
                combat.hud_outer_visible =
                    level_uses_outer_hud(combat.campaign.level_id());
                combat.result_started_at = frame_start;
                combat.briefing_voice_played = false;
                combat.briefing_hud_sound_played = false;
                scene = Scene::Briefing;
            } else {
                begin_result_scene(Scene::Victory);
            }
        } else if ((scene == Scene::GameOver &&
                    frame_start - combat.result_started_at >= 15000U) ||
                   (scene == Scene::Victory &&
                    frame_start - combat.result_started_at >= 10000U)) {
            if (entering_initials) {
                // The cabinet waits for the player to finish entering
                // initials; the dwell no longer pulls the screen away.
                if (frame_start - combat.result_started_at >= 45000U) {
                    commit_high_score();
                }
            } else {
                enter_attract();
            }
        }

        // Looping channels and one-shot scene music follow the scene.
        if (scene != previous_scene) {
            if (audio) {
                if (previous_scene == Scene::Attract &&
                    music_loop_token != 0) {
                    audio->stop_loop(music_loop_token);
                    music_loop_token = 0;
                }
                if (scene == Scene::Playing && previous_scene != Scene::Playing) {
                    engine_loop_token = audio->play_loop(engine_loop_sound, 0.55F);
                }
                if (previous_scene == Scene::Playing &&
                    scene != Scene::Playing) {
                    audio->stop_loop(engine_loop_token);
                    engine_loop_token = 0;
                    audio->stop_loop(db_loop_token);
                    db_loop_token = 0;
                }
                if ((scene == Scene::Victory || scene == Scene::GameOver) &&
                    previous_scene != Scene::Victory &&
                    previous_scene != Scene::GameOver) {
                    // Stop any previous sting first so replays never stack.
                    audio->stop(hiscore_token);
                    // Defeat plays its own sting; victory gets the
                    // high-score theme.
                    hiscore_token = audio->play(
                        scene == Scene::GameOver ? defeat_sound
                                                 : hiscore_music);
                }
                if (previous_scene == Scene::Victory ||
                    previous_scene == Scene::GameOver) {
                    // Continuing or timing out must silence the game-end
                    // theme; it is a long one-shot and would otherwise keep
                    // playing into the next sector.
                    audio->stop(hiscore_token);
                    hiscore_token = 0;
                }
            }
            if (scene == Scene::Attract && previous_scene != Scene::Attract &&
                !entering_initials) {
                // Non-enter_attract transitions into Attract keep the clock
                // coherent so the showcase cycle restarts from the emblem.
                attract_entered_at = frame_start;
                attract_greeting_played = false;
                attract_welcome_played = false;
            }
            previous_scene = scene;
        }
        // Death Blossom loop tracks the effect window.
        if (db_loop_token != 0 && frame_start > combat.death_blossom_until) {
            if (audio) {
                audio->stop_loop(db_loop_token);
            }
            db_loop_token = 0;
        }
        // The between-level recharge ticks every briefing frame against
        // the dipswitch rates (a per-frame top-up).
        if (scene == Scene::Briefing) {
            combat.campaign.briefing_recharge(1);
        }
        // The attract title/greeting sample plays once per attract visit,
        // shortly after the emblem appears.
        if (scene == Scene::Attract && !attract_greeting_played &&
            frame_start - attract_entered_at >= 900U) {
            attract_greeting_played = true;
            if (audio) {
                greeting_token =
                    audio->play((std::rand() % 2) == 0 ? attract_greeting_alt
                                                       : attract_greeting_sound);
            }
        }
        // Leaving attract (starting a game) cuts the greeting off cleanly.
        if (greeting_token != 0 && scene != Scene::Attract) {
            audio->stop(greeting_token);
            greeting_token = 0;
        }

        std::string title = std::string("STARFIGHTER | ") + scene_name(scene) +
                            " | credits=" + std::to_string(credits);
        SDL_SetWindowTitle(window, title.c_str());
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (scene == Scene::Boot && !model_gallery) {
            // Captured from a cold start:
            //   alignment grid -> Star League emblem -> 3x3 self-test
            //   -> UNIT OK -> chrome logo -> INSERT COIN -> attract
            // Every panel except the emblem and the logo is a shipped frame.
            // Durations follow the run lengths measured in the cold-start
            // capture at 0.66 s per frame; the whole boot is about 33 s.
            constexpr std::uint32_t kGridMs = 2600U;      // c002-c006
            constexpr std::uint32_t kEmblemMs = 2600U;    // c007-c010
            constexpr std::uint32_t kTestStepMs = 1300U;  // c011-c022
            constexpr std::uint32_t kTestMs = 6U * kTestStepMs;
            constexpr std::uint32_t kAllOkMs = 1300U;
            constexpr std::uint32_t kUnitOkMs = 5300U;    // c023-c030
            constexpr std::uint32_t kBootLogoMs = 5300U;  // c031-c038
            constexpr std::uint32_t kBootCoinMs = 7900U;  // c039-c050
            const std::uint32_t boot_age = frame_start - boot_started_at;
            std::uint32_t at = 0;
            if (boot_age < (at += kGridMs)) {
                SDL_RenderCopy(renderer, boot_grid, nullptr, nullptr);
            } else if (boot_age < (at += kEmblemMs)) {
                SDL_RenderCopy(renderer, emblem, nullptr, nullptr);
            } else if (boot_age < (at += kTestMs)) {
                const std::size_t step =
                    ((boot_age - (at - kTestMs)) / kTestStepMs) % 6U;
                SDL_RenderCopy(renderer, boot_frames[step], nullptr, nullptr);
            } else if (boot_age < (at += kAllOkMs)) {
                SDL_RenderCopy(renderer, boot_frames[6], nullptr, nullptr);
            } else if (boot_age < (at += kUnitOkMs)) {
                SDL_RenderCopy(renderer, boot_frames[7], nullptr, nullptr);
            } else if (boot_age < (at += kBootLogoMs)) {
                render_3ds_model(renderer, title_logo_model, 400.0F, 300.0F,
                                 345.0F, 0.0F, 0.0F, 255, 0.0F, false,
                                 SDL_Color{212, 196, 120, 255});
            } else if (boot_age < (at += kBootCoinMs)) {
                // the boot card's INSERT COIN is large - the same 60 px
                // "last starfighter" face the intro uses, with a new
                // RND(255) ink each frame.
                const std::uint32_t boot_tick =
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(boot_age) * sync_rate) /
                        1000U);
                intro_font->draw("INSERT COIN", 400, 270,
                                 random_ink(boot_tick, 21U),
                                 TextAlign::Center);
            } else {
                scene = Scene::Attract;
                attract_entered_at = frame_start;
            }
        } else if (scene == Scene::Diagnostics && !model_gallery) {
            // The interactive diagnostics bank. UP/DOWN selects a switch,
            // LEFT/RIGHT edits it, 0 applies and returns to attract.
            const SDL_Color menu_color{80, 255, 120, 255};
            const SDL_Color dim_color{120, 200, 130, 255};
            message_font->draw("DIAGNOSTICS", 400, 24, menu_color,
                               TextAlign::Center);
            const std::vector<std::string> rows =
                diagnostic_rows(active_switches.values, diag_selected);
            for (std::size_t index = 0; index < rows.size(); ++index) {
                hud_font->draw(rows[index], 150,
                               66 + static_cast<int>(index) * 25,
                               index == diag_selected ? menu_color : dim_color,
                               TextAlign::Left);
            }
            hud_font->draw("UP/DOWN SELECT - LEFT/RIGHT ADJUST - 0 SAVES+EXITS",
                           400, 574, menu_color, TextAlign::Center);
        } else if (scene != Scene::Intro) {
            // The intro clears to a solid backdrop colour and only brings
            // the starfield in as a textured plane at frame 305, so the
            // intro must not start from the shared starfield blit.
            //
            // The stars move because the camera turns inside object 100.
            // The two object rotations are state-specific, not general
            // turret parallax: level 2 rotates by pitch/10, while the
            // command state 8 rotation uses its own phase counter.
            //
            if (scene == Scene::Playing) {
                // Object 100 is the shipped 18,876-vertex star sphere,
                // scaled 3900 percent with the camera inside it. During
                // ordinary play it remains fixed in world
                // space and the camera takes the full turret angles,
                // producing visible spherical motion.
                constexpr float kDeg =
                    3.14159265358979323846F / 180.0F;
                ModelWorldTransform sky;
                sky.scale_x = 39.0F;
                sky.scale_y = 39.0F;
                sky.scale_z = 39.0F;
                if (combat.campaign.level_id() == 2) {
                    // Only level/state 2 rotates object 100
                    // by one tenth of the accumulated pitch.
                    sky.rotation_x = combat.turret_pitch * kDeg / 10.0F;
                }
                if (combat.campaign.level_id() == 7 &&
                    combat.campaign.command_phase() == 8) {
                    // The command phase counter drives the rotation:
                    // object 100 rotates by counter/2.5.
                    sky.rotation_y =
                        static_cast<float>(combat.campaign.command_phase_age()) *
                        kDeg / 2.5F;
                }
                ModelCamera sky_camera;
                sky_camera.rotation_x = combat.turret_pitch * kDeg;
                sky_camera.rotation_y = combat.turret_yaw * kDeg;
                WorldMaterial sky_material;
                sky_material.lit = false;
                sky_material.cull_backfaces = false;
                intro_raster->clear({0, 0, 0, 255});
                render_world_model(*intro_raster, star_sphere_model, sky,
                                   sky_camera, sky_material);

                if (combat.campaign.level_id() == 1) {
                    for (const PracticeTarget& target :
                         combat.campaign.practice().targets()) {
                        if (target.status != PracticeTargetStatus::Active) {
                            continue;
                        }
                        const ModelWorldTransform object =
                            practice_target_transform(
                                target, stage_frame(frame_start));
                        render_world_model(*intro_raster, light_target, object,
                                           sky_camera);
                    }
                }

                // Level 4's base is a physical 49-piece model wall. The
                // game loads the 49 segments and applies frontier.dat's
                // column-major XYZ and uniform percentage scale;
                // an earlier revision omitted it entirely.
                if (combat.campaign.level_id() == 4) {
                    for (const ModelWorldTransform& segment : frontier_layout) {
                        render_world_model(*intro_raster, frontier_model,
                                           segment, sky_camera);
                    }
                }

                // Combat objects use the same camera and depth buffer as the
                // star sphere. Their world positions, rotations,
                // and object scales reach the rasterizer
                // directly; the former path re-projected them to 2D and then
                // invented a pose from screen velocity.
                for (std::size_t index = 0;
                     index < combat.campaign.enemy_slots(); ++index) {
                    const CampaignEnemy& enemy = combat.campaign.enemies()[index];
                    if (enemy.status != EnemyStatus::Active ||
                        !enemy.world_motion || !enemy.render_model) {
                        continue;
                    }
                    const Model3ds* model = &scout_model;
                    const Model3ds* flame = &scout_flame_model;
                    float scale = 1.5F; // scout scale 150%
                    if (enemy.kind == EnemyKind::Meteor) {
                        model = &meteor_model;
                        flame = nullptr;
                        // Meteors are scaled 50%.
                        scale = 0.5F;
                    } else if (enemy.kind == EnemyKind::Fighter) {
                        model = &fighter_model;
                        flame = &fighter_flame_model;
                        // Both fighter and flame are scaled 80%.
                        scale = 0.8F;
                    } else if (enemy.kind == EnemyKind::CommandShip) {
                        model = &command_ship_combat_model;
                        flame = nullptr;
                        // The command ship is scaled 300%.
                        scale = 3.0F;
                    }
                    ModelWorldTransform object;
                    object.x = enemy.wx;
                    object.y = enemy.wy;
                    object.z = enemy.wz;
                    object.scale_x = object.scale_y = object.scale_z = scale;
                    object.rotation_x = enemy.angle_x * kDeg;
                    object.rotation_y = enemy.angle_y * kDeg;
                    object.rotation_z = enemy.angle_z * kDeg;
                    if (flame != nullptr) {
                        WorldMaterial exhaust;
                        exhaust.lit = false;
                        exhaust.alpha = static_cast<std::uint8_t>(
                            185.0F + 55.0F * std::sin(
                                               enemy.phase * 3.0F +
                                               stage_frame(frame_start) * 0.31F));
                        exhaust.cull_backfaces = true;
                        render_world_model(*intro_raster, *flame, object,
                                           sky_camera, exhaust);
                    }
                    render_world_model(*intro_raster, *model, object,
                                       sky_camera);
                }
                for (const CampaignProjectile& projectile :
                     combat.campaign.projectiles()) {
                    if (!projectile.active || !projectile.world_motion) {
                        continue;
                    }
                    const bool command =
                        projectile.source_kind == EnemyKind::CommandShip;
                    const Model3ds& model =
                        projectile.source_kind == EnemyKind::Scout
                            ? scout_missile_model
                            : command
                                  ? command_missile_model
                                  : projectile.model_variant == 0U
                                        ? missile_model
                                        : missile_model_alt;
                    ModelWorldTransform object;
                    object.x = projectile.wx;
                    object.y = projectile.wy;
                    object.z = projectile.wz;
                    object.rotation_y =
                        std::atan2(projectile.wvx, projectile.wvz);
                    const float horizontal = std::sqrt(
                        projectile.wvx * projectile.wvx +
                        projectile.wvz * projectile.wvz);
                    object.rotation_x = -std::atan2(projectile.wvy, horizontal);
                    const float projectile_scale = command ? 0.3F : 0.6F;
                    object.scale_x = object.scale_y = object.scale_z =
                        projectile_scale;
                    if (command) {
                        WorldMaterial exhaust;
                        exhaust.lit = false;
                        exhaust.alpha = 220;
                        exhaust.cull_backfaces = true;
                        render_world_model(*intro_raster,
                                           command_missile_flame_model, object,
                                           sky_camera, exhaust);
                    }
                    render_world_model(*intro_raster, model, object, sky_camera);
                }
                if (frame_start < combat.command_explosion_until) {
                    const std::uint32_t duration = 20U * 1000U / 30U;
                    const std::uint32_t age =
                        duration -
                        (combat.command_explosion_until - frame_start);
                    const std::uint32_t breakup_frame =
                        std::min(20U, age * 30U / 1000U + 1U);
                    const Model3ds breakup = command_breakup_model(
                        command_ship_breakup_model, breakup_frame);
                    ModelWorldTransform object;
                    object.x = combat.command_explosion_wx;
                    object.y = combat.command_explosion_wy;
                    object.z = combat.command_explosion_wz;
                    object.scale_x = object.scale_y = object.scale_z = 3.0F;
                    object.rotation_y = 180.0F * kDeg;
                    render_world_model(*intro_raster, breakup, object,
                                       sky_camera);
                }
                intro_raster->present(renderer);
            } else if (scene == Scene::Briefing &&
                       (combat.campaign.level_id() == 2 ||
                        combat.campaign.level_id() == 4 ||
                        combat.campaign.level_id() == 7)) {
                // Three briefings bring real meshes into the view after
                // frame 150. They were absent in earlier revisions: asteroid4 in
                // level 2, the 49 frontier sections plus laser bore and
                // command ship in level 4, and the command ship in level 7.
                constexpr float kDeg =
                    3.14159265358979323846F / 180.0F;
                const std::uint32_t briefing_frame =
                    (frame_start - combat.result_started_at) * 30U / 1000U;
                ModelWorldTransform sky;
                sky.scale_x = sky.scale_y = sky.scale_z = 39.0F;
                ModelCamera briefing_camera;
                WorldMaterial sky_material;
                sky_material.lit = false;
                sky_material.cull_backfaces = false;
                intro_raster->clear({0, 0, 0, 255});
                render_world_model(*intro_raster, star_sphere_model, sky,
                                   briefing_camera, sky_material);

                if (briefing_frame >= 151U) {
                    const float growing_value =
                        static_cast<float>(briefing_frame + 150U);
                    if (combat.campaign.level_id() == 2) {
                        ModelWorldTransform asteroid;
                        asteroid.y = -25.0F - growing_value;
                        asteroid.z = 280.0F + growing_value;
                        asteroid.rotation_x =
                            (80.0F + growing_value) * kDeg;
                        asteroid.scale_x = asteroid.scale_y =
                            asteroid.scale_z =
                                (875.0F - growing_value) / 100.0F;
                        render_world_model(*intro_raster,
                                           briefing_asteroid_model, asteroid,
                                           briefing_camera);
                    } else if (combat.campaign.level_id() == 4) {
                        const float frontier_scale = growing_value / 1000.0F;
                        for (const ModelWorldTransform& stored :
                             frontier_layout) {
                            ModelWorldTransform segment = stored;
                            segment.scale_x = segment.scale_y =
                                segment.scale_z =
                                    std::min(stored.scale_x, frontier_scale);
                            render_world_model(*intro_raster, frontier_model,
                                               segment, briefing_camera);
                        }

                        ModelWorldTransform bore;
                        bore.y = -1.0F;
                        bore.z = 1.5F;
                        bore.scale_x = bore.scale_y =
                            growing_value / 1500.0F;
                        bore.scale_z = 0.18F;
                        bore.rotation_x =
                            static_cast<float>(briefing_frame + 160U) * kDeg;
                        render_world_model(*intro_raster, laser_bore_model,
                                           bore, briefing_camera);

                        ModelWorldTransform command;
                        command.x = -10.0F;
                        command.y = -40.0F;
                        command.z = 2900.0F;
                        command.rotation_y = 180.0F * kDeg;
                        command.scale_x = command.scale_y = command.scale_z =
                            growing_value / 600.0F;
                        render_world_model(*intro_raster,
                                           command_ship_combat_model, command,
                                           briefing_camera);
                    } else {
                        ModelWorldTransform command;
                        command.x = -10.0F;
                        command.y = -40.0F;
                        command.z = 2900.0F;
                        command.rotation_y = 180.0F * kDeg;
                        command.scale_x = command.scale_y = command.scale_z =
                            growing_value / 600.0F;
                        render_world_model(*intro_raster, command_ship_model,
                                           command, briefing_camera);
                    }
                }
                intro_raster->present(renderer);
            } else {
                SDL_RenderCopy(renderer, stars, nullptr, nullptr);
            }
        }
        if (model_gallery) {
            constexpr float kPi = 3.14159265358979323846F;
            const SDL_Color gallery_color{80, 255, 120, 255};
            message_font->draw("ORIGINAL MODEL GALLERY", 400, 30,
                               gallery_color, TextAlign::Center);
            const std::array<float, 4> views{0.0F, kPi * 0.5F, kPi,
                                              kPi * 1.5F};
            for (std::size_t index = 0; index < views.size(); ++index) {
                const float x = 130.0F + static_cast<float>(index) * 180.0F;
                render_3ds_model(renderer, scout_model, x, 175.0F, 65.0F,
                                 views[index], 0.0F);
                render_3ds_model(renderer, fighter_model, x, 350.0F, 72.0F,
                                 views[index], 0.0F);
            }
            hud_font->draw("XURIAN SCOUT: 0 / 90 / 180 / 270", 400, 245,
                           gallery_color, TextAlign::Center);
            hud_font->draw("KO-DAN FIGHTER: 0 / 90 / 180 / 270", 400, 425,
                           gallery_color, TextAlign::Center);
            render_3ds_model(renderer, meteor_model, 265.0F, 520.0F, 55.0F,
                             0.7F, 0.45F);
            render_3ds_model(renderer, command_ship_model, 535.0F, 515.0F,
                             85.0F, kPi, 0.0F);
        } else if (scene == Scene::Attract) {
            const SDL_Color prompt_color{80, 255, 120, 255};
            // Timed attract cycle: emblem and high scores, then a rotating
            // showcase of each enemy the player will face.
            constexpr std::uint32_t kEmblemMs = 7000U;
            constexpr std::uint32_t kShowcaseMs = 5000U;
            // Title sequence, measured from the dense attract capture.
            constexpr std::uint32_t kCentauriMs = 4000U;
            constexpr std::uint32_t kLogoMs = 5000U;
            constexpr std::uint32_t kPitchFrames = 1300U;
            constexpr std::uint32_t kPitchMs =
                kPitchFrames * 1000U / 30U;
            // The attract HUD screen. Classifying the 200-frame dense capture
            // shows the game spends roughly a THIRD of its attract cycle
            // sitting on a live HUD (runs of ~42 s, ~11 s and ~34 s), with
            // in-fiction messages written into the central ring. An earlier
            // revision had no HUD in attract at all.
            constexpr std::uint32_t kHudScreenMs = 24000U;
            // The "Gunstar Turret Control" attract tutorial runs
            // 1520 frames at the shipped 30 Hz after the
            // emblem and enemy showcases.
            constexpr std::uint32_t kTutorialFrames = 1520U;
            constexpr std::uint32_t kTutorialMs =
                kTutorialFrames * 1000U / 30U;
            constexpr std::uint32_t kTitleMs =
                kCentauriMs + kLogoMs + kPitchMs;
            constexpr std::uint32_t kCycleMs = kHudScreenMs + kTitleMs +
                                               kEmblemMs +
                                               4U * kShowcaseMs + kTutorialMs;
            const std::uint32_t this_attract_age =
                (frame_start - attract_entered_at) % kCycleMs;
            const std::uint32_t age = this_attract_age;
            const std::uint32_t original_tick =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(age) * sync_rate) / 1000U);
            if (age < frame_ms) {
                attract_welcome_played = false;
            }
            const float spin = static_cast<float>(frame_start % 6000U) *
                               (6.283185307179586F / 6000.0F);
            // ---- attract HUD screen ---------------------------------
            // the full HUD over the starfield, INSERT COIN below, and an
            // in-fiction message written into the central ring. The message
            // text is the level briefing table.
            if (age < kHudScreenMs) {
                SDL_RenderCopy(renderer, hud, nullptr, &kHudRect);
                constexpr float kDeg = kPi / 180.0F;
                const auto scanner_px = [](const Model3ds& m) {
                    return kScannerRadius * (m.radius / 30.0F);
                };
                const float drift =
                    static_cast<float>(frame_start % 20000U) *
                    (6.283185307179586F / 20000.0F);
                render_3ds_model(renderer, left_scanner_model, kLeftScannerX,
                                 kLeftScannerY,
                                 scanner_px(left_scanner_model),
                                 335.0F * kDeg + drift, 345.0F * kDeg, 255,
                                 5.0F * kDeg, true);
                render_3ds_model(renderer, right_scanner_model,
                                 kRightScannerX, kRightScannerY,
                                 scanner_px(right_scanner_model),
                                 30.0F * kDeg + drift, 340.0F * kDeg, 255,
                                 355.0F * kDeg, true);
                const SDL_Rect ulg{180, 100, 191, 198};
                const SDL_Rect urg{432, 100, 191, 198};
                const SDL_Rect llg{180, 300, 191, 198};
                const SDL_Rect lrg{432, 300, 191, 198};
                SDL_RenderCopy(renderer, gauge_frames[99], nullptr, &ulg);
                SDL_RenderCopyEx(renderer, gauge_frames[99], nullptr, &urg,
                                 0.0, nullptr, SDL_FLIP_HORIZONTAL);
                SDL_RenderCopyEx(renderer, gauge_frames[99], nullptr, &llg,
                                 0.0, nullptr, SDL_FLIP_VERTICAL);
                SDL_RenderCopyEx(renderer, gauge_frames[99], nullptr, &lrg,
                                 0.0, nullptr,
                                 static_cast<SDL_RendererFlip>(
                                     SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
                const SDL_Rect ht{static_cast<int>(kHullTempX) -
                                      kTempGaugePx / 2,
                                  static_cast<int>(kLowerScannerY) -
                                      kTempGaugePx / 2,
                                  kTempGaugePx, kTempGaugePx};
                const SDL_Rect tt{static_cast<int>(kTargetTempX) -
                                      kTempGaugePx / 2,
                                  static_cast<int>(kLowerScannerY) -
                                      kTempGaugePx / 2,
                                  kTempGaugePx, kTempGaugePx};
                SDL_RenderCopy(renderer, hull_temp_frames[3], nullptr, &ht);
                SDL_RenderCopy(renderer, target_temp_frames[3], nullptr, &tt);
                // the in-ring message, cycling through the sector briefings
                static const char* const kRingMessages[][3] = {
                    {"KO-DAN COMMAND", "SHIP DETECTED IN", "THE NEXT SECTOR"},
                    {"ENEMY SCOUT SHIPS", "DETECTED OVER", "ASTEROID"},
                    {"ENEMY FIGHTERS", "DETECTED IN THE", "NEXT SECTOR"},
                    {"ENEMY SWARM", "AMASSING IN THE", "NEXT SECTOR"},
                };
                const std::size_t which =
                    (age / 6000U) %
                    (sizeof(kRingMessages) / sizeof(kRingMessages[0]));
                // the ring message is re-inked like the rest of attract -
                // frame 12 catches it magenta, frame 01's prompt is blue
                const SDL_Color ring_ink = random_ink(original_tick, 81U);
                for (int line = 0; line < 3; ++line) {
                    hud_font->draw(kRingMessages[which][line], 400,
                                   258 + line * 38, ring_ink,
                                   TextAlign::Center);
                }
                // the live readouts the game shows here too
                hud_font->draw("1.00", 157, 263, kHudLabelRed,
                               TextAlign::Left);
                hud_font->draw("100", 157, 291, kHudLabelRed,
                               TextAlign::Left);
                hud_font->draw("1.00", 648, 263, kHudLabelRed,
                               TextAlign::Right);
                hud_font->draw("1.00", 648, 291, kHudLabelRed,
                               TextAlign::Right);
            } else
            // ---- title sequence -------------------------------------
            // An earlier revision opened attract on the emblem. The game runs
            // a title sequence first: a CENTAURI card, the chrome
            // STARFIGHTER logo (colour-cycled), then the recruitment pitch.
            if (age - kHudScreenMs < kTitleMs) {
                const std::uint32_t age = this_attract_age - kHudScreenMs;
                if (!attract_welcome_played) {
                    attract_welcome_played = true;
                    if (audio) {
                        audio->play(welcome_rylos_sound);
                    }
                }
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(renderer, nullptr);
                // The title is re-inked every frame; the capture
                // shows it cycling pink, green, purple, orange and blue.
                const SDL_Color ink = bright_random_ink(original_tick, 91U);
                if (age < kCentauriMs) {
                    // yaw 0, not kPi: the lettering faces the camera as
                    // authored, and a half turn mirrors it. The captured
                    // CENTAURI card is white, not colour-cycled.
                    // ORTHOGRAPHIC, not perspective. Boot flattens the
                    // lettering with a 100/100/10 scale - the glyphs are
                    // extruded 3D text and the 10 per cent Z scale collapses
                    // that depth. Rendering the
                    // full-depth mesh under perspective displaces each glyph
                    // radially by its own Z, which threw the R, the I and the
                    // TM off the baseline.
                    render_3ds_model(renderer, centauri_model, 400.0F, 300.0F,
                                     150.0F, 0.0F, 0.0F, 255, 0.0F, false,
                                     SDL_Color{235, 235, 235, 255});
                } else if (age < kCentauriMs + kLogoMs) {
                    render_3ds_model(renderer, title_logo_model, 400.0F,
                                     300.0F, 345.0F, 0.0F, 0.0F, 255, 0.0F,
                                     false, ink);
                } else if (age < kCentauriMs + kLogoMs + kPitchMs) {
                    const std::uint32_t pitch = age - kCentauriMs - kLogoMs;
                    const std::uint32_t pitch_frame =
                        std::min(kPitchFrames,
                                 pitch * 30U / 1000U + 1U);
                    constexpr float kDegF = kPi / 180.0F;

                    // The pitch is a 1,300-frame world-camera sequence,
                    // not a screen-space scale-up. The ship and both flame
                    // objects stay at (-50,0,0), scale 30%, while four exact
                    // camera shots move around them.
                    ModelWorldTransform gunstar;
                    gunstar.x = -50.0F;
                    gunstar.scale_x = gunstar.scale_y = gunstar.scale_z =
                        0.3F;
                    // The .x import basis contributes the quarter turn that
                    // the object state reports as a 270-degree X rotation.
                    // load_x_model preserves the authored DirectX basis, so
                    // applying that turn again makes the ship nose point up
                    // the screen.  Zero is the equivalent renderer-space
                    // attitude and reproduces the broadside flybys.
                    gunstar.rotation_x = 0.0F;
                    gunstar.rotation_y = 90.0F * kDegF;
                    gunstar.rotation_z = 270.0F * kDegF;
                    if (pitch_frame > 390U && pitch_frame < 427U) {
                        gunstar.rotation_z =
                            static_cast<float>((pitch_frame - 390U) * 10U +
                                               270U) *
                            kDegF;
                    } else if (pitch_frame > 760U && pitch_frame < 797U) {
                        gunstar.rotation_z =
                            static_cast<float>((pitch_frame - 760U) * 10U +
                                               270U) *
                            kDegF;
                    }

                    ModelCamera flyby_camera;
                    flyby_camera.z = 200.0F;
                    const auto orbit = [](float degrees) {
                        return degrees *
                               (3.14159265358979323846F / 180.0F);
                    };
                    if (pitch_frame < 361U) {
                        const float angle = orbit(
                            static_cast<float>(pitch_frame));
                        flyby_camera.x = std::sin(angle) * 300.0F;
                        flyby_camera.y = std::cos(angle) * 300.0F;
                        flyby_camera.z = std::cos(angle) * 300.0F;
                    } else {
                        const float angle = orbit(360.0F);
                        flyby_camera.x = std::sin(angle) * 300.0F;
                        flyby_camera.y = std::cos(angle) * 300.0F;
                        flyby_camera.z = std::cos(angle) * 300.0F;
                    }
                    // These are strict source gates.  The camera is stateful:
                    // on the boundary frame it keeps the preceding frame's
                    // transform, and after a gate closes it keeps the final
                    // transform written inside that gate.
                    if (pitch_frame > 450U) {
                        const std::uint32_t held =
                            std::min(pitch_frame, 719U) - 450U;
                        const float angle = orbit(static_cast<float>(held));
                        flyby_camera.x = std::sin(angle) * 300.0F;
                        flyby_camera.y = 0.0F;
                        flyby_camera.z = std::cos(angle) * 300.0F;
                    }
                    if (pitch_frame > 850U) {
                        const std::uint32_t held =
                            std::min(pitch_frame, 940U);
                        const float angle = orbit(
                            static_cast<float>(held - 850U));
                        // The camera distance is a subtraction: it
                        // closes from 299 to 210 units, creating the enormous
                        // flame pass at the end.  Treating it as addition was
                        // why an earlier revision receded into a speck.
                        const float distance =
                            1150.0F - static_cast<float>(held);
                        flyby_camera.x = 0.0F;
                        flyby_camera.y = std::sin(angle) * distance;
                        flyby_camera.z = -std::cos(angle) * distance;
                    }
                    // The camera looks at the origin, followed by the
                    // authored Y/Z overrides in the final two shots.
                    const float dx = -flyby_camera.x;
                    const float dy = -flyby_camera.y;
                    const float dz = -flyby_camera.z;
                    const float horizontal = std::hypot(dx, dz);
                    flyby_camera.rotation_x = -std::atan2(dy, horizontal);
                    flyby_camera.rotation_y = std::atan2(dx, dz);
                    if (pitch_frame > 850U) {
                        flyby_camera.rotation_y =
                            static_cast<float>(std::min(pitch_frame, 940U) -
                                               850U) *
                            kDegF;
                    }
                    if (pitch_frame > 1100U) {
                        flyby_camera.rotation_z =
                            static_cast<float>(
                                std::min(pitch_frame, 1279U) - 1100U) *
                            0.5F * kDegF;
                    }

                    // The source shows its bank of 200 tiny star objects for
                    // this scene.  It does not show object 100's dense
                    // 3dstarscape sphere (the old port did, producing an
                    // obviously busier background than every reference frame).
                    static const std::array<std::array<float, 3>, 200>
                        flyby_stars = [] {
                            std::array<std::array<float, 3>, 200> result{};
                            std::uint32_t state = 0x4c534654U;
                            const auto next = [&state]() {
                                state = state * 1664525U + 1013904223U;
                                return state;
                            };
                            for (auto& star : result) {
                                // RND(400)-250 for X/Y and
                                // RND(1000)-500 for Z.  The later warm-up
                                // mutates the backing array, but the stars are
                                // placed only once, so these are the positions
                                // that remain visible in the flyby.
                                star[0] = static_cast<float>(next() % 401U) -
                                          250.0F;
                                star[1] = static_cast<float>(next() % 401U) -
                                          250.0F;
                                star[2] = static_cast<float>(next() % 1001U) -
                                          500.0F;
                            }
                            return result;
                        }();
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    SDL_RenderFillRect(renderer, nullptr);
                    for (std::size_t index = 0;
                         index < flyby_stars.size(); ++index) {
                        ModelWorldTransform star;
                        star.x = flyby_stars[index][0];
                        star.y = flyby_stars[index][1];
                        star.z = flyby_stars[index][2];
                        float star_x = 0.0F;
                        float star_y = 0.0F;
                        if (!project_world_point(star, flyby_camera, 0.0F,
                                                 0.0F, 0.0F, 800, 600,
                                                 star_x, star_y) ||
                            star_x < 0.0F || star_x >= 800.0F ||
                            star_y < 0.0F || star_y >= 600.0F) {
                            continue;
                        }
                        const std::uint8_t brightness =
                            index % 5U == 0U ? 245U : 185U;
                        SDL_SetRenderDrawColor(renderer, brightness,
                                               brightness, brightness, 255);
                        const int size = index % 17U == 0U ? 2 : 1;
                        const SDL_Rect point{
                            static_cast<int>(star_x),
                            static_cast<int>(star_y), size, size};
                        SDL_RenderFillRect(renderer, &point);
                    }

                    // The text gates leave 50-frame clean cuts between
                    // shots. Text is drawn before the object pass, so the
                    // Gunstar correctly occludes letters where they cross.
                    const std::uint8_t shade = ink_channel(
                        original_tick, 117U, 156U);
                    const SDL_Color pitch_ink{
                        shade, shade,
                        static_cast<std::uint8_t>(shade + 100U), 255};
                    if (pitch_frame > 50U && pitch_frame < 300U) {
                        const SDL_Color title_ink{
                            255, ink_channel(original_tick, 121U, 101U),
                            ink_channel(original_tick, 122U, 101U, 50U), 255};
                        pitch_title_font->draw("STARFIGHTER", 402, 250,
                                               title_ink, TextAlign::Center);
                    } else if (pitch_frame > 350U && pitch_frame < 600U) {
                        static const char* const lines[]{
                            "Do you", "have", "what it", "takes?"};
                        for (std::size_t index = 0; index < 4U; ++index) {
                            pitch_large_font->draw(
                                lines[index], 402,
                                100 + static_cast<int>(index) * 100,
                                pitch_ink, TextAlign::Center);
                        }
                    } else if (pitch_frame > 650U && pitch_frame < 900U) {
                        static const char* const lines[]{
                            "Can you", "defeat Xur", "and the", "Ko-Dan",
                            "armada?"};
                        for (std::size_t index = 0; index < 5U; ++index) {
                            pitch_medium_font->draw(
                                lines[index], 402,
                                50 + static_cast<int>(index) * 100,
                                pitch_ink, TextAlign::Center);
                        }
                    } else if (pitch_frame > 950U && pitch_frame < 1050U) {
                        static const char* const lines[]{
                            "The fate of", "the galaxy", "depends on..."};
                        for (std::size_t index = 0; index < 3U; ++index) {
                            pitch_medium_font->draw(
                                lines[index], 402,
                                50 + static_cast<int>(index) * 200,
                                pitch_ink, TextAlign::Center);
                        }
                    } else if (pitch_frame > 1100U && pitch_frame < 1220U) {
                        pitch_you_font->draw("YOU", 402, 200, pitch_ink,
                                             TextAlign::Center);
                    }

                    intro_raster->clear({0, 0, 0, 0});
                    WorldMaterial flame_material;
                    flame_material.lit = false;
                    flame_material.alpha = 138;
                    flame_material.cull_backfaces = true;
                    const Model3ds& flame = (pitch_frame & 1U) == 0U
                                                ? gunstar_flame_short
                                                : gunstar_flame_long;
                    render_world_model(*intro_raster, flame, gunstar,
                                       flyby_camera, flame_material);
                    WorldMaterial ship_material;
                    ship_material.texture = &gunstar_texture;
                    render_world_model(*intro_raster, gunstar_model, gunstar,
                                       flyby_camera, ship_material);
                    intro_raster->present(renderer);
                }
            } else if (age - kHudScreenMs - kTitleMs < kEmblemMs) {
                const double phase = static_cast<double>(frame_start % 2400U) /
                                     2400.0 * 6.283185307179586;
                const auto alpha = static_cast<std::uint8_t>(
                    205.0 + 45.0 * (0.5 + 0.5 * std::sin(phase)));
                SDL_SetTextureAlphaMod(emblem, alpha);
                SDL_RenderCopy(renderer, emblem, nullptr, nullptr);
                message_font->draw("HIGH SCORES", 590, 96, prompt_color,
                                   TextAlign::Center);
                char line[32];
                for (std::size_t index = 0; index < HighScoreTable::kEntries;
                     ++index) {
                    const auto& entry = high_scores.entries()[index];
                    std::snprintf(line, sizeof(line), "%-3s %7d",
                                  entry.initials.c_str(), entry.score);
                    hud_font->draw(line, 590,
                                   130 + static_cast<int>(index) * 30,
                                   prompt_color, TextAlign::Center);
                }
            } else if (age - kHudScreenMs - kTitleMs <
                       kEmblemMs + 4U * kShowcaseMs) {
                const std::uint32_t phase_age = age - kHudScreenMs - kTitleMs;
                // The ENEMY IDENTIFICATION CHART: every enemy is on screen AT
                // ONCE, in two stages - five
                // entries, then the same five plus the Ko-Dan command ship.
                // An earlier revision showed them one at a time on separate
                // cards and omitted LIGHT TARGET and HEAVY MISSILE entirely.
                //
                // Positions are measured from frame 08 at 800x600. The label
                // ink is re-randomised every frame, which is why the same
                // label reads magenta in one captured frame and purple in the
                // next; each frame rolls RND(255) per channel.
                struct ChartEntry {
                    const Model3ds* model;
                    float model_x;
                    float model_y;
                    float scale;
                    float label_x;
                    float label_y;
                    const char* line1;
                    const char* line2;
                    float yaw;
                    float pitch;
                };
                // Build order measured from the fast capture: the chart adds
                // ONE entry at a time, LIGHT TARGET first, and the models
                // turn slowly. The previous "five then six, fixed attitude"
                // reading came from comparing two frames that happened to
                // look alike; a 0.66 s capture shows both wrong.
                const ChartEntry chart[6]{
                    {&light_target, 405.0F, 300.0F, 30.0F, 405.0F, 244.0F,
                     "LIGHT", "TARGET", kPi * 0.5F, 0.0F},
                    {&scout_model, 205.0F, 90.0F, 140.0F, 205.0F, 130.0F,
                     "XURIAN", "SCOUT SHIP", kPi, -0.30F},
                    {&fighter_model, 610.0F, 85.0F, 110.0F, 610.0F, 122.0F,
                     "KO-DAN", "FIGHTER", kPi * 0.88F, 0.30F},
                    {&meteor_model, 140.0F, 290.0F, 105.0F, 200.0F, 322.0F,
                     "METEOR", "BOMB", 0.6F, 0.2F},
                    {&missile_model, 608.0F, 295.0F, 55.0F, 645.0F, 344.0F,
                     "HEAVY", "MISSILE", kPi * 0.5F, 0.0F},
                    {&command_ship_model, 400.0F, 455.0F, 200.0F, 400.0F,
                     504.0F, "KO-DAN", "COMMAND SHIP", kPi, 0.22F},
                };
                // The chart is drawn on plain black - no
                // starfield behind it (frames 07 and 08).
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(renderer, nullptr);
                const std::uint32_t chart_age = phase_age - kEmblemMs;
                // A red "STARFIGHTER / ENEMIES" card introduces the chart.
                constexpr std::uint32_t kEnemiesTitleMs = 3000U;
                if (chart_age < kEnemiesTitleMs) {
                    const SDL_Color title_red{225, 40, 30, 255};
                    message_font->draw("STARFIGHTER", 400, 210, title_red,
                                       TextAlign::Center);
                    message_font->draw("ENEMIES", 400, 285, title_red,
                                       TextAlign::Center);
                }
                // then entries appear one at a time
                const std::size_t shown =
                    chart_age < kEnemiesTitleMs
                        ? 0U
                        : std::min<std::size_t>(
                              6U, 1U + (chart_age - kEnemiesTitleMs) / 2200U);
                for (std::size_t index = 0; index < shown; ++index) {
                    const ChartEntry& entry = chart[index];
                    // The entries turn slowly; the per-entry yaw above is the
                    // presentation attitude they drift around.
                    const float drift =
                        static_cast<float>(frame_start % 24000U) *
                        (6.283185307179586F / 24000.0F);
                    render_3ds_model(renderer, *entry.model, entry.model_x,
                                     entry.model_y, entry.scale,
                                     entry.yaw + drift * 0.5F, entry.pitch);
                }
                for (std::size_t index = 0; index < shown; ++index) {
                    const ChartEntry& entry = chart[index];
                    const SDL_Color ink = random_ink(
                        original_tick,
                        121U + static_cast<std::uint32_t>(index) * 3U);
                    message_font->draw(entry.line1,
                                       static_cast<int>(entry.label_x),
                                       static_cast<int>(entry.label_y), ink,
                                       TextAlign::Center);
                    message_font->draw(entry.line2,
                                       static_cast<int>(entry.label_x),
                                       static_cast<int>(entry.label_y) + 18,
                                       ink, TextAlign::Center);
                }
            } else {
                // ---- "Gunstar Turret Control" attract tutorial ----------
                // A live demo (the Gunstar with
                // targets orbiting it) overlaid by text cards whose frame
                // ranges, positions and shimmering random ink match the
                // reference captures exactly.
                const std::uint32_t tutorial_base =
                    kEmblemMs + 4U * kShowcaseMs;
                const int tf = static_cast<int>(
                    ((age - tutorial_base) * 30U) / 1000U);
                // NOT drawn: an earlier revision put the Gunstar at the bottom
                // of frame here on the reasoning that the tutorial shares the
                // origin camera. Measured against 22 captured tutorial frames
                // that is wrong - the reference capture's lower centre holds a
                // median of 17 light-grey pixels (i.e. stars) where the
                // earlier revision's held 17,393. The tutorial shows the
                // instruments over the starfield, with no ship in view.
                float tracked_x = 400.0F;
                float tracked_y = 200.0F;
                for (int drift = 0; drift < 4; ++drift) {
                    const float phase =
                        static_cast<float>(tf) * (0.012F + 0.004F *
                                                  static_cast<float>(drift)) +
                        static_cast<float>(drift) * 1.7F;
                    const float ox =
                        400.0F + std::sin(phase) * (260.0F +
                                                    40.0F *
                                                    static_cast<float>(drift));
                    const float oy =
                        285.0F + std::cos(phase * 0.7F) * (90.0F +
                                                           18.0F *
                                                           static_cast<float>(
                                                               drift));
                    const float size = 20.0F + 9.0F * static_cast<float>(
                                                             (drift * 3) % 4);
                    render_3ds_model(renderer, light_target, ox, oy, size,
                                     phase, std::sin(phase) * 0.4F);
                    if (drift == 0) {
                        // The demo turret tracks the first target: the
                        // reticle chases its screen position and the lock
                        // ring closes when it lands.
                        tracked_x = ox;
                        tracked_y = oy;
                    }
                }
                const SDL_Rect track_reticle{
                    static_cast<int>(tracked_x) - 50,
                    static_cast<int>(tracked_y) - 50, 100, 100};
                SDL_RenderCopy(renderer, reticle, nullptr, &track_reticle);
                // The weapon-switch model, shown only for the right-hand
                // weapon-toggle card. It flies through Z while
                // rotating all three axes; preserving that changing attitude
                // is the visible part of the demonstration in screen space.
                if (tf >= 861 && tf <= 930) {
                    const float switch_age = static_cast<float>(tf - 861);
                    render_3ds_model(
                        renderer, weapon_switch_model, 400.0F, 470.0F,
                        72.0F + switch_age * 0.45F,
                        switch_age * 0.25F, switch_age * 0.087F, 255,
                        switch_age * 0.087F, true);
                }
                // The four gauge quads live at Z=+5 in front of the origin
                // camera, so they are visible during the
                // attract tutorial - the cards point straight at them. The
                // outer hud bitmap is a gameplay-state paste and is not
                // drawn here, exactly as the game stages it.
                // The attract tutorial draws the SAME instruments as the
                // gameplay HUD, at the same places - the reference
                // captures show the two real scanner meshes at their HUD
                // positions plus the TARGET TEMP gauge, over the starfield,
                // with no outer chrome and no compass. An earlier revision
                // used to draw a hand-made skeleton here and 250x250 gauge
                // quads.
                // NOT drawn either: sharing the origin camera does not make
                // the gauge quads visible during the
                // tutorial. 22 captured tutorial frames
                // contain a median of 16 red pixels in the arc band (max
                // 1079, which is the TARGET TEMP ring) against ~21,000 on the
                // attract HUD screen - the weapon gauges are NOT drawn here.
                {
                    // The demo turret tracks the first target; both scanner
                    // meshes swing with it exactly as they do in play.
                    constexpr float kDeg = kPi / 180.0F;
                    constexpr std::uint8_t kGhostAlpha = 255;
                    const auto scanner_px = [](const Model3ds& m) {
                        return kScannerRadius * (m.radius / 30.0F);
                    };
                    const float tyaw =
                        std::clamp((tracked_x - 400.0F) / 300.0F, -1.0F,
                                   1.0F);
                    const float tpitch =
                        std::clamp((tracked_y - 285.0F) / 250.0F, -1.0F,
                                   1.0F);
                    render_3ds_model(renderer, left_scanner_model,
                                     kLeftScannerX, kLeftScannerY,
                                     scanner_px(left_scanner_model),
                                     335.0F * kDeg - tyaw,
                                     345.0F * kDeg + tpitch, kGhostAlpha,
                                     5.0F * kDeg, true);
                    render_3ds_model(renderer, right_scanner_model,
                                     kRightScannerX, kRightScannerY,
                                     scanner_px(right_scanner_model),
                                     30.0F * kDeg - tyaw,
                                     340.0F * kDeg + tpitch, kGhostAlpha,
                                     355.0F * kDeg, true);
                    // the tracked target's bearing, as a yellow dot on the
                    // right globe
                    const float dx = tracked_x - 400.0F;
                    const float dy = tracked_y - 300.0F;
                    if (dx * dx + dy * dy >= 1.0F) {
                        SDL_SetRenderDrawColor(renderer, 235, 225, 0, 255);
                        const float angle = std::atan2(dy, dx);
                        const SDL_Rect dot{
                            static_cast<int>(kRightScannerX +
                                             std::cos(angle) * 70.0F) - 2,
                            static_cast<int>(kRightScannerY +
                                             std::sin(angle) * 70.0F) - 2,
                            5, 5};
                        SDL_RenderFillRect(renderer, &dot);
                    }
                    // TARGET TEMP sweeps while its card explains it
                    // (frames 1061-1090). The real entemp gauge
                    // is shown here, not death gauges 2.bmp with a HULL TEMP
                    // caption.
                    const float heat =
                        0.5F + 0.5F * std::sin(spin * 1.1F);
                    const std::size_t stage =
                        3U - static_cast<std::size_t>(
                                 std::clamp(heat, 0.0F, 1.0F) * 3.0F);
                    const SDL_Rect gauge_rect{
                        static_cast<int>(kTargetTempX) - kTempGaugePx / 2,
                        static_cast<int>(kLowerScannerY) - kTempGaugePx / 2,
                        kTempGaugePx, kTempGaugePx};
                    SDL_RenderCopy(renderer, target_temp_frames[stage],
                                   nullptr, &gauge_rect);
                }
                // The ink colour is re-randomized every frame, so
                // all tutorial text shimmers.
                const SDL_Color shimmer = random_ink(tf, 141U);
                struct TutorialLine {
                    int begin;
                    int end;
                    bool title;
                    const char* text;
                    float x;
                    float y;
                };
                const TutorialLine lines[] = {
                    {10, 90, true, "Gunstar", 0.5F, 0.3333F},
                    {10, 90, true, "Turret", 0.5F, 0.4666F},
                    {10, 90, true, "Control", 0.5F, 0.6F},
                    {180, 360, false, "Left scanner", 0.15F, 0.3833F},
                    {180, 360, false, "shows ship", 0.15F, 0.4333F},
                    {180, 360, false, "orientation", 0.15F, 0.4833F},
                    {180, 360, false, "relative to turret", 0.15F, 0.5333F},
                    {180, 360, false, "Right shows", 0.875F, 0.3833F},
                    {180, 360, false, "orientation", 0.875F, 0.4333F},
                    {180, 360, false, "relative to", 0.875F, 0.4833F},
                    {180, 360, false, "objects in space", 0.875F, 0.5333F},
                    {360, 500, false, "Scanner tracks", 0.875F, 0.3833F},
                    {360, 500, false, "targets as they", 0.875F, 0.4333F},
                    {360, 500, false, "orbit the ship", 0.875F, 0.4833F},
                    {540, 650, false, "Tracking controls", 0.5F, 0.7F},
                    {540, 650, false, "Turret movement:", 0.5F, 0.7666F},
                    {540, 650, false, "left, right, up, and down", 0.5F, 0.8333F},
                    {660, 760, false, "Targeting controls", 0.5F, 0.7F},
                    {660, 760, false, "Target reticle movement:", 0.5F, 0.7666F},
                    {660, 760, false, "left, right, up, and down", 0.5F, 0.8333F},
                    {760, 861, false, "Analog guages", 0.5F, 0.45F},
                    {760, 861, false, "show weapon", 0.5F, 0.5F},
                    {760, 861, false, "charge", 0.5F, 0.55F},
                    {760, 861, false, "Laser           Laser", 0.5F, 0.35F},
                    {760, 861, false, "Photon         Particle", 0.5F, 0.5833F},
                    {760, 861, false, "Bolts            Beam", 0.5F, 0.61666F},
                    {861, 910, false, "Right-sided", 0.5F, 0.2833F},
                    {861, 910, false, "toggles select", 0.5F, 0.3333F},
                    {861, 910, false, "current weapon", 0.5F, 0.3833F},
                    {920, 980, false, "Either", 0.5F, 0.45F},
                    {920, 980, false, "controller", 0.5F, 0.5F},
                    {920, 980, false, "button fires", 0.5F, 0.55F},
                    {1061, 1090, false, "Lower right", 0.5F, 0.45F},
                    {1061, 1090, false, "scanner shows", 0.5F, 0.5F},
                    {1061, 1090, false, "damage to target", 0.5F, 0.55F},
                    {1160, 1230, false, "Missiles lock-on", 0.5F, 0.45F},
                    {1160, 1230, false, "to target if in", 0.5F, 0.5F},
                    {1160, 1230, false, "reticle when fired", 0.5F, 0.55F},
                    {1250, 1310, false, "Lock-on warning", 0.5F, 0.45F},
                    {1250, 1310, false, "detects enemy missile", 0.5F, 0.5F},
                    {1250, 1310, false, "locked on to gunstar", 0.5F, 0.55F},
                    {1330, 1390, false, "Gunstar damage is", 0.5F, 0.45F},
                    {1330, 1390, false, "shown in left upper", 0.5F, 0.5F},
                    {1330, 1390, false, "and lower gauges", 0.5F, 0.55F},
                    {1400, 1460, false, "Partial Gunstar repair", 0.5F, 0.45F},
                    {1400, 1460, false, "and weapons recharge", 0.5F, 0.5F},
                    {1400, 1460, false, "occurs between levels", 0.5F, 0.55F},
                    {1460, 1520, false, "Depress All weapon", 0.5F, 0.4F},
                    {1460, 1520, false, "toggles at once", 0.5F, 0.45F},
                    {1460, 1520, false, "to engage", 0.5F, 0.5F},
                    {1460, 1520, false, "Death Blossom", 0.5F, 0.55F},
                    {1520, 4000, false, "Death Blossom", 0.5F, 0.4F},
                    {1520, 4000, false, "theoretically restores", 0.5F, 0.45F},
                    {1520, 4000, false, "all systems to full", 0.5F, 0.5F},
                    {1520, 4000, false, "One DB per level.", 0.5F, 0.55F},
                };
                for (const TutorialLine& line : lines) {
                    if (tf < line.begin || tf >= line.end) {
                        continue;
                    }
                    const int px = static_cast<int>(line.x * 800.0F);
                    const int py = static_cast<int>(line.y * 600.0F);
                    // Text PRINTs left-aligned from the cursor, but
                    // the right-hand column (x = 0.875 * width) runs off the
                    // 800-pixel screen; right-align those cards so every
                    // line stays readable.
                    const TextAlign align =
                        line.x >= 0.8F ? TextAlign::Right : TextAlign::Left;
                    const int draw_x = line.x >= 0.8F ? 780 : px;
                    if (line.title) {
                        intro_font->draw(line.text, draw_x, py, shimmer,
                                         align);
                    } else {
                        message_font->draw(line.text, draw_x, py, shimmer,
                                           align);
                    }
                }
            }
            // The prompt counter wraps at 100 and the prompt is
            // drawn only while it is below 51. During that on half, each
            // channel is a fresh RND(255) every 30 Hz tick. Thus this is a
            // true blink combined with colour flicker, not a slow bright
            // colour cycle.
            const bool in_recruitment_flyby =
                this_attract_age >=
                    kHudScreenMs + kCentauriMs + kLogoMs &&
                this_attract_age <
                    kHudScreenMs + kCentauriMs + kLogoMs + kPitchMs;
            if (!in_recruitment_flyby && original_tick % 100U < 51U) {
                const SDL_Color prompt_ink = random_ink(original_tick, 31U);
                message_font->draw(credits >= coin_price_start
                                       ? "PRESS FIRE TO START"
                                       : "INSERT COIN",
                                   400, 570, prompt_ink, TextAlign::Center);
            }
            // No credits line is shown while the ledger is empty;
            // every captured attract frame carries "INSERT COIN" alone.
            if (credits > 0) {
                hud_font->draw("CREDITS " + std::to_string(credits), 400, 548,
                               prompt_color, TextAlign::Center);
            }
        } else if (scene == Scene::Intro) {
            const std::uint32_t age =
                frame_start - combat.result_started_at + intro_preview_ms;
            constexpr float kPi = 3.14159265358979323846F;
            const float degrees = kPi / 180.0F;
            // The intro counter starts at -30 and the frames
            // run -30 through 433 at the shipped 30 Hz sync rate.
            // The counter is additionally pinned at 245 until the greeting
            // sample finishes; the shipped clip ends before that frame is
            // reached, so the hold is a no-op at the release's own timing.
            const int frame =
                static_cast<int>((static_cast<std::uint64_t>(age) * 30U) /
                                 1000U) -
                30;
            const auto fframe = static_cast<float>(frame);

            // ---- camera -----------------------------------------------------
            ModelCamera camera3d;
            camera3d.x = 0.0F;
            if (frame < 176) {
                camera3d.y = frame < 100 ? fframe / 2.0F : 50.0F;
                camera3d.z = fframe * 2.0F - 250.0F;
            } else {
                // Nothing repositions the camera between 176 and 250, so it
                // holds frame 175's value, which is exactly the (0,50,100)
                // the launch branch below resumes from.
                camera3d.y = 50.0F;
                camera3d.z = 100.0F;
            }
            // The Gunstar's height is derived analytically and
            // reused by the trailing camera, so compute it first.
            const float gunstar_y =
                frame > 250 ? 10.0F * (fframe - 250.0F) + 20.0F : -54.0F;
            if (frame > 250 && frame < 350) {
                camera3d.y = frame < 270
                                 ? 10.0F * (fframe - 250.0F) +
                                       2.0F * (275.0F - fframe)
                                 : 10.0F * (fframe - 250.0F) + 10.0F;
                camera3d.z = 100.0F;
            } else if (frame > 349) {
                camera3d.y = gunstar_y - 10.0F + (fframe - 350.0F) * 1.6F;
                camera3d.z = 100.0F + (fframe - 350.0F) * 2.7F;
            }
            // The camera pitch is X rotation, not Y: the camera
            // starts pitched 13 degrees down onto the pad,
            // levels off exactly at the launch frame and then tilts up to
            // follow the Gunstar. Fitting the preserved
            // screenshot's ridge silhouette and plaza horizon against the
            // shipped bkgd.3ds confirms pitch: a yaw of any size would break
            // the shot's left-to-right symmetry, and pitch reproduces both
            // boundaries to within a few pixels.
            float camera_pitch = 13.0F;
            if (frame > 245 && frame < 250) {
                camera_pitch = (250.0F - fframe) * 3.0F;
            } else if (frame >= 250 && frame < 266) {
                camera_pitch = 250.0F - fframe;
            } else if (frame >= 266) {
                camera_pitch = -15.0F;
            }
            camera3d.rotation_x = camera_pitch * degrees;

            // ---- backdrop -------------------------------------------------
            SDL_Color backdrop{100, 100, 150, 255};
            if (frame > 300 && frame < 432) {
                const int fade = std::min(frame - 300, 100);
                backdrop.r = static_cast<std::uint8_t>(100 - fade);
                backdrop.g = static_cast<std::uint8_t>(100 - fade);
                backdrop.b = static_cast<std::uint8_t>(
                    std::max(0, 150 - static_cast<int>(1.5F * fade)));
            } else if (frame >= 432) {
                backdrop = {0, 0, 0, 255};
            }
            SDL_SetRenderDrawColor(renderer, backdrop.r, backdrop.g,
                                   backdrop.b, 255);
            SDL_RenderClear(renderer);

            // ---- starfield plane ------------------------------------------
            // A 1150x800 plane textured with images/starfield2.bmp, hidden
            // until frame 305 and then parked 160 above the climbing Gunstar.
            if (frame >= 305 && frame < 422) {
                ModelWorldTransform plane;
                plane.x = 0.0F;
                plane.y = frame > 300 ? gunstar_y + 160.0F : -200.0F;
                plane.z = frame > 300 ? 700.0F : 400.0F;
                plane.rotation_x = 350.0F * degrees;
                draw_textured_plane(renderer, stars, 1150.0F, 800.0F, plane,
                                    camera3d, -0.05F);
            }

            intro_raster->clear(SDL_Color{0, 0, 0, 0});

            // ---- bkgd.3ds mountains ----------------------------------------
            // Drawn unlit, flat, at its own material colours, and hidden
            // from frame 400.
            if (frame < 400) {
                ModelWorldTransform background;
                background.x = 0.0F;
                background.y = -200.0F;
                // The backdrop RECEDES at 1.5 per frame while the camera
                // advances at 2. It therefore stays roughly 600 units ahead
                // throughout and never comes between the camera and the pad.
                // Reading the motion
                // as a subtract drove the camera into the ridge at frame 176
                // and hid the Gunstar behind it.
                background.z =
                    frame < 176 ? 400.0F + 1.5F * fframe : 400.0F + 1.5F * 175.0F;
                background.scale_x = background.scale_y = background.scale_z =
                    10.0F;
                background.rotation_x =
                    (frame > 300 ? 345.0F : 10.0F) * degrees;
                WorldMaterial unlit;
                unlit.lit = false;
                render_world_model(*intro_raster, rylos_background_model,
                                   background, camera3d, unlit);
            }

            // ---- landing5.3ds plaza ----------------------------------------
            const ModelWorldTransform landing{0.0F, -90.0F, 100.0F,
                                              12.0F, 1.0F, 11.0F};
            WorldMaterial landing_material;
            landing_material.override_color = true;
            landing_material.color = kLandingTexture;
            render_world_model(*intro_raster, landing_model, landing, camera3d,
                               landing_material);

            // ---- pad.3ds ----------------------------------------------------
            const ModelWorldTransform pad{0.0F, -53.0F, 300.0F,
                                          0.4F, 0.4F, 0.4F};
            WorldMaterial pad_material;
            pad_material.override_color = true;
            pad_material.color = kPadTexture;
            render_world_model(*intro_raster, launch_pad_model, pad, camera3d,
                               pad_material);

            // ---- Gunstar and flames ------------------------------------------
            ModelWorldTransform gunstar;
            gunstar.x = 0.0F;
            gunstar.y = gunstar_y;
            gunstar.z = 300.0F;
            gunstar.scale_x = gunstar.scale_y = gunstar.scale_z = 0.3F;
            gunstar.rotation_x = 270.0F * degrees;
            gunstar.rotation_z = 180.0F * degrees;
            // The ship spins two degrees per frame, but only
            // once the launch begins; before that it stands
            // still on the pad at its preload attitude.
            gunstar.rotation_y =
                frame > 250
                    ? std::fmod(2.0F * (fframe - 250.0F), 360.0F) * degrees
                    : 0.0F;

            if (frame < 422) {
                if (frame > 250) {
                    // The short and long flame mesh
                    // alternate every frame.
                    const Model3ds& flame = ((frame - 251) & 1) == 0
                                                ? gunstar_flame_short
                                                : gunstar_flame_long;
                    // flames.X/flamel.X each hold four flame meshes, one per
                    // engine, placed by their Frame chain. Their material is
                    // a pale exhaust white, RGB(224,255,222) at 54 per cent,
                    // so nothing here invents a colour.
                    WorldMaterial flame_material;
                    flame_material.lit = false;
                    flame_material.alpha = 138;
                    flame_material.cull_backfaces = true;
                    render_world_model(*intro_raster, flame, gunstar, camera3d,
                                       flame_material);
                }
                // gunstar3.x carries two materials: "Legs" at flat grey
                // 0.502 and "Gunstar" at white modulated by gunstar.bmp. Both
                // now come from the file, so nothing is overridden here.
                WorldMaterial ship;
                ship.texture = &gunstar_texture;
                render_world_model(*intro_raster, gunstar_model, gunstar,
                                   camera3d, ship);
            }

            intro_raster->present(renderer);

            // ---- scrolling recruitment line ----------------------------------
            // One sentence, drawn twice per frame: first in the backdrop's
            // own colour, then sixteen units further left in green. The
            // leading copy erases the previous frame's trailing edge, which
            // is why its colour tracks the backdrop rather than the text.
            // x scales with the selected display width; y and the text size
            // scale with the display height and width respectively.
            constexpr const char* kRecruitment =
                "You have been recruited by the star league to defend the "
                "frontier against xur and the ko-dan armada.";
            if (frame >= 1) {
                const int scroll = 800 - 16 * frame;
                const int text_y = static_cast<int>(0.05F * 600.0F);
                intro_font->draw(kRecruitment, scroll, text_y,
                                 SDL_Color{100, 100, 160, 255},
                                 TextAlign::Left);
                intro_font->draw(kRecruitment, scroll - 16, text_y,
                                 SDL_Color{80, 100, 80, 255}, TextAlign::Left);
            }
        } else if (scene == Scene::Briefing || scene == Scene::Playing) {
            const bool briefing = scene == Scene::Briefing;
            const std::uint32_t briefing_frame =
                briefing ? (frame_start - combat.result_started_at) * 30U /
                                1000U
                         : 200U;
            if (!briefing && combat.campaign.level_id() == 1) {
                // Practice targets were rendered from their world orbits
                // into the shared depth buffer above.
            } else if (!briefing) {
                const float view_x = horizontal_view_offset(combat.turret_yaw);
                const float view_y = vertical_view_offset(combat.turret_pitch);
                // Live enemies were already drawn into the shared world
                // depth buffer with the star sphere above. Projectiles keep
                // their screen-space homing path for now.
                for (const CampaignProjectile& projectile :
                     combat.campaign.projectiles()) {
                    if (!projectile.active || projectile.world_motion) {
                        continue;
                    }
                    const Model3ds& projectile_model =
                        projectile.source_kind == EnemyKind::Scout
                            ? scout_missile_model
                            : projectile.model_variant == 0U
                                  ? missile_model
                                  : missile_model_alt;
                    render_3ds_model(
                        renderer, projectile_model, projectile.x + view_x,
                        projectile.y + view_y, projectile.radius * 0.6F,
                        3.14159265358979323846F, 0.0F);
                }
            }
            if (frame_start < combat.explosion_until) {
                const int age =
                    360 - static_cast<int>(combat.explosion_until - frame_start);
                const std::size_t frame = std::min<std::size_t>(
                    6U, static_cast<std::size_t>(age) * 7U / 360U);
                const int size = 105 + age / 3;
                const SDL_Rect burst{
                    static_cast<int>(combat.explosion_x) - size / 2,
                    static_cast<int>(combat.explosion_y) - size / 2, size,
                    size};
                SDL_RenderCopy(renderer,
                               explosion_sets[combat.explosion_set][frame],
                               nullptr, &burst);
            }
            if (frame_start < combat.command_explosion_until) {
                const std::uint32_t age =
                    20U * 1000U / 30U -
                    (combat.command_explosion_until - frame_start);
                const std::size_t command_frame = std::min<std::size_t>(
                    19U, static_cast<std::size_t>(age) * 30U / 1000U);
                const SDL_Rect burst{
                    static_cast<int>(combat.command_explosion_x) - 200,
                    static_cast<int>(combat.command_explosion_y) - 200,
                    400, 400};
                SDL_RenderCopy(renderer,
                               command_explosion_frames[command_frame],
                               nullptr, &burst);
            }

            if (frame_start < combat.laser_until) {
                SDL_Color beam{255, 25, 25, 255};
                if (combat.weapon == Weapon::ParticleBeam) {
                    beam = {255, 255, 40, 255};
                } else if (combat.weapon == Weapon::PhotonBolts) {
                    beam = {80, 130, 255, 255};
                }
                if (combat.weapon == Weapon::ParticleBeam) {
                    const std::size_t particle_frame =
                        ((95U - (combat.laser_until - frame_start)) * 30U /
                         1000U) % particle_frames.size();
                    for (const int gun_x : {265, 535}) {
                        const float dx = combat.reticle_x - gun_x;
                        const float dy = combat.reticle_y - 585.0F;
                        const int length = static_cast<int>(std::hypot(dx, dy));
                        const SDL_Rect beam_rect{
                            static_cast<int>((gun_x + combat.reticle_x) *
                                             0.5F) - 25,
                            static_cast<int>((585.0F + combat.reticle_y) *
                                             0.5F) - length / 2,
                            50, length};
                        const double angle =
                            std::atan2(static_cast<double>(dy),
                                       static_cast<double>(dx)) *
                                180.0 / 3.14159265358979323846 +
                            90.0;
                        SDL_RenderCopyEx(renderer,
                                         particle_frames[particle_frame],
                                         nullptr, &beam_rect, angle, nullptr,
                                         SDL_FLIP_NONE);
                    }
                } else {
                    SDL_SetRenderDrawColor(renderer, beam.r, beam.g, beam.b,
                                           beam.a);
                    SDL_RenderDrawLine(renderer, 265, 585,
                                       static_cast<int>(combat.reticle_x),
                                       static_cast<int>(combat.reticle_y));
                    SDL_RenderDrawLine(renderer, 535, 585,
                                       static_cast<int>(combat.reticle_x),
                                       static_cast<int>(combat.reticle_y));
                }
            }

            // Measured against the reference captures: the HUD is drawn at
            // 720x540 centred, not at the bitmap's native 760x570. An earlier
            // revision was 5.6 per cent oversized, which is part of why its
            // gauge arcs read too large.
            const bool hud_outer_visible =
                briefing && briefing_frame < 110U
                    ? true
                    : combat.hud_outer_visible;
            SDL_RenderCopy(renderer, hud_outer_visible ? hud : hud_inner,
                           nullptr, &kHudRect);

            const auto gauge_index = [](float value) {
                return static_cast<std::size_t>(std::clamp(
                    static_cast<int>(std::lround(value)), 0, 99));
            };
            // The gauge quads are 3D objects at X=+/-1.26, Y=+/-1.005,
            // Z=5 in front of the origin camera - with the
            // 500px focal length they project to centers (274/526, 199/401),
            // flanking the central targeting circle. The bmp's corner
            // circles are the four spheres, not the gauge housings.
            // The gauge sizes come straight from the placement data, not a
            // fit: each gauge is a
            //   1.91 x 1.98 plane
            // positioned at z=5 in front of the origin camera:
            //   13 (-1.25,  1.005, 5)   14 ( 1.27,  1.005, 5)
            //   15 (-1.25, -0.99,  5)   16 ( 1.27, -0.99,  5)
            // At the 500 px focal length (800x600 with the default
            // 61.93-degree vertical FOV) one world unit is 100 px, so the
            // quads are 191x198 px centred at (275,199.5), (527,199.5),
            // (275,399) and (527,399).
            //
            // An earlier revision drew them 237x237, which put the arc mass
            // at radius 190-210 from screen centre where the reference's sits
            // at 180-200 - the arcs read too large and too thick.
            const SDL_Rect upper_left_gauge{180, 100, 191, 198};
            const SDL_Rect upper_right_gauge{432, 100, 191, 198};
            const SDL_Rect lower_left_gauge{180, 300, 191, 198};
            const SDL_Rect lower_right_gauge{432, 300, 191, 198};
            SDL_RenderCopy(renderer,
                           gauge_frames[gauge_index(
                               combat.campaign.laser_energy())],
                           nullptr, &upper_left_gauge);
            SDL_RenderCopyEx(renderer,
                             gauge_frames[gauge_index(
                                 combat.campaign.laser_energy())],
                             nullptr, &upper_right_gauge, 0.0, nullptr,
                             SDL_FLIP_HORIZONTAL);
            SDL_RenderCopyEx(
                renderer,
                gauge_frames[gauge_index(
                    static_cast<float>(combat.campaign.photon_bolts()) * 5.0F)],
                nullptr, &lower_left_gauge, 0.0, nullptr, SDL_FLIP_VERTICAL);
            SDL_RenderCopyEx(renderer,
                             gauge_frames[gauge_index(
                                 combat.campaign.particle_energy())],
                             nullptr, &lower_right_gauge, 0.0, nullptr,
                             static_cast<SDL_RendererFlip>(
                                 SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

            // ---- HUD instruments -----------------------------------------
            const float view_x = horizontal_view_offset(combat.turret_yaw);
            const float view_y = vertical_view_offset(combat.turret_pitch);
            const std::size_t enemy_slots = combat.campaign.enemy_slots();
            const std::array<CampaignEnemy, Campaign::kMaxEnemies>& enemies =
                combat.campaign.enemies();
            // scanner.bmp carries the scanner draw colour RGB(197,197,0).
            const SDL_Color scanner_color{197, 197, 0, 255};
            // Measured against the reference captures: the Gunstar attitude
            // scanner is the UPPER-LEFT
            // sphere, not the upper-right, and it is the shipped mesh
            // "L sphere fin 4.3ds" - a green cage with the Gunstar (mesh07,
            // material mat1 = yellow) inside it and red attitude bars. An
            // earlier revision drew a hand-made skeleton at (657,138), the
            // wrong corner, with no globe at all. The attract
            // tutorial states the split: "LEFT SCANNER SHOWS SHIP ORIENTATION
            // RELATIVE TO TURRET" / "RIGHT SHOWS ORIENTATION RELATIVE TO
            // OBJECTS IN SPACE".
            // The two scanners are
            // ghosted - which is why they read as translucent cages
            // rather than solid globes - scaled to 3.5/3.7/3.5 per cent, and
            // given base rotations. The left one starts at X=345, Y=335,
            // Z=5; the right one at X=340, Y=30, Z=355.
            constexpr float kDeg = kPi / 180.0F;
            // Ghosting over a black background reads at full
            // brightness in the reference captures - the cage is (0,212,0)
            // solid green, not the washed-out green a 170 alpha gave.
            constexpr std::uint8_t kGhostAlpha = 255;
            // render_3ds_model scales the model's BOUNDING radius to
            // pixel_radius, but the placement data fixes the SPHERE's
            // 30-unit radius at 84.1 px. The two scanner meshes have
            // different bounding radii (the right one carries more
            // protruding geometry), so passing 84.1 to both rendered the
            // right sphere too small.
            const auto scanner_px = [](const Model3ds& m) {
                return kScannerRadius * (m.radius / 30.0F);
            };
            {
                // X rotation is PITCH and Y rotation is YAW: the left
                // scanner carries 345 / 335 / 5. These two were
                // passed the wrong way round before, which tilted both
                // scanners and pushed the right one 26 px too high.
                render_3ds_model(renderer, left_scanner_model, kLeftScannerX,
                                 kLeftScannerY, scanner_px(left_scanner_model),
                                 (335.0F - combat.turret_yaw) * kDeg,
                                 (345.0F + combat.turret_pitch) * kDeg,
                                 kGhostAlpha, 5.0F * kDeg, true);
            }

            // The upper-RIGHT sphere is the object/bearing display: the
            // shipped mesh "R sphere fin 5.3ds", a denser globe, carrying one
            // YELLOW dot per object in space. The reference's dots are
            // yellow and steady, not the blinking white an earlier revision
            // drew.
            {
                const float cx = kRightScannerX;
                const float cy = kRightScannerY;
                // The right scanner carries 340 / 30 / 355 rotations.
                render_3ds_model(renderer, right_scanner_model, cx, cy,
                                 scanner_px(right_scanner_model),
                                 (30.0F - combat.turret_yaw) * kDeg,
                                 (340.0F + combat.turret_pitch) * kDeg,
                                 kGhostAlpha, 355.0F * kDeg, true);
                {
                    SDL_SetRenderDrawColor(renderer, 235, 225, 0, 255);
                    for (std::size_t index = 0; index < enemy_slots;
                         ++index) {
                        const CampaignEnemy& enemy = enemies[index];
                        if (enemy.status != EnemyStatus::Active ||
                            !enemy.render_model) {
                            continue;
                        }
                        const float dx = enemy.world_motion
                                             ? enemy.x - 400.0F
                                             : enemy.x + view_x - 400.0F;
                        const float dy = enemy.world_motion
                                             ? enemy.y - 300.0F
                                             : enemy.y + view_y - 300.0F;
                        if (dx * dx + dy * dy < 1.0F) {
                            continue;
                        }
                        const float angle = std::atan2(dy, dx);
                        const float dot_x =
                            cx + std::cos(angle) * 70.0F;
                        const float dot_y =
                            cy + std::sin(angle) * 70.0F;
                        SDL_Rect dot{static_cast<int>(dot_x) - 2,
                                     static_cast<int>(dot_y) - 2, 5, 5};
                        SDL_RenderFillRect(renderer, &dot);
                    }
                    if (combat.campaign.level_id() == 1) {
                        const std::uint32_t stage = stage_frame(frame_start);
                        for (const PracticeTarget& target :
                             combat.campaign.practice().targets()) {
                            if (target.status != PracticeTargetStatus::Active) {
                                continue;
                            }
                            float target_x{};
                            float target_y{};
                            float target_radius{};
                            target_position(combat, target, light_target, stage, target_x,
                                            target_y, target_radius);
                            const float dx = target_x - 400.0F;
                            const float dy = target_y - 300.0F;
                            if (dx * dx + dy * dy < 1.0F) {
                                continue;
                            }
                            const float angle = std::atan2(dy, dx);
                            const SDL_Rect dot{
                                static_cast<int>(cx + std::cos(angle) * 70.0F) -
                                    2,
                                static_cast<int>(cy + std::sin(angle) * 70.0F) -
                                    2, 5, 5};
                            SDL_RenderFillRect(renderer, &dot);
                        }
                    }
                }
                (void)scanner_color;
            }

            // Lower-right circle: the targeted enemy's damage readout.
            // Measured against the reference captures:
            // this circle is labelled TARGET TEMP, not HULL TEMP - an earlier
            // revision duplicated the left label - and the label is RED and
            // sits INSIDE the circle, not yellow below it. The gauge itself
            // is an arc following the circle's ring.
            if (!briefing || briefing_frame < 151U) {
                const CampaignEnemy* target = nullptr;
                float best = std::numeric_limits<float>::max();
                for (std::size_t index = 0; index < enemy_slots; ++index) {
                    const CampaignEnemy& enemy = enemies[index];
                    if (enemy.status != EnemyStatus::Active) {
                        continue;
                    }
                    const float dx = combat.reticle_x -
                                     (enemy.x + view_x);
                    const float dy = combat.reticle_y -
                                     (enemy.y + view_y);
                    const float hit_radius = enemy.radius + 12.0F;
                    const float distance = dx * dx + dy * dy;
                    if (distance <= hit_radius * hit_radius &&
                        distance < best) {
                        best = distance;
                        target = &enemy;
                    }
                }
                if (target != nullptr) {
                    const float fraction = static_cast<float>(target->hit_points) /
                                           static_cast<float>(
                                               target->max_hit_points);
                    const float heat = 1.0F - std::clamp(fraction, 0.0F, 1.0F);
                    const int frame = static_cast<int>(heat * 9.0F);
                    // entemp below is the real gauge; death gauges 2.bmp is
                    // not part of this circle in any captured frame, so it is
                    // no longer pasted over the label.
                    (void)frame;
                }
                // The shipped entemp gauge. Frame 4 is cool (label only) and
                // frame 1 is the full ring, so the index runs backwards.
                const float heat = target != nullptr
                                       ? 1.0F - std::clamp(
                                             static_cast<float>(
                                                 target->hit_points) /
                                                 static_cast<float>(
                                                     target->max_hit_points),
                                             0.0F, 1.0F)
                                       : 0.0F;
                const std::size_t stage = 3U - static_cast<std::size_t>(
                                                   std::clamp(heat, 0.0F, 1.0F) *
                                                   3.0F);
                const SDL_Rect gauge_rect{
                    static_cast<int>(kTargetTempX) - kTempGaugePx / 2,
                    static_cast<int>(kLowerScannerY) - kTempGaugePx / 2,
                    kTempGaugePx, kTempGaugePx};
                SDL_RenderCopy(renderer, target_temp_frames[stage], nullptr,
                               &gauge_rect);
            }

            // Lower-left circle: the player's hull temperature.
            // The gauge fills an ARC that follows the circle's inner ring
            // from the bottom, not the slim vertical thermometer an earlier
            // revision drew. The label is RED and inside the
            // circle, not yellow below it.
            if (!briefing || briefing_frame < 151U) {
                const float fill = std::clamp(
                    static_cast<float>(std::max(0, combat.campaign.shields())) /
                        static_cast<float>(combat.campaign.max_shields()),
                    0.0F, 1.0F);
                // The shipped ptemp gauge: frame 4 cool, frame 1 full ring
                // with DANGER. Hull heat is the inverse of the shield pool,
                // so a full pool shows the cool frame.
                const float heat = 1.0F - fill;
                const std::size_t stage = 3U - static_cast<std::size_t>(
                                                   std::clamp(heat, 0.0F, 1.0F) *
                                                   3.0F);
                const SDL_Rect gauge_rect{
                    static_cast<int>(kHullTempX) - kTempGaugePx / 2,
                    static_cast<int>(kLowerScannerY) - kTempGaugePx / 2,
                    kTempGaugePx, kTempGaugePx};
                SDL_RenderCopy(renderer, hull_temp_frames[stage], nullptr,
                               &gauge_rect);
            }

            const SDL_Rect reticle_destination{
                static_cast<int>(combat.reticle_x) - 50,
                static_cast<int>(combat.reticle_y) - 50, 100, 100};
            SDL_RenderCopy(renderer, reticle, nullptr, &reticle_destination);

            // Briefing copy is drawn over the live HUD only
             // after frame 150.  A new random RGB color is rolled for it
             // every frame.  The frontier warning is the sole four-line
             // layout and uses its own x/y anchors.
            if (briefing && briefing_frame > 150U) {
                const SDL_Color briefing_color =
                    random_ink(briefing_frame, 41U);
                const LevelInfo& info = combat.campaign.level_info();
                if (combat.campaign.level_id() == 4) {
                    briefing_font->draw(info.line1, 410, 240, briefing_color,
                                       TextAlign::Center);
                    briefing_font->draw(info.line2, 410, 270, briefing_color,
                                       TextAlign::Center);
                    briefing_font->draw(info.line3, 410, 320, briefing_color,
                                       TextAlign::Center);
                    briefing_font->draw(info.line4, 410, 350, briefing_color,
                                       TextAlign::Center);
                } else {
                    briefing_font->draw(info.line1, 400, 250, briefing_color,
                                       TextAlign::Center);
                    briefing_font->draw(info.line2, 400, 300, briefing_color,
                                       TextAlign::Center);
                    briefing_font->draw(info.line3, 400, 350, briefing_color,
                                       TextAlign::Center);
                }
            }

            // The live weapon readouts. The game prints these in red
            // immediately inboard of the arrow glyphs the bitmap pre-draws;
            // an earlier revision drew none of them.
            {
                const auto two_dp = [](float value) {
                    char buffer[16];
                    std::snprintf(buffer, sizeof(buffer), "%.2f",
                                  static_cast<double>(value));
                    return std::string(buffer);
                };
                // anchors measured from the reference captures: the left pair is
                // left-aligned at x=157, the right pair right-aligned at
                // x=648, with tops at y=263 and y=291.
                hud_font->draw(two_dp(combat.campaign.laser_energy() / 100.0F),
                               157, 263, kHudLabelRed, TextAlign::Left);
                // Internal magazine is 0..20; the cabinet readout is
                // magazine * 5, so it displays 100,95,...,0.
                hud_font->draw(std::to_string(combat.campaign.photon_bolts() * 5),
                               157, 291, kHudLabelRed, TextAlign::Left);
                hud_font->draw(two_dp(combat.campaign.laser_energy() / 100.0F),
                               648, 263, kHudLabelRed, TextAlign::Right);
                hud_font->draw(
                    two_dp(combat.campaign.particle_energy() / 100.0F), 648,
                    291, kHudLabelRed, TextAlign::Right);
            }
            // These VALUES are drawn in red; only their labels are
            // green, and those come from the bitmap.
            hud_font->draw(
                           std::to_string(combat.campaign.destroyed_count()),
                           330, 505,
                           kHudLabelRed, TextAlign::Right);
            hud_font->draw(std::to_string(combat.campaign.score()), 400, 505,
                           kHudLabelRed, TextAlign::Center);
            hud_font->draw(std::to_string(combat.campaign.remaining_count()),
                           548, 505,
                           kHudLabelRed, TextAlign::Right);
            if (combat.campaign.photon_locked()) {
                // the shipped 74x37 LOCK ON banner, not drawn text
                const SDL_Rect lock_rect{401 - 34, 110 - 17, 69, 34};
                SDL_RenderCopy(renderer, lock_on_image, nullptr, &lock_rect);
            }

            SDL_SetRenderDrawColor(renderer, 255, 45, 35, 255);
            int weapon_y = 169;
            if (combat.weapon == Weapon::ParticleBeam) {
                weapon_y = 198;
            } else if (combat.weapon == Weapon::PhotonBolts) {
                weapon_y = 184;
            }
            SDL_Rect weapon_lamp{119, weapon_y, 20, 4};
            SDL_RenderFillRect(renderer, &weapon_lamp);
            if (frame_start < combat.death_blossom_until) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 80, 35, 210);
                for (int angle = 0; angle < 360; angle += 15) {
                    const float radians =
                        static_cast<float>(angle) * 0.01745329251994329577F;
                    SDL_RenderDrawLine(
                        renderer, static_cast<int>(combat.reticle_x),
                        static_cast<int>(combat.reticle_y),
                        static_cast<int>(combat.reticle_x +
                                         std::cos(radians) * 520.0F),
                        static_cast<int>(combat.reticle_y +
                                         std::sin(radians) * 520.0F));
                }
            }
            if (frame_start < combat.damage_until) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 75);
                SDL_Rect screen_flash{0, 0, 800, 600};
                SDL_RenderFillRect(renderer, &screen_flash);
            }
            if (frame_start < combat.hit_flash_until) {
                const std::uint32_t remaining =
                    combat.hit_flash_until - frame_start;
                const std::size_t flash_frame = std::min<std::size_t>(
                    3U, (4U * 1000U / 30U - remaining) * 30U / 1000U);
                SDL_RenderCopy(renderer,
                               combat.hit_flash_white
                                   ? flash_white[flash_frame]
                                   : flash_red[flash_frame],
                               nullptr, nullptr);
            }
        } else if (scene == Scene::LevelResult) {
            // All sector-result cards roll RND(255) for each
            // INK channel on every frame. Dark frames are intentional and
            // are part of the conspicuous flashing appearance.
            const std::uint32_t result_tick =
                (frame_start - combat.result_started_at) * sync_rate / 1000U;
            const SDL_Color result_color = random_ink(result_tick, 51U);
            const PracticeResult result = combat.campaign.practice().result();
            if (combat.campaign.outcome() == LevelOutcome::Failed) {
                message_font->draw("GUNSTAR DESTROYED", 400, 230,
                                   result_color, TextAlign::Center);
                if (combat.campaign.level_id() == 4) {
                    message_font->draw("Star League Destroyed by Meteors.",
                                       400, 290, result_color,
                                       TextAlign::Center);
                }
            } else if (combat.campaign.outcome() == LevelOutcome::Victory) {
                message_font->draw("COMMAND SHIP DESTROYED", 400, 230,
                                   result_color, TextAlign::Center);
                message_font->draw("GOOD SHOW STARFIGHTER", 400, 290,
                                   result_color, TextAlign::Center);
            } else if (combat.campaign.level_id() != 1) {
                // Result cards: every sector grades
                // at the shared frame gates with the film's own lines.
                const int result_class = combat.campaign.result_class();
                if (result_class == 0) {
                    message_font->draw("Many targets", 400, 220,
                                       result_color, TextAlign::Center);
                    message_font->draw("escaped", 400, 265, result_color,
                                       TextAlign::Center);
                    message_font->draw("No bonus", 400, 310, result_color,
                                       TextAlign::Center);
                } else if (result_class == 3) {
                    message_font->draw("All targets cleared", 400, 210,
                                       result_color, TextAlign::Center);
                    message_font->draw("Outstanding", 400, 255, result_color,
                                       TextAlign::Center);
                    message_font->draw("Bonus 100,000", 400, 300,
                                       result_color, TextAlign::Center);
                } else if (result_class == 2) {
                    message_font->draw("Sharp Shooting", 400, 210,
                                       result_color, TextAlign::Center);
                    message_font->draw("My Boy", 400, 255, result_color,
                                       TextAlign::Center);
                    message_font->draw("Bonus 50,000", 400, 300,
                                       result_color, TextAlign::Center);
                } else {
                    message_font->draw("Good work", 400, 210, result_color,
                                       TextAlign::Center);
                    message_font->draw("Starfighter", 400, 255, result_color,
                                       TextAlign::Center);
                    message_font->draw("Bonus 25,000", 400, 300,
                                       result_color, TextAlign::Center);
                }
            } else if (result == PracticeResult::RemainingTargets) {
                message_font->draw("Remaining Targets", 400, 220,
                                   result_color, TextAlign::Center);
                message_font->draw("Recalled- Moving", 400, 275,
                                   result_color, TextAlign::Center);
                message_font->draw("to next sector", 400, 315,
                                   result_color, TextAlign::Center);
            } else {
                message_font->draw("All targets cleared", 400, 210,
                                   result_color, TextAlign::Center);
                if (result == PracticeResult::TimeToSpare) {
                    message_font->draw("with time to spare", 400, 250,
                                       result_color, TextAlign::Center);
                    message_font->draw("Bonus 50,000", 400, 310,
                                       result_color, TextAlign::Center);
                } else if (result == PracticeResult::RecordTime) {
                    message_font->draw("in record time", 400, 250,
                                       result_color, TextAlign::Center);
                    message_font->draw("Bonus 100,000", 400, 310,
                                       result_color, TextAlign::Center);
                } else {
                    message_font->draw("Bonus", 400, 260, result_color,
                                       TextAlign::Center);
                    message_font->draw("25,000", 400, 310, result_color,
                                       TextAlign::Center);
                }
            }
            hud_font->draw("SCORE " +
                               std::to_string(combat.campaign.score()),
                           400, 390, result_color, TextAlign::Center);
        } else if (scene == Scene::GameOver) {
            // Both game-over variants use a new full-range random INK every
            // frame.
            const std::uint32_t result_tick =
                (frame_start - combat.result_started_at) * sync_rate / 1000U;
            const SDL_Color result_color = random_ink(result_tick, 61U);
            // The standard game over is
            // "BAD LUCK STARFIGHTER", the frontier failure keeps its own
            // "Star League Destroyed by Meteors." line, a new high score is
            // announced as "RECORD BREAKER!" at 0.5/0.41666 of the screen,
            // and the prompt is "Insert another quarter please".
            message_font->draw("BAD LUCK STARFIGHTER", 400, 200, result_color,
                               TextAlign::Center);
            message_font->draw("SCORE " +
                                   std::to_string(combat.campaign.score()),
                               400, 260, result_color, TextAlign::Center);
            if (entering_initials) {
                std::string shown = initials;
                while (shown.size() < 3) {
                    shown.push_back('_');
                }
                message_font->draw("RECORD BREAKER!", 400, 250, result_color,
                                   TextAlign::Center);
                message_font->draw("ENTER INITIALS", 400,
                                   330, result_color, TextAlign::Center);
                message_font->draw(shown, 400, 385, result_color,
                                   TextAlign::Center);
                hud_font->draw("TYPE LETTERS - BACKSPACE ERASES - ENTER SAVES",
                               400, 450, result_color, TextAlign::Center);
            } else {
                hud_font->draw(credits >= coin_price_continue
                                   ? "PRESS FIRE TO CONTINUE"
                                   : "Insert another quarter please",
                               400, 340, result_color, TextAlign::Center);
            }
        } else if (scene == Scene::Victory) {
            const std::uint32_t result_tick =
                (frame_start - combat.result_started_at) * sync_rate / 1000U;
            const SDL_Color result_color = victory_ink(result_tick);
            message_font->draw("WELCOME TO THE STAR LEAGUE", 400, 190,
                               result_color, TextAlign::Center);
            message_font->draw("COMMAND SHIP DESTROYED", 400, 245,
                               result_color, TextAlign::Center);
            message_font->draw("FINAL SCORE " +
                                   std::to_string(combat.campaign.score()),
                               400, 300, result_color, TextAlign::Center);
            if (entering_initials) {
                // The ink switches back to unrestricted RND(255) for
                // the initials-entry phase after the warm accolade.
                const SDL_Color entry_color = random_ink(result_tick, 71U);
                std::string shown = initials;
                while (shown.size() < 3) {
                    shown.push_back('_');
                }
                message_font->draw("NEW HIGH SCORE - ENTER YOUR INITIALS", 400,
                                   360, entry_color, TextAlign::Center);
                message_font->draw(shown, 400, 410, entry_color,
                                   TextAlign::Center);
                hud_font->draw("TYPE LETTERS - BACKSPACE ERASES - ENTER SAVES",
                               400, 468, entry_color, TextAlign::Center);
            }
        }
        // The high-score table rides along on every result screen.
        if ((scene == Scene::GameOver || scene == Scene::Victory ||
             scene == Scene::LevelResult) &&
            !model_gallery) {
            const SDL_Color table_color{80, 255, 120, 255};
            message_font->draw("HIGH SCORES", 660, 96, table_color,
                               TextAlign::Center);
            char line[32];
            for (std::size_t index = 0; index < HighScoreTable::kEntries;
                 ++index) {
                const auto& entry = high_scores.entries()[index];
                std::snprintf(line, sizeof(line), "%-3s %7d",
                              entry.initials.c_str(), entry.score);
                hud_font->draw(line, 660, 130 + static_cast<int>(index) * 28,
                               table_color, TextAlign::Center);
            }
        }
        if (frames == 0 && !screenshot_path.empty()) {
            SDL_Surface* screenshot = SDL_CreateRGBSurfaceWithFormat(
                0, 800, 600, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!screenshot ||
                SDL_RenderReadPixels(renderer, nullptr, screenshot->format->format,
                                     screenshot->pixels, screenshot->pitch) != 0 ||
                SDL_SaveBMP(screenshot, screenshot_path.string().c_str()) != 0) {
                const std::string error = SDL_GetError();
                SDL_FreeSurface(screenshot);
                throw std::runtime_error("cannot save screenshot: " + error);
            }
            SDL_FreeSurface(screenshot);
        }
        SDL_RenderPresent(renderer);

        ++frames;
        if (smoke_test && frames >= 3) {
            running = false;
        }
        const std::uint32_t elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < frame_ms) {
            SDL_Delay(frame_ms - elapsed);
        }
    }

    // Stop the callback before its SoundEffect storage leaves scope. This is
    // especially important when quitting during the long recruitment clip.
    audio.reset();
    for (const auto& set : explosion_sets) {
        for (SDL_Texture* texture : set) {
            if (texture != nullptr) {
                SDL_DestroyTexture(texture);
            }
        }
    }
    for (SDL_Texture* texture : particle_frames) {
        SDL_DestroyTexture(texture);
    }
    for (SDL_Texture* texture : command_explosion_frames) {
        SDL_DestroyTexture(texture);
    }
    for (SDL_Texture* texture : flash_red) {
        SDL_DestroyTexture(texture);
    }
    for (SDL_Texture* texture : flash_white) {
        SDL_DestroyTexture(texture);
    }
    for (SDL_Texture* texture : gauge_frames) {
        SDL_DestroyTexture(texture);
    }
    message_font.reset();
    briefing_font.reset();
    pitch_title_font.reset();
    pitch_you_font.reset();
    pitch_medium_font.reset();
    pitch_large_font.reset();
    intro_raster.reset();
    intro_font.reset();
    hud_font.reset();
    SDL_DestroyTexture(reticle);
    SDL_DestroyTexture(hud);
    SDL_DestroyTexture(emblem);
    SDL_DestroyTexture(stars);
    for (SDL_Joystick* joystick : joysticks) {
        SDL_JoystickClose(joystick);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return 0;
}

int main(int argc, char** argv) {
#ifdef __HAIKU__
    // On Haiku, the Tracker launches apps with the Tracker's working
    // directory, not the executable's.  SDL2's Haiku driver used to
    // chdir() to the executable's directory during SDL_Init, but our
    // run() tries to find the asset directory before SDL_Init runs.
    // Look for the assets/ directory starting from the executable's
    // directory and walking up the tree (up to 3 levels), setting CWD
    // to the matching directory.  Based on the haikuports cavepacker
    // patch -- chdir() before SDL_Init is required.
    char binpath[PATH_MAX];
    if (realpath(argv[0], binpath) != nullptr) {
        char* dir = dirname(binpath);
        if (dir != nullptr) {
            chdir(dir);
            for (int i = 0; i < 4; ++i, chdir("..")) {
                struct stat st;
                if (stat("assets/Images", &st) == 0 && S_ISDIR(st.st_mode)) {
                    break;
                }
            }
        }
    }
#endif
    fs::path assets = "assets";
    bool smoke_test = false;
    bool fullscreen = false;
    bool cabinet_mode = false;
    bool model_gallery = false;
    bool intro_test = false;
    std::uint32_t intro_preview_ms = 0;
    std::uint32_t attract_preview_ms = 0;
    std::uint32_t briefing_preview_ms = 0;
    bool practice_preview = false;
    bool spanish_voice = false;
    fs::path screenshot_path;
    std::uint32_t boot_preview_ms = 0;
    GameMode game_mode = GameMode::Arcade;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) {
            assets = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "arcade") {
                game_mode = GameMode::Arcade;
            } else if (value == "recruit") {
                game_mode = GameMode::Recruit;
            } else {
                std::cerr << "unknown --mode value: " << value
                          << " (use arcade or recruit)\n";
                return 2;
            }
        } else if (arg == "--smoke-test") {
            smoke_test = true;
        } else if (arg == "--fullscreen") {
            fullscreen = true;
        } else if (arg == "--cabinet") {
            cabinet_mode = true;
            fullscreen = true;
        } else if (arg == "--model-gallery") {
            model_gallery = true;
        } else if (arg == "--intro-test") {
            intro_test = true;
            smoke_test = true;
        } else if (arg == "--boot-preview-ms" && i + 1 < argc) {
            try {
                boot_preview_ms = static_cast<std::uint32_t>(
                    std::stoul(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "invalid --boot-preview-ms value\n";
                return 1;
            }
            smoke_test = true;
        } else if (arg == "--attract-preview-ms" && i + 1 < argc) {
            try {
                attract_preview_ms =
                    static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "invalid --attract-preview-ms value\n";
                return 2;
            }
            smoke_test = true;
        } else if (arg == "--intro-preview-ms" && i + 1 < argc) {
            try {
                intro_preview_ms = static_cast<std::uint32_t>(
                    std::stoul(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "invalid --intro-preview-ms value\n";
                return 2;
            }
            intro_test = true;
            smoke_test = true;
        } else if (arg == "--briefing-preview-ms" && i + 1 < argc) {
            try {
                briefing_preview_ms = static_cast<std::uint32_t>(
                    std::stoul(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "invalid --briefing-preview-ms value\n";
                return 2;
            }
            smoke_test = true;
        } else if (arg == "--practice-preview") {
            practice_preview = true;
            smoke_test = true;
        } else if (arg == "--voice" && i + 1 < argc) {
            const std::string voice = argv[++i];
            if (voice == "en") {
                spanish_voice = false;
            } else if (voice == "es") {
                spanish_voice = true;
            } else {
                std::cerr << "unknown --voice value: " << voice
                          << " (use en or es)\n";
                return 2;
            }
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (arg == "--help") {
            std::cout << "usage: starfighter [--assets DIR] [--fullscreen] "
                         "[--cabinet] [--mode arcade|recruit] [--model-gallery] "
                         "[--voice en|es] "
                         "[--smoke-test] [--intro-test] [--intro-preview-ms N] "
                         "[--briefing-preview-ms N] "
                         "[--practice-preview] "
                         "[--screenshot FILE]\n";
            return 0;
        } else {
            std::cerr << "unknown or incomplete option: " << arg << '\n';
            return 2;
        }
    }

    try {
        return run(fs::absolute(assets), smoke_test, fullscreen, cabinet_mode,
                   model_gallery, intro_test, intro_preview_ms,
                   screenshot_path, game_mode, attract_preview_ms,
                   boot_preview_ms, briefing_preview_ms, practice_preview,
                   spanish_voice);
    } catch (const std::exception& error) {
        std::cerr << "starfighter: " << error.what() << '\n';
        return 1;
    }
}
