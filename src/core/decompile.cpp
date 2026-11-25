// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - QC and SMD writers.
// Ported from Crowbar by ZeqMacaw (SourceSmdFile48.vb, SourceQcFile48.vb, SourceModel48.vb).
#include "czd/decompile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include "czd/model.hpp"

namespace fs = std::filesystem;

namespace czd {
namespace {

constexpr double kPi = 3.14159265358979323846;
double deg(double rad) { return rad * 180.0 / kPi; }

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string f6(double v) {  // Crowbar's "0.000000"
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.6f", std::isfinite(v) ? v : 0.0);
  return std::string(buf) == "-0.000000" ? "0.000000" : buf;
}

std::string g6(double v, int decimals = 6) {  // Crowbar's "0.######" after rounding
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, std::isfinite(v) ? v : 0.0);
  std::string s = buf;
  s.erase(s.find_last_not_of('0') + 1);
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s == "-0" || s.empty() ? "0" : s;
}

std::string baseName(std::string path) {  // file name without directory or extension
  std::replace(path.begin(), path.end(), '\\', '/');
  if (const auto slash = path.find_last_of('/'); slash != std::string::npos) path.erase(0, slash + 1);
  return path;
}
std::string stem(std::string path) {
  path = baseName(std::move(path));
  if (const auto dot = path.find_last_of('.'); dot != std::string::npos && dot > 0) path.erase(dot);
  return path;
}
std::string safeFileName(std::string s) {
  for (char& c : s)
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) c = '_';
  while (!s.empty() && (s.front() == '@' || s.front() == '.')) s.erase(0, 1);
  return s.empty() ? "unnamed" : s;
}

std::ofstream openOut(const fs::path& file) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + file.string());
  return out;
}

// ---- Animation frame evaluation (SourceSmdFile48.CalcAnimation) --------------

double extractAnimValue(int frame, const std::vector<int16_t>& v, double scale) {
  if (v.empty()) return 0;
  size_t i = 0;
  int k = frame;
  auto valid = [&](size_t at) { return static_cast<int>(static_cast<uint16_t>(v[at]) & 0xff); };
  auto total = [&](size_t at) { return static_cast<int>(static_cast<uint16_t>(v[at]) >> 8); };
  while (total(i) <= k) {
    k -= total(i);
    i += valid(i) + 1;
    if (i >= v.size() || total(i) == 0) return 0;
  }
  const size_t at = valid(i) > k ? i + k + 1 : i + valid(i);
  return at < v.size() ? v[at] * scale : 0;
}

struct BonePose {
  Vec3 pos, rot;
};

std::map<int, BonePose> calcFrame(const MdlFile& m, const MdlAnimDesc& anim, const MdlSequence* seq, int frame) {
  int section = 0, sectionFrame = frame;
  if (anim.sectionFrames != 0) {
    section = frame / anim.sectionFrames;
    sectionFrame = frame - section * anim.sectionFrames;
  }
  static const std::vector<MdlAnimTrack> kNone;
  const auto& tracks = section < static_cast<int>(anim.sections.size()) ? anim.sections[section] : kNone;
  const bool deltaAnim = anim.flags & MdlAnimDesc::DeltaFlag;
  std::map<int, BonePose> out;
  size_t t = 0;
  for (int b = 0; b < static_cast<int>(m.bones.size()); ++b) {
    const MdlBone& bone = m.bones[b];
    const double weight = seq && b < static_cast<int>(seq->boneWeights.size()) ? seq->boneWeights[b] : 1.0;
    if (t < tracks.size() && tracks[t].bone == b) {
      const MdlAnimTrack& tr = tracks[t++];
      if (weight <= 0) continue;
      const bool delta = tr.flags & MdlAnimTrack::Delta;
      BonePose p;
      if (tr.flags & (MdlAnimTrack::RawRot | MdlAnimTrack::RawRot2)) {
        p.rot = quatToEuler(tr.rawRot);
      } else if (tr.flags & MdlAnimTrack::AnimRot) {
        p.rot = {extractAnimValue(sectionFrame, tr.rotValues[0], bone.rotScale.x), extractAnimValue(sectionFrame, tr.rotValues[1], bone.rotScale.y),
                 extractAnimValue(sectionFrame, tr.rotValues[2], bone.rotScale.z)};
        if (!delta) { p.rot.x += bone.rot.x; p.rot.y += bone.rot.y; p.rot.z += bone.rot.z; }
      } else if (!delta) {
        p.rot = bone.rot;
      }
      if (tr.flags & MdlAnimTrack::RawPos) {
        p.pos = tr.rawPos;
      } else if (tr.flags & MdlAnimTrack::AnimPos) {
        p.pos = {extractAnimValue(sectionFrame, tr.posValues[0], bone.posScale.x), extractAnimValue(sectionFrame, tr.posValues[1], bone.posScale.y),
                 extractAnimValue(sectionFrame, tr.posValues[2], bone.posScale.z)};
        if (!delta) { p.pos.x += bone.pos.x; p.pos.y += bone.pos.y; p.pos.z += bone.pos.z; }
      } else if (!delta) {
        p.pos = bone.pos;
      }
      out[b] = p;
    } else if (weight > 0) {
      out[b] = deltaAnim ? BonePose{} : BonePose{bone.pos, bone.rot};
    }
  }
  return out;
}

// Root bones: add back the motion studiomdl extracted (AdjustPositionAndRotationByPiecewiseMovement),
// then undo the 90 degree turn it applies to animation data (AdjustPositionAndRotation).
void adjustRoot(const MdlAnimDesc& anim, int frame, BonePose& p) {
  if (!anim.movements.empty() && frame > 0) {
    int prevEnd = 0;
    Vec3 vecPos;
    double yaw = 0;
    for (const MdlMovement& mv : anim.movements) {
      if (frame <= mv.endFrame) {
        const double f = static_cast<double>(frame - prevEnd) / std::max(1, mv.endFrame - prevEnd);
        const double d = mv.v0 * f + 0.5 * (mv.v1 - mv.v0) * f * f;
        vecPos.x += d * mv.vector.x;
        vecPos.y += d * mv.vector.y;
        vecPos.z += d * mv.vector.z;
        yaw = yaw * (1 - f) + mv.angle * f;
        break;
      }
      prevEnd = mv.endFrame;
      vecPos = mv.position;
      yaw = mv.angle;
    }
    const double r = yaw * kPi / 180.0, s = std::sin(r), c = std::cos(r);
    const Vec3 in = p.pos;
    p.pos = {in.x * c - in.y * s + vecPos.x, in.x * s + in.y * c + vecPos.y, in.z + vecPos.z};
    p.rot.z += r;
  }
  p.pos = {p.pos.y, -p.pos.x, p.pos.z};
  p.rot.z -= kPi / 2;
}

// ---- SMD ---------------------------------------------------------------------

void writeSmdHeader(std::ostream& o, const MdlFile& m) {
  o << "// Created by CrowbarZD (SMD writer ported from Crowbar by ZeqMacaw)\nversion 1\nnodes\n";
  for (size_t i = 0; i < m.bones.size(); ++i) o << "  " << i << " \"" << m.bones[i].name << "\" " << m.bones[i].parent << "\n";
  o << "end\n";
}

void writeBindSkeleton(std::ostream& o, const MdlFile& m) {
  o << "skeleton\ntime 0\n";
  for (size_t i = 0; i < m.bones.size(); ++i) {
    const MdlBone& b = m.bones[i];
    o << "    " << i << " " << f6(b.pos.x) << " " << f6(b.pos.y) << " " << f6(b.pos.z) << " " << f6(b.rot.x) << " " << f6(b.rot.y) << " " << f6(b.rot.z) << "\n";
  }
  o << "end\n";
}

// Returns the number of triangles written.
int writeTriangles(std::ostream& o, const MdlFile& m, const VvdFile& vvd, const MdlModel& model, const VtxLod& lod, int& skipped) {
  const bool staticProp = m.flags & MdlFile::StaticProp;
  const int modelStart = model.vertexIndex / 48;
  int written = 0;
  o << "triangles\n";
  for (size_t mi = 0; mi < lod.meshes.size() && mi < model.meshes.size(); ++mi) {
    const MdlMesh& mesh = model.meshes[mi];
    const std::string material = mesh.material >= 0 && mesh.material < static_cast<int>(m.textures.size()) ? baseName(m.textures[mesh.material]) : "unknown_material";
    for (const VtxStripGroup& sg : lod.meshes[mi].stripGroups) {
      for (size_t i = 0; i + 2 < sg.indices.size(); i += 3) {
        const size_t order[3] = {i, i + 2, i + 1};  // Crowbar's winding
        std::string lines;
        bool ok = true;
        for (const size_t idx : order) {
          const uint16_t sv = sg.indices[idx];
          if (sv >= sg.meshVertex.size()) { ok = false; break; }
          const size_t vi = static_cast<size_t>(sg.meshVertex[sv]) + modelStart + mesh.vertexOffset;
          if (vi >= vvd.vertices.size()) { ok = false; break; }
          const Vertex& v = vvd.vertices[vi];
          lines += "  " + std::to_string(v.bone[0]) + " ";
          if (staticProp) lines += f6(v.pos[1]) + " " + f6(-v.pos[0]);
          else lines += f6(v.pos[0]) + " " + f6(v.pos[1]);
          lines += " " + f6(v.pos[2]) + " ";
          if (staticProp) lines += f6(v.normal[1]) + " " + f6(-v.normal[0]);
          else lines += f6(v.normal[0]) + " " + f6(v.normal[1]);
          lines += " " + f6(v.normal[2]) + " " + f6(v.uv[0]) + " " + f6(1.0 - v.uv[1]) + " " + std::to_string(std::min<int>(v.boneCount, 3));
          for (int k = 0; k < std::min<int>(v.boneCount, 3); ++k) lines += " " + std::to_string(v.bone[k]) + " " + f6(v.weight[k]);
          lines += "\n";
        }
        if (!ok) { ++skipped; continue; }
        o << material << "\n" << lines;
        ++written;
      }
    }
  }
  o << "end\n";
  return written;
}

void writeAnimSmd(const fs::path& file, const MdlFile& m, const MdlAnimDesc& anim, const MdlSequence* seq) {
  auto o = openOut(file);
  writeSmdHeader(o, m);
  o << "skeleton\n";
  for (int frame = 0; frame < std::max(anim.frameCount, 1); ++frame) {
    o << "time " << frame << "\n";
    for (auto& [bone, pose] : calcFrame(m, anim, seq, frame)) {
      BonePose p = pose;
      if (m.bones[bone].parent == -1) adjustRoot(anim, frame, p);
      o << "    " << bone << " " << f6(p.pos.x) << " " << f6(p.pos.y) << " " << f6(p.pos.z) << " " << f6(p.rot.x) << " " << f6(p.rot.y) << " " << f6(p.rot.z) << "\n";
    }
  }
  o << "end\n";
}

// ---- Flexes (SourceMdlFile48.CreateFlexFrameList, SourceVtaFile48) -----------

// One frame of the VTA file: a flex shape, gathered from every mesh that has a piece of it.
struct FlexFrame {
  std::string name;         // flex description, e.g. "smile"
  int desc = 0, partner = 0;
  double target[4] = {0, 0, 0, 0};
  double split = 1;         // flexpair blend width, estimated from the stored left/right weights
  std::vector<std::pair<const MdlFlex*, int>> parts;  // flex, index of its mesh's first vertex in the VVD
};

// Frame 0 is the base shape; real frames start at 1.
std::vector<FlexFrame> buildFlexFrames(const MdlFile& m, const VvdFile& vvd) {
  std::vector<FlexFrame> frames(1);
  for (const auto& bp : m.bodyParts)
    for (const auto& model : bp.models)
      for (const auto& mesh : model.meshes)
        for (const MdlFlex& flex : mesh.flexes) {
          if (flex.desc < 0 || flex.desc >= static_cast<int>(m.flexDescs.size())) continue;
          FlexFrame* frame = nullptr;
          for (size_t i = 1; i < frames.size() && !frame; ++i)
            if (frames[i].desc == flex.desc && std::equal(flex.target, flex.target + 4, frames[i].target)) frame = &frames[i];
          if (!frame) {
            frames.emplace_back();
            frame = &frames.back();
            frame->name = m.flexDescs[flex.desc];
            frame->desc = flex.desc;
            frame->partner = flex.partner > 0 && flex.partner < static_cast<int>(m.flexDescs.size()) ? flex.partner : 0;
            std::copy(flex.target, flex.target + 4, frame->target);
          }
          frame->parts.emplace_back(&flex, model.vertexIndex / 48 + mesh.vertexOffset);
        }
  // A flex pair is one shape the compiler split into a left and a right half, fading from one to
  // the other across "split" units either side of the centre line. The model keeps only each
  // vertex's left share (0..255), so the width is recovered by trying widths and keeping the one
  // whose predicted shares match the stored ones best:
  // share = 1 - smoothstep(t), t = (split - x) / (2 * split), clamped outside +/- split.
  for (size_t i = 1; i < frames.size(); ++i) {
    if (!frames[i].partner) continue;
    std::vector<std::pair<double, double>> samples;  // vertex x, stored share 0..1
    for (const auto& [flex, start] : frames[i].parts)
      for (const MdlVertAnim& va : flex->verts)
        if (const size_t vi = static_cast<size_t>(start + va.index); vi < vvd.vertices.size()) samples.emplace_back(vvd.vertices[vi].pos[0] + va.delta[0], va.side / 255.0);  // the compiler tests the moved position
    double best = 1, bestError = -1;
    for (int step = 1; step <= 1000 && !samples.empty(); ++step) {
      const double split = step * 0.01;
      double error = 0;
      for (const auto& [x, share] : samples) {
        const double t = std::clamp((split - x) / (2 * split), 0.0, 1.0);
        const double predicted = 1.0 - (3 * t * t - 2 * t * t * t);
        error += (predicted - share) * (predicted - share);
      }
      if (bestError < 0 || error < bestError - 1e-12) { bestError = error; best = split; }
    }
    frames[i].split = best;
  }
  return frames;
}

void writeVta(const fs::path& file, const MdlFile& m, const VvdFile& vvd, const std::vector<FlexFrame>& frames) {
  auto o = openOut(file);
  writeSmdHeader(o, m);
  o << "skeleton\n  time 0 # basis shape key\n";
  for (size_t i = 1; i < frames.size(); ++i) o << "  time " << i << " # " << frames[i].name << (frames[i].partner ? "+" + m.flexDescs[frames[i].partner] : "") << "\n";
  o << "end\nvertexanimation\n  time 0 # basis shape key\n";
  auto line = [&](size_t index, const float* pos, const float* normal) {
    o << "    " << index << " " << f6(pos[0]) << " " << f6(pos[1]) << " " << f6(pos[2]) << " " << f6(normal[0]) << " " << f6(normal[1]) << " " << f6(normal[2]) << "\n";
  };
  for (size_t v = 0; v < vvd.vertices.size(); ++v) line(v, vvd.vertices[v].pos, vvd.vertices[v].normal);
  for (size_t i = 1; i < frames.size(); ++i) {
    o << "  time " << i << " # " << frames[i].name << (frames[i].partner ? "+" + m.flexDescs[frames[i].partner] : "") << "\n";
    for (const auto& [flex, start] : frames[i].parts)
      for (const MdlVertAnim& va : flex->verts) {
        const size_t vi = static_cast<size_t>(start + va.index);
        if (vi >= vvd.vertices.size()) continue;
        const Vertex& base = vvd.vertices[vi];
        const float pos[3] = {base.pos[0] + va.delta[0], base.pos[1] + va.delta[1], base.pos[2] + va.delta[2]};
        const float normal[3] = {base.normal[0] + va.normalDelta[0], base.normal[1] + va.normalDelta[1], base.normal[2] + va.normalDelta[2]};
        line(vi, pos, normal);
      }
  }
  o << "end\n";
}

// Common.GetFlexRule: turns a rule's reverse-Polish operations back into "%name = expression".
// `approximate` is set when DMX-only operations had to be spelled out; `ok` is cleared when the
// rule could not be rebuilt.
std::string flexRuleText(const MdlFile& m, const MdlFlexRule& rule, bool& approximate, bool& ok) {
  struct Term {
    std::string text;
    int precedence;
  };
  std::vector<Term> stack;
  ok = true;
  approximate = false;
  auto pop = [&]() -> Term {
    if (stack.empty()) { ok = false; return {"0", 10}; }
    Term t = stack.back();
    stack.pop_back();
    return t;
  };
  auto wrap = [](const Term& t, int precedence) { return t.precedence < precedence ? "(" + t.text + ")" : t.text; };
  auto controller = [&](int i) { return i >= 0 && i < static_cast<int>(m.flexControllers.size()) ? m.flexControllers[i].name : (ok = false, std::string("unknown")); };
  using Op = MdlFlexRule::Op;
  for (const Op& op : rule.ops) {
    switch (op.op) {
      case Op::Const: stack.push_back({g6(op.value), 10}); break;
      case Op::Fetch1: stack.push_back({controller(op.index), 10}); break;
      case Op::Fetch2:
        if (op.index >= 0 && op.index < static_cast<int>(m.flexDescs.size())) stack.push_back({"%" + m.flexDescs[op.index], 10});
        else ok = false;
        break;
      case Op::Add: { const Term r = pop(), l = pop(); stack.push_back({l.text + " + " + r.text, 1}); break; }
      case Op::Sub: { const Term r = pop(), l = pop(); stack.push_back({l.text + " - " + wrap(r, 2), 1}); break; }
      case Op::Mul: { const Term r = pop(), l = pop(); stack.push_back({wrap(l, 2) + " * " + wrap(r, 2), 2}); break; }
      case Op::Div: { const Term r = pop(), l = pop(); stack.push_back({wrap(l, 2) + " / " + wrap(r, 3), 2}); break; }
      case Op::Neg: { const Term r = pop(); stack.push_back({"-" + wrap(r, 10), 10}); break; }
      case Op::Max: { const Term r = pop(), l = pop(); stack.push_back({"max(" + l.text + ", " + r.text + ")", 10}); break; }
      case Op::Min: { const Term r = pop(), l = pop(); stack.push_back({"min(" + l.text + ", " + r.text + ")", 10}); break; }
      case Op::TwoWay0: stack.push_back({"(1 - (min(max(" + controller(op.index) + " + 1, 0), 1)))", 10}); approximate = true; break;
      case Op::TwoWay1: stack.push_back({"(min(max(" + controller(op.index) + ", 0), 1))", 10}); approximate = true; break;
      case Op::Combo:
      case Op::Dominate: {
        std::string product = pop().text;
        for (int k = 2; k <= op.index; ++k) product += " * " + pop().text;
        if (op.op == Op::Dominate) product = pop().text + " * (1 - " + product + ")";
        stack.push_back({"(" + product + ")", 10});
        approximate = true;
        break;
      }
      default: ok = false;  // NWay and the DMX eyelid operations have no QC spelling worth trusting
    }
  }
  if (stack.size() != 1) ok = false;
  const std::string name = rule.flexDesc >= 0 && rule.flexDesc < static_cast<int>(m.flexDescs.size()) ? m.flexDescs[rule.flexDesc] : "unknown";
  return "%" + name + " = " + (stack.empty() ? std::string("0") : stack.back().text);
}

// ---- Physics SMD (SourceSmdFile48.WriteTrianglesSectionForPhysics) ----------

struct Mat34 {
  Vec3 c0, c1, c2, c3;  // columns, the way Crowbar's MathModule passes matrices around
};
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Mat34 angleMatrix(double pitch, double yaw, double roll) {
  const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch), sr = std::sin(roll), cr = std::cos(roll);
  return {{cp * cy, cp * sy, -sp}, {sr * sp * cy + cr * -sy, sr * sp * sy + cr * cy, sr * cp}, {cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp}, {}};
}
Mat34 matrixInvert(const Mat34& m) {
  Mat34 o;
  o.c0 = {m.c0.x, m.c1.x, m.c2.x};
  o.c1 = {m.c0.y, m.c1.y, m.c2.y};
  o.c2 = {m.c0.z, m.c1.z, m.c2.z};
  o.c3 = {-dot(m.c3, m.c0), -dot(m.c3, m.c1), -dot(m.c3, m.c2)};  // -R^T * t
  return o;
}
Vec3 vectorTransform(const Vec3& v, const Mat34& m) {
  return {dot(v, {m.c0.x, m.c1.x, m.c2.x}) + m.c3.x, dot(v, {m.c0.y, m.c1.y, m.c2.y}) + m.c3.y, dot(v, {m.c0.z, m.c1.z, m.c2.z}) + m.c3.z};
}
Vec3 vectorITransform(const Vec3& v, const Mat34& m) {
  const Vec3 t{v.x - m.c3.x, v.y - m.c3.y, v.z - m.c3.z};
  return {dot(t, m.c0), dot(t, m.c1), dot(t, m.c2)};
}
Mat34 poseToBone(const MdlBone& b) {
  const auto& p = b.poseToBone;
  return {{p[0], p[4], p[8]}, {p[1], p[5], p[9]}, {p[2], p[6], p[10]}, {p[3], p[7], p[11]}};
}

// A single solid is a $collisionmodel; several are $collisionjoints (one per ragdoll part).
bool isCollisionModel(const PhyFile& phy) { return phy.solids.size() == 1; }

std::vector<const PhyBlock*> phyBlocks(const PhyFile& phy, const std::string& name) {
  std::vector<const PhyBlock*> out;
  for (const PhyBlock& b : phy.blocks)
    if (b.name == name) out.push_back(&b);
  return out;
}

int writePhysicsSmd(const fs::path& file, const MdlFile& m, const PhyFile& phy) {
  auto o = openOut(file);
  writeSmdHeader(o, m);
  writeBindSkeleton(o, m);
  o << "triangles\n";
  const bool single = isCollisionModel(phy);
  const double scale = 1.0 / 0.0254;  // physics units are metres
  // A $collisionmodel is stored in world space as the model stands in frame 0 of its first
  // animation. To get back to reference-mesh space: undo that root bone transform, then apply
  // the root bone's bind transform.
  Mat34 worldToRoot{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}};
  int rootBone = 0;
  bool haveRoot = false;
  if (single && !m.anims.empty() && !m.bones.empty()) {
    const auto frame = calcFrame(m, m.anims[0], nullptr, 0);
    if (!frame.empty()) {
      rootBone = frame.begin()->first;
      const BonePose& p = frame.begin()->second;
      Mat34 rootToWorld = angleMatrix(p.rot.y, p.rot.z, p.rot.x);  // RadianEuler x,y,z -> pitch,yaw,roll
      rootToWorld.c3 = p.pos;
      worldToRoot = matrixInvert(rootToWorld);
      haveRoot = true;
    }
  }
  const auto solids = phyBlocks(phy, "solid");
  int triangles = 0;
  for (size_t s = 0; s < phy.solids.size(); ++s) {
    const PhySolid& solid = phy.solids[s];
    for (const PhyConvex& piece : solid.pieces) {
      int boneIndex = piece.bone;
      if (s < solids.size())
        for (size_t b = 0; b < m.bones.size(); ++b)
          if (m.bones[b].name == solids[s]->get("name")) { boneIndex = static_cast<int>(b); break; }
      if (boneIndex < 0 && single && !m.bones.empty()) boneIndex = 0;  // props name their solid "phys", not after a bone
      if (boneIndex < 0 || boneIndex >= static_cast<int>(m.bones.size())) continue;
      const Mat34 bone = poseToBone(m.bones[boneIndex]);
      // Transform this piece's corners once, then give every corner one normal (the average of
      // its faces). studiomdl welds collision vertices by position and normal, so a corner that
      // carried a different normal per face would split the piece into flat fragments.
      std::vector<Vec3> pos(piece.vertices.size()), normal(piece.vertices.size());
      for (size_t i = 0; i < piece.vertices.size(); ++i) {
        const Vec3& in = piece.vertices[i];
        const Vec3 world{scale * in.x, scale * in.z, scale * -in.y};  // physics axes -> Source axes
        if (!single) {
          pos[i] = vectorITransform(world, bone);  // ragdoll parts are stored in their bone's space
        } else if (m.version <= 47 || !haveRoot) {
          pos[i] = {world.y, -world.x, world.z};  // older compilers only apply the usual quarter turn
        } else {
          pos[i] = vectorITransform(vectorTransform(world, worldToRoot), poseToBone(m.bones[rootBone]));
          if (m.flags & MdlFile::StaticProp) pos[i] = {pos[i].y, -pos[i].x, pos[i].z};  // same turn the reference mesh gets
        }
      }
      auto valid = [&](const std::array<uint16_t, 3>& f) { return f[0] < pos.size() && f[1] < pos.size() && f[2] < pos.size(); };
      for (const auto& face : piece.faces) {
        if (!valid(face)) continue;
        const Vec3 &p0 = pos[face[0]], &p1 = pos[face[1]], &p2 = pos[face[2]];
        const Vec3 a{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, c{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        Vec3 n{a.y * c.z - a.z * c.y, a.z * c.x - a.x * c.z, a.x * c.y - a.y * c.x};
        if (const double len = std::sqrt(dot(n, n)); len > 1e-12) n = {n.x / len, n.y / len, n.z / len};
        for (const uint16_t i : face) normal[i] = {normal[i].x + n.x, normal[i].y + n.y, normal[i].z + n.z};
      }
      for (Vec3& n : normal)
        if (const double len = std::sqrt(dot(n, n)); len > 1e-12) n = {n.x / len, n.y / len, n.z / len};
      for (const auto& face : piece.faces) {
        if (!valid(face)) continue;
        o << "phy\n";
        for (const uint16_t i : face)
          o << "  " << boneIndex << " " << f6(pos[i].x) << " " << f6(pos[i].y) << " " << f6(pos[i].z) << " " << f6(normal[i].x) << " " << f6(normal[i].y) << " " << f6(normal[i].z) << " 0 0\n";
        ++triangles;
      }
    }
  }
  o << "end\n";
  return triangles;
}

// ---- QC ----------------------------------------------------------------------

std::string q(const std::string& s) { return "\"" + s + "\""; }

void matrixToDegrees(const std::array<float, 12>& m, double& ax, double& ay, double& az) {  // MathModule.ConvertRotationMatrixToDegrees
  auto r6 = [](double v) { return std::round(v * 1e6) / 1e6; };
  const double m0 = m[0], m1 = m[4], m2 = std::clamp<double>(m[8], -1, 1), m3 = m[1], m4 = m[5], m5 = m[9], m8 = m[10];
  ay = -std::asin(r6(m2));
  const double c = std::cos(ay);
  ay = deg(ay);
  if (std::fabs(c) > 0.005) {
    ax = deg(std::atan2(r6(-m5) / c, r6(m8) / c));
    az = deg(std::atan2(r6(-m1) / c, r6(m0) / c));
  } else {
    ax = 0;
    az = deg(std::atan2(r6(m3), r6(m4)));
  }
}

std::string motionFlags(uint32_t f) {  // the axes Crowbar writes after "walkframe"
  static const std::pair<uint32_t, const char*> names[] = {{0x40, "LX"}, {0x80, "LY"}, {0x100, "LZ"}, {0x200, "LXR"}, {0x400, "LYR"}, {0x800, "LZR"}};
  std::string s;
  for (const auto& [bit, name] : names)
    if (f & bit) s += std::string(" ") + name;
  return s;
}

// SourceQcFile48.WriteCmdListOptions: one "ikrule" line per rule of the animation.
std::string ikRuleLines(const MdlFile& m, const MdlAnimDesc& a) {
  std::string out;
  const int endFrame = std::max(a.frameCount - 1, 0);
  for (const MdlIkRule& r : a.ikRules) {
    if (r.chain < 0 || r.chain >= static_cast<int>(m.ikChains.size())) continue;
    std::string line = "\tikrule \"" + m.ikChains[r.chain].name + "\"";
    switch (r.type) {
      case MdlIkRule::Self:
        line += " touch \"" + (r.bone >= 0 && r.bone < static_cast<int>(m.bones.size()) ? m.bones[r.bone].name : std::string()) + "\"";
        break;
      case MdlIkRule::Ground: line += " footstep"; break;
      case MdlIkRule::Release: line += " release"; break;
      case MdlIkRule::Attachment: line += " attachment \"" + r.attachment + "\""; break;
      case MdlIkRule::Unlatch: line += " unlatch"; break;
      default: continue;  // IK_WORLD has no QC option
    }
    bool wrapped = false;  // a range that runs past the last frame needs "usesequence"
    auto frame = [&](double fraction) {
      int f = static_cast<int>(std::lround(fraction * endFrame));
      while (endFrame > 0 && f > endFrame) { f -= endFrame; wrapped = true; }
      return std::to_string(f);
    };
    line += " contact " + frame(r.contact);
    if (r.type == MdlIkRule::Ground || r.type == MdlIkRule::Self) line += " floor " + g6(r.floor, 2) + " height " + g6(r.height, 2) + " radius " + g6(r.radius, 2);
    line += " range " + frame(r.start) + " " + frame(r.peak) + " " + frame(r.tail) + " " + frame(r.end);
    line += " target " + std::to_string(r.slot);
    if (wrapped || !a.movements.empty()) line += " usesequence";
    out += line + "\n";
  }
  return out;
}

// SourceVrdFile48.WriteCommands: the VRD file $proceduralbones reads. Bone names lose everything
// up to their first dot ("ValveBiped."), which is how VRD files name bones.
bool writeVrd(const fs::path& file, const MdlFile& m) {
  auto shortName = [](const std::string& name) {
    const auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
  };
  auto bone = [&](int i) -> const MdlBone* { return i >= 0 && i < static_cast<int>(m.bones.size()) ? &m.bones[i] : nullptr; };
  std::string text;
  for (const MdlBone& b : m.bones) {
    if (!b.quatInterp) continue;
    const MdlBone* parent = bone(b.parent);
    const MdlBone* control = bone(b.quatInterp->controlBone);
    const MdlBone* controlParent = control ? bone(control->parent) : nullptr;
    if (!parent || !control || !controlParent) continue;
    text += "\n<helper> " + shortName(b.name) + " " + shortName(parent->name) + " " + shortName(controlParent->name) + " " + shortName(control->name) + "\n<basepos> 0 0 0\n";
    for (const auto& t : b.quatInterp->triggers) {
      const Vec3 trigger = quatToEuler(t.trigger), rot = quatToEuler(t.quat);
      text += "<trigger> " + g6(t.invTolerance != 0 ? deg(1.0 / t.invTolerance) : 0, 4) + " " + g6(deg(trigger.x), 4) + " " + g6(deg(trigger.y), 4) + " " + g6(deg(trigger.z), 4) + " " +
              g6(deg(rot.x), 4) + " " + g6(deg(rot.y), 4) + " " + g6(deg(rot.z), 4) + " " + g6(t.pos.x) + " " + g6(t.pos.y) + " " + g6(t.pos.z) + "\n";
    }
  }
  if (text.empty()) return false;
  auto o = openOut(file);
  o << "// Created by CrowbarZD (VRD writer ported from Crowbar by ZeqMacaw)\n" << text;
  return true;
}

void writeJiggle(std::ostream& o, const MdlBone& b) {  // SourceQcFile48.WriteJiggleBoneCommand
  const MdlJiggle& j = *b.jiggle;
  auto constraints = [&] {
    if (j.flags & MdlJiggle::PitchConstraint)
      o << "\t\tpitch_constraint " << g6(deg(j.minPitch), 4) << " " << g6(deg(j.maxPitch), 4) << "\n\t\tpitch_friction " << g6(j.pitchFriction) << "\n\t\tpitch_bounce " << g6(j.pitchBounce) << "\n";
    if (j.flags & MdlJiggle::YawConstraint)
      o << "\t\tyaw_constraint " << g6(deg(j.minYaw), 4) << " " << g6(deg(j.maxYaw), 4) << "\n\t\tyaw_friction " << g6(j.yawFriction) << "\n\t\tyaw_bounce " << g6(j.yawBounce) << "\n";
    if (j.flags & MdlJiggle::AngleConstraint) o << "\t\tangle_constraint " << g6(deg(j.angleLimit), 4) << "\n";
  };
  o << "$jigglebone " << q(b.name) << "\n{\n";
  if (j.flags & MdlJiggle::Flexible) {
    o << "\tis_flexible\n\t{\n\t\tlength " << g6(j.length) << "\n\t\ttip_mass " << g6(j.tipMass) << "\n\t\tpitch_stiffness " << g6(j.pitchStiffness) << "\n\t\tpitch_damping "
      << g6(j.pitchDamping) << "\n\t\tyaw_stiffness " << g6(j.yawStiffness) << "\n\t\tyaw_damping " << g6(j.yawDamping) << "\n";
    if (!(j.flags & MdlJiggle::LengthConstraint)) o << "\t\tallow_length_flex\n";
    o << "\t\talong_stiffness " << g6(j.alongStiffness) << "\n\t\talong_damping " << g6(j.alongDamping) << "\n";
    constraints();
    o << "\t}\n";
  }
  if (j.flags & MdlJiggle::Rigid) {
    o << "\tis_rigid\n\t{\n\t\tlength " << g6(j.length) << "\n\t\ttip_mass " << g6(j.tipMass) << "\n";
    constraints();
    o << "\t}\n";
  }
  if (j.flags & MdlJiggle::BaseSpring) {
    o << "\thas_base_spring\n\t{\n\t\tbase_mass " << g6(j.baseMass) << "\n\t\tstiffness " << g6(j.baseStiffness) << "\n\t\tdamping " << g6(j.baseDamping) << "\n\t\tleft_constraint "
      << g6(j.baseMinLeft) << " " << g6(j.baseMaxLeft) << "\n\t\tleft_friction " << g6(j.baseLeftFriction) << "\n\t\tup_constraint " << g6(j.baseMinUp) << " " << g6(j.baseMaxUp)
      << "\n\t\tup_friction " << g6(j.baseUpFriction) << "\n\t\tforward_constraint " << g6(j.baseMinForward) << " " << g6(j.baseMaxForward) << "\n\t\tforward_friction "
      << g6(j.baseForwardFriction) << "\n\t}\n";
  }
  o << "}\n";
}

struct QcContext {
  const MdlFile& m;
  std::vector<std::vector<std::string>> bodyFiles;       // [bodypart][model] reference SMD name, "" = blank
  std::vector<std::string> lodBlocks;                    // ready-made $lod text
  std::map<int, std::string> animFiles;                  // animdesc index -> SMD path relative to the QC
  int modelBodyPart = -1;                                // body part written as $model (eyes, flexes)
  std::string vtaFile;                                   // set when a VTA file was written
  std::vector<FlexFrame> flexFrames;                     // [0] is the base shape
  std::vector<std::string> flexWarnings;
  std::string vrdFile;                                   // set when a VRD file was written
  const PhyFile* phy = nullptr;                          // set when a physics SMD was written
  std::string physicsFile;
};

std::string num(const std::string& text) {  // PHY text value -> QC number
  try { return g6(std::stod(text)); } catch (const std::exception&) { return "0"; }
}

// SourceQcFile48.WriteCollisionModelOrCollisionJointsCommand and WriteCollisionTextCommand.
void writeCollision(std::ostream& o, const PhyFile& phy, const std::string& smd) {
  const bool single = isCollisionModel(phy);
  const auto solids = phyBlocks(phy, "solid");
  const auto edit = phyBlocks(phy, "editparams");
  // Per-joint values are written only where they differ from the most common one.
  auto mostUsed = [&](const char* key) {
    std::map<double, int> counts;
    for (const PhyBlock* s : solids)
      if (s->has(key)) ++counts[std::atof(s->get(key).c_str())];
    double best = 0;
    int bestCount = 0;
    for (const auto& [value, count] : counts)
      if (count >= bestCount) { best = value; bestCount = count; }
    return best;
  };
  const double inertia = mostUsed("inertia"), damping = mostUsed("damping"), rotDamping = mostUsed("rotdamping");
  o << (single ? "$collisionmodel " : "$collisionjoints ") << q(smd) << "\n{\n";
  o << "\t$mass " << num(edit.empty() ? std::string("0") : edit[0]->get("totalmass", "0")) << "\n";
  o << "\t$inertia " << g6(inertia) << "\n\t$damping " << g6(damping) << "\n\t$rotdamping " << g6(rotDamping) << "\n";
  if (!edit.empty() && edit[0]->get("rootname").find_first_not_of(" \t") != std::string::npos) o << "\t$rootbone " << q(edit[0]->get("rootname")) << "\n";
  if (!edit.empty() && edit[0]->get("concave") == "1") {
    size_t pieces = 0;
    for (const PhySolid& s : phy.solids) pieces = std::max(pieces, s.pieces.size());
    o << "\t$concave\n\t$maxconvexpieces " << pieces << "\n";
  }
  if (!single) {
    std::map<int, const PhyBlock*> constraints;  // by child solid index
    for (const PhyBlock* c : phyBlocks(phy, "ragdollconstraint")) constraints[std::atoi(c->get("child", "-1").c_str())] = c;
    for (const PhyBlock* s : solids) {
      const std::string name = q(s->get("name"));
      if (s->has("massbias")) o << "\t$jointmassbias " << name << " " << num(s->get("massbias")) << "\n";
      if (s->has("damping") && std::atof(s->get("damping").c_str()) != damping) o << "\t$jointdamping " << name << " " << num(s->get("damping")) << "\n";
      if (s->has("inertia") && std::atof(s->get("inertia").c_str()) != inertia) o << "\t$jointinertia " << name << " " << num(s->get("inertia")) << "\n";
      if (s->has("rotdamping") && std::atof(s->get("rotdamping").c_str()) != rotDamping) o << "\t$jointrotdamping " << name << " " << num(s->get("rotdamping")) << "\n";
      if (const auto c = constraints.find(std::atoi(s->get("index", "-1").c_str())); c != constraints.end())
        for (const char* axis : {"x", "y", "z"})
          o << "\t$jointconstrain " << name << " " << axis << " limit " << num(c->second->get(std::string(axis) + "min")) << " " << num(c->second->get(std::string(axis) + "max")) << " "
            << num(c->second->get(std::string(axis) + "friction")) << "\n";
    }
    for (const PhyBlock* rules : phyBlocks(phy, "collisionrules")) {
      if (rules->get("selfcollisions", "1") == "0") { o << "\t$noselfcollisions\n"; continue; }
      for (const auto& [key, value] : rules->values) {
        if (key != "collisionpair") continue;
        const auto comma = value.find(',');
        if (comma == std::string::npos) continue;
        const size_t a = std::strtoul(value.c_str(), nullptr, 10), b = std::strtoul(value.c_str() + comma + 1, nullptr, 10);
        if (a < solids.size() && b < solids.size()) o << "\t$jointcollide " << q(solids[a]->get("name")) << " " << q(solids[b]->get("name")) << "\n";
      }
    }
    for (const PhyBlock* f : phyBlocks(phy, "animatedfriction"))
      o << "\t$animatedfriction " << num(f->get("animfrictionmin")) << " " << num(f->get("animfrictionmax")) << " " << num(f->get("animfrictiontimein")) << " "
        << num(f->get("animfrictiontimehold")) << " " << num(f->get("animfrictiontimeout")) << "\n";
  }
  o << "}\n\n";
  // Anything else in the PHY text (break pieces, vehicle data, ...) goes back in as $collisiontext.
  std::string other;
  for (const PhyBlock& b : phy.blocks)
    if (b.name != "solid" && b.name != "ragdollconstraint" && b.name != "collisionrules" && b.name != "animatedfriction" && b.name != "editparams") other += b.raw;
  if (!other.empty()) o << "$collisiontext\n{\n" << other << "}\n\n";
}

// The body of a $model block: eyeballs, eyelids, mouths and flexes
// (SourceQcFile48.WriteEyeballLines, WriteEyelidLines, WriteMouthLines, WriteGroupFlex).
void writeModelBlock(std::ostream& o, QcContext& c, const MdlModel& model) {
  const MdlFile& m = c.m;
  auto desc = [&](int i) { return i >= 0 && i < static_cast<int>(m.flexDescs.size()) ? m.flexDescs[i] : std::string("unknown"); };
  auto boneName = [&](int i) { return i >= 0 && i < static_cast<int>(m.bones.size()) ? m.bones[i].name : std::string(); };
  std::set<int> usedByFlex, usedByEyelid;

  std::vector<std::string> eyeNames;
  for (size_t e = 0; e < model.eyeballs.size(); ++e) {
    const MdlEyeball& eye = model.eyeballs[e];
    if (eye.bone < 0 || eye.bone >= static_cast<int>(m.bones.size())) { eyeNames.emplace_back(); continue; }
    const Vec3 pos = vectorITransform(eye.org, poseToBone(m.bones[eye.bone]));
    const double angle = deg(std::atan(eye.zOffset));
    eyeNames.push_back(e == 0 && angle > 0 ? "eye_right" : e == 1 && angle < 0 ? "eye_left" : "eye_" + std::to_string(e));
    // The eye's material is on the mesh flagged as this eyeball.
    std::string texture = "unknown_material";
    for (const MdlMesh& mesh : model.meshes)
      if (mesh.materialType == 1 && mesh.materialParam == static_cast<int>(e) && mesh.material >= 0 && mesh.material < static_cast<int>(m.textures.size())) texture = baseName(m.textures[mesh.material]);
    o << "\teyeball " << q(eyeNames.back()) << " " << q(boneName(eye.bone)) << " " << f6(pos.x) << " " << f6(pos.y) << " " << f6(pos.z) << " " << q(texture) << " " << g6(eye.radius * 2) << " "
      << g6(angle) << " \"iris_unused\" " << g6(eye.irisScale != 0 ? 1.0 / eye.irisScale : 1.0) << "\n";
  }

  // Eyelids: three VTA frames per lid (lowered, neutral, raised). Models compiled without eyelids
  // get a placeholder flex ("dummy_eyelid") in every slot, which is not written.
  auto frameFor = [&](int lid, int targetIndex, double value) {
    for (size_t i = 1; i < c.flexFrames.size(); ++i)
      if (c.flexFrames[i].desc == lid && c.flexFrames[i].target[targetIndex] == value) return i;
    return size_t{0};
  };
  for (size_t e = 0; e < model.eyeballs.size() && !c.vtaFile.empty(); ++e) {
    const MdlEyeball& eye = model.eyeballs[e];
    if (eye.upperLidFlexDesc == eye.upperFlexDesc[0] || eyeNames[e].empty()) continue;
    const struct { int lid; const int* descs; const double* targets; } lids[] = {{eye.upperLidFlexDesc, eye.upperFlexDesc, eye.upperTarget}, {eye.lowerLidFlexDesc, eye.lowerFlexDesc, eye.lowerTarget}};
    for (const auto& lid : lids) {
      const std::string name = desc(lid.lid);
      // The stored lid shapes were already cut to their own eye by the original "split". Split 0
      // applies no further cut; any other value would fade vertices near the nose a second time.
      const char* split = "0";
      o << "\teyelid " << name << " " << q(c.vtaFile) << " lowerer " << frameFor(lid.lid, 0, -11) << " " << g6(lid.targets[0], 2) << " neutral 0 " << g6(lid.targets[1], 2) << " raiser "
        << frameFor(lid.lid, 3, 11) << " " << g6(lid.targets[2], 2) << " split " << split << " eyeball " << q(eyeNames[e]) << "\n";
      usedByEyelid.insert(lid.lid);
      usedByFlex.insert(lid.lid);
      for (int x = 0; x < 3; ++x) usedByFlex.insert(lid.descs[x]);
    }
  }

  for (size_t i = 0; i < m.mouths.size(); ++i) {
    const MdlMouth& mouth = m.mouths[i];
    o << "\tmouth " << i << " " << q(desc(mouth.flexDesc)) << " " << q(boneName(mouth.bone)) << " " << g6(mouth.forward.x, 3) << " " << g6(mouth.forward.y, 3) << " " << g6(mouth.forward.z, 3) << "\n";
    usedByFlex.insert(mouth.flexDesc);
  }

  if (!c.vtaFile.empty() && c.flexFrames.size() > 1) {
    o << "\tflexfile " << q(c.vtaFile) << " {\n\t\tdefaultflex frame 0\n";
    for (size_t i = 1; i < c.flexFrames.size(); ++i) {
      const FlexFrame& f = c.flexFrames[i];
      usedByFlex.insert(f.desc);
      if (f.partner) usedByFlex.insert(f.partner);
      o << "\t\t" << (usedByEyelid.count(f.desc) ? "// Already in eyelid lines: " : "");
      // A pair is stored as "nameR" + "nameL"; the QC names it once, without the letter.
      if (f.partner) o << "flexpair " << q(f.name.substr(0, f.name.size() - 1)) << " " << g6(f.split) << " frame " << i << "\n";
      else o << "flex " << q(f.name) << " frame " << i << "\n";
    }
    o << "\t}\n";
  }
  for (const MdlFlexController& fc : m.flexControllers) o << "\tflexcontroller " << fc.type << " range " << g6(fc.min) << " " << g6(fc.max) << " " << q(fc.name) << "\n";

  // Flex descriptions that only exist as the result of a rule are local variables.
  std::set<int> usedByRule;
  for (const MdlFlexRule& rule : m.flexRules) {
    usedByRule.insert(rule.flexDesc);
    for (const auto& op : rule.ops)
      if (op.op == MdlFlexRule::Op::Fetch2) usedByRule.insert(op.index);
  }
  for (size_t i = 0; i < m.flexDescs.size(); ++i)
    if (usedByRule.count(static_cast<int>(i)) && !usedByFlex.count(static_cast<int>(i))) o << "\tlocalvar " << m.flexDescs[i] << "\n";
  int approximated = 0, failed = 0;
  for (const MdlFlexRule& rule : m.flexRules) {
    bool approximate = false, ok = true;
    const std::string text = flexRuleText(m, rule, approximate, ok);
    if (!ok) { o << "\t// " << text << " // could not be rebuilt\n"; ++failed; }
    else o << "\t" << text << (approximate ? " // approximation of a DMX-only operation" : "") << "\n";
    approximated += approximate && ok;
  }
  if (failed) c.flexWarnings.push_back(std::to_string(failed) + " flex rule(s) use operations that cannot be written in a QC; they are left in the QC as comments");
  if (approximated) c.flexWarnings.push_back(std::to_string(approximated) + " flex rule(s) use DMX-only operations and were written as approximations");
}

void writeQc(std::ostream& o, QcContext& c, const DecompileOptions& opt) {
  const MdlFile& m = c.m;
  const bool staticProp = m.flags & MdlFile::StaticProp;
  std::string modelName = m.name;
  std::replace(modelName.begin(), modelName.end(), '\\', '/');
  o << "// Created by CrowbarZD (QC writer ported from Crowbar by ZeqMacaw)\n\n$modelname " << q(modelName) << "\n\n";
  if (staticProp) o << "$staticprop\n\n";

  for (size_t b = 0; b < m.bodyParts.size(); ++b) {
    if (static_cast<int>(b) == c.modelBodyPart) {
      o << "$model " << q(m.bodyParts[b].name) << " " << q(c.bodyFiles[b][0]) << " {\n";
      writeModelBlock(o, c, m.bodyParts[b].models[0]);
      o << "}\n";
      continue;
    }
    o << "$bodygroup " << q(m.bodyParts[b].name) << "\n{\n";
    for (const std::string& file : c.bodyFiles[b]) o << (file.empty() ? "\tblank\n" : "\tstudio " + q(file) + "\n");
    o << "}\n";
  }
  for (const std::string& block : c.lodBlocks) o << "\n" << block;
  o << "\n";

  if (m.flags & MdlFile::ForceOpaque) o << "$opaque\n";
  if (m.flags & MdlFile::TranslucentTwoPass) o << "$mostlyopaque\n";
  if (m.flags & MdlFile::Obsolete) o << "$obsolete\n";
  if (m.flags & MdlFile::NoForcedFade) o << "$noforcedfade\n";
  if (m.flags & MdlFile::ForcePhonemeCrossfade) o << "$forcephonemecrossfade\n";
  if (m.flags & MdlFile::ConstantDirectionalLight) o << "$constantdirectionallight " << g6(m.constDirectionalLightDot / 255.0, 2) << "\n";
  if (m.flags & MdlFile::AmbientBoost) o << "$ambientboost\n";
  if (m.flags & MdlFile::DoNotCastShadows) o << "$donotcastshadows\n";
  if (m.flags & MdlFile::CastTextureShadows) o << "$casttextureshadows\n";

  if (!m.surfaceProp.empty()) o << "$surfaceprop " << q(m.surfaceProp) << "\n";
  if (m.contents != 1) {
    std::string names;
    if (m.contents == 0) names = " \"notsolid\"";
    if (m.contents & 0x1) names += " \"solid\"";
    if (m.contents & 0x8) names += " \"grate\"";
    if (m.contents & 0x2000000) names += " \"monster\"";
    if (m.contents & 0x20000000) names += " \"ladder\"";
    if (!names.empty()) o << "$contents" << names << "\n";
  }
  if (m.eyePos.x != 0 || m.eyePos.y != 0 || m.eyePos.z != 0) o << "$eyeposition " << g6(m.eyePos.y, 3) << " " << g6(-m.eyePos.x, 3) << " " << g6(m.eyePos.z, 3) << "\n";
  if (m.illumAttachment == 0) o << "$illumposition " << g6(m.illumPos.y, 3) << " " << g6(-m.illumPos.x, 3) << " " << g6(m.illumPos.z, 3) << "\n";
  o << "\n";

  for (std::string path : m.texturePaths) {
    std::replace(path.begin(), path.end(), '\\', '/');  // both compilers accept forward slashes
    o << "$cdmaterials " << q(path) << "\n";
  }
  if (m.texturePaths.empty()) o << "$cdmaterials \"\"\n";
  if (m.skinFamilies.size() > 1) {
    // Only columns a mesh actually uses: the compiler drops the others, with or without a skin entry.
    std::set<int> meshColumns;
    for (const auto& bp : m.bodyParts)
      for (const auto& md : bp.models)
        for (const auto& mesh : md.meshes) meshColumns.insert(mesh.material);
    std::vector<int> columns;
    for (int r = 0; r < m.skinRefCount; ++r)
      for (const auto& fam : m.skinFamilies)
        if (fam[r] != m.skinFamilies[0][r] && meshColumns.count(r)) { columns.push_back(r); break; }
    if (!columns.empty()) {
      o << "\n$texturegroup \"skinfamilies\"\n{\n";
      for (const auto& fam : m.skinFamilies) {
        o << "\t{";
        for (const int r : columns) o << " " << q(fam[r] >= 0 && fam[r] < static_cast<int>(m.textures.size()) ? baseName(m.textures[fam[r]]) : "");
        o << " }\n";
      }
      o << "}\n";
    }
  }
  o << "\n";

  for (size_t i = 0; i < m.attachments.size(); ++i) {
    if (static_cast<int>(i) == m.illumAttachment - 1) continue;
    const MdlAttachment& a = m.attachments[i];
    if (a.bone < 0 || a.bone >= static_cast<int>(m.bones.size())) continue;
    double ax, ay, az;
    matrixToDegrees(a.local, ax, ay, az);
    o << "$attachment " << q(a.name.empty() ? std::to_string(i) : a.name) << " " << q(m.bones[a.bone].name) << " " << g6(a.local[3], 2) << " " << g6(a.local[7], 2) << " "
      << g6(a.local[11], 2) << " rotate " << g6(ay, 2) << " " << g6(-az, 2) << " " << g6(-ax, 2) << "\n";
  }
  if (!m.attachments.empty()) o << "\n";

  o << "$cbox " << g6(m.viewMin.x, 3) << " " << g6(m.viewMin.y, 3) << " " << g6(m.viewMin.z, 3) << " " << g6(m.viewMax.x, 3) << " " << g6(m.viewMax.y, 3) << " " << g6(m.viewMax.z, 3) << "\n";
  o << "$bbox " << g6(m.hullMin.x, 3) << " " << g6(m.hullMin.y, 3) << " " << g6(m.hullMin.z, 3) << " " << g6(m.hullMax.x, 3) << " " << g6(m.hullMax.y, 3) << " " << g6(m.hullMax.z, 3) << "\n";
  if (!(m.flags & MdlFile::AutoHitbox)) {
    for (const MdlHitboxSet& set : m.hitboxSets) {
      o << "\n$hboxset " << q(set.name) << "\n";
      for (const MdlHitbox& h : set.boxes) {
        if (h.bone < 0 || h.bone >= static_cast<int>(m.bones.size())) continue;
        o << "$hbox " << h.group << " " << q(m.bones[h.bone].name) << " " << g6(h.min.x, 3) << " " << g6(h.min.y, 3) << " " << g6(h.min.z, 3) << " " << g6(h.max.x, 3) << " "
          << g6(h.max.y, 3) << " " << g6(h.max.z, 3) << (h.name.empty() ? "" : " " + q(h.name)) << "\n";
      }
    }
  }
  o << "\n";

  if (opt.defineBones && !staticProp) {
    for (const MdlBone& b : m.bones)
      o << "$definebone " << q(b.name) << " " << q(b.parent >= 0 && b.parent < static_cast<int>(m.bones.size()) ? m.bones[b.parent].name : "") << " " << g6(b.pos.x) << " " << g6(b.pos.y)
        << " " << g6(b.pos.z) << " " << g6(deg(b.rot.y)) << " " << g6(deg(b.rot.z)) << " " << g6(deg(b.rot.x)) << " 0 0 0 0 0 0\n";
    o << "\n";
  }

  if (!c.vrdFile.empty()) o << "$proceduralbones " << q(c.vrdFile) << "\n\n";

  bool anyJiggle = false;
  for (const MdlBone& b : m.bones)
    if (b.jiggle) { writeJiggle(o, b); anyJiggle = true; }
  if (anyJiggle) o << "\n";

  // $ikchain names the chain's last bone (the foot or hand); the knee direction comes from its first link.
  for (const MdlIkChain& chain : m.ikChains) {
    if (chain.links.empty() || chain.links.back().bone < 0 || chain.links.back().bone >= static_cast<int>(m.bones.size())) continue;
    const Vec3& knee = chain.links.front().kneeDir;
    o << "$ikchain " << q(chain.name) << " " << q(m.bones[chain.links.back().bone].name) << " knee " << g6(knee.x, 3) << " " << g6(knee.y, 3) << " " << g6(knee.z, 3) << "\n";
  }
  for (const MdlIkLock& l : m.ikAutoplayLocks)
    if (l.chain >= 0 && l.chain < static_cast<int>(m.ikChains.size())) o << "$ikautoplaylock " << q(m.ikChains[l.chain].name) << " " << g6(l.posWeight) << " " << g6(l.localQWeight) << "\n";
  if (!m.ikChains.empty()) o << "\n";

  for (const MdlPoseParam& p : m.poseParams) o << "$poseparameter " << q(p.name) << " " << g6(p.start) << " " << g6(p.end) << (p.loop != 0 ? " loop " + g6(p.loop) : "") << "\n";
  if (!m.poseParams.empty()) o << "\n";
  for (const auto& [label, file] : m.includeModels) {
    std::string name = file;  // stored as "models/x.mdl"; the compiler adds "models/" itself
    std::replace(name.begin(), name.end(), '\\', '/');
    if (lower(name).rfind("models/", 0) == 0) name.erase(0, 7);
    o << "$includemodel " << q(name) << "\n";
  }
  if (!m.includeModels.empty()) o << "\n";

  // Weight lists: one per distinct set of bone weights that is not all ones.
  std::map<std::vector<float>, std::string> weightLists;
  for (const MdlSequence& s : m.sequences) {
    if (s.boneWeights.empty() || std::all_of(s.boneWeights.begin(), s.boneWeights.end(), [](float w) { return w == 1.0f; })) continue;
    if (weightLists.count(s.boneWeights)) continue;
    const std::string name = "weights_" + safeFileName(s.label);
    weightLists[s.boneWeights] = name;
    o << "$weightlist " << q(name) << " {\n";  // the compiler wants the brace on this line
    for (size_t b = 0; b < s.boneWeights.size() && b < m.bones.size(); ++b) o << "\t" << q(m.bones[b].name) << " " << g6(s.boneWeights[b]) << "\n";
    o << "}\n\n";
  }

  auto animOptions = [&](const MdlAnimDesc& a) {
    // The compiler adds "release" rules of its own to chains an animation does not move ("auto IK").
    // The rules it added the first time are already in the model and are written out below, so it is
    // told not to add any: otherwise the animations of a blend can end up with different rule counts,
    // which is a compile error.
    std::string s = m.ikChains.empty() ? std::string() : "\tnoautoik\n";
    s += ikRuleLines(m, a);
    for (const MdlMovement& mv : a.movements) s += "\twalkframe " + std::to_string(mv.endFrame) + motionFlags(mv.motionFlags) + "\n";
    return s;
  };

  // Animations that are not owned by a single sequence ("@name") get their own $animation.
  for (size_t i = 0; i < m.anims.size(); ++i) {
    const MdlAnimDesc& a = m.anims[i];
    if (!c.animFiles.count(static_cast<int>(i)) || (!a.name.empty() && a.name[0] == '@')) continue;
    o << "$animation " << q(a.name) << " " << q(c.animFiles.at(static_cast<int>(i))) << " {\n\tfps " << g6(a.fps) << "\n";
    // A sequence takes "loop" from its animations, so only say it here if every sequence that
    // uses this animation loops too.
    bool loop = a.flags & MdlAnimDesc::Looping;
    for (const MdlSequence& s : m.sequences)
      if (std::find(s.anims.begin(), s.anims.end(), static_cast<int>(i)) != s.anims.end() && !(s.flags & MdlAnimDesc::Looping)) loop = false;
    if (loop) o << "\tloop\n";
    o << animOptions(a);
    // A sequence built from declared animations takes its bone weights from them.
    for (const MdlSequence& s : m.sequences) {
      if (std::find(s.anims.begin(), s.anims.end(), static_cast<int>(i)) == s.anims.end()) continue;
      if (const auto wl = weightLists.find(s.boneWeights); wl != weightLists.end()) o << "\tweightlist " << q(wl->second) << "\n";
      break;
    }
    o << "}\n\n";
  }

  for (const MdlSequence& s : m.sequences) {
    o << "$sequence " << q(s.label) << " {\n";
    const MdlAnimDesc* first = nullptr;
    for (const int ai : s.anims) {
      if (ai < 0 || ai >= static_cast<int>(m.anims.size())) continue;
      const MdlAnimDesc& a = m.anims[ai];
      if (!first) first = &a;
      if (!a.name.empty() && a.name[0] == '@' && c.animFiles.count(ai)) o << "\t" << q(c.animFiles.at(ai)) << "\n";
      else o << "\t" << q(a.name) << "\n";
    }
    if (!s.activity.empty()) o << "\tactivity " << q(s.activity) << " " << s.activityWeight << "\n";
    for (const MdlEvent& e : s.events) {
      const int frame = first ? static_cast<int>(std::lround(e.cycle * std::max(first->frameCount - 1, 0))) : 0;
      o << "\t{ event " << ((e.type & 0x400) && !e.name.empty() ? e.name : std::to_string(e.event)) << " " << frame << " " << q(e.options) << " }\n";
    }
    if (s.groupSize[0] > 1) o << "\tblendwidth " << s.groupSize[0] << "\n";
    for (int k = 0; k < 2; ++k)
      if (s.paramIndex[k] >= 0 && s.paramIndex[k] < static_cast<int>(m.poseParams.size()))
        o << "\tblend " << q(m.poseParams[s.paramIndex[k]].name) << " " << g6(s.paramStart[k]) << " " << g6(s.paramEnd[k]) << "\n";
    if (s.flags & MdlAnimDesc::Autoplay) o << "\tautoplay\n";
    if (s.flags & MdlAnimDesc::DeltaFlag) o << ((s.flags & MdlAnimDesc::Post) ? "\tdelta\n" : "\tpredelta\n");
    if (s.flags & MdlAnimDesc::Snap) o << "\tsnap\n";
    if (s.flags & MdlAnimDesc::Hidden) o << "\thidden\n";
    if (s.flags & MdlAnimDesc::Realtime) o << "\trealtime\n";
    o << "\tfadein " << g6(s.fadeIn) << "\n\tfadeout " << g6(s.fadeOut) << "\n";
    auto node = [&](int n) { return n >= 1 && n <= static_cast<int>(m.nodeNames.size()) ? m.nodeNames[n - 1] : std::to_string(n); };
    if (s.entryNode != 0 && s.entryNode == s.exitNode) o << "\tnode " << q(node(s.entryNode)) << "\n";
    else if (s.entryNode != 0 && s.exitNode != 0) o << (s.nodeFlags ? "\trtransition " : "\ttransition ") << q(node(s.entryNode)) << " " << q(node(s.exitNode)) << "\n";
    if (first && !first->name.empty() && first->name[0] == '@') {
      o << "\tfps " << g6(first->fps) << "\n" << animOptions(*first);
    }
    if (s.flags & MdlAnimDesc::Looping) o << "\tloop\n";
    for (const MdlSequence::Layer& l : s.layers) {  // SourceQcFile48.WriteSequenceLayerInfo
      if (l.sequence < 0 || l.sequence >= static_cast<int>(m.sequences.size())) continue;
      const std::string other = q(m.sequences[l.sequence].label);
      if (l.flags == 0 && l.start == l.end) { o << "\taddlayer " << other << "\n"; continue; }
      o << "\tblendlayer " << other;
      const bool pose = l.flags & MdlSequence::Layer::Pose;
      const int endFrame = first ? std::max(first->frameCount - 1, 0) : 0;
      for (const double v : {l.start, l.peak, l.tail, l.end}) o << " " << (pose ? g6(v) : std::to_string(std::lround(v * endFrame)));
      if (l.flags & MdlSequence::Layer::XFade) o << " xfade";
      if (l.flags & MdlSequence::Layer::Spline) o << " spline";
      if (l.flags & MdlSequence::Layer::NoBlend) o << " noblend";
      if (pose && l.pose >= 0 && l.pose < static_cast<int>(m.poseParams.size())) o << " poseparameter " << q(m.poseParams[l.pose].name);
      if (l.flags & MdlSequence::Layer::Local) o << " local";
      o << "\n";
    }
    for (const MdlIkLock& l : s.ikLocks)
      if (l.chain >= 0 && l.chain < static_cast<int>(m.ikChains.size())) o << "\tiklock " << q(m.ikChains[l.chain].name) << " " << g6(l.posWeight) << " " << g6(l.localQWeight) << "\n";
    if (const auto wl = weightLists.find(s.boneWeights); wl != weightLists.end()) o << "\tweightlist " << q(wl->second) << "\n";
    if (!s.keyValues.empty()) {
      const auto open = s.keyValues.find('{'), close = s.keyValues.rfind('}');
      if (open != std::string::npos && close != std::string::npos && close > open) o << "\tkeyvalues " << s.keyValues.substr(open, close - open + 1) << "\n";
    }
    o << "}\n\n";
  }

  if (c.phy) writeCollision(o, *c.phy, c.physicsFile);

  if (!m.keyValues.empty()) {
    const auto open = m.keyValues.find('{'), close = m.keyValues.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) o << "$keyvalues\n" << m.keyValues.substr(open, close - open + 1) << "\n";
  }
}

}  // namespace

fs::path findFileNoCase(const fs::path& dir, std::string_view name) {
  std::error_code ec;
  if (fs::exists(dir / name, ec)) return dir / name;
  const std::string want = lower(std::string(name));
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (lower(it->path().filename().string()) == want) return it->path();
  return {};
}

DecompileResult decompile(const fs::path& mdlPath, const fs::path& outDir, const DecompileOptions& opt, const LogFn& log) {
  auto say = [&](const std::string& s) { if (log) log(s); };
  DecompileResult res;
  auto warn = [&](const std::string& s) { res.warnings.push_back(s); say("WARNING: " + s); };

  say("Reading " + mdlPath.filename().string());
  const MdlFile m = readMdl(mdlPath);
  if (!m.aniFile.empty()) say("  animation blocks read from " + m.aniFile);
  say("  MDL version " + std::to_string(m.version) + ", " + std::to_string(m.bones.size()) + " bones, " + std::to_string(m.sequences.size()) + " sequences, " +
      std::to_string(m.textures.size()) + " materials");

  const fs::path dir = mdlPath.parent_path().empty() ? fs::path(".") : mdlPath.parent_path();
  const std::string base = mdlPath.stem().string();
  const fs::path vvdPath = findFileNoCase(dir, base + ".vvd");
  fs::path vtxPath;
  for (const char* ext : {".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"})
    if (vtxPath = findFileNoCase(dir, base + ext); !vtxPath.empty()) break;

  bool haveMesh = !vvdPath.empty() && !vtxPath.empty();
  VvdFile vvd;
  VtxFile vtx;
  if (haveMesh) {
    vvd = readVvd(vvdPath);
    vtx = readVtx(vtxPath);
    say("  " + vvdPath.filename().string() + ": " + std::to_string(vvd.vertices.size()) + " vertices; " + vtxPath.filename().string() + ": " + std::to_string(vtx.lodCount) + " LOD(s)");
    if (vvd.checksum != m.checksum) warn(vvdPath.filename().string() + " has a different checksum than the MDL; the mesh may not match");
    if (vtx.checksum != m.checksum) warn(vtxPath.filename().string() + " has a different checksum than the MDL; the mesh may not match");
  } else {
    bool anyMesh = false;
    for (const auto& bp : m.bodyParts)
      for (const auto& md : bp.models) anyMesh |= !md.meshes.empty();
    if (anyMesh) warn("no VVD/VTX files found next to the MDL; meshes were not written");
  }

  std::error_code ec;
  fs::create_directories(outDir, ec);
  if (ec) throw std::runtime_error("cannot create " + outDir.string() + ": " + ec.message());

  QcContext ctx{m, {}, {}, {}, -1, {}, {}, {}, {}, nullptr, {}};
  std::set<std::string> usedMeshes, usedAnims;
  auto unique = [](std::set<std::string>& used, std::string name) {
    name = lower(safeFileName(name));
    std::string candidate = name;
    for (int n = 2; !used.insert(candidate).second; ++n) candidate = name + "_" + std::to_string(n);
    return candidate;
  };
  auto wrote = [&](const fs::path& p) { res.files.push_back(p); say("  wrote " + fs::relative(p, outDir, ec).string()); };

  // Reference meshes and LODs
  std::map<int, std::string> lodLines;  // lod index -> replacemodel/removemodel lines
  std::map<int, float> lodSwitch;
  int skipped = 0;
  for (size_t b = 0; b < m.bodyParts.size(); ++b) {
    ctx.bodyFiles.emplace_back();
    for (size_t k = 0; k < m.bodyParts[b].models.size(); ++k) {
      const MdlModel& model = m.bodyParts[b].models[k];
      const bool inVtx = haveMesh && b < vtx.bodyParts.size() && k < vtx.bodyParts[b].models.size() && !vtx.bodyParts[b].models[k].lods.empty();
      if (model.meshes.empty() || !inVtx) { ctx.bodyFiles.back().emplace_back(); continue; }
      const VtxModel& vm = vtx.bodyParts[b].models[k];
      std::string ref = stem(model.name);
      if (ref.empty()) ref = base + "_" + m.bodyParts[b].name + "_" + std::to_string(k);
      else if (opt.prefixMeshNames && lower(ref).rfind(lower(base), 0) != 0) ref = base + "_" + ref;
      ref = unique(usedMeshes, ref);
      {
        auto o = openOut(outDir / (ref + ".smd"));
        writeSmdHeader(o, m);
        writeBindSkeleton(o, m);
        writeTriangles(o, m, vvd, model, vm.lods[0], skipped);
      }
      wrote(outDir / (ref + ".smd"));
      ctx.bodyFiles.back().push_back(ref + ".smd");
      for (size_t l = 1; opt.lods && l < vm.lods.size(); ++l) {
        const std::string lodName = ref + "_lod" + std::to_string(l);
        int tris;
        {
          auto o = openOut(outDir / (lodName + ".smd"));
          writeSmdHeader(o, m);
          writeBindSkeleton(o, m);
          tris = writeTriangles(o, m, vvd, model, vm.lods[l], skipped);
        }
        lodSwitch[static_cast<int>(l)] = vm.lods[l].switchPoint;
        if (tris == 0) {
          fs::remove(outDir / (lodName + ".smd"), ec);
          lodLines[static_cast<int>(l)] += "\tremovemodel " + q(ref + ".smd") + "\n";
        } else {
          wrote(outDir / (lodName + ".smd"));
          lodLines[static_cast<int>(l)] += "\treplacemodel " + q(ref + ".smd") + " " + q(lodName + ".smd") + "\n";
        }
      }
    }
  }
  for (const auto& [l, lines] : lodLines)
    ctx.lodBlocks.push_back((lodSwitch[l] < 0 ? std::string("$shadowlod") : "$lod " + g6(lodSwitch[l])) + "\n{\n" + lines + "}\n");
  if (skipped > 0) warn(std::to_string(skipped) + " triangle(s) referenced vertices outside the VVD and were skipped");

  // Animations
  const std::string animDir = base + "_anims";
  bool blockWarned = false;
  for (size_t i = 0; i < m.anims.size(); ++i) {
    const MdlAnimDesc& a = m.anims[i];
    const MdlSequence* owner = nullptr;
    for (const MdlSequence& s : m.sequences)
      if (std::find(s.anims.begin(), s.anims.end(), static_cast<int>(i)) != s.anims.end()) { owner = &s; break; }
    const std::string name = unique(usedAnims, a.name.empty() ? "anim_" + std::to_string(i) : a.name);
    fs::create_directories(outDir / animDir, ec);
    const fs::path file = outDir / animDir / (name + ".smd");
    if (opt.animations && a.loaded) {
      writeAnimSmd(file, m, a, owner);
    } else {
      if (opt.animations && !blockWarned) {
        blockWarned = true;
        warn("some animations are stored in an animation block file that could not be read; they were written as a single bind-pose frame");
      }
      auto o = openOut(file);
      writeSmdHeader(o, m);
      writeBindSkeleton(o, m);
    }
    wrote(file);
    ctx.animFiles[static_cast<int>(i)] = animDir + "/" + name + ".smd";
  }

  // Features that are read but not written yet: say so rather than drop them silently.
  // Collision model
  PhyFile phy;
  if (const fs::path phyPath = findFileNoCase(dir, base + ".phy"); !phyPath.empty() && opt.physics) {
    try {
      phy = readPhy(phyPath);
      if (phy.checksum != m.checksum) warn(phyPath.filename().string() + " has a different checksum than the MDL; the collision model may not match");
      if (!phy.solids.empty()) {
        const std::string name = unique(usedMeshes, base + "_physics") + ".smd";
        const int triangles = writePhysicsSmd(outDir / name, m, phy);
        say("  " + phyPath.filename().string() + ": " + std::to_string(phy.solids.size()) + " solid(s), " + std::to_string(triangles) + " triangles");
        wrote(outDir / name);
        ctx.phy = &phy;
        ctx.physicsFile = name;
      }
    } catch (const std::exception& e) {
      warn(std::string("the collision model could not be read and was left out: ") + e.what());
    }
  }
  // Eyes and flexes live in a $model block, which takes the place of one single-model body group.
  if (opt.flexes) {
    bool unplaced = false;
    for (size_t b = 0; b < m.bodyParts.size(); ++b) {
      bool face = false;
      for (const auto& model : m.bodyParts[b].models) {
        face |= !model.eyeballs.empty();
        for (const auto& mesh : model.meshes) face |= !mesh.flexes.empty();
      }
      if (!face) continue;
      if (ctx.modelBodyPart < 0 && m.bodyParts[b].models.size() == 1 && b < ctx.bodyFiles.size() && !ctx.bodyFiles[b].empty() && !ctx.bodyFiles[b][0].empty()) ctx.modelBodyPart = static_cast<int>(b);
      else unplaced = true;
    }
    if (ctx.modelBodyPart < 0 && !unplaced && (!m.flexControllers.empty() || !m.flexRules.empty() || !m.mouths.empty()) && !ctx.bodyFiles.empty() &&
        m.bodyParts[0].models.size() == 1 && !ctx.bodyFiles[0].empty() && !ctx.bodyFiles[0][0].empty())
      ctx.modelBodyPart = 0;
    if (unplaced) warn("eyes or flexes on more than one body group, or on a body group with several models, were left out; a QC can only describe them on one single-model group");
    if (ctx.modelBodyPart >= 0 && haveMesh) {
      ctx.flexFrames = buildFlexFrames(m, vvd);
      if (ctx.flexFrames.size() > 1) {
        ctx.vtaFile = base + ".vta";
        writeVta(outDir / ctx.vtaFile, m, vvd, ctx.flexFrames);
        say("  " + std::to_string(ctx.flexFrames.size() - 1) + " flex shape(s)");
        wrote(outDir / ctx.vtaFile);
      }
    }
  }
  {
    // A mesh names a column of the skin table; each skin puts one material in that column.
    std::set<int> columns, used;
    for (const auto& bp : m.bodyParts)
      for (const auto& md : bp.models)
        for (const auto& mesh : md.meshes) columns.insert(mesh.material);
    for (const auto& family : m.skinFamilies)
      for (const int column : columns)
        if (column >= 0 && column < static_cast<int>(family.size())) used.insert(family[column]);
    if (m.skinFamilies.empty()) used = columns;
    if (!used.empty() && used.size() < m.textures.size())
      warn(std::to_string(m.textures.size() - used.size()) + " material(s) listed in the model are not used by any mesh; the compiler drops them on a recompile");
  }
  if (!m.aniProblem.empty()) warn(m.aniProblem);
  for (const MdlAnimDesc& a : m.anims)
    if (a.ikRuleCount > static_cast<int>(a.ikRules.size())) { warn("some IK rules are stored in an animation block file that could not be read; those ikrule options were left out"); break; }
  for (const MdlBone& b : m.bones)
    if (b.procType != 0 && !b.jiggle && !b.quatInterp) { warn("bone \"" + b.name + "\" is an axis-interpolated or aim-at procedural bone; those kinds are not decompiled yet"); break; }
  if (opt.proceduralBones && writeVrd(outDir / (base + ".vrd"), m)) {
    ctx.vrdFile = base + ".vrd";
    wrote(outDir / ctx.vrdFile);
  }


  res.qc = outDir / (base + ".qc");
  {
    auto o = openOut(res.qc);
    writeQc(o, ctx, opt);
  }
  wrote(res.qc);
  for (const std::string& w : ctx.flexWarnings) warn(w);
  say("Decompile finished: " + std::to_string(res.files.size()) + " file(s), " + std::to_string(res.warnings.size()) + " warning(s)");
  return res;
}

}  // namespace czd
