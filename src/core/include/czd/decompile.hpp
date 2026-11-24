// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - decompiles a compiled model (MDL + VVD + VTX) to QC and SMD files.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace czd {

struct DecompileOptions {
  bool defineBones = true;  // write $definebone lines so the skeleton survives a recompile unchanged
  bool lods = true;         // write LOD meshes and $lod blocks
  bool animations = true;   // write animation SMDs; off = bind-pose idle only
  // Start mesh file names with the model's name. Needed when several models share an output
  // folder, since many models call their mesh simply "ref" or "reference".
  bool prefixMeshNames = false;
  bool flexes = true;           // write eyeballs, flex shapes (VTA) and flex rules in a $model block
  bool proceduralBones = true;  // write a VRD file and $proceduralbones for helper bones
  bool physics = true;      // write the collision model (.phy) as a physics SMD and $collisionmodel / $collisionjoints
};

struct DecompileResult {
  std::filesystem::path qc;
  std::vector<std::filesystem::path> files;  // everything written, QC included
  std::vector<std::string> warnings;         // model features that were not carried over
};

using LogFn = std::function<void(std::string_view line)>;

// Writes <outDir>/<model>.qc plus its SMD files. The VVD and VTX are looked up
// next to the MDL (any letter case). Throws std::runtime_error on failure.
DecompileResult decompile(const std::filesystem::path& mdl, const std::filesystem::path& outDir,
                          const DecompileOptions& options = {}, const LogFn& log = {});

// Finds `name` inside `dir` ignoring letter case; empty path when absent.
std::filesystem::path findFileNoCase(const std::filesystem::path& dir, std::string_view name);

}  // namespace czd
