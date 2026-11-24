// SPDX-License-Identifier: CC-BY-SA-3.0
// crowbarzd-cli - command-line compile / decompile.
#include <cstring>
#include <iostream>

#include "czd/batch.hpp"
#include "czd/compile.hpp"
#include "czd/decompile.hpp"
#include "czd/model.hpp"

namespace {

int usage() {
  std::cerr << "CrowbarZD command-line tool\n\n"
               "  crowbarzd-cli decompile <model.mdl | folder> [-r] [-o <folder>] [--flat | --folder-per-model]\n"
               "                          [--no-definebones] [--no-lods] [--no-anims] [--no-physics] [--no-procedural] [--no-flexes]\n"
               "  crowbarzd-cli compile <model.qc | folder> [-r] -game <game folder> [-studiomdl <binary>] [-verbose]\n"
               "                        [-o <work folder> | -subfolder <name>] [-- extra studiomdl args]\n"
               "  crowbarzd-cli info <model.mdl>\n\n"
               "  A folder works on every MDL or QC file in it; -r includes the folders below it.\n"
               "  Decompiling a folder gives each model its own folder unless --flat is given.\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string cmd = argv[1];
  try {
    if (cmd == "info") {
      const czd::MdlFile m = czd::readMdl(argv[2]);
      std::cout << "name: " << m.name << "\nversion: " << m.version << "\nflags: 0x" << std::hex << m.flags << std::dec << "\nbones: " << m.bones.size()
                << "\nbody parts: " << m.bodyParts.size() << "\nmaterials: " << m.textures.size() << "\nanimations: " << m.anims.size() << "\nsequences: " << m.sequences.size()
                << "\nattachments: " << m.attachments.size() << "\nik chains: " << m.ikChains.size() << "\n";
      size_t rules = 0, jiggles = 0, layers = 0;
      for (const auto& a : m.anims) rules += static_cast<size_t>(a.ikRuleCount);
      size_t procedural = 0;
      for (const auto& bone : m.bones) { jiggles += bone.jiggle ? 1 : 0; procedural += bone.procType != 0 && !bone.jiggle ? 1 : 0; }
      std::cout << "procedural bones: " << procedural << "\n";
      size_t eyes = 0, shapes = 0;
      for (const auto& bp : m.bodyParts)
        for (const auto& model : bp.models) {
          eyes += model.eyeballs.size();
          for (const auto& mesh : model.meshes) shapes += mesh.flexes.size();
        }
      std::cout << "eyeballs: " << eyes << "\nflex descriptions: " << m.flexDescs.size() << "\nmesh flexes: " << shapes << "\nflex controllers: " << m.flexControllers.size()
                << "\nflex rules: " << m.flexRules.size() << "\nmouths: " << m.mouths.size() << "\n";
      for (const auto& s : m.sequences) layers += s.layers.size();
      std::cout << "ik rules: " << rules << "\njiggle bones: " << jiggles << "\nsequence layers: " << layers << "\n";
      for (const auto& s : m.sequences) std::cout << "  sequence " << s.label << (s.activity.empty() ? "" : " (" + s.activity + ")") << "\n";
      return 0;
    }
    if (cmd == "decompile") {
      const std::filesystem::path input = argv[2];
      std::filesystem::path out;
      czd::DecompileOptions opt;
      const bool isFolder = std::filesystem::is_directory(input);
      czd::InputMode mode = isFolder ? czd::InputMode::Folder : czd::InputMode::File;
      bool folderPerModel = isFolder;
      for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "-r" && isFolder) mode = czd::InputMode::FolderRecursive;
        else if (a == "--flat") folderPerModel = false;
        else if (a == "--folder-per-model") folderPerModel = true;
        else if (a == "--no-definebones") opt.defineBones = false;
        else if (a == "--no-lods") opt.lods = false;
        else if (a == "--no-anims") opt.animations = false;
        else if (a == "--no-physics") opt.physics = false;
        else if (a == "--no-procedural") opt.proceduralBones = false;
        else if (a == "--no-flexes") opt.flexes = false;
        else return usage();
      }
      if (out.empty()) out = isFolder ? input / "decompiled" : input.parent_path() / (input.stem().string() + "_decompiled");
      const auto models = czd::collectInputs(input, mode, ".mdl");
      if (models.empty()) { std::cerr << "ERROR: no MDL files found in " << input.string() << "\n"; return 1; }
      opt.prefixMeshNames = models.size() > 1 && !folderPerModel;
      int failed = 0;
      for (const auto& mdl : models) {
        try {
          czd::decompile(mdl, czd::decompileOutputDir(out, input, mode, mdl, folderPerModel), opt, [](std::string_view line) { std::cout << line << "\n"; });
        } catch (const std::exception& e) {
          std::cerr << "ERROR: " << mdl.string() << ": " << e.what() << "\n";
          ++failed;
        }
      }
      if (models.size() > 1) std::cout << "Decompiled " << models.size() - failed << " of " << models.size() << " model(s)\n";
      return failed ? 1 : 0;
    }
    if (cmd == "compile") {
      czd::CompileOptions opt;
      const std::filesystem::path input = std::filesystem::absolute(argv[2]);
      const bool isFolder = std::filesystem::is_directory(input);
      czd::InputMode mode = isFolder ? czd::InputMode::Folder : czd::InputMode::File;
      opt.studiomdl = czd::findStudiomdl();
      for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-game" && i + 1 < argc) opt.gameDir = std::filesystem::absolute(argv[++i]);
        else if (a == "-studiomdl" && i + 1 < argc) opt.studiomdl = argv[++i];
        else if (a == "-verbose") opt.verbose = true;
        else if (a == "-r" && isFolder) mode = czd::InputMode::FolderRecursive;
        else if (a == "-o" && i + 1 < argc) { opt.outputMode = czd::OutputMode::WorkFolder; opt.outputFolder = std::filesystem::absolute(argv[++i]); }
        else if (a == "-subfolder" && i + 1 < argc) { opt.outputMode = czd::OutputMode::QcSubfolder; opt.outputFolder = argv[++i]; }
        else if (a == "--") { opt.extraArgs.assign(argv + i + 1, argv + argc); break; }
        else return usage();
      }
      const auto qcs = czd::collectInputs(input, mode, ".qc");
      if (qcs.empty()) { std::cerr << "ERROR: no QC files found in " << input.string() << "\n"; return 1; }
      int failed = 0, lastCode = 0;
      for (const auto& qc : qcs) {
        opt.qc = qc;
        const czd::Preflight pf = czd::preflight(opt);
        for (const auto& w : pf.warnings) std::cout << "WARNING: " << w << "\n";
        for (const auto& e : pf.errors) std::cerr << "ERROR: " << e << "\n";
        if (!pf.errors.empty()) { ++failed; lastCode = 1; continue; }
        bool sawError = false;  // some studiomdl builds exit with 0 even after an error
        const auto print = [&](std::string_view line) {
          sawError = sawError || line.starts_with("ERROR");
          std::cout << line << "\n";
        };
        czd::OutputRedirect redirect(opt);
        redirect.begin(print);
        int code = czd::runCompile(opt, print);
        if (code == 0 && sawError) code = 1;
        redirect.finish(code == 0, print);
        if (code < 0) { std::cerr << "ERROR: could not start " << opt.studiomdl.string() << "\n"; return 1; }
        if (code != 0) { ++failed; lastCode = code; }
      }
      if (qcs.size() > 1) std::cout << "Compiled " << qcs.size() - failed << " of " << qcs.size() << " QC file(s)\n";
      return lastCode;
    }
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
  return usage();
}
