// model_3ds.cpp loads 3DS and DirectX .x geometry and renders them with a
// small software rasteriser (DepthRaster). The 3DS loader applies the Y/Z axis
// exchange and winding reversal so models authored in 3ds Max's right-handed
// Z-up space render correctly in the left-handed Y-up world used here.
// Materials, per-object transforms, and named limbs are preserved so effects
// like the command-ship breakup can animate the authored pieces independently.
#include "model_3ds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint16_t kMain = 0x4d4d;
constexpr std::uint16_t kEditor = 0x3d3d;
constexpr std::uint16_t kMaterial = 0xafff;
constexpr std::uint16_t kMaterialName = 0xa000;
constexpr std::uint16_t kDiffuse = 0xa020;
constexpr std::uint16_t kColorFloat = 0x0010;
constexpr std::uint16_t kColorByte = 0x0011;
constexpr std::uint16_t kObject = 0x4000;
constexpr std::uint16_t kTriangleMesh = 0x4100;
constexpr std::uint16_t kVertices = 0x4110;
constexpr std::uint16_t kFaces = 0x4120;
constexpr std::uint16_t kFaceMaterial = 0x4130;

struct Chunk {
    std::uint16_t id{};
    std::size_t begin{};
    std::size_t data_begin{};
    std::size_t end{};
};

struct PendingFace {
    std::uint16_t a{};
    std::uint16_t b{};
    std::uint16_t c{};
    std::string material;
};

std::uint16_t read_u16(const std::vector<std::uint8_t>& data, std::size_t at) {
    if (at + 2 > data.size()) {
        throw std::runtime_error("truncated 3DS u16");
    }
    return static_cast<std::uint16_t>(data[at]) |
           static_cast<std::uint16_t>(data[at + 1] << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t at) {
    if (at + 4 > data.size()) {
        throw std::runtime_error("truncated 3DS u32");
    }
    return static_cast<std::uint32_t>(data[at]) |
           (static_cast<std::uint32_t>(data[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(data[at + 2]) << 16U) |
           (static_cast<std::uint32_t>(data[at + 3]) << 24U);
}

float read_f32(const std::vector<std::uint8_t>& data, std::size_t at) {
    const std::uint32_t bits = read_u32(data, at);
    float value{};
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Chunk read_chunk(const std::vector<std::uint8_t>& data, std::size_t at,
                 std::size_t parent_end) {
    if (at + 6 > parent_end || parent_end > data.size()) {
        throw std::runtime_error("truncated 3DS chunk header");
    }
    const std::uint32_t size = read_u32(data, at + 2);
    if (size < 6 || size > parent_end - at) {
        throw std::runtime_error("invalid 3DS chunk length");
    }
    return {read_u16(data, at), at, at + 6, at + size};
}

std::pair<std::string, std::size_t>
read_string(const std::vector<std::uint8_t>& data, std::size_t at,
            std::size_t end) {
    const auto terminator =
        std::find(data.begin() + static_cast<std::ptrdiff_t>(at),
                  data.begin() + static_cast<std::ptrdiff_t>(end), 0);
    if (terminator == data.begin() + static_cast<std::ptrdiff_t>(end)) {
        throw std::runtime_error("unterminated 3DS string");
    }
    const auto stop = static_cast<std::size_t>(terminator - data.begin());
    return {std::string(reinterpret_cast<const char*>(data.data() + at),
                        stop - at),
            stop + 1};
}

SDL_Color parse_color(const std::vector<std::uint8_t>& data,
                      const Chunk& parent) {
    std::size_t cursor = parent.data_begin;
    while (cursor < parent.end) {
        const Chunk color = read_chunk(data, cursor, parent.end);
        if (color.id == kColorByte && color.data_begin + 3 <= color.end) {
            return {data[color.data_begin], data[color.data_begin + 1],
                    data[color.data_begin + 2], 255};
        }
        if (color.id == kColorFloat && color.data_begin + 12 <= color.end) {
            auto channel = [&](std::size_t offset) {
                const float value = std::clamp(read_f32(data, offset), 0.0F, 1.0F);
                return static_cast<std::uint8_t>(std::lround(value * 255.0F));
            };
            return {channel(color.data_begin), channel(color.data_begin + 4),
                    channel(color.data_begin + 8), 255};
        }
        cursor = color.end;
    }
    return {180, 220, 255, 255};
}

void parse_material(const std::vector<std::uint8_t>& data, const Chunk& material,
                    std::map<std::string, SDL_Color>& materials) {
    std::string name;
    SDL_Color diffuse{180, 220, 255, 255};
    std::size_t cursor = material.data_begin;
    while (cursor < material.end) {
        const Chunk child = read_chunk(data, cursor, material.end);
        if (child.id == kMaterialName) {
            name = read_string(data, child.data_begin, child.end).first;
        } else if (child.id == kDiffuse) {
            diffuse = parse_color(data, child);
        }
        cursor = child.end;
    }
    if (!name.empty()) {
        materials[name] = diffuse;
    }
}

void parse_mesh(const std::vector<std::uint8_t>& data, const Chunk& mesh,
                const std::map<std::string, SDL_Color>& materials,
                Model3ds& model, const ModelVertex& raw_offset,
                std::size_t limb_index) {
    std::vector<ModelVertex> local_vertices;
    std::vector<PendingFace> local_faces;
    std::size_t cursor = mesh.data_begin;
    while (cursor < mesh.end) {
        const Chunk child = read_chunk(data, cursor, mesh.end);
        if (child.id == kVertices) {
            const std::uint16_t count = read_u16(data, child.data_begin);
            std::size_t item = child.data_begin + 2;
            if (item + static_cast<std::size_t>(count) * 12 > child.end) {
                throw std::runtime_error("truncated 3DS vertex list");
            }
            local_vertices.reserve(count);
            for (std::uint16_t index = 0; index < count; ++index, item += 12) {
                local_vertices.push_back(
                    {read_f32(data, item) + raw_offset.x,
                     read_f32(data, item + 4) + raw_offset.y,
                     read_f32(data, item + 8) + raw_offset.z});
            }
        } else if (child.id == kFaces) {
            const std::uint16_t count = read_u16(data, child.data_begin);
            std::size_t item = child.data_begin + 2;
            if (item + static_cast<std::size_t>(count) * 8 > child.end) {
                throw std::runtime_error("truncated 3DS face list");
            }
            local_faces.reserve(count);
            for (std::uint16_t index = 0; index < count; ++index, item += 8) {
                local_faces.push_back({read_u16(data, item),
                                       read_u16(data, item + 2),
                                       read_u16(data, item + 4), {}});
            }
            while (item < child.end) {
                const Chunk face_child = read_chunk(data, item, child.end);
                if (face_child.id == kFaceMaterial) {
                    const auto [name, count_at] =
                        read_string(data, face_child.data_begin, face_child.end);
                    const std::uint16_t face_count = read_u16(data, count_at);
                    std::size_t face_at = count_at + 2;
                    if (face_at + static_cast<std::size_t>(face_count) * 2 >
                        face_child.end) {
                        throw std::runtime_error("truncated 3DS material face list");
                    }
                    for (std::uint16_t n = 0; n < face_count; ++n, face_at += 2) {
                        const std::uint16_t face_index = read_u16(data, face_at);
                        if (face_index >= local_faces.size()) {
                            throw std::runtime_error("invalid 3DS material face index");
                        }
                        local_faces[face_index].material = name;
                    }
                }
                item = face_child.end;
            }
        }
        cursor = child.end;
    }

    if (local_vertices.empty()) {
        return;
    }
    if (model.vertices.size() + local_vertices.size() > 65535) {
        throw std::runtime_error("3DS model exceeds 16-bit renderer indices");
    }
    const auto base = static_cast<std::uint16_t>(model.vertices.size());
    model.vertices.insert(model.vertices.end(), local_vertices.begin(),
                          local_vertices.end());
    model.limbs.insert(model.limbs.end(), local_vertices.size(),
                       static_cast<std::uint16_t>(limb_index));
    for (const PendingFace& face : local_faces) {
        if (face.a >= local_vertices.size() || face.b >= local_vertices.size() ||
            face.c >= local_vertices.size()) {
            throw std::runtime_error("3DS face references an invalid vertex");
        }
        SDL_Color color{180, 220, 255, 255};
        const auto found = materials.find(face.material);
        if (found != materials.end()) {
            color = found->second;
        }
        model.faces.push_back(
            {static_cast<std::uint16_t>(base + face.a),
             static_cast<std::uint16_t>(base + face.b),
             static_cast<std::uint16_t>(base + face.c), color});
    }
}

void parse_object(const std::vector<std::uint8_t>& data, const Chunk& object,
                  const std::map<std::string, SDL_Color>& materials,
                  Model3ds& model, bool seat_scout_pieces,
                  std::size_t limb_index) {
    const auto [name, after_name] =
        read_string(data, object.data_begin, object.end);
    // The Xurian scout is authored as 13 loose pieces with visible gaps:
    // the engine pods (mesh08/mesh10) float 7.7 units behind their pylons
    // and 3.6 above their seats, the rear rails (mesh09/mesh11) and pylon
    // mounts (mesh13/mesh14) hover 1.6-2.1 units above the hull plane,
    // and the cockpit dome (sphere01) hangs below the hull instead of
    // sitting on top like the reference canopy. Snap every piece onto the
    // body so the ship reads as one vessel.
    //
    // CRITICAL: these offsets are keyed on generic mesh names that almost
    // every shipped .3ds reuses, so they must apply ONLY to the scout. They
    // were previously applied to every model, which silently corrupted
    // anything containing a mesh07/08/09/10/11/13/14 or sphere01 - the
    // CENTAURI and STARFIGHTER title meshes (letters jumping off the
    // baseline), both scanner globes, and LOGO.3ds among them.
    ModelVertex raw_offset{0.0F, 0.0F, 0.0F};
    if (!seat_scout_pieces) {
        // no fixups for anything but the scout
    } else if (name == "mesh08") {
        // DERIVED from the file's own 0x4160 local matrices rather than
        // eyeballed. mesh08 and mesh10 are the ONLY objects in the scout with
        // a non-identity local transform: both are rotated 90 degrees about Y
        // with pivots at (+/-14, 33.10, 8) - which is exactly the top of the
        // pylons (mesh09/mesh11 span X +/-11.6..16.4, Y 16..34, Z 5.4..10.6).
        // So the pods are authored to sit ON the pylons; their stored vertices
        // just never moved there. Offsets place each pod's pivot at the
        // matrix origin: X to the pylon centre, Y so the pod meets the pylon
        // top, Z to the pylon's mid-depth. The X term is MIRRORED between the
        // two pods, which the previous single shared offset could not express.
        raw_offset = {8.0F, -16.4F, -14.0F};
    } else if (name == "mesh10") {
        raw_offset = {-8.0F, -16.4F, -14.0F};
    } else if (name == "mesh13" || name == "mesh14") {
        raw_offset = {0.0F, 0.0F, -1.6F};
    } else if (name == "mesh09" || name == "mesh11") {
        raw_offset = {0.0F, 0.0F, -2.1F};
    } else if (name == "sphere01") {
        raw_offset = {0.0F, 0.0F, 10.1F};
    } else if (name == "mesh07") {
        raw_offset = {0.0F, 0.0F, 5.8F};
    }
    std::size_t cursor = after_name;
    while (cursor < object.end) {
        const Chunk child = read_chunk(data, cursor, object.end);
        if (child.id == kTriangleMesh) {
            parse_mesh(data, child, materials, model, raw_offset, limb_index);
        }
        cursor = child.end;
    }
}

void parse_container(const std::vector<std::uint8_t>& data, const Chunk& parent,
                     std::map<std::string, SDL_Color>& materials,
                     Model3ds& model, bool seat_scout_pieces,
                     const std::vector<std::size_t>& hidden_limbs,
                     std::size_t& limb_index) {
    std::size_t cursor = parent.data_begin;
    while (cursor < parent.end) {
        const Chunk child = read_chunk(data, cursor, parent.end);
        if (child.id == kMain || child.id == kEditor) {
            parse_container(data, child, materials, model, seat_scout_pieces,
                            hidden_limbs, limb_index);
        } else if (child.id == kMaterial) {
            parse_material(data, child, materials);
        } else if (child.id == kObject) {
            // Limb numbering starts at zero for the root and counts 3DS
            // objects from one. Preserve that ordering so the HIDE LIMB list
            // can be applied while flattening the file into one render mesh.
            ++limb_index;
            if (std::find(hidden_limbs.begin(), hidden_limbs.end(),
                          limb_index) == hidden_limbs.end()) {
                parse_object(data, child, materials, model,
                             seat_scout_pieces, limb_index);
            }
        }
        cursor = child.end;
    }
}

struct RenderVertex {
    float x{};
    float y{};
    float z{};
};

} // namespace

namespace {

// The release's Gunstar is a DirectX 3 binary .x mesh.  This deliberately
// small reader handles the scalar and list tokens used by that asset and
// ignores normals/texture coordinates.
enum class XTokenKind { Name, Integer, Float, Open, Close, Other, End };

struct XToken {
    XTokenKind kind = XTokenKind::End;
    std::string text;
    std::uint32_t integer{};
    float number{};
};

class XBinaryTokens final {
  public:
    explicit XBinaryTokens(std::vector<std::uint8_t> bytes)
        : data_(std::move(bytes)), cursor_(16) {}

    XToken next() {
        if (!pending_integers_.empty()) {
            const std::uint32_t value = pending_integers_.front();
            pending_integers_.pop_front();
            return {XTokenKind::Integer, {}, value, 0.0F};
        }
        if (!pending_floats_.empty()) {
            const float value = pending_floats_.front();
            pending_floats_.pop_front();
            return {XTokenKind::Float, {}, 0, value};
        }
        if (cursor_ + 2 > data_.size()) {
            return {};
        }
        const std::uint16_t code = take_u16();
        if (code == 1 || code == 2) { // NAME or STRING
            const std::uint32_t length = take_u32();
            require(length);
            std::string value(reinterpret_cast<const char*>(data_.data() + cursor_),
                              length);
            cursor_ += length;
            while (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            return {XTokenKind::Name, std::move(value), 0, 0.0F};
        }
        if (code == 3) {
            return {XTokenKind::Integer, {}, take_u32(), 0.0F};
        }
        if (code == 6) { // INTEGER_LIST
            const std::uint32_t count = take_u32();
            require(static_cast<std::size_t>(count) * 4U);
            for (std::uint32_t n = 0; n < count; ++n) {
                pending_integers_.push_back(take_u32());
            }
            return next();
        }
        if (code == 7) { // FLOAT_LIST (0032 files store IEEE float32)
            const std::uint32_t count = take_u32();
            require(static_cast<std::size_t>(count) * 4U);
            for (std::uint32_t n = 0; n < count; ++n) {
                const std::uint32_t bits = take_u32();
                float value{};
                std::memcpy(&value, &bits, sizeof(value));
                pending_floats_.push_back(value);
            }
            return next();
        }
        if (code == 5) { // GUID
            require(16);
            cursor_ += 16;
        }
        if (code == 10) {
            return {XTokenKind::Open, {}, 0, 0.0F};
        }
        if (code == 11) {
            return {XTokenKind::Close, {}, 0, 0.0F};
        }
        return {XTokenKind::Other, {}, 0, 0.0F};
    }

  private:
    void require(std::size_t count) const {
        if (count > data_.size() - cursor_) {
            throw std::runtime_error("truncated binary .x token");
        }
    }
    std::uint16_t take_u16() {
        require(2);
        const std::uint16_t value = static_cast<std::uint16_t>(data_[cursor_]) |
                                    static_cast<std::uint16_t>(data_[cursor_ + 1] << 8U);
        cursor_ += 2;
        return value;
    }
    std::uint32_t take_u32() {
        require(4);
        const std::uint32_t value =
            static_cast<std::uint32_t>(data_[cursor_]) |
            (static_cast<std::uint32_t>(data_[cursor_ + 1]) << 8U) |
            (static_cast<std::uint32_t>(data_[cursor_ + 2]) << 16U) |
            (static_cast<std::uint32_t>(data_[cursor_ + 3]) << 24U);
        cursor_ += 4;
        return value;
    }

    std::vector<std::uint8_t> data_;
    std::size_t cursor_{};
    std::deque<std::uint32_t> pending_integers_;
    std::deque<float> pending_floats_;
};

XToken next_value(XBinaryTokens& tokens, XTokenKind kind) {
    for (;;) {
        XToken token = tokens.next();
        if (token.kind == kind || token.kind == XTokenKind::End) {
            return token;
        }
    }
}


// DirectX .x frames compose with row vectors: a mesh's world position is
// v * M_leaf * ... * M_root. flames.X depends on this - it stores one flame
// mesh four times with identical vertices and places each at a different
// engine through its Frame chain, so ignoring the frames stacks all four
// flames on top of each other.
using XMatrix = std::array<float, 16>;

constexpr XMatrix kIdentityMatrix{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                  0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F,
                                  0.0F, 1.0F};

XMatrix multiply_x(const XMatrix& left, const XMatrix& right) {
    XMatrix result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += left[row * 4 + k] * right[k * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    return result;
}

ModelVertex transform_x(const XMatrix& m, const ModelVertex& v) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12],
            v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13],
            v.x * m[2] + v.y * m[6] + v.z * m[10] + m[14]};
}

struct XMaterial {
    SDL_Color color{255, 255, 255, 255};
    std::string texture;
};

// Material Legs { 0.502;0.502;0.502;1.0;; 3.2; ... }
// Material Gunstar { 1;1;1;1;; 3.2; ... TextureFilename { "gunstar.bmp"; } }
void parse_x_material(XBinaryTokens& tokens, const std::string& name,
                      std::map<std::string, XMaterial>& materials) {
    XToken token = tokens.next();
    std::string material_name = name;
    if (token.kind == XTokenKind::Name) {
        if (material_name.empty()) {
            material_name = token.text;
        }
        token = tokens.next();
    }
    if (token.kind != XTokenKind::Open) {
        return;
    }
    XMaterial material;
    auto channel = [](float value) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    const XToken red = next_value(tokens, XTokenKind::Float);
    const XToken green = next_value(tokens, XTokenKind::Float);
    const XToken blue = next_value(tokens, XTokenKind::Float);
    const XToken alpha = next_value(tokens, XTokenKind::Float);
    if (alpha.kind == XTokenKind::End) {
        return;
    }
    material.color = {channel(red.number), channel(green.number),
                      channel(blue.number), channel(alpha.number)};

    // Walk the rest of the block for a TextureFilename child.
    int depth = 1;
    bool expecting_texture = false;
    while (depth > 0) {
        const XToken child = tokens.next();
        if (child.kind == XTokenKind::End) {
            break;
        }
        if (child.kind == XTokenKind::Open) {
            ++depth;
        } else if (child.kind == XTokenKind::Close) {
            --depth;
        } else if (child.kind == XTokenKind::Name) {
            if (child.text == "TextureFilename") {
                expecting_texture = true;
            } else if (expecting_texture && !child.text.empty()) {
                material.texture = child.text;
                expecting_texture = false;
            }
        }
    }
    if (!material_name.empty()) {
        materials[material_name] = material;
    }
}

void parse_x_mesh(XBinaryTokens& tokens, const std::string& object_name,
                  Model3ds& model,
                  const std::map<std::string, XMaterial>& materials,
                  const XMatrix& frame_transform,
                  bool open_already_read = false) {
    XToken token = open_already_read
                       ? XToken{XTokenKind::Open, {}, 0, 0.0F}
                       : tokens.next();
    if (token.kind == XTokenKind::Name) {
        token = tokens.next();
    }
    if (token.kind != XTokenKind::Open) {
        return;
    }
    const XToken vertex_count_token = next_value(tokens, XTokenKind::Integer);
    if (vertex_count_token.kind == XTokenKind::End) {
        throw std::runtime_error("binary .x mesh has no vertex count");
    }
    const std::uint32_t vertex_count = vertex_count_token.integer;
    if (vertex_count > 65535U || model.vertices.size() + vertex_count > 65535U) {
        throw std::runtime_error("binary .x mesh exceeds renderer limits");
    }
    const auto base = static_cast<std::uint16_t>(model.vertices.size());
    for (std::uint32_t n = 0; n < vertex_count; ++n) {
        const XToken x = next_value(tokens, XTokenKind::Float);
        const XToken y = next_value(tokens, XTokenKind::Float);
        const XToken z = next_value(tokens, XTokenKind::Float);
        if (z.kind == XTokenKind::End) {
            throw std::runtime_error("truncated binary .x vertex list");
        }
        model.vertices.push_back(
            transform_x(frame_transform, {x.number, y.number, z.number}));
    }
    const XToken face_count_token = next_value(tokens, XTokenKind::Integer);
    const std::uint32_t face_count = face_count_token.integer;
    // The real colour comes from this mesh's MeshMaterialList, read below.
    const SDL_Color color{255, 255, 255, 255};
    const std::size_t first_face = model.faces.size();
    for (std::uint32_t n = 0; n < face_count; ++n) {
        const std::uint32_t corners =
            next_value(tokens, XTokenKind::Integer).integer;
        if (corners > 1024U) {
            throw std::runtime_error("invalid binary .x polygon");
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(corners);
        for (std::uint32_t corner = 0; corner < corners; ++corner) {
            indices.push_back(next_value(tokens, XTokenKind::Integer).integer);
        }
        for (std::size_t corner = 1; corner + 1 < indices.size(); ++corner) {
            if (indices[0] >= vertex_count || indices[corner] >= vertex_count ||
                indices[corner + 1] >= vertex_count) {
                throw std::runtime_error("binary .x face has an invalid index");
            }
            model.faces.push_back(
                {static_cast<std::uint16_t>(base + indices[0]),
                 static_cast<std::uint16_t>(base + indices[corner]),
                 static_cast<std::uint16_t>(base + indices[corner + 1]), color});
        }
    }

    // Walk the remaining child templates instead of skipping them: the shipped
    // Gunstar keeps its hull colour in a MeshMaterialList and its hull texture
    // coordinates in a MeshTextureCoords, and the ship reads wrong without
    // both. Mesh names occurring inside those children must still not be
    // mistaken for new objects.
    model.uvs.resize(model.vertices.size());
    std::string material_name;
    XMaterial local_material;
    bool has_local_material = false;
    bool expecting_material = false;
    int depth = 1;
    while (depth > 0) {
        const XToken child = tokens.next();
        if (child.kind == XTokenKind::End) {
            break;
        }
        if (child.kind == XTokenKind::Open) {
            ++depth;
            continue;
        }
        if (child.kind == XTokenKind::Close) {
            --depth;
            continue;
        }
        if (child.kind != XTokenKind::Name) {
            continue;
        }
        if (child.text == "MeshTextureCoords") {
            XToken lead = tokens.next();
            if (lead.kind == XTokenKind::Name) {
                lead = tokens.next();
            }
            if (lead.kind != XTokenKind::Open) {
                continue;
            }
            ++depth;
            const XToken count = tokens.next();
            if (count.kind != XTokenKind::Integer ||
                count.integer != vertex_count) {
                continue;
            }
            for (std::uint32_t n = 0; n < vertex_count; ++n) {
                const XToken u = tokens.next();
                const XToken v = tokens.next();
                if (u.kind != XTokenKind::Float || v.kind != XTokenKind::Float) {
                    break;
                }
                model.uvs[static_cast<std::size_t>(base) + n] = {u.number,
                                                                 v.number};
            }
            continue;
        }
        if (child.text == "MeshMaterialList") {
            expecting_material = true;
            continue;
        }
        if (expecting_material && child.text == "Material") {
            // flames.X defines its material inline inside MeshMaterialList
            // rather than referring to a named one, so it never reaches the
            // file-level material table.
            std::map<std::string, XMaterial> inline_material;
            parse_x_material(tokens, "?inline", inline_material);
            const auto entry = inline_material.find("?inline");
            if (entry != inline_material.end()) {
                local_material = entry->second;
                has_local_material = true;
            }
            expecting_material = false;
            continue;
        }
        if (expecting_material && !child.text.empty()) {
            material_name = child.text;
            expecting_material = false;
        }
    }

    const auto found = materials.find(material_name.empty() ? object_name
                                                            : material_name);
    const XMaterial* applied = has_local_material ? &local_material
                               : found != materials.end() ? &found->second
                                                          : nullptr;
    if (applied != nullptr) {
        for (std::size_t index = first_face; index < model.faces.size();
             ++index) {
            model.faces[index].color = applied->color;
            model.faces[index].textured = !applied->texture.empty();
        }
        if (!applied->texture.empty()) {
            model.texture_name = applied->texture;
        }
    }
}

void finish_model(Model3ds& model) {
    model.normals.assign(model.vertices.size(), ModelVertex{});
    for (const ModelFace& face : model.faces) {
        const ModelVertex& a = model.vertices[face.a];
        const ModelVertex& b = model.vertices[face.b];
        const ModelVertex& c = model.vertices[face.c];
        const float abx = b.x - a.x;
        const float aby = b.y - a.y;
        const float abz = b.z - a.z;
        const float acx = c.x - a.x;
        const float acy = c.y - a.y;
        const float acz = c.z - a.z;
        const ModelVertex normal{aby * acz - abz * acy,
                                 abz * acx - abx * acz,
                                 abx * acy - aby * acx};
        for (const std::uint16_t index : {face.a, face.b, face.c}) {
            model.normals[index].x += normal.x;
            model.normals[index].y += normal.y;
            model.normals[index].z += normal.z;
        }
    }
    for (ModelVertex& normal : model.normals) {
        const float length = std::sqrt(normal.x * normal.x +
                                       normal.y * normal.y +
                                       normal.z * normal.z);
        if (length > 0.00001F) {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        } else {
            normal.z = 1.0F;
        }
    }
    for (const ModelVertex& vertex : model.vertices) {
        model.radius = std::max(
            model.radius,
            std::sqrt(vertex.x * vertex.x + vertex.y * vertex.y +
                      vertex.z * vertex.z));
    }
}

} // namespace

// Loads a 3DS model, applying the Y/Z axis exchange and winding reversal
// (see the header comment on Model3ds).
Model3ds load_3ds_model(const std::filesystem::path& path) {
    return load_3ds_model(path, {});
}

// Loads a 3DS model with a set of limbs to hide (e.g. already-exploded
// command-ship pieces). hidden_limbs is a list of limb record indices whose
// vertices/faces are dropped so the model can be reloaded after a breakup.
Model3ds load_3ds_model(const std::filesystem::path& path,
                        const std::vector<std::size_t>& hidden_limbs) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::streamsize size = input.tellg();
    if (size < 6 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("invalid 3DS file size for " + path.string());
    }
    input.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("cannot read " + path.string());
    }

    const Chunk root = read_chunk(data, 0, data.size());
    if (root.id != kMain || root.end != data.size()) {
        throw std::runtime_error("not a supported 3DS file: " + path.string());
    }
    Model3ds model;
    std::map<std::string, SDL_Color> materials;
    // The loose-piece fixups above are specific to the Xurian scout mesh.
    std::string lowered = path.filename().string();
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool seat_scout_pieces = lowered.rfind("xurian ship", 0) == 0;
    std::size_t limb_index = 0;
    parse_container(data, root, materials, model, seat_scout_pieces,
                    hidden_limbs, limb_index);
    if (model.vertices.empty() || model.faces.empty()) {
        throw std::runtime_error("3DS file contains no renderable mesh: " +
                                 path.string());
    }
    // 3DS files are authored in 3ds Max's right-handed Z-up space. They are
    // converted to the left-handed Y-up world used here by exchanging Y and
    // Z,
    // which also converts the handedness. Every shipped intro mesh depends on
    // this: bkgd.3ds stores its ridge heights in Z, and landing5.3ds stores
    // the plaza plane in X/Y with the slab heights in Z. Without the exchange
    // the mountains and the landing plaza both render edge-on.
    for (ModelVertex& vertex : model.vertices) {
        std::swap(vertex.y, vertex.z);
    }
    // Exchanging two axes mirrors the space, so every face's winding — and
    // with it the averaged vertex normal finish_model derives — would point
    // inwards. Reverse the winding to keep the normals facing outwards.
    for (ModelFace& face : model.faces) {
        std::swap(face.b, face.c);
    }
    finish_model(model);
    return model;
}

// Loads a binary DirectX 8 .x model (the editor's export format). Unlike the
// 3DS loader this does not swap axes: the .x files are already authored in the
// left-handed Y-up space this port uses.
Model3ds load_x_model(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::streamsize size = input.tellg();
    if (size < 16 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("invalid .x file size for " + path.string());
    }
    input.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const std::string header(reinterpret_cast<const char*>(data.data()), 16);
    if (header.substr(0, 12) != "xof 0303bin ") {
        throw std::runtime_error("not a supported binary DirectX .x file: " +
                                 path.string());
    }

    XBinaryTokens tokens(std::move(data));
    Model3ds model;
    std::map<std::string, XMaterial> x_materials;
    // accum.back() is the transform in force for the frame we are inside.
    std::vector<XMatrix> accum{kIdentityMatrix};
    std::vector<int> frame_depth;
    int depth = 0;
    for (;;) {
        const XToken token = tokens.next();
        if (token.kind == XTokenKind::End) {
            break;
        }
        if (token.kind == XTokenKind::Open) {
            ++depth;
            continue;
        }
        if (token.kind == XTokenKind::Close) {
            --depth;
            while (!frame_depth.empty() && depth < frame_depth.back()) {
                frame_depth.pop_back();
                accum.pop_back();
            }
            continue;
        }
        if (token.kind != XTokenKind::Name) {
            continue;
        }
        if (token.text == "Frame") {
            // The Frame's own brace is consumed by the loop, so its contents
            // live one level deeper than the current depth.
            accum.push_back(accum.back());
            frame_depth.push_back(depth + 1);
            continue;
        }
        if (token.text == "FrameTransformMatrix") {
            XToken lead = tokens.next();
            if (lead.kind == XTokenKind::Name) {
                lead = tokens.next();
            }
            if (lead.kind != XTokenKind::Open) {
                continue;
            }
            ++depth;
            XMatrix local{};
            bool complete = true;
            for (float& element : local) {
                const XToken value = tokens.next();
                if (value.kind != XTokenKind::Float) {
                    complete = false;
                    break;
                }
                element = value.number;
            }
            if (complete && accum.size() >= 2) {
                accum.back() =
                    multiply_x(local, accum[accum.size() - 2]);
            } else if (complete) {
                accum.back() = local;
            }
            continue;
        }
        if (token.text == "Material") {
            parse_x_material(tokens, {}, x_materials);
            continue;
        }
        if (token.text == "Mesh") {
            XToken name = tokens.next();
            std::string object_name;
            if (name.kind == XTokenKind::Name) {
                object_name = name.text;
            }
            if (name.kind != XTokenKind::Name &&
                name.kind != XTokenKind::Open) {
                throw std::runtime_error("invalid binary .x mesh declaration");
            }
            if (name.kind == XTokenKind::Open) {
                ++depth;
            }
            parse_x_mesh(tokens, object_name, model, x_materials, accum.back(),
                         name.kind == XTokenKind::Open);
            // parse_x_mesh consumes the mesh block's closing brace itself.
            if (name.kind == XTokenKind::Open) {
                --depth;
            }
        }
    }
    if (model.vertices.empty() || model.faces.empty()) {
        throw std::runtime_error("binary .x file contains no renderable mesh: " +
                                 path.string());
    }
    model.uvs.resize(model.vertices.size());
    finish_model(model);
    return model;
}

void render_3ds_model(SDL_Renderer* renderer, const Model3ds& model,
                      float center_x, float center_y, float pixel_radius,
                      float yaw, float pitch, std::uint8_t alpha, float roll,
                      bool perspective, SDL_Color tint) {
    const float sy = std::sin(yaw);
    const float cy = std::cos(yaw);
    const float sp = std::sin(pitch);
    const float cp = std::cos(pitch);
    const float sr = std::sin(roll);
    const float cr = std::cos(roll);
    const float scale = pixel_radius / model.radius;
    // Combat objects are normally hundreds or thousands of world units from
    // the camera. A long model-space focal distance avoids the fisheye
    // distortion that made the scout and fighter break into
    // oversized-looking pieces when they banked.
    const float camera_distance = model.radius * 10.0F;
    // Rotate around the model's bounding-box center, not its file origin:
    // several shipped meshes (the Xurian scout among them) place their
    // geometry tens of units off the origin, which made yaw swings look
    // like the ship was flailing around a point near its tail.
    ModelVertex pivot{0.0F, 0.0F, 0.0F};
    if (!model.vertices.empty()) {
        float min_x = model.vertices.front().x;
        float max_x = min_x;
        float min_y = model.vertices.front().y;
        float max_y = min_y;
        float min_z = model.vertices.front().z;
        float max_z = min_z;
        for (const ModelVertex& vertex : model.vertices) {
            min_x = std::min(min_x, vertex.x);
            max_x = std::max(max_x, vertex.x);
            min_y = std::min(min_y, vertex.y);
            max_y = std::max(max_y, vertex.y);
            min_z = std::min(min_z, vertex.z);
            max_z = std::max(max_z, vertex.z);
        }
        pivot = {(min_x + max_x) * 0.5F, (min_y + max_y) * 0.5F,
                 (min_z + max_z) * 0.5F};
    }
    std::vector<RenderVertex> transformed;
    std::vector<ModelVertex> transformed_normals;
    transformed.reserve(model.vertices.size());
    transformed_normals.reserve(model.normals.size());
    for (std::size_t index = 0; index < model.vertices.size(); ++index) {
        const ModelVertex& source = model.vertices[index];
        const float px = source.x - pivot.x;
        const float py = source.y - pivot.y;
        const float pz = source.z - pivot.z;
        const float x1 = px * cy + pz * sy;
        const float z1 = -px * sy + pz * cy;
        const float y2 = py * cp - z1 * sp;
        const float z2 = py * sp + z1 * cp;
        const float x3 = x1 * cr - y2 * sr;
        const float y3 = x1 * sr + y2 * cr;
        const float projection =
            perspective
                ? camera_distance /
                      std::max(model.radius * 1.25F, camera_distance - z2)
                : 1.0F;
        transformed.push_back({center_x + x3 * scale * projection,
                               center_y - y3 * scale * projection, z2});
        const ModelVertex& source_normal = model.normals[index];
        const float nx1 = source_normal.x * cy + source_normal.z * sy;
        const float nz1 = -source_normal.x * sy + source_normal.z * cy;
        const float ny2 = source_normal.y * cp - nz1 * sp;
        const float nz2 = source_normal.y * sp + nz1 * cp;
        transformed_normals.push_back(
            {nx1 * cr - ny2 * sr, nx1 * sr + ny2 * cr, nz2});
    }

    struct OrderedFace {
        const ModelFace* face{};
        float depth{};
    };
    std::vector<OrderedFace> ordered;
    ordered.reserve(model.faces.size());
    for (const ModelFace& face : model.faces) {
        ordered.push_back(
            {&face, (transformed[face.a].z + transformed[face.b].z +
                     transformed[face.c].z) /
                        3.0F});
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const OrderedFace& left, const OrderedFace& right) {
                  return left.depth < right.depth;
              });

    std::vector<SDL_Vertex> triangles;
    triangles.reserve(ordered.size() * 3);
    for (const OrderedFace& item : ordered) {
        const ModelFace& face = *item.face;
        const RenderVertex& a = transformed[face.a];
        const RenderVertex& b = transformed[face.b];
        const RenderVertex& c = transformed[face.c];
        auto lit_color = [&](std::uint16_t vertex_index) {
            const ModelVertex& normal = transformed_normals[vertex_index];
            // Direct3D's default material is Gouraud shaded.  A
            // two-sided key light keeps these old low-poly meshes readable
            // even where the source face winding is inconsistent.
            constexpr float lx = -0.324F;
            constexpr float ly = 0.487F;
            constexpr float lz = 0.811F;
            const float diffuse =
                std::abs(normal.x * lx + normal.y * ly + normal.z * lz);
            const float light = 0.42F + 0.58F * diffuse;
            return SDL_Color{
                static_cast<std::uint8_t>(face.color.r * tint.r * light /
                                          255.0F),
                static_cast<std::uint8_t>(face.color.g * tint.g * light /
                                          255.0F),
                static_cast<std::uint8_t>(face.color.b * tint.b * light /
                                          255.0F),
                static_cast<std::uint8_t>(alpha * tint.a / 255U)};
        };
        triangles.push_back(
            {{a.x, a.y}, lit_color(face.a), {0.0F, 0.0F}});
        triangles.push_back(
            {{b.x, b.y}, lit_color(face.b), {0.0F, 0.0F}});
        triangles.push_back(
            {{c.x, c.y}, lit_color(face.c), {0.0F, 0.0F}});
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (SDL_RenderGeometry(renderer, nullptr, triangles.data(),
                           static_cast<int>(triangles.size()), nullptr, 0) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
}


DepthRaster::DepthRaster(SDL_Renderer* renderer, int width, int height)
    : width_(width), height_(height),
      color_(static_cast<std::size_t>(width) * height, 0xFF000000U),
      depth_(static_cast<std::size_t>(width) * height, 0.0F) {
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (texture_ == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    // The intro clears the raster transparent and composites it over the
    // backdrop colour and the starfield plane already on the back buffer.
    if (SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

DepthRaster::~DepthRaster() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
    }
}

void DepthRaster::clear(SDL_Color color) {
    const std::uint32_t packed = (static_cast<std::uint32_t>(color.a) << 24) |
                                 (static_cast<std::uint32_t>(color.r) << 16) |
                                 (static_cast<std::uint32_t>(color.g) << 8) |
                                 static_cast<std::uint32_t>(color.b);
    std::fill(color_.begin(), color_.end(), packed);
    std::fill(depth_.begin(), depth_.end(), 0.0F);
}

void DepthRaster::triangle(const Vertex& a, const Vertex& b, const Vertex& c,
                           std::uint8_t alpha, const TextureImage* texture) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 0.000001F) {
        return;
    }
    const float inverse_area = 1.0F / area;

    int min_x = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
    int max_x = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
    int min_y = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
    int max_y = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
    min_x = std::max(min_x, 0);
    min_y = std::max(min_y, 0);
    max_x = std::min(max_x, width_ - 1);
    max_y = std::min(max_y, height_ - 1);
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    const bool blend = alpha < 255;
    const float source_weight = static_cast<float>(alpha) / 255.0F;
    for (int y = min_y; y <= max_y; ++y) {
        const float py = static_cast<float>(y) + 0.5F;
        std::size_t row = static_cast<std::size_t>(y) * width_;
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            // Barycentric weights; the sign of the area absorbs the winding,
            // so both face orientations rasterise without back-face culling.
            const float w0 =
                ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) *
                inverse_area;
            const float w1 =
                ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) *
                inverse_area;
            const float w2 = 1.0F - w0 - w1;
            if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) {
                continue;
            }
            const float inverse_depth = w0 * a.inverse_depth +
                                        w1 * b.inverse_depth +
                                        w2 * c.inverse_depth;
            const std::size_t index = row + static_cast<std::size_t>(x);
            if (inverse_depth <= depth_[index]) {
                continue;
            }
            float r = w0 * a.r + w1 * b.r + w2 * c.r;
            float g = w0 * a.g + w1 * b.g + w2 * c.g;
            float blue = w0 * a.b + w1 * b.b + w2 * c.b;
            if (texture != nullptr && texture->valid()) {
                // u and v were premultiplied by 1/z, so dividing the
                // interpolated pair by the interpolated 1/z restores the
                // perspective-correct coordinate.
                const float inverse = 1.0F / inverse_depth;
                const float u = (w0 * a.u_over_z + w1 * b.u_over_z +
                                 w2 * c.u_over_z) * inverse;
                const float v = (w0 * a.v_over_z + w1 * b.v_over_z +
                                 w2 * c.v_over_z) * inverse;
                auto wrap = [](float value, int extent) {
                    int texel = static_cast<int>(
                        std::floor(value * static_cast<float>(extent)));
                    texel %= extent;
                    return texel < 0 ? texel + extent : texel;
                };
                const std::uint32_t sample =
                    texture->pixels[static_cast<std::size_t>(
                                        wrap(v, texture->height)) *
                                        texture->width +
                                    wrap(u, texture->width)];
                // The face colour carries the material tint times the light
                // level; the shipped hull material is white, so this reduces
                // to texel * light.
                r *= static_cast<float>((sample >> 16) & 0xFFU) / 255.0F;
                g *= static_cast<float>((sample >> 8) & 0xFFU) / 255.0F;
                blue *= static_cast<float>(sample & 0xFFU) / 255.0F;
            }
            if (blend) {
                const std::uint32_t behind = color_[index];
                const float inverse_weight = 1.0F - source_weight;
                r = r * source_weight +
                    static_cast<float>((behind >> 16) & 0xFFU) *
                        inverse_weight;
                g = g * source_weight +
                    static_cast<float>((behind >> 8) & 0xFFU) * inverse_weight;
                blue = blue * source_weight +
                       static_cast<float>(behind & 0xFFU) * inverse_weight;
            } else {
                // Translucent surfaces (the launch flames) must not occlude
                // whatever is drawn after them.
                depth_[index] = inverse_depth;
            }
            const auto pack = [](float value) {
                return static_cast<std::uint32_t>(
                    std::clamp(value, 0.0F, 255.0F));
            };
            color_[index] = 0xFF000000U | (pack(r) << 16) | (pack(g) << 8) |
                            pack(blue);
        }
    }
}

void DepthRaster::present(SDL_Renderer* renderer) {
    if (SDL_UpdateTexture(texture_, nullptr, color_.data(),
                          width_ * static_cast<int>(sizeof(std::uint32_t))) !=
        0) {
        throw std::runtime_error(SDL_GetError());
    }
    if (SDL_RenderCopy(renderer, texture_, nullptr, nullptr) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

void render_world_model(DepthRaster& raster, const Model3ds& model,
                        const ModelWorldTransform& transform,
                        const ModelCamera& camera,
                        const WorldMaterial& material) {
    const float sx = std::sin(transform.rotation_x);
    const float cx = std::cos(transform.rotation_x);
    const float sy = std::sin(transform.rotation_y);
    const float cy = std::cos(transform.rotation_y);
    const float sz = std::sin(transform.rotation_z);
    const float cz = std::cos(transform.rotation_z);
    const float csx = std::sin(camera.rotation_x);
    const float ccx = std::cos(camera.rotation_x);
    const float csy = std::sin(camera.rotation_y);
    const float ccy = std::cos(camera.rotation_y);
    const float csz = std::sin(camera.rotation_z);
    const float ccz = std::cos(camera.rotation_z);
    constexpr float kPi = 3.14159265358979323846F;
    const float half_width = static_cast<float>(raster.width()) * 0.5F;
    const float half_height = static_cast<float>(raster.height()) * 0.5F;
    const float focal_length =
        half_height / std::tan(camera.vertical_fov_degrees * kPi / 360.0F);

    // Calibrated against the preserved blast-off screenshot: the landing
    // plaza's up-facing surface renders at its full texture colour
    // (172,156,98) while the launch pad's camera-facing wall renders at about
    // half of its own (123,74,49). A mostly overhead key light with a strong
    // ambient floor reproduces both.
    constexpr float kAmbient = 0.42F;
    constexpr float kDiffuse = 0.58F;
    constexpr float klx = 0.0F;
    constexpr float kly = 0.970F;
    constexpr float klz = -0.243F;

    struct ViewVertex {
        float x{};
        float y{};
        float z{};
        float light{};
        float u{};
        float v{};
    };
    std::vector<ViewVertex> view(model.vertices.size());
    // The shipped meshes carry no 3DS smoothing groups, so they are shaded
    // flat. Averaging adjacent face normals rounded off the plaza and
    // the Gunstar's facets, which the preserved screenshot shows hard-edged.
    std::vector<ModelVertex> world(model.vertices.size());
    for (std::size_t index = 0; index < model.vertices.size(); ++index) {
        const ModelVertex& source = model.vertices[index];
        const float px = source.x * transform.scale_x;
        const float py = source.y * transform.scale_y;
        const float pz = source.z * transform.scale_z;
        // The object matrix is built through DirectX's yaw-pitch-roll helper,
        // so ROTATE OBJECT x,y,z applies roll (Z)
        // first, then pitch (X), then yaw (Y). The Gunstar depends on it:
        // gunstar3.x models the ship nose-along-+Z, and its shipped
        // (270,0,180) attitude only stands it upright on the pad in this
        // order. Applying X before Z inverts it beneath the plaza.
        const float rx = px * cz - py * sz;
        const float ry = px * sz + py * cz;
        const float yx = ry * cx - pz * sx;
        const float zx = ry * sx + pz * cx;
        const float xy = rx * cy + zx * sy;
        const float zy = -rx * sy + zx * cy;

        const float dx = xy + transform.x - camera.x;
        const float dy = yx + transform.y - camera.y;
        const float dz = zy + transform.z - camera.z;
        const float xcr = dx * ccz + dy * csz;
        const float ycr = -dx * csz + dy * ccz;
        const float xcy = xcr * ccy - dz * csy;
        const float zcy = xcr * csy + dz * ccy;
        const float ycx = ycr * ccx + zcy * csx;
        const float zcx = -ycr * csx + zcy * ccx;

        world[index] = {xy, yx, zy};
        const ModelUV uv =
            index < model.uvs.size() ? model.uvs[index] : ModelUV{};
        view[index] = {xcy, ycx, zcx, 1.0F, uv.u, uv.v};
    }

    // Near-plane clipping. Discarding straddling faces removes the entire
    // plaza as soon as the camera passes over its leading edge.
    constexpr float kNearPlane = 1.0F;
    std::array<ViewVertex, 4> polygon{};
    for (const ModelFace& face : model.faces) {
        const SDL_Color base =
            material.override_color ? material.color : face.color;

        float light = 1.0F;
        if (material.lit) {
            const ModelVertex& wa = world[face.a];
            const ModelVertex& wb = world[face.b];
            const ModelVertex& wc = world[face.c];
            const float ux = wb.x - wa.x;
            const float uy = wb.y - wa.y;
            const float uz = wb.z - wa.z;
            const float vx = wc.x - wa.x;
            const float vy = wc.y - wa.y;
            const float vz = wc.z - wa.z;
            const float nx = uy * vz - uz * vy;
            const float ny = uz * vx - ux * vz;
            const float nz = ux * vy - uy * vx;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 0.00001F) {
                // Two-sided: landing5.3ds winds its base plate opposite to
                // the ten raised slabs, and the preserved screenshot shows
                // both at the same brightness, so the shading cannot depend
                // on the mesh's winding.
                const float diffuse =
                    std::abs(nx * klx + ny * kly + nz * klz) / length;
                light = kAmbient + kDiffuse * diffuse;
            } else {
                light = kAmbient;
            }
        }
        std::array<ViewVertex, 3> shaded{view[face.a], view[face.b],
                                         view[face.c]};
        for (ViewVertex& corner : shaded) {
            corner.light = light;
        }
        const std::array<const ViewVertex*, 3> corners{
            &shaded[0], &shaded[1], &shaded[2]};

        std::size_t count = 0;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const ViewVertex& current = *corners[edge];
            const ViewVertex& next = *corners[(edge + 1) % 3];
            const bool current_inside = current.z >= kNearPlane;
            const bool next_inside = next.z >= kNearPlane;
            if (current_inside && count < polygon.size()) {
                polygon[count++] = current;
            }
            if (current_inside != next_inside && count < polygon.size()) {
                const float t = (kNearPlane - current.z) / (next.z - current.z);
                polygon[count++] = ViewVertex{
                    current.x + (next.x - current.x) * t,
                    current.y + (next.y - current.y) * t,
                    kNearPlane,
                    current.light + (next.light - current.light) * t,
                    current.u + (next.u - current.u) * t,
                    current.v + (next.v - current.v) * t};
            }
        }
        if (count < 3) {
            continue;
        }

        const auto to_screen = [&](const ViewVertex& vertex) {
            const float inverse_depth = 1.0F / vertex.z;
            return DepthRaster::Vertex{
                half_width + vertex.x * focal_length * inverse_depth,
                half_height - vertex.y * focal_length * inverse_depth,
                inverse_depth,
                static_cast<float>(base.r) * vertex.light,
                static_cast<float>(base.g) * vertex.light,
                static_cast<float>(base.b) * vertex.light,
                vertex.u * inverse_depth,
                vertex.v * inverse_depth};
        };
        const TextureImage* face_texture =
            face.textured ? material.texture : nullptr;
        const DepthRaster::Vertex first = to_screen(polygon[0]);
        for (std::size_t corner = 1; corner + 1 < count; ++corner) {
            const DepthRaster::Vertex second = to_screen(polygon[corner]);
            const DepthRaster::Vertex third = to_screen(polygon[corner + 1]);
            if (material.cull_backfaces) {
                const float area = (second.x - first.x) * (third.y - first.y) -
                                   (second.y - first.y) * (third.x - first.x);
                if (area >= 0.0F) {
                    continue;
                }
            }
            raster.triangle(first, second, third, material.alpha,
                            face_texture);
        }
    }
}

bool project_world_point(const ModelWorldTransform& transform,
                         const ModelCamera& camera, float local_x,
                         float local_y, float local_z, int width, int height,
                         float& screen_x, float& screen_y) {
    const float sx = std::sin(transform.rotation_x);
    const float cx = std::cos(transform.rotation_x);
    const float sy = std::sin(transform.rotation_y);
    const float cy = std::cos(transform.rotation_y);
    const float sz = std::sin(transform.rotation_z);
    const float cz = std::cos(transform.rotation_z);
    const float csy = std::sin(camera.rotation_y);
    const float ccy = std::cos(camera.rotation_y);
    const float csx = std::sin(camera.rotation_x);
    const float ccx = std::cos(camera.rotation_x);
    const float csz = std::sin(camera.rotation_z);
    const float ccz = std::cos(camera.rotation_z);
    constexpr float kPi = 3.14159265358979323846F;
    const float focal_length =
        static_cast<float>(height) * 0.5F /
        std::tan(camera.vertical_fov_degrees * kPi / 360.0F);

    const float px = local_x * transform.scale_x;
    const float py = local_y * transform.scale_y;
    const float pz = local_z * transform.scale_z;
    const float rx = px * cz - py * sz;
    const float ry = px * sz + py * cz;
    const float yx = ry * cx - pz * sx;
    const float zx = ry * sx + pz * cx;
    const float xy = rx * cy + zx * sy;
    const float zy = -rx * sy + zx * cy;
    const float dx = xy + transform.x - camera.x;
    const float dy = yx + transform.y - camera.y;
    const float dz = zy + transform.z - camera.z;
    const float xcr = dx * ccz + dy * csz;
    const float ycr = -dx * csz + dy * ccz;
    const float xcy = xcr * ccy - dz * csy;
    const float zcy = xcr * csy + dz * ccy;
    const float ycx = ycr * ccx + zcy * csx;
    const float zcx = -ycr * csx + zcy * ccx;
    if (zcx <= 1.0F) {
        return false;
    }
    screen_x = static_cast<float>(width) * 0.5F + xcy * focal_length / zcx;
    screen_y = static_cast<float>(height) * 0.5F - ycx * focal_length / zcx;
    return true;
}


TextureImage load_texture_image(const std::filesystem::path& path) {
    TextureImage image;
    SDL_Surface* loaded = SDL_LoadBMP(path.string().c_str());
    if (loaded == nullptr) {
        throw std::runtime_error("cannot load " + path.string() + ": " +
                                 SDL_GetError());
    }
    SDL_Surface* converted =
        SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(loaded);
    if (converted == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    image.width = converted->w;
    image.height = converted->h;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
    for (int y = 0; y < image.height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(converted->pixels) +
            static_cast<std::size_t>(y) * converted->pitch);
        for (int x = 0; x < image.width; ++x) {
            image.pixels[static_cast<std::size_t>(y) * image.width + x] =
                row[x] & 0x00FFFFFFU;
        }
    }
    SDL_FreeSurface(converted);
    return image;
}
