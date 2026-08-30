// GameFont loads a TrueType font with FreeType and renders it to SDL textures
// on demand, caching each requested glyph. Bitmaps are rendered at normal
// quality and blitted with per-character kerning and a configurable alpha so
// text can be tinted and faded. FreeType operations throw std::runtime_error
// on error; glyph caching means a character is only rasterized once per font.
#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

namespace {

struct Glyph {
    SDL_Texture* texture{};
    int width{};
    int height{};
    int bearing_x{};
    int bearing_y{};
    int advance{};
};

std::runtime_error ft_error(const std::string& operation, FT_Error error) {
    return std::runtime_error(operation + " failed (FreeType error " +
                              std::to_string(error) + ")");
}

} // namespace

// Pooled FreeType state and lazily-built glyph textures for one GameFont.
struct GameFont::Impl {
    SDL_Renderer* renderer{};
    FT_Library library{};
    FT_Face face{};
    int ascent{};
    std::map<unsigned char, Glyph> glyphs;

    ~Impl() {
        for (auto& item : glyphs) {
            SDL_DestroyTexture(item.second.texture);
        }
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }

    Glyph& glyph(unsigned char character) {
        const auto found = glyphs.find(character);
        if (found != glyphs.end()) {
            return found->second;
        }
        const FT_Error load_error =
            FT_Load_Char(face, character, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
        if (load_error != 0) {
            throw ft_error("FT_Load_Char", load_error);
        }
        const FT_GlyphSlot slot = face->glyph;
        Glyph result;
        result.width = static_cast<int>(slot->bitmap.width);
        result.height = static_cast<int>(slot->bitmap.rows);
        result.bearing_x = slot->bitmap_left;
        result.bearing_y = slot->bitmap_top;
        result.advance = static_cast<int>(slot->advance.x >> 6);

        if (result.width > 0 && result.height > 0) {
            SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
                0, result.width, result.height, 32, SDL_PIXELFORMAT_RGBA32);
            if (!surface) {
                throw std::runtime_error(SDL_GetError());
            }
            auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
            const int stride = surface->pitch / 4;
            for (int y = 0; y < result.height; ++y) {
                const unsigned char* source =
                    slot->bitmap.buffer + y * slot->bitmap.pitch;
                for (int x = 0; x < result.width; ++x) {
                    unsigned char alpha{};
                    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                        alpha = (source[x / 8] & (0x80U >> (x % 8))) ? 255 : 0;
                    } else {
                        alpha = source[x];
                    }
                    pixels[y * stride + x] =
                        SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
                }
            }
            result.texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
            if (!result.texture) {
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(result.texture, SDL_BLENDMODE_BLEND);
        }
        return glyphs.emplace(character, result).first->second;
    }

    int kerning(unsigned char left, unsigned char right) const {
        if (!left || !FT_HAS_KERNING(face)) {
            return 0;
        }
        FT_Vector delta{};
        FT_Get_Kerning(face, FT_Get_Char_Index(face, left),
                       FT_Get_Char_Index(face, right), FT_KERNING_DEFAULT,
                       &delta);
        return static_cast<int>(delta.x >> 6);
    }
};

GameFont::GameFont(SDL_Renderer* renderer, const std::filesystem::path& path,
                   unsigned pixel_height)
    : impl_(std::make_unique<Impl>()) {
    // pixel_height sets the font's pixel size; the 0 low value tells
    // FreeType to size it from the requested height.
    impl_->renderer = renderer;
    const FT_Error init_error = FT_Init_FreeType(&impl_->library);
    if (init_error != 0) {
        throw ft_error("FT_Init_FreeType", init_error);
    }
    const FT_Error face_error =
        FT_New_Face(impl_->library, path.string().c_str(), 0, &impl_->face);
    if (face_error != 0) {
        throw ft_error("cannot load " + path.string(), face_error);
    }
    FT_Error charmap_error =
        FT_Select_Charmap(impl_->face, FT_ENCODING_UNICODE);
    if (charmap_error != 0) {
        charmap_error =
            FT_Select_Charmap(impl_->face, FT_ENCODING_APPLE_ROMAN);
    }
    if (charmap_error != 0) {
        throw ft_error("cannot select a text character map", charmap_error);
    }
    const FT_Error size_error =
        FT_Set_Pixel_Sizes(impl_->face, 0, pixel_height);
    if (size_error != 0) {
        throw ft_error("FT_Set_Pixel_Sizes", size_error);
    }
    impl_->ascent = static_cast<int>(impl_->face->size->metrics.ascender >> 6);
}

GameFont::~GameFont() = default;

int GameFont::width(const std::string& text) {
    int result = 0;
    unsigned char previous = 0;
    for (const unsigned char character : text) {
        result += impl_->kerning(previous, character);
        result += impl_->glyph(character).advance;
        previous = character;
    }
    return result;
}

void GameFont::draw(const std::string& text, int x, int top, SDL_Color color,
                    TextAlign align) {
    // Center/Right alignment shifts the pen left by half the text width or
    // the full width before the first glyph is drawn.
    if (align == TextAlign::Center) {
        x -= width(text) / 2;
    } else if (align == TextAlign::Right) {
        x -= width(text);
    }
    int pen = x;
    unsigned char previous = 0;
    for (const unsigned char character : text) {
        pen += impl_->kerning(previous, character);
        Glyph& glyph = impl_->glyph(character);
        if (glyph.texture) {
            SDL_SetTextureColorMod(glyph.texture, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(glyph.texture, color.a);
            const SDL_Rect destination{pen + glyph.bearing_x,
                                       top + impl_->ascent - glyph.bearing_y,
                                       glyph.width, glyph.height};
            SDL_RenderCopy(impl_->renderer, glyph.texture, nullptr,
                           &destination);
        }
        pen += glyph.advance;
        previous = character;
    }
}
