// Shaders missing from --native_shader_dir, translated inside the game: see shader_translate.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace blitz {

// What tools/translate_shaders.py does for one container: saves it as
// <name>.xsc in `dir`, translates it to <name>.hlsl and compiles <name>.dxil
// (as a library if it uses specialization constants). Returns the DXIL, empty
// on failure (dxc's messages go to <name>.dxc.log). Thread-safe; runs a few at
// a time, below the game's priority.
std::vector<uint8_t> TranslateShader(const std::filesystem::path& dir, const std::string& name,
                                     const std::string& container);

}  // namespace blitz
