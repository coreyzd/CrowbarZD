// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - working on a whole folder of QC or MDL files.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace czd {

enum class InputMode {
  File,             // the one file given
  Folder,           // every matching file in the folder
  FolderRecursive,  // ... and in the folders below it
};

// The files to work on, sorted by path. `extension` includes the dot and is matched in any
// letter case (".mdl" also finds "Model.MDL"). Empty when nothing matches.
std::vector<std::filesystem::path> collectInputs(const std::filesystem::path& input, InputMode mode, std::string_view extension);

// Where one model is decompiled to. With subfolders, the model's folder below `input` is kept
// below `outRoot`; `folderPerModel` adds a folder named after the model.
std::filesystem::path decompileOutputDir(const std::filesystem::path& outRoot, const std::filesystem::path& input, InputMode mode,
                                         const std::filesystem::path& mdl, bool folderPerModel);

}  // namespace czd
