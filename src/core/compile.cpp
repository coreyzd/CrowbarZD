// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - studiomdl front end.
#include "czd/compile.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "czd/decompile.hpp"

namespace fs = std::filesystem;

namespace czd {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// QC tokens: quoted strings or bare words; // comments skipped.
std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (c == '"') {
      const size_t end = text.find('"', i + 1);
      out.push_back(text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
      i = end == std::string::npos ? text.size() : end + 1;
      continue;
    }
    size_t j = i;
    while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != '"') ++j;
    out.push_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

// Resolves a QC-relative path one component at a time. Returns 0 = exact, 1 = only with other letter case, 2 = missing.
int resolve(const fs::path& base, std::string rel, fs::path& found) {
  std::replace(rel.begin(), rel.end(), '\\', '/');
  fs::path cur = fs::path(rel).is_absolute() ? fs::path("/") : base;
  int state = 0;
  for (const fs::path& part : fs::path(rel).relative_path()) {
    if (part == ".") continue;
    if (part == "..") { cur = cur.parent_path(); continue; }
    std::error_code ec;
    if (fs::exists(cur / part, ec)) { cur /= part; continue; }
    const fs::path alt = findFileNoCase(cur, part.string());
    if (alt.empty()) return 2;
    cur = alt;
    state = 1;
  }
  found = cur;
  return state;
}

void checkQc(const fs::path& qc, Preflight& pf, std::set<std::string>& seen, int depth) {
  std::ifstream in(qc, std::ios::binary);
  if (!in) { pf.errors.push_back("cannot read " + qc.string()); return; }
  std::stringstream ss;
  ss << in.rdbuf();
  const fs::path dir = qc.parent_path().empty() ? fs::path(".") : qc.parent_path();
  for (const std::string& tok : tokenize(ss.str())) {
    const std::string ext = lower(fs::path(tok).extension().string());
    const bool source = ext == ".smd" || ext == ".vta" || ext == ".dmx" || ext == ".vrd";
    const bool include = ext == ".qci" || ext == ".qc";
    if ((!source && !include) || !seen.insert(tok).second) continue;
    fs::path found;
    const int state = resolve(dir, tok, found);
    if (state == 1)
      pf.warnings.push_back(qc.filename().string() + " refers to \"" + tok + "\" but the file on disk is \"" + fs::relative(found, dir).string() +
                            "\" (letter case differs; rename one of them)");
    else if (state == 2 && source)
      pf.warnings.push_back(qc.filename().string() + " refers to \"" + tok + "\", which was not found next to the QC (fine if $pushd or $cd is used)");
    if (state != 2 && include && depth < 8) checkQc(found, pf, seen, depth + 1);
  }
}

}  // namespace

fs::path outputRoot(const CompileOptions& o) {
  if (o.outputMode == OutputMode::WorkFolder) return o.outputFolder;
  if (o.outputMode == OutputMode::QcSubfolder) return o.qc.parent_path() / (o.outputFolder.empty() ? fs::path("compiled") : o.outputFolder);
  return {};
}

std::string qcModelName(const fs::path& qc) {
  std::ifstream in(qc, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::vector<std::string> tokens = tokenize(ss.str());
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (lower(tokens[i]) != "$modelname") continue;
    std::string name = tokens[i + 1];
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') name.erase(0, 1);
    if (lower(name).rfind("models/", 0) == 0) name.erase(0, 7);  // some QCs spell out the models folder
    return name;
  }
  return {};
}

OutputRedirect::OutputRedirect(const CompileOptions& options) : options_(options) {
  modelName_ = qcModelName(options_.qc);
  const fs::path sub = fs::path(modelName_).parent_path();
  sourceDir_ = options_.gameDir / "models" / sub;
  const fs::path root = outputRoot(options_);
  targetDir_ = root.empty() ? sourceDir_ : root / "models" / sub;
}

fs::path OutputRedirect::modelFile() const { return modelName_.empty() ? fs::path() : targetDir_ / fs::path(modelName_).filename(); }

// Files in the game's models folder that belong to this model: name.mdl, .vvd, .phy, .ani, .*.vtx
std::vector<fs::path> OutputRedirect::compiledFiles() const {
  std::vector<fs::path> out;
  const std::string stem = lower(fs::path(modelName_).stem().string()) + ".";
  std::error_code ec;
  for (fs::directory_iterator it(sourceDir_, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = lower(it->path().filename().string());
    const std::string ext = lower(it->path().extension().string());
    if (name.rfind(stem, 0) != 0 || !it->is_regular_file(ec)) continue;
    const std::string middle = name.substr(stem.size() - 1, name.size() - (stem.size() - 1) - ext.size());  // "" or ".dx90" etc.
    if (((ext == ".mdl" || ext == ".vvd" || ext == ".phy" || ext == ".ani") && middle.empty()) || (ext == ".vtx" && middle.find('.', 1) == std::string::npos)) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void OutputRedirect::begin(const Log& log) {
  if (options_.outputMode == OutputMode::GameModels) return;
  if (modelName_.empty()) {
    if (log) log("WARNING: the QC has no $modelname, so the compiled files cannot be moved; they stay in the game's models folder");
    return;
  }
  std::error_code ec;
  if (fs::equivalent(sourceDir_, targetDir_, ec)) return;
  // Remember which game folders do not exist yet, so empty ones can be removed afterwards.
  for (fs::path d = sourceDir_; d != options_.gameDir && d.has_relative_path() && !fs::exists(d, ec); d = d.parent_path()) createdDirs_.push_back(d);
  for (const fs::path& f : compiledFiles()) {
    const fs::path parked = f.string() + ".crowbarzd-keep";
    fs::rename(f, parked, ec);
    if (!ec) setAside_.emplace_back(f, parked);
  }
  if (!setAside_.empty() && log) log("CrowbarZD: set aside the existing \"" + modelName_ + "\" in the game's models folder; it is put back after the compile");
}

void OutputRedirect::finish(bool succeeded, const Log& log) {
  if (options_.outputMode == OutputMode::GameModels || modelName_.empty()) return;
  auto say = [&](const std::string& s) { if (log) log(s); };
  std::error_code ec;
  if (!fs::equivalent(sourceDir_, targetDir_, ec)) {
    const std::vector<fs::path> files = compiledFiles();
    if (succeeded && !files.empty()) {
      fs::create_directories(targetDir_, ec);
      if (ec) say("WARNING: could not create " + targetDir_.string() + ": " + ec.message());
      for (const fs::path& f : files) {
        const fs::path to = targetDir_ / f.filename();
        fs::rename(f, to, ec);
        if (ec) {  // different drive: copy, then delete
          ec.clear();
          fs::copy_file(f, to, fs::copy_options::overwrite_existing, ec);
          if (!ec) fs::remove(f, ec);
        }
        if (ec) say("WARNING: could not move " + f.string() + " to " + targetDir_.string() + ": " + ec.message());
        else say("CrowbarZD: moved " + f.filename().string() + " to " + targetDir_.string());
      }
    } else {
      for (const fs::path& f : files) fs::remove(f, ec);  // leftovers of a failed compile
    }
    for (const auto& [original, parked] : setAside_) {
      fs::rename(parked, original, ec);
      if (ec) say("WARNING: could not put back " + original.string() + " (it is saved as " + parked.filename().string() + "): " + ec.message());
    }
    for (const fs::path& d : createdDirs_)  // deepest first; remove() only succeeds on empty folders
      if (fs::is_empty(d, ec) && !ec) fs::remove(d, ec);
  }
  setAside_.clear();
  createdDirs_.clear();
}

std::vector<std::string> compileArgs(const CompileOptions& o) {
  std::vector<std::string> args;
  if (!o.gameDir.empty()) { args.push_back("-game"); args.push_back(o.gameDir.string()); }
  if (o.noP4) args.push_back("-nop4");
  if (o.verbose) args.push_back("-verbose");
  args.insert(args.end(), o.extraArgs.begin(), o.extraArgs.end());
  args.push_back(o.qc.string());
  return args;
}

Preflight preflight(const CompileOptions& o) {
  Preflight pf;
  std::error_code ec;
  if (o.studiomdl.empty() || !fs::is_regular_file(o.studiomdl, ec)) pf.errors.push_back("model compiler not found: " + (o.studiomdl.empty() ? std::string("(not set)") : o.studiomdl.string()));
  else if (access(o.studiomdl.c_str(), X_OK) != 0) pf.errors.push_back(o.studiomdl.string() + " is not executable (chmod +x it)");
  if (o.gameDir.empty()) pf.errors.push_back("game folder is not set");
  else if (findFileNoCase(o.gameDir, "gameinfo.txt").empty()) pf.errors.push_back("no gameinfo.txt in " + o.gameDir.string());
  if (o.outputMode == OutputMode::WorkFolder) {
    if (o.outputFolder.empty()) pf.errors.push_back("work folder is not set");
    else if (!o.outputFolder.is_absolute()) pf.errors.push_back("work folder must be a full path: " + o.outputFolder.string());
  }
  if (o.outputMode == OutputMode::QcSubfolder && (o.outputFolder.is_absolute() || o.outputFolder.string().find("..") != std::string::npos))
    pf.errors.push_back("the subfolder must be a plain folder name, not a path: " + o.outputFolder.string());
  if (o.qc.empty() || !fs::is_regular_file(o.qc, ec)) pf.errors.push_back("QC file not found: " + o.qc.string());
  else {
    std::set<std::string> seen;
    checkQc(o.qc, pf, seen, 0);
  }
  return pf;
}

std::string stripAnsi(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

int runCompile(const CompileOptions& o, const std::function<void(std::string_view)>& out) {
  std::vector<std::string> args = compileArgs(o);
  args.insert(args.begin(), o.studiomdl.string());
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  int fds[2];
  if (pipe(fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    if (!o.qc.parent_path().empty() && chdir(o.qc.parent_path().c_str()) != 0) _exit(126);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  std::string pending;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    pending.append(buf, static_cast<size_t>(n));
    for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1))
      if (out) out(stripAnsi(std::string_view(pending).substr(0, nl)));
  }
  close(fds[0]);
  if (!pending.empty() && out) out(stripAnsi(pending));
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

fs::path findStudiomdl() {
  std::vector<fs::path> dirs;
  if (const char* home = std::getenv("HOME")) dirs.push_back(fs::path(home) / ".local/opt/tools++");
  if (const char* path = std::getenv("PATH")) {
    std::stringstream ss(path);
    for (std::string d; std::getline(ss, d, ':');)
      if (!d.empty()) dirs.emplace_back(d);
  }
  std::error_code ec;
  for (const fs::path& d : dirs)
    for (const char* name : {"studiomdl++", "studiomdl"})
      if (fs::is_regular_file(d / name, ec) && access((d / name).c_str(), X_OK) == 0) return d / name;
  return {};
}

std::vector<fs::path> findGameDirs() {
  std::vector<fs::path> out;
  const char* home = std::getenv("HOME");
  if (!home) return out;
  std::set<fs::path> seen;
  std::error_code ec;
  auto consider = [&](const fs::path& dir) {
    if (findFileNoCase(dir, "gameinfo.txt").empty()) return;
    const fs::path real = fs::weakly_canonical(dir, ec);
    if (seen.insert(ec ? dir : real).second) out.push_back(dir);
  };
  for (const char* root : {".local/share/Steam/steamapps", ".steam/steam/steamapps", ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps"}) {
    const fs::path apps = fs::path(home) / root;
    for (fs::directory_iterator it(apps / "sourcemods", ec), end; !ec && it != end; it.increment(ec))
      if (it->is_directory(ec)) consider(it->path());
    for (fs::directory_iterator it(apps / "common", ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_directory(ec)) continue;
      std::error_code ec2;
      for (fs::directory_iterator sub(it->path(), ec2); !ec2 && sub != end; sub.increment(ec2))
        if (sub->is_directory(ec2)) consider(sub->path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace czd
