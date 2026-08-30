#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ModelVertex {
    float x{};
    float y{};
    float z{};
};

struct ModelFace {
    std::uint16_t a{};
    std::uint16_t b{};
    std::uint16_t c{};
    SDL_Color color{110, 190, 255, 255};
    // Set when the face's .x material carries a TextureFilename.
    bool textured{};
};

struct ModelUV {
    float u{};
    float v{};
};

// A CPU-side copy of a shipped bitmap, sampled by the software rasteriser.
struct TextureImage {
    int width{};
    int height{};
    std::vector<std::uint32_t> pixels; // 0x00RRGGBB
    bool valid() const { return width > 0 && height > 0; }
};

TextureImage load_texture_image(const std::filesystem::path& path);

struct Model3ds {
    std::vector<ModelVertex> vertices;
    std::vector<ModelVertex> normals;
    // Each named 3DS object is kept as a numbered limb. Keep the
    // association so effects such as the 47-piece command-ship breakup can
    // animate the authored pieces independently instead of flattening the
    // model irreversibly at load time.
    std::vector<std::uint16_t> limbs;
    // Parallel to vertices; zero-filled when the source carries no
    // MeshTextureCoords.
    std::vector<ModelUV> uvs;
    std::vector<ModelFace> faces;
    // Texture named by the .x material, e.g. "gunstar.bmp".
    std::string texture_name;
    float radius = 1.0F;
};

struct ModelWorldTransform {
    float x{};
    float y{};
    float z{};
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    float scale_z = 1.0F;
    float rotation_x{};
    float rotation_y{};
    float rotation_z{};
};

struct ModelCamera {
    float x{};
    float y{};
    float z{};
    float rotation_x{};
    float rotation_y{};
    float rotation_z{};
    // Shipped default: radians 3.14 / 2.905.
    float vertical_fov_degrees = 61.9307F;
};

// The intro's landing plaza is two very large triangles that a launch pad and
// a Gunstar stand on top of. Painter sorting orders whole triangles by their
// centroid, so the plaza always sorted in front of the pad and erased it.
// This is a small portable software rasteriser with a real depth buffer; it
// also clips against the near plane instead of discarding straddling faces,
// which the plaza needs as soon as the camera dollies over its near edge.
class DepthRaster {
public:
    struct Vertex {
        float x{};
        float y{};
        // Interpolating 1/z is linear in screen space; larger is nearer.
        float inverse_depth{};
        float r{};
        float g{};
        float b{};
        // Premultiplied by inverse_depth so screen-space interpolation stays
        // perspective correct.
        float u_over_z{};
        float v_over_z{};
    };

    DepthRaster(SDL_Renderer* renderer, int width, int height);
    ~DepthRaster();
    DepthRaster(const DepthRaster&) = delete;
    DepthRaster& operator=(const DepthRaster&) = delete;

    void clear(SDL_Color color);
    void triangle(const Vertex& a, const Vertex& b, const Vertex& c,
                  std::uint8_t alpha, const TextureImage* texture = nullptr);
    void present(SDL_Renderer* renderer);

    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_{};
    int height_{};
    std::vector<std::uint32_t> color_;
    std::vector<float> depth_;
    SDL_Texture* texture_{};
};

// Per-object render state for the intro's 3D objects.
struct WorldMaterial {
    // Object 9 (bkgd.3ds) is loaded with its light flag cleared, so it draws
    // at flat material colour; every other intro object is lit.
    bool lit = true;
    // pad.3ds and landing5.3ds are textured with 5x5 single-colour bitmaps,
    // so their texture is reproduced exactly by one flat colour.
    bool override_color = false;
    SDL_Color color{255, 255, 255, 255};
    std::uint8_t alpha = 255;
    // Applied to faces whose .x material named a texture.
    const TextureImage* texture = nullptr;
    // The launch flames are translucent and authored double-sided: each
    // triangle appears twice with opposite winding, so both cull signs give
    // the same image and culling neither adds nor removes surfaces, it just
    // stops every triangle blending over its own duplicate. Culling is on
    // for the flames.
    bool cull_backfaces = false;
};

Model3ds load_3ds_model(const std::filesystem::path& path);
Model3ds load_3ds_model(const std::filesystem::path& path,
                        const std::vector<std::size_t>& hidden_limbs);
Model3ds load_x_model(const std::filesystem::path& path);

void render_world_model(DepthRaster& raster, const Model3ds& model,
                        const ModelWorldTransform& transform,
                        const ModelCamera& camera,
                        const WorldMaterial& material = {});

// Projects one point of an object's local space to screen pixels. Returns
// false when the point falls behind the near plane.
bool project_world_point(const ModelWorldTransform& transform,
                         const ModelCamera& camera, float local_x,
                         float local_y, float local_z, int width, int height,
                         float& screen_x, float& screen_y);

void render_3ds_model(SDL_Renderer* renderer, const Model3ds& model,
                      float center_x, float center_y, float pixel_radius,
                      float yaw, float pitch, std::uint8_t alpha = 255,
                      float roll = 0.0F, bool perspective = true,
                      SDL_Color tint = {255, 255, 255, 255});
