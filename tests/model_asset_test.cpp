// Loads every shipped 3DS and .x mesh, checks structural validity, and pins
// the measured post-import dimensions so axis regressions are caught.
#include "model_3ds.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Bounds {
    float x{};
    float y{};
    float z{};
};

Bounds dimensions(const Model3ds& model) {
    require(!model.vertices.empty(), "model has no vertices");
    ModelVertex low = model.vertices.front();
    ModelVertex high = low;
    for (const ModelVertex& vertex : model.vertices) {
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
                    std::isfinite(vertex.z),
                "model contains a non-finite vertex");
        low.x = std::min(low.x, vertex.x);
        low.y = std::min(low.y, vertex.y);
        low.z = std::min(low.z, vertex.z);
        high.x = std::max(high.x, vertex.x);
        high.y = std::max(high.y, vertex.y);
        high.z = std::max(high.z, vertex.z);
    }
    for (const ModelFace& face : model.faces) {
        require(face.a < model.vertices.size() &&
                    face.b < model.vertices.size() &&
                    face.c < model.vertices.size(),
                "model face index is out of bounds");
    }
    require(std::isfinite(model.radius) && model.radius > 0.0F,
            "model radius is invalid");
    return {high.x - low.x, high.y - low.y, high.z - low.z};
}

void near(float actual, float expected, float tolerance,
          const std::string& label) {
    require(std::abs(actual - expected) <= tolerance,
            label + " differs from the shipped model");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: model_asset_test ASSET_ROOT");
        const fs::path models = fs::path(argv[1]) / "Models";
        const std::vector<std::string> model_3ds{
            "3dstarscape.3ds",       "CENTAURI.3ds",
            "CENTAURI2.3ds",         "L sphere 4.3ds",
            "L sphere fin 4.3ds",    "LOGO.3ds",
            "R sphere fin 5.3ds",    "asteroid4.3ds",
            "backslope.3ds",         "bkgd.3ds",
            "comm miss 3.3ds",       "comm miss flame.3ds",
            "comm ship exp.3ds",     "command ship 2.3ds",
            "fighter flame 1.3ds",   "frontier prim 2.3ds",
            "kd fighter 5.3ds",      "kd fighter 6.3ds",
            "landing5.3ds",          "laser bore 2.3ds",
            "lighttarget.3ds",       "meteor 3d 1.3ds",
            "missile 1-2.3ds",       "missile 1-3.3ds",
            "missile 1-6.3ds",       "missile 2-3.3ds",
            "pad.3ds",               "pad2.3ds",
            "weapons switches.3ds",  "xurian flames 6.3ds",
            "xurian ship 4.3DS"};
        for (const std::string& name : model_3ds) {
            const Model3ds model = load_3ds_model(models / name);
            dimensions(model);
            require(model.limbs.size() == model.vertices.size(),
                    name + " lost its named-object limb mapping");
        }
        for (const char* name : {"gunstar3.x", "flames.X", "flamel.X"}) {
            const Model3ds model = load_x_model(models / name);
            dimensions(model);
        }

        // These dimensions are measured after the same Y/Z import exchange
        // the loader performs. They catch upright/on-edge regressions as well
        // as accidentally loading the older Ko-Dan model.
        const Bounds fighter = dimensions(load_3ds_model(models / "kd fighter 6.3ds"));
        near(fighter.x, 213.29F, 0.1F, "Ko-Dan fighter width");
        near(fighter.y, 37.67F, 0.1F, "Ko-Dan fighter height");
        near(fighter.z, 66.73F, 0.1F, "Ko-Dan fighter depth");
        const Bounds command = dimensions(load_3ds_model(models / "command ship 2.3ds"));
        near(command.x, 249.4F, 0.2F, "command ship width");
        near(command.y, 30.6F, 0.2F, "command ship height");
        near(command.z, 94.6F, 0.2F, "command ship depth");
        const Bounds meteor = dimensions(load_3ds_model(models / "meteor 3d 1.3ds"));
        near(meteor.x, 72.42F, 0.1F, "meteor width");
        near(meteor.y, 70.27F, 0.1F, "meteor height");
        near(meteor.z, 65.81F, 0.1F, "meteor depth");
        const Model3ds breakup =
            load_3ds_model(models / "comm ship exp.3ds");
        require(!breakup.limbs.empty() &&
                    *std::max_element(breakup.limbs.begin(),
                                      breakup.limbs.end()) == 47U,
                "command breakup model does not contain its 47 limbs");
    } catch (const std::exception& error) {
        std::cerr << "model_asset_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
