// GameFont renders text with FreeType and blits each glyph through SDL.
// The font file is loaded once and its per-character bitmaps are cached on
// demand; draw() lays glyphs out left to right with kerning between adjacent
// characters. All FreeType calls are fallible and throw std::runtime_error on
// failure rather than silently producing blank text.
#pragma once

#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>

// Horizontal alignment of draw() output relative to the supplied x coordinate.
enum class TextAlign { Left, Center, Right };

class GameFont final {
  public:
    GameFont(SDL_Renderer* renderer, const std::filesystem::path& path,
             unsigned pixel_height);
    ~GameFont();
    GameFont(const GameFont&) = delete;
    GameFont& operator=(const GameFont&) = delete;

    // Pixel width of the text, including kerning between adjacent glyphs.
    // Used by draw() to center or right-align a string.
    int width(const std::string& text);

    // Blits the text at (x, top) using the given color and alignment. The
    // baseline sits at y = top + ascent; each glyph is positioned by its
    // bitmap bearing so the ink lines up on that baseline. x is adjusted for
    // Center/Right alignment before the first glyph is drawn.
    void draw(const std::string& text, int x, int top, SDL_Color color,
              TextAlign align = TextAlign::Left);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
