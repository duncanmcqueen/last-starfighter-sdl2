// Renders the star sphere from two camera yaw angles and checks the field
// visibly moves with the camera instead of blitting a fixed copy.
#include "model_3ds.h"

#include <SDL.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, 800, 600, 32, SDL_PIXELFORMAT_ARGB8888);
    assert(surface != nullptr);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    assert(renderer != nullptr);

    const Model3ds stars = load_3ds_model(
        std::filesystem::path(argv[1]) / "Models" / "3dstarscape.3ds");
    assert(!stars.faces.empty());
    ModelWorldTransform transform;
    transform.scale_x = transform.scale_y = transform.scale_z = 39.0F;
    WorldMaterial material;
    material.lit = false;
    material.cull_backfaces = false;
    DepthRaster raster(renderer, 800, 600);

    const auto render = [&](float camera_yaw) {
        raster.clear({0, 0, 0, 255});
        ModelCamera camera;
        camera.rotation_y = camera_yaw;
        render_world_model(raster, stars, transform, camera, material);
        raster.present(renderer);
        SDL_RenderPresent(renderer);
        std::vector<std::uint32_t> pixels(800U * 600U);
        assert(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                    pixels.data(), 800 * 4) == 0);
        return pixels;
    };

    const std::vector<std::uint32_t> centered = render(0.0F);
    const std::vector<std::uint32_t> turned = render(0.35F);
    std::size_t changed = 0;
    for (std::size_t index = 0; index < centered.size(); ++index) {
        if (centered[index] != turned[index]) {
            ++changed;
        }
    }
    // A fixed world-space sphere viewed from a turned camera must visibly
    // move. The reference field is extremely sparse (the measured render
    // changes 426 pixels at 20 degrees), so keep the floor below that while
    // still rejecting a stationary copy or screen-space blit.
    assert(changed > 300U);

    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    SDL_Quit();
}
