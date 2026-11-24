// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - MDL / VVD / VTX readers.
// Ported from Crowbar by ZeqMacaw (SourceMdlFile48.vb, SourceVvdFile04.vb, SourceVtxFile07.vb).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "czd/model.hpp"

namespace czd {
namespace {

class Bytes {
 public:
  explicit Bytes(const std::filesystem::path& file) : name_(file.filename().string()) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  size_t size() const { return data_.size(); }
  bool has(size_t off, size_t n) const { return off <= data_.size() && n <= data_.size() - off; }
  template <typename T>
  T get(size_t off) const {
    if (!has(off, sizeof(T))) throw std::runtime_error(name_ + ": read past end of file at offset " + std::to_string(off));
    T v;
    std::memcpy(&v, data_.data() + off, sizeof(T));
    return v;
  }
  int32_t i32(size_t off) const { return get<int32_t>(off); }
  int16_t i16(size_t off) const { return get<int16_t>(off); }
  uint16_t u16(size_t off) const { return get<uint16_t>(off); }
  uint8_t u8(size_t off) const { return get<uint8_t>(off); }
  float f32(size_t off) const { return get<float>(off); }
  Vec3 vec3(size_t off) const { return {f32(off), f32(off + 4), f32(off + 8)}; }
  std::string str(size_t off) const {  // null-terminated
    std::string s;
    while (off < data_.size() && data_[off] != 0) s.push_back(data_[off++]);
    return s;
  }
  std::string fixedStr(size_t off, size_t n) const {
    if (!has(off, n)) throw std::runtime_error(name_ + ": read past end of file");
    return std::string(data_.data() + off, strnlen(data_.data() + off, n));
  }
  // String referenced by an int offset stored at `field`, relative to `base` (0 offset = empty).
  std::string strAt(size_t base, size_t field) const {
    const int32_t o = i32(field);
    return o == 0 ? std::string() : str(base + o);
  }
  // Guards table reads so a corrupt count cannot ask for gigabytes.
  void need(size_t off, size_t count, size_t stride, const char* what) const {
    if (count > 0 && (stride == 0 || count > data_.size() / stride || !has(off, count * stride)))
      throw std::runtime_error(name_ + ": " + what + " table runs past end of file");
  }

 private:
  std::string name_;
  std::vector<char> data_;
};

double halfToFloat(uint16_t h) {
  const int sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
  double v;
  if (exp == 0) v = std::ldexp(static_cast<double>(man), -24);
  else if (exp == 31) v = man == 0 ? 65504.0 : 0.0;  // Valve's float16 clamps infinity and zeroes NaN
  else v = std::ldexp(static_cast<double>(man + 1024), exp - 25);
  return sign ? -v : v;
}

Quat readQuat48(const Bytes& b, size_t off) {
  const uint16_t xi = b.u16(off), yi = b.u16(off + 2), zw = b.u16(off + 4);
  Quat q;
  q.x = (static_cast<int>(xi) - 32768) * (1.0 / 32768.0);
  q.y = (static_cast<int>(yi) - 32768) * (1.0 / 32768.0);
  q.z = (static_cast<int>(zw & 0x7fff) - 16384) * (1.0 / 16384.0);
  q.w = std::sqrt(std::max(0.0, 1.0 - q.x * q.x - q.y * q.y - q.z * q.z));
  if (zw & 0x8000) q.w = -q.w;
  return q;
}

Quat readQuat64(const Bytes& b, size_t off) {
  const uint64_t v = b.get<uint64_t>(off);
  Quat q;
  q.x = (static_cast<int>(v & 0x1fffff) - 1048576) * (1.0 / 1048576.5);
  q.y = (static_cast<int>((v >> 21) & 0x1fffff) - 1048576) * (1.0 / 1048576.5);
  q.z = (static_cast<int>((v >> 42) & 0x1fffff) - 1048576) * (1.0 / 1048576.5);
  q.w = std::sqrt(std::max(0.0, 1.0 - q.x * q.x - q.y * q.y - q.z * q.z));
  if (v >> 63) q.w = -q.w;
  return q;
}

// Reads one RLE stream of mstudioanimvalue_t covering `frames` frames.
std::vector<int16_t> readAnimValues(const Bytes& b, size_t off, int frames) {
  std::vector<int16_t> out;
  int covered = 0;
  while (covered < frames && b.has(off, 2)) {
    const uint8_t valid = b.u8(off), total = b.u8(off + 1);
    if (total == 0 || !b.has(off + 2, static_cast<size_t>(valid) * 2)) break;
    out.push_back(b.i16(off));
    for (int i = 0; i < valid; ++i) out.push_back(b.i16(off + 2 + i * 2));
    off += 2 + static_cast<size_t>(valid) * 2;
    covered += total;
  }
  return out;
}

std::vector<MdlAnimTrack> readAnimTracks(const Bytes& b, size_t off, int boneCount, int frames) {
  std::vector<MdlAnimTrack> tracks;
  for (int j = 0; j < boneCount; ++j) {
    if (!b.has(off, 4)) break;
    const uint8_t bone = b.u8(off);
    if (bone == 255 || bone >= boneCount) break;
    MdlAnimTrack t;
    t.bone = bone;
    t.flags = b.u8(off + 1);
    const int16_t next = b.i16(off + 2);
    size_t p = off + 4;
    if (t.flags & MdlAnimTrack::RawRot2) { t.rawRot = readQuat64(b, p); p += 8; }
    if (t.flags & MdlAnimTrack::RawRot) { t.rawRot = readQuat48(b, p); p += 6; }
    if (t.flags & MdlAnimTrack::RawPos) {
      t.rawPos = {halfToFloat(b.u16(p)), halfToFloat(b.u16(p + 2)), halfToFloat(b.u16(p + 4))};
      p += 6;
    }
    if (t.flags & MdlAnimTrack::AnimRot) {
      for (int a = 0; a < 3; ++a)
        if (const int16_t o = b.i16(p + a * 2); o > 0) t.rotValues[a] = readAnimValues(b, p + o, frames);
      p += 6;
    }
    if (t.flags & MdlAnimTrack::AnimPos) {
      for (int a = 0; a < 3; ++a)
        if (const int16_t o = b.i16(p + a * 2); o > 0) t.posValues[a] = readAnimValues(b, p + o, frames);
      p += 6;
    }
    tracks.push_back(std::move(t));
    if (next == 0) break;
    off += next;
  }
  return tracks;
}

void readAnimDescs(const Bytes& b, MdlFile& m, size_t table, int count) {
  constexpr size_t kSize = 100;
  b.need(table, count, kSize, "animation");
  const int boneCount = static_cast<int>(m.bones.size());
  for (int i = 0; i < count; ++i) {
    const size_t a = table + i * kSize;
    MdlAnimDesc d;
    d.name = b.strAt(a, a + 4);
    d.fps = b.f32(a + 8);
    d.flags = b.i32(a + 12);
    d.frameCount = b.i32(a + 16);
    const int moveCount = b.i32(a + 20), moveIndex = b.i32(a + 24);
    d.animBlock = b.i32(a + 52);
    const int animIndex = b.i32(a + 56);
    const int sectionIndex = b.i32(a + 80);
    d.sectionFrames = b.i32(a + 84);

    if (moveCount > 0 && b.has(a + moveIndex, static_cast<size_t>(moveCount) * 44)) {
      for (int k = 0; k < moveCount; ++k) {
        const size_t p = a + moveIndex + k * 44;
        MdlMovement mv;
        mv.endFrame = b.i32(p);
        mv.motionFlags = b.i32(p + 4);
        mv.v0 = b.f32(p + 8);
        mv.v1 = b.f32(p + 12);
        mv.angle = b.f32(p + 16);
        mv.vector = b.vec3(p + 20);
        mv.position = b.vec3(p + 32);
        d.movements.push_back(mv);
      }
    }

    // IK rules (mstudioikrule_t, 152 bytes). With an animation block they live in the .ani file instead.
    d.ikRuleCount = b.i32(a + 60);
    if (const int ruleIndex = b.i32(a + 64); d.ikRuleCount > 0 && d.ikRuleCount < 4096 && ruleIndex != 0 && b.has(a + ruleIndex, static_cast<size_t>(d.ikRuleCount) * 152)) {
      for (int k = 0; k < d.ikRuleCount; ++k) {
        const size_t r = a + ruleIndex + static_cast<size_t>(k) * 152;
        MdlIkRule rule;
        rule.type = b.i32(r + 4);
        rule.chain = b.i32(r + 8);
        rule.bone = b.i32(r + 12);
        rule.slot = b.i32(r + 16);
        rule.height = b.f32(r + 20);
        rule.radius = b.f32(r + 24);
        rule.floor = b.f32(r + 28);
        rule.pos = b.vec3(r + 32);
        rule.q = {b.f32(r + 44), b.f32(r + 48), b.f32(r + 52), b.f32(r + 56)};
        rule.start = b.f32(r + 76);
        rule.peak = b.f32(r + 80);
        rule.tail = b.f32(r + 84);
        rule.end = b.f32(r + 88);
        rule.contact = b.f32(r + 96);
        rule.drop = b.f32(r + 100);
        rule.top = b.f32(r + 104);
        rule.attachment = b.strAt(r, r + 120);
        d.ikRules.push_back(std::move(rule));
      }
    }

    if (d.flags & MdlAnimDesc::FrameAnim) {
      // v49-style frame animation; not produced for v44-v48 models.
    } else if (sectionIndex != 0 && d.sectionFrames > 0) {
      const int sectionCount = d.frameCount / d.sectionFrames + 2;
      b.need(a + sectionIndex, sectionCount, 8, "animation section");
      const int firstOffset = b.i32(a + sectionIndex + 4);
      d.loaded = true;
      for (int s = 0; s < sectionCount; ++s) {
        const int block = b.i32(a + sectionIndex + s * 8), offset = b.i32(a + sectionIndex + s * 8 + 4);
        if (block != 0) { d.loaded = false; d.sections.emplace_back(); continue; }
        const int frames = s < sectionCount - 2 ? d.sectionFrames : d.frameCount - (sectionCount - 2) * d.sectionFrames;
        d.sections.push_back(readAnimTracks(b, a + offset + (animIndex - firstOffset), boneCount, std::max(frames, 1)));
      }
    } else if (d.animBlock == 0) {
      d.sections.push_back(readAnimTracks(b, a + animIndex, boneCount, d.frameCount));
      d.loaded = true;
    }
    if (d.sections.empty()) d.sections.emplace_back();
    m.anims.push_back(std::move(d));
  }
}

void readSequences(const Bytes& b, MdlFile& m, size_t table, int count) {
  constexpr size_t kSize = 212;
  b.need(table, count, kSize, "sequence");
  for (int i = 0; i < count; ++i) {
    const size_t s = table + i * kSize;
    MdlSequence q;
    q.label = b.strAt(s, s + 4);
    q.activity = b.strAt(s, s + 8);
    q.flags = b.i32(s + 12);
    q.activityWeight = b.i32(s + 20);
    const int eventCount = b.i32(s + 24), eventIndex = b.i32(s + 28);
    const int animIndexIndex = b.i32(s + 60);
    for (int k = 0; k < 2; ++k) {
      q.groupSize[k] = b.i32(s + 68 + k * 4);
      q.paramIndex[k] = b.i32(s + 76 + k * 4);
      q.paramStart[k] = b.f32(s + 84 + k * 4);
      q.paramEnd[k] = b.f32(s + 92 + k * 4);
    }
    q.fadeIn = b.f32(s + 104);
    q.fadeOut = b.f32(s + 108);
    q.entryNode = b.i32(s + 112);
    q.exitNode = b.i32(s + 116);
    q.nodeFlags = b.i32(s + 120);
    if (const int layers = b.i32(s + 148), layerIndex = b.i32(s + 152); layers > 0 && layerIndex != 0 && b.has(s + layerIndex, static_cast<size_t>(layers) * 24))
      for (int k = 0; k < layers; ++k) {
        const size_t l = s + layerIndex + static_cast<size_t>(k) * 24;
        q.layers.push_back({b.i16(l), b.i16(l + 2), static_cast<uint32_t>(b.i32(l + 4)), b.f32(l + 8), b.f32(l + 12), b.f32(l + 16), b.f32(l + 20)});
      }
    const int weightIndex = b.i32(s + 156);
    const int ikLockCount = b.i32(s + 164), ikLockIndex = b.i32(s + 168);
    if (ikLockCount > 0 && ikLockIndex != 0 && b.has(s + ikLockIndex, static_cast<size_t>(ikLockCount) * 32))
      for (int k = 0; k < ikLockCount; ++k) {
        const size_t l = s + ikLockIndex + static_cast<size_t>(k) * 32;
        q.ikLocks.push_back({b.i32(l), b.f32(l + 4), b.f32(l + 8)});
      }
    const int kvIndex = b.i32(s + 172), kvSize = b.i32(s + 176);

    if (eventCount > 0 && b.has(s + eventIndex, static_cast<size_t>(eventCount) * 80)) {
      for (int k = 0; k < eventCount; ++k) {
        const size_t e = s + eventIndex + k * 80;
        MdlEvent ev;
        ev.cycle = b.f32(e);
        ev.event = b.i32(e + 4);
        ev.type = b.i32(e + 8);
        ev.options = b.fixedStr(e + 12, 64);
        ev.name = b.strAt(e, e + 76);
        q.events.push_back(std::move(ev));
      }
    }
    const int animCount = q.groupSize[0] * q.groupSize[1];
    if (animCount > 0 && animCount < 4096 && animIndexIndex != 0 && b.has(s + animIndexIndex, static_cast<size_t>(animCount) * 2))
      for (int k = 0; k < animCount; ++k) q.anims.push_back(b.i16(s + animIndexIndex + k * 2));
    if (weightIndex != 0 && b.has(s + weightIndex, m.bones.size() * 4))
      for (size_t k = 0; k < m.bones.size(); ++k) q.boneWeights.push_back(b.f32(s + weightIndex + k * 4));
    if (kvSize > 0 && kvIndex != 0 && b.has(s + kvIndex, kvSize)) q.keyValues = b.fixedStr(s + kvIndex, kvSize);
    m.sequences.push_back(std::move(q));
  }
}

}  // namespace

Vec3 quatToEuler(const Quat& q) {
  // Rotation matrix elements, then Shoemake's XYZ static-frame extraction (Crowbar: Eul_FromQuat).
  const double m00 = 1 - 2 * (q.y * q.y + q.z * q.z), m10 = 2 * (q.x * q.y + q.w * q.z), m20 = 2 * (q.x * q.z - q.w * q.y);
  const double m21 = 2 * (q.y * q.z + q.w * q.x), m22 = 1 - 2 * (q.x * q.x + q.y * q.y);
  const double m11 = 1 - 2 * (q.x * q.x + q.z * q.z), m12 = 2 * (q.y * q.z - q.w * q.x);
  const double cy = std::sqrt(m00 * m00 + m10 * m10);
  if (cy > 16 * 1.1920929e-07) return {std::atan2(m21, m22), std::atan2(-m20, cy), std::atan2(m10, m00)};
  return {std::atan2(-m12, m11), std::atan2(-m20, cy), 0};
}

MdlFile readMdl(const std::filesystem::path& file) {
  const Bytes b(file);
  if (b.size() < 408 || b.fixedStr(0, 4) != "IDST") throw std::runtime_error(file.filename().string() + ": not a Source MDL file");
  MdlFile m;
  m.version = b.i32(4);
  if (m.version < 44 || m.version > 48)
    throw std::runtime_error(file.filename().string() + ": MDL version " + std::to_string(m.version) +
                             " is not supported yet (CrowbarZD reads versions 44 to 48)");
  m.checksum = b.i32(8);
  m.name = b.fixedStr(12, 64);
  m.eyePos = b.vec3(80);
  m.illumPos = b.vec3(92);
  m.hullMin = b.vec3(104);
  m.hullMax = b.vec3(116);
  m.viewMin = b.vec3(128);
  m.viewMax = b.vec3(140);
  m.flags = b.i32(152);

  // Bones
  {
    const int n = b.i32(156);
    const size_t t = b.i32(160);
    b.need(t, n, 216, "bone");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + i * 216;
      MdlBone bn;
      bn.name = b.strAt(o, o);
      bn.parent = b.i32(o + 4);
      bn.pos = b.vec3(o + 32);
      bn.quat = {b.f32(o + 44), b.f32(o + 48), b.f32(o + 52), b.f32(o + 56)};
      bn.rot = b.vec3(o + 60);
      bn.posScale = b.vec3(o + 72);
      bn.rotScale = b.vec3(o + 84);
      for (int k = 0; k < 12; ++k) bn.poseToBone[k] = b.f32(o + 96 + k * 4);
      bn.flags = b.i32(o + 160);
      bn.procType = b.i32(o + 164);
      if (const int proc = b.i32(o + 168); bn.procType == 5 && proc != 0 && b.has(o + proc, 120)) {  // mstudiojigglebone_t
        const size_t j = o + proc;
        MdlJiggle g;
        g.flags = b.i32(j);
        double* const fields[] = {&g.length, &g.tipMass, &g.yawStiffness, &g.yawDamping, &g.pitchStiffness, &g.pitchDamping, &g.alongStiffness, &g.alongDamping,
                                  &g.angleLimit, &g.minYaw, &g.maxYaw, &g.yawFriction, &g.yawBounce, &g.minPitch, &g.maxPitch, &g.pitchFriction, &g.pitchBounce,
                                  &g.baseMass, &g.baseStiffness, &g.baseDamping, &g.baseMinLeft, &g.baseMaxLeft, &g.baseLeftFriction, &g.baseMinUp, &g.baseMaxUp,
                                  &g.baseUpFriction, &g.baseMinForward, &g.baseMaxForward, &g.baseForwardFriction};
        for (size_t k = 0; k < std::size(fields); ++k) *fields[k] = b.f32(j + 4 + k * 4);
        bn.jiggle = g;
      }
      if (const int proc = b.i32(o + 168); bn.procType == 2 && proc != 0 && b.has(o + proc, 12)) {  // mstudioquatinterpbone_t
        const size_t qi = o + proc;
        MdlQuatInterp interp;
        interp.controlBone = b.i32(qi);
        const int triggers = b.i32(qi + 4);
        const size_t tt = qi + b.i32(qi + 8);
        if (triggers > 0 && triggers < 1024 && b.has(tt, static_cast<size_t>(triggers) * 48))
          for (int k = 0; k < triggers; ++k) {
            const size_t tr = tt + static_cast<size_t>(k) * 48;
            interp.triggers.push_back({b.f32(tr), {b.f32(tr + 4), b.f32(tr + 8), b.f32(tr + 12), b.f32(tr + 16)}, b.vec3(tr + 20), {b.f32(tr + 32), b.f32(tr + 36), b.f32(tr + 40), b.f32(tr + 44)}});
          }
        bn.quatInterp = std::move(interp);
      }
      bn.physicsBone = b.i32(o + 172);
      bn.surfaceProp = b.strAt(o, o + 176);
      bn.contents = b.i32(o + 180);
      m.bones.push_back(std::move(bn));
    }
  }
  // Hitbox sets
  {
    const int n = b.i32(172);
    const size_t t = b.i32(176);
    b.need(t, n, 12, "hitbox set");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + i * 12;
      MdlHitboxSet set;
      set.name = b.strAt(o, o);
      const int boxes = b.i32(o + 4);
      const size_t bt = o + b.i32(o + 8);
      b.need(bt, boxes, 68, "hitbox");
      for (int k = 0; k < boxes; ++k) {
        const size_t h = bt + k * 68;
        set.boxes.push_back({b.i32(h), b.i32(h + 4), b.vec3(h + 8), b.vec3(h + 20), b.strAt(h, h + 32)});
      }
      m.hitboxSets.push_back(std::move(set));
    }
  }
  // Textures, texture paths, skin families
  {
    const int n = b.i32(204);
    const size_t t = b.i32(208);
    b.need(t, n, 64, "texture");
    for (int i = 0; i < n; ++i) m.textures.push_back(b.strAt(t + i * 64, t + i * 64));
    const int np = b.i32(212);
    const size_t tp = b.i32(216);
    b.need(tp, np, 4, "texture path");
    for (int i = 0; i < np; ++i) m.texturePaths.push_back(b.strAt(0, tp + i * 4));
    m.skinRefCount = b.i32(220);
    const int families = b.i32(224);
    const size_t ts = b.i32(228);
    if (m.skinRefCount > 0 && families > 0) {
      b.need(ts, static_cast<size_t>(families) * m.skinRefCount, 2, "skin");
      for (int f = 0; f < families; ++f) {
        std::vector<int> row;
        for (int r = 0; r < m.skinRefCount; ++r) row.push_back(b.i16(ts + (static_cast<size_t>(f) * m.skinRefCount + r) * 2));
        m.skinFamilies.push_back(std::move(row));
      }
    }
  }
  // Body parts -> models -> meshes
  {
    const int n = b.i32(232);
    const size_t t = b.i32(236);
    b.need(t, n, 16, "body part");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + i * 16;
      MdlBodyPart bp;
      bp.name = b.strAt(o, o);
      const int models = b.i32(o + 4);
      bp.base = b.i32(o + 8);
      const size_t mt = o + b.i32(o + 12);
      b.need(mt, models, 148, "model");
      for (int k = 0; k < models; ++k) {
        const size_t mo = mt + k * 148;
        MdlModel md;
        md.name = b.fixedStr(mo, 64);
        const int meshes = b.i32(mo + 72);
        const size_t mesht = mo + b.i32(mo + 76);
        md.vertexCount = b.i32(mo + 80);
        md.vertexIndex = b.i32(mo + 84);
        // Eyeballs (mstudioeyeball_t, 172 bytes)
        if (const int eyes = b.i32(mo + 100); eyes > 0 && eyes < 64 && b.has(mo + b.i32(mo + 104), static_cast<size_t>(eyes) * 172)) {
          for (int e = 0; e < eyes; ++e) {
            const size_t eo = mo + b.i32(mo + 104) + static_cast<size_t>(e) * 172;
            MdlEyeball eye;
            eye.name = b.strAt(eo, eo);
            eye.bone = b.i32(eo + 4);
            eye.org = b.vec3(eo + 8);
            eye.zOffset = b.f32(eo + 20);
            eye.radius = b.f32(eo + 24);
            eye.irisScale = b.f32(eo + 60);
            for (int x = 0; x < 3; ++x) {
              eye.upperFlexDesc[x] = b.i32(eo + 68 + x * 4);
              eye.lowerFlexDesc[x] = b.i32(eo + 80 + x * 4);
              eye.upperTarget[x] = b.f32(eo + 92 + x * 4);
              eye.lowerTarget[x] = b.f32(eo + 104 + x * 4);
            }
            eye.upperLidFlexDesc = b.i32(eo + 116);
            eye.lowerLidFlexDesc = b.i32(eo + 120);
            md.eyeballs.push_back(std::move(eye));
          }
        }
        b.need(mesht, meshes, 116, "mesh");
        for (int j = 0; j < meshes; ++j) {
          const size_t me = mesht + j * 116;
          MdlMesh mesh;
          mesh.material = b.i32(me);
          mesh.vertexCount = b.i32(me + 8);
          mesh.vertexOffset = b.i32(me + 12);
          mesh.materialType = b.i32(me + 24);
          mesh.materialParam = b.i32(me + 28);
          // Flex shapes (mstudioflex_t, 60 bytes) and their moved vertices (16 bytes, 18 with wrinkle data)
          const int flexes = b.i32(me + 16);
          const size_t ft = me + b.i32(me + 20);
          if (flexes > 0 && flexes < 65536 && b.has(ft, static_cast<size_t>(flexes) * 60)) {
            for (int f = 0; f < flexes; ++f) {
              const size_t fo = ft + static_cast<size_t>(f) * 60;
              MdlFlex flex;
              flex.desc = b.i32(fo);
              for (int x = 0; x < 4; ++x) flex.target[x] = b.f32(fo + 4 + x * 4);
              const int verts = b.i32(fo + 20);
              const size_t vt = fo + b.i32(fo + 24);
              flex.partner = b.i32(fo + 28);
              const size_t stride = b.u8(fo + 32) == 1 ? 18 : 16;
              if (verts > 0 && b.has(vt, static_cast<size_t>(verts) * stride)) {
                flex.verts.reserve(verts);
                for (int v = 0; v < verts; ++v) {
                  const size_t vo = vt + static_cast<size_t>(v) * stride;
                  MdlVertAnim va;
                  va.index = b.u16(vo);
                  va.side = b.u8(vo + 3);
                  for (int x = 0; x < 3; ++x) {
                    va.delta[x] = static_cast<float>(halfToFloat(b.u16(vo + 4 + x * 2)));
                    va.normalDelta[x] = static_cast<float>(halfToFloat(b.u16(vo + 10 + x * 2)));
                  }
                  flex.verts.push_back(va);
                }
              }
              mesh.flexes.push_back(std::move(flex));
            }
          }
          md.meshes.push_back(std::move(mesh));
        }
        bp.models.push_back(std::move(md));
      }
      m.bodyParts.push_back(std::move(bp));
    }
  }
  // Attachments
  {
    const int n = b.i32(240);
    const size_t t = b.i32(244);
    b.need(t, n, 92, "attachment");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + i * 92;
      MdlAttachment at;
      at.name = b.strAt(o, o);
      at.flags = b.i32(o + 4);
      at.bone = b.i32(o + 8);
      for (int k = 0; k < 12; ++k) at.local[k] = b.f32(o + 12 + k * 4);
      m.attachments.push_back(std::move(at));
    }
  }
  // Transition node names
  {
    const int n = b.i32(248);
    const size_t t = b.i32(256);
    if (n > 0 && t != 0 && b.has(t, static_cast<size_t>(n) * 4))
      for (int i = 0; i < n; ++i) m.nodeNames.push_back(b.strAt(0, t + i * 4));
  }
  // Flex descriptions, controllers and rules
  {
    const int n = b.i32(260);
    const size_t t = b.i32(264);
    b.need(t, n, 4, "flex description");
    for (int i = 0; i < n; ++i) m.flexDescs.push_back(b.strAt(t + static_cast<size_t>(i) * 4, t + static_cast<size_t>(i) * 4));
    const int nc = b.i32(268);
    const size_t tc = b.i32(272);
    b.need(tc, nc, 20, "flex controller");
    for (int i = 0; i < nc; ++i) {
      const size_t o = tc + static_cast<size_t>(i) * 20;
      m.flexControllers.push_back({b.strAt(o, o), b.strAt(o, o + 4), b.f32(o + 12), b.f32(o + 16)});
    }
    const int nr = b.i32(276);
    const size_t tr = b.i32(280);
    b.need(tr, nr, 12, "flex rule");
    for (int i = 0; i < nr; ++i) {
      const size_t o = tr + static_cast<size_t>(i) * 12;
      MdlFlexRule rule;
      rule.flexDesc = b.i32(o);
      const int ops = b.i32(o + 4);
      const size_t ot = o + b.i32(o + 8);
      if (ops > 0 && ops < 65536 && b.has(ot, static_cast<size_t>(ops) * 8))
        for (int k = 0; k < ops; ++k) rule.ops.push_back({b.i32(ot + static_cast<size_t>(k) * 8), b.i32(ot + static_cast<size_t>(k) * 8 + 4), b.f32(ot + static_cast<size_t>(k) * 8 + 4)});
      m.flexRules.push_back(std::move(rule));
    }
  }
  // IK chains (mstudioikchain_t 16 bytes; links 28 bytes) and $ikautoplaylock entries (32 bytes)
  {
    const int n = b.i32(284);
    const size_t t = b.i32(288);
    b.need(t, n, 16, "IK chain");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + static_cast<size_t>(i) * 16;
      MdlIkChain chain;
      chain.name = b.strAt(o, o);
      const int links = b.i32(o + 8);
      const size_t lt = o + b.i32(o + 12);
      b.need(lt, links, 28, "IK link");
      for (int k = 0; k < links; ++k) chain.links.push_back({b.i32(lt + static_cast<size_t>(k) * 28), b.vec3(lt + static_cast<size_t>(k) * 28 + 4)});
      m.ikChains.push_back(std::move(chain));
    }
    const int locks = b.i32(320);
    const size_t lockTable = b.i32(324);
    b.need(lockTable, locks, 32, "IK autoplay lock");
    for (int i = 0; i < locks; ++i) m.ikAutoplayLocks.push_back({b.i32(lockTable + static_cast<size_t>(i) * 32), b.f32(lockTable + static_cast<size_t>(i) * 32 + 4), b.f32(lockTable + static_cast<size_t>(i) * 32 + 8)});
  }
  {
    const int n = b.i32(292);
    const size_t t = b.i32(296);
    b.need(t, n, 20, "mouth");
    for (int i = 0; i < n; ++i) m.mouths.push_back({b.i32(t + static_cast<size_t>(i) * 20), b.vec3(t + static_cast<size_t>(i) * 20 + 4), b.i32(t + static_cast<size_t>(i) * 20 + 16)});
  }
  // Pose parameters
  {
    const int n = b.i32(300);
    const size_t t = b.i32(304);
    b.need(t, n, 20, "pose parameter");
    for (int i = 0; i < n; ++i) {
      const size_t o = t + i * 20;
      m.poseParams.push_back({b.strAt(o, o), static_cast<uint32_t>(b.i32(o + 4)), b.f32(o + 8), b.f32(o + 12), b.f32(o + 16)});
    }
  }
  m.surfaceProp = b.strAt(0, 308);
  if (const int kv = b.i32(312), size = b.i32(316); kv != 0 && size > 0 && b.has(kv, size)) m.keyValues = b.fixedStr(kv, size);
  m.mass = b.f32(328);
  m.contents = b.i32(332);
  // $includemodel
  {
    const int n = b.i32(336);
    const size_t t = b.i32(340);
    b.need(t, n, 8, "include model");
    for (int i = 0; i < n; ++i) m.includeModels.emplace_back(b.strAt(t + i * 8, t + i * 8), b.strAt(t + i * 8, t + i * 8 + 4));
  }
  m.animBlockName = b.strAt(0, 348);
  m.animBlockCount = b.i32(352);
  m.constDirectionalLightDot = b.u8(376);
  if (const int h2 = b.i32(400); h2 != 0 && b.has(h2, 12)) m.illumAttachment = b.i32(h2 + 8);

  readAnimDescs(b, m, b.i32(184), b.i32(180));
  readSequences(b, m, b.i32(192), b.i32(188));
  return m;
}

VvdFile readVvd(const std::filesystem::path& file) {
  const Bytes b(file);
  if (b.size() < 64 || b.fixedStr(0, 4) != "IDSV") throw std::runtime_error(file.filename().string() + ": not a VVD file");
  VvdFile v;
  v.version = b.i32(4);
  if (v.version != 4) throw std::runtime_error(file.filename().string() + ": unsupported VVD version " + std::to_string(v.version));
  v.checksum = b.i32(8);
  v.lodCount = b.i32(12);
  const int count = b.i32(16);  // LOD 0 holds every vertex
  const int fixups = b.i32(48);
  const size_t fixupStart = b.i32(52), vertexStart = b.i32(56);
  b.need(vertexStart, count, 48, "vertex");
  auto readVertex = [&](int i) {
    const size_t o = vertexStart + static_cast<size_t>(i) * 48;
    Vertex x;
    for (int k = 0; k < 3; ++k) { x.weight[k] = b.f32(o + k * 4); x.bone[k] = b.u8(o + 12 + k); }
    x.boneCount = b.u8(o + 15);
    for (int k = 0; k < 3; ++k) { x.pos[k] = b.f32(o + 16 + k * 4); x.normal[k] = b.f32(o + 28 + k * 4); }
    x.uv[0] = b.f32(o + 40);
    x.uv[1] = b.f32(o + 44);
    return x;
  };
  if (fixups == 0) {
    v.vertices.reserve(count);
    for (int i = 0; i < count; ++i) v.vertices.push_back(readVertex(i));
  } else {
    b.need(fixupStart, fixups, 12, "vertex fixup");
    for (int f = 0; f < fixups; ++f) {
      const size_t o = fixupStart + f * 12;
      const int source = b.i32(o + 4), n = b.i32(o + 8);
      if (source < 0 || n < 0 || source > count || n > count - source) throw std::runtime_error(file.filename().string() + ": bad vertex fixup");
      for (int i = 0; i < n; ++i) v.vertices.push_back(readVertex(source + i));
    }
  }
  return v;
}

VtxFile readVtx(const std::filesystem::path& file) {
  const Bytes b(file);
  if (b.size() < 36) throw std::runtime_error(file.filename().string() + ": not a VTX file");
  VtxFile v;
  v.version = b.i32(0);
  if (v.version != 7) throw std::runtime_error(file.filename().string() + ": unsupported VTX version " + std::to_string(v.version));
  v.checksum = b.i32(16);
  v.lodCount = b.i32(20);
  const int bodyParts = b.i32(28);
  const size_t bpt = b.i32(32);
  b.need(bpt, bodyParts, 8, "VTX body part");
  for (int i = 0; i < bodyParts; ++i) {
    const size_t bo = bpt + i * 8;
    VtxBodyPart bp;
    const int models = b.i32(bo);
    const size_t mt = bo + b.i32(bo + 4);
    b.need(mt, models, 8, "VTX model");
    for (int k = 0; k < models; ++k) {
      const size_t mo = mt + k * 8;
      VtxModel md;
      const int lods = b.i32(mo);
      const size_t lt = mo + b.i32(mo + 4);
      b.need(lt, lods, 12, "VTX LOD");
      for (int l = 0; l < lods; ++l) {
        const size_t lo = lt + l * 12;
        VtxLod lod;
        const int meshes = b.i32(lo);
        const size_t mesht = lo + b.i32(lo + 4);
        lod.switchPoint = b.f32(lo + 8);
        b.need(mesht, meshes, 9, "VTX mesh");
        for (int j = 0; j < meshes; ++j) {
          const size_t me = mesht + j * 9;
          VtxMesh mesh;
          const int groups = b.i32(me);
          const size_t gt = me + b.i32(me + 4);
          b.need(gt, groups, 25, "VTX strip group");
          for (int g = 0; g < groups; ++g) {
            const size_t go = gt + g * 25;
            VtxStripGroup sg;
            const int verts = b.i32(go), indices = b.i32(go + 8);
            const size_t vt = go + b.i32(go + 4), it = go + b.i32(go + 12);
            b.need(vt, verts, 9, "VTX vertex");
            b.need(it, indices, 2, "VTX index");
            sg.meshVertex.reserve(verts);
            for (int x = 0; x < verts; ++x) sg.meshVertex.push_back(b.u16(vt + x * 9 + 4));
            sg.indices.reserve(indices);
            for (int x = 0; x < indices; ++x) sg.indices.push_back(b.u16(it + x * 2));
            mesh.stripGroups.push_back(std::move(sg));
          }
          lod.meshes.push_back(std::move(mesh));
        }
        md.lods.push_back(std::move(lod));
      }
      bp.models.push_back(std::move(md));
    }
    v.bodyParts.push_back(std::move(bp));
  }
  return v;
}

PhyFile readPhy(const std::filesystem::path& file) {
  const Bytes b(file);
  if (b.size() < 16) throw std::runtime_error(file.filename().string() + ": not a PHY file");
  PhyFile phy;
  const int headerSize = b.i32(0), solidCount = b.i32(8);
  phy.checksum = b.i32(12);
  if (headerSize < 16 || solidCount < 0 || solidCount > 4096) throw std::runtime_error(file.filename().string() + ": bad PHY header");
  size_t pos = headerSize;
  for (int s = 0; s < solidCount; ++s) {
    const int size = b.i32(pos);
    const size_t data = pos + 4, next = data + size;
    if (size < 48 || !b.has(data, size)) throw std::runtime_error(file.filename().string() + ": collision solid runs past end of file");
    // "VPHY" surface header (28 bytes) when present, then the physics engine's compact surface:
    // a 48-byte header whose ledge tree leads to the convex pieces ("ledges").
    const size_t surface = data + (b.fixedStr(data, 4) == "VPHY" ? 28 : 0);
    PhySolid solid;
    std::vector<size_t> nodes{surface + static_cast<size_t>(b.i32(surface + 32))};
    for (int guard = 0; !nodes.empty() && guard < 65536; ++guard) {
      const size_t node = nodes.back();
      nodes.pop_back();
      if (node < data || node + 28 > next) continue;
      const int right = b.i32(node), ledgeOffset = b.i32(node + 4);
      if (right != 0) {  // inner node: left child follows it, right child is at the offset
        nodes.push_back(node + right);
        nodes.push_back(node + 28);
        continue;
      }
      const size_t ledge = node + ledgeOffset;
      if (ledge < data || ledge + 16 > next) continue;
      const size_t points = ledge + b.i32(ledge);
      PhyConvex piece;
      piece.bone = b.i32(ledge + 4) - 1;
      const int triangles = b.i16(ledge + 12);
      if (triangles <= 0 || ledge + 16 + static_cast<size_t>(triangles) * 16 > next || points < data || points > next) continue;
      int maxIndex = -1;
      for (int t = 0; t < triangles; ++t) {
        const size_t tp = ledge + 16 + static_cast<size_t>(t) * 16;
        std::array<uint16_t, 3> face{b.u16(tp + 4), b.u16(tp + 8), b.u16(tp + 12)};
        for (const uint16_t i : face) maxIndex = std::max<int>(maxIndex, i);
        piece.faces.push_back(face);
      }
      for (int i = 0; i <= maxIndex && points + static_cast<size_t>(i + 1) * 16 <= next; ++i) piece.vertices.push_back(b.vec3(points + static_cast<size_t>(i) * 16));
      solid.pieces.push_back(std::move(piece));
    }
    phy.solids.push_back(std::move(solid));
    pos = next;
  }

  // Text section: name { "key" "value" ... } blocks up to the end of the file.
  std::vector<std::string> tokens;
  std::vector<bool> quoted;
  for (size_t i = pos; i < b.size();) {
    const char c = static_cast<char>(b.u8(i));
    if (c == 0) break;
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    std::string tok;
    if (c == '"') {
      for (++i; i < b.size() && b.u8(i) != '"' && b.u8(i) != 0; ++i) tok.push_back(static_cast<char>(b.u8(i)));
      ++i;
      quoted.push_back(true);
    } else if (c == '{' || c == '}') {
      tok = c;
      ++i;
      quoted.push_back(false);
    } else {
      for (; i < b.size() && b.u8(i) != 0 && !std::isspace(b.u8(i)) && b.u8(i) != '"' && b.u8(i) != '{' && b.u8(i) != '}'; ++i) tok.push_back(static_cast<char>(b.u8(i)));
      quoted.push_back(false);
    }
    tokens.push_back(std::move(tok));
  }
  auto brace = [&](size_t i, char c) { return i < tokens.size() && !quoted[i] && tokens[i].size() == 1 && tokens[i][0] == c; };
  for (size_t i = 0; i + 1 < tokens.size();) {
    if (!brace(i + 1, '{')) { ++i; continue; }
    PhyBlock block;
    block.name = tokens[i];
    block.raw = block.name + " {\n";
    int depth = 1;
    size_t j = i + 2;
    for (; j < tokens.size() && depth > 0; ++j) {
      if (brace(j, '{')) { ++depth; block.raw += "{\n"; continue; }
      if (brace(j, '}')) { --depth; block.raw += "}\n"; continue; }
      const bool pair = j + 1 < tokens.size() && !brace(j + 1, '{') && !brace(j + 1, '}');
      if (pair) {
        if (depth == 1) block.values.emplace_back(tokens[j], tokens[j + 1]);
        block.raw += "\"" + tokens[j] + "\" \"" + tokens[j + 1] + "\"\n";
        ++j;
      } else {
        block.raw += tokens[j] + " ";
      }
    }
    phy.blocks.push_back(std::move(block));
    i = j;
  }
  return phy;
}

}  // namespace czd
