// SPDX-License-Identifier: CC-BY-SA-3.0
#include "czd/batch.hpp"

#include <algorithm>
#include <string>

namespace fs = std::filesystem;

namespace czd {
namespace {

bool hasExtension(const fs::path& p, std::string_view extension) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == extension;
}

}  // namespace

std::vector<fs::path> collectInputs(const fs::path& input, InputMode mode, std::string_view extension) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (mode == InputMode::File) {
    if (fs::is_regular_file(input, ec)) out.push_back(input);
    return out;
  }
  // A file given in a folder mode means "the folder it is in", as in Crowbar.
  const fs::path dir = fs::is_directory(input, ec) ? input : input.parent_path();
  if (mode == InputMode::Folder) {
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file(ec) && hasExtension(it->path(), extension)) out.push_back(it->path());
  } else {
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file(ec) && hasExtension(it->path(), extension)) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

fs::path decompileOutputDir(const fs::path& outRoot, const fs::path& input, InputMode mode, const fs::path& mdl, bool folderPerModel) {
  fs::path dir = outRoot;
  if (mode == InputMode::FolderRecursive) {
    std::error_code ec;
    const fs::path base = fs::is_directory(input, ec) ? input : input.parent_path();
    const fs::path rel = fs::relative(mdl.parent_path(), base, ec);
    if (!ec && !rel.empty() && rel != "." && *rel.begin() != "..") dir /= rel;
  }
  if (folderPerModel) dir /= mdl.stem();
  return dir;
}

}  // namespace czd
