// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - compiling a QC with studiomdl (studiomdl++ or any compatible build).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace czd {

// Where the compiled model files end up.
enum class OutputMode {
  GameModels,   // leave them in <game folder>/models, where studiomdl writes them
  WorkFolder,   // move them to <outputFolder>/models/...
  QcSubfolder,  // move them to <QC's folder>/<outputFolder>/models/...
};

struct CompileOptions {
  std::filesystem::path studiomdl;  // the compiler binary
  std::filesystem::path gameDir;    // folder that holds gameinfo.txt
  std::filesystem::path qc;
  bool noP4 = true;
  bool verbose = false;
  std::vector<std::string> extraArgs;
  OutputMode outputMode = OutputMode::GameModels;
  std::filesystem::path outputFolder;  // work folder path, or the subfolder name for QcSubfolder
};

// The folder compiled files are moved to (it gets a "models" folder inside); empty for GameModels.
std::filesystem::path outputRoot(const CompileOptions& options);

// The QC's $modelname with forward slashes, or an empty string if it has none.
std::string qcModelName(const std::filesystem::path& qc);

// studiomdl always writes into <game folder>/models, so for the other output modes the files are
// moved afterwards, the way Crowbar does it. Unlike Crowbar, a model with the same name that was
// already in the game folder is set aside before the compile and put back afterwards.
class OutputRedirect {
 public:
  using Log = std::function<void(std::string_view line)>;
  explicit OutputRedirect(const CompileOptions& options);
  void begin(const Log& log);                  // call before the compiler starts
  void finish(bool succeeded, const Log& log);  // call after it exits
  // Where the .mdl is expected once finish() has run (also valid for GameModels).
  std::filesystem::path modelFile() const;

 private:
  std::vector<std::filesystem::path> compiledFiles() const;
  CompileOptions options_;
  std::string modelName_;
  std::filesystem::path sourceDir_, targetDir_;
  std::vector<std::filesystem::path> createdDirs_;                                     // game folders the compile had to create
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> setAside_;  // original, parked copy
};

// Arguments for the compiler, program name excluded.
std::vector<std::string> compileArgs(const CompileOptions& options);

// Checks the options and the files the QC refers to before running anything.
// Linux file names are case sensitive while QC files written on Windows often
// are not, so a reference that only matches in another letter case is reported.
struct Preflight {
  std::vector<std::string> errors;    // compile cannot work
  std::vector<std::string> warnings;  // compile will probably fail or misbehave
};
Preflight preflight(const CompileOptions& options);

// Removes ANSI colour escapes (studiomdl++ colours its warnings).
std::string stripAnsi(std::string_view text);

// Runs the compiler and streams its output line by line. Returns the exit code, or -1 if it could not start.
int runCompile(const CompileOptions& options, const std::function<void(std::string_view line)>& out);

// Best-effort discovery for first-run defaults.
std::filesystem::path findStudiomdl();
std::vector<std::filesystem::path> findGameDirs();

}  // namespace czd
