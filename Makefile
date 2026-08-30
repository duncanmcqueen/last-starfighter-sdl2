# Starfighter Linux build.
#
#   make          build the game to build/bin/starfighter
#   make test     build everything and run the unit and smoke tests
#   make clean    remove all build artifacts
#
# Requires: g++ (C++17), make, pkg-config, unzip, and the SDL2 + FreeType
# development packages.

CXX      ?= g++
PKG_CONFIG ?= pkg-config

BUILD_DIR := build
BIN_DIR   := $(BUILD_DIR)/bin
OBJ_DIR   := $(BUILD_DIR)/obj
FONT_DIR  := $(BUILD_DIR)/fonts

ASSET_DIR := assets
USUZI_ZIP := $(ASSET_DIR)/FONTS- INSTALL ME/USUZI.ZIP
FONT_TTF  := $(FONT_DIR)/USUZI.TTF

SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL2_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2)
FT_CFLAGS   := $(shell $(PKG_CONFIG) --cflags freetype2)
FT_LIBS     := $(shell $(PKG_CONFIG) --libs freetype2)

CXXFLAGS += -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += $(SDL2_CFLAGS) $(FT_CFLAGS) -Isrc \
            -DSTARFIGHTER_FONT_DIR='"$(FONT_DIR)"'
LDLIBS   += $(SDL2_LIBS) $(FT_LIBS)

GAME_SRC := main.cpp model_3ds.cpp audio.cpp font.cpp practice_stage.cpp \
            campaign.cpp dipswitches.cpp arcade.cpp music.cpp
GAME_OBJ := $(addprefix $(OBJ_DIR)/,$(GAME_SRC:.cpp=.o))

TEST_BINS := practice_stage_test campaign_test arcade_test \
             starfield_test model_asset_test

SDL_DUMMY := SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

.PHONY: all test clean

all: $(BIN_DIR)/starfighter

$(BIN_DIR)/starfighter: $(GAME_OBJ) | $(BIN_DIR) $(FONT_TTF)
	$(CXX) $(CXXFLAGS) -o $@ $(GAME_OBJ) $(LDLIBS)

# Unit tests: each links only the modules it exercises.
$(BIN_DIR)/practice_stage_test: $(OBJ_DIR)/tests/practice_stage_test.o \
                                $(OBJ_DIR)/practice_stage.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/campaign_test: $(OBJ_DIR)/tests/campaign_test.o \
                          $(OBJ_DIR)/campaign.o $(OBJ_DIR)/dipswitches.o \
                          $(OBJ_DIR)/practice_stage.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/arcade_test: $(OBJ_DIR)/tests/arcade_test.o \
                        $(OBJ_DIR)/arcade.o $(OBJ_DIR)/music.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/starfield_test: $(OBJ_DIR)/tests/starfield_test.o \
                           $(OBJ_DIR)/model_3ds.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/model_asset_test: $(OBJ_DIR)/tests/model_asset_test.o \
                             $(OBJ_DIR)/model_3ds.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/tests/%.o: tests/%.cpp | $(OBJ_DIR)/tests
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# The release ships the Usuzi face in a ZIP; extract it into the build tree
# without touching the asset directory. (The asset path contains spaces, so
# it cannot be a prerequisite; re-extracting a small font is cheap.)
$(FONT_TTF):
	@mkdir -p $(FONT_DIR)
	unzip -o -j "$(USUZI_ZIP)" USUZI.TTF -d "$(FONT_DIR)"

$(OBJ_DIR) $(BIN_DIR) $(OBJ_DIR)/tests:
	mkdir -p $@

test: all $(addprefix $(BIN_DIR)/,$(TEST_BINS))
	$(SDL_DUMMY) $(BIN_DIR)/starfighter --assets "$(ASSET_DIR)" --smoke-test
	$(SDL_DUMMY) $(BIN_DIR)/starfighter --assets "$(ASSET_DIR)" --intro-test
	$(BIN_DIR)/practice_stage_test
	$(BIN_DIR)/campaign_test
	$(BIN_DIR)/arcade_test "$(ASSET_DIR)"
	SDL_VIDEODRIVER=dummy $(BIN_DIR)/starfield_test "$(ASSET_DIR)"
	$(BIN_DIR)/model_asset_test "$(ASSET_DIR)"

clean:
	rm -rf $(BUILD_DIR)
