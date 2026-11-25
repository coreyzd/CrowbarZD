// SPDX-License-Identifier: CC-BY-SA-3.0
// CrowbarZD - compiled Source model data (MDL v44-v48, VVD v4, VTX v7).
// Ported from Crowbar by ZeqMacaw (GameModel/SourceModel48 and friends).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace czd {

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
};

// R = Rz(z) * Ry(y) * Rx(x), the RadianEuler convention SMD files use.
Vec3 quatToEuler(const Quat& q);

// ---- MDL -------------------------------------------------------------------

// mstudiojigglebone_t: the settings behind $jigglebone.
struct MdlJiggle {
  enum : uint32_t { Flexible = 0x1, Rigid = 0x2, YawConstraint = 0x4, PitchConstraint = 0x8, AngleConstraint = 0x10,
                    LengthConstraint = 0x20, BaseSpring = 0x40 };
  uint32_t flags = 0;
  double length = 0, tipMass = 0;
  double yawStiffness = 0, yawDamping = 0, pitchStiffness = 0, pitchDamping = 0, alongStiffness = 0, alongDamping = 0;
  double angleLimit = 0;  // radians, like the yaw and pitch limits
  double minYaw = 0, maxYaw = 0, yawFriction = 0, yawBounce = 0;
  double minPitch = 0, maxPitch = 0, pitchFriction = 0, pitchBounce = 0;
  double baseMass = 0, baseStiffness = 0, baseDamping = 0;
  double baseMinLeft = 0, baseMaxLeft = 0, baseLeftFriction = 0;
  double baseMinUp = 0, baseMaxUp = 0, baseUpFriction = 0;
  double baseMinForward = 0, baseMaxForward = 0, baseForwardFriction = 0;
};

// mstudioquatinterpbone_t: a helper bone driven by another bone's rotation ($proceduralbones, VRD file).
struct MdlQuatInterp {
  struct Trigger {
    double invTolerance = 0;  // 1 / tolerance angle in radians
    Quat trigger;
    Vec3 pos;
    Quat quat;
  };
  int controlBone = 0;
  std::vector<Trigger> triggers;
};

struct MdlIkChain {
  struct Link {
    int bone = 0;
    Vec3 kneeDir;  // ideal bending direction
  };
  std::string name;
  std::vector<Link> links;  // thigh, knee, foot
};

struct MdlIkLock {
  int chain = 0;
  double posWeight = 0, localQWeight = 0;
};

// mstudioikrule_t: one "ikrule" option of an animation.
struct MdlIkRule {
  enum { Self = 1, World = 2, Ground = 3, Release = 4, Attachment = 5, Unlatch = 6 };
  int type = 0, chain = 0, bone = -1, slot = 0;
  double height = 0, radius = 0, floor = 0;
  Vec3 pos;
  Quat q;
  double start = 0, peak = 0, tail = 0, end = 0;  // influence range, as fractions of the animation
  double contact = 0, drop = 0, top = 0;
  std::string attachment;
};

struct MdlBone {
  std::string name;
  int parent = -1;
  Vec3 pos;
  Quat quat;
  Vec3 rot;  // radians
  Vec3 posScale;
  Vec3 rotScale;
  std::array<float, 12> poseToBone{};
  uint32_t flags = 0;
  int procType = 0;  // 0 none, 1 axis interp, 2 quat interp, 3/4 aim-at, 5 jiggle
  std::optional<MdlJiggle> jiggle;
  std::optional<MdlQuatInterp> quatInterp;
  int physicsBone = 0;
  std::string surfaceProp;
  int contents = 0;
};

struct MdlHitbox {
  int bone = 0;
  int group = 0;
  Vec3 min, max;
  std::string name;
};
struct MdlHitboxSet {
  std::string name;
  std::vector<MdlHitbox> boxes;
};

struct MdlAttachment {
  std::string name;
  uint32_t flags = 0;
  int bone = 0;
  std::array<float, 12> local{};  // row-major 3x4
};

// One moved vertex of a flex shape (mstudiovertanim_t).
struct MdlVertAnim {
  int index = 0;    // vertex within the mesh
  int side = 0;     // 0..255: how much of a flex pair's left half this vertex belongs to
  float delta[3] = {0, 0, 0};
  float normalDelta[3] = {0, 0, 0};
};
// A flex shape on one mesh (mstudioflex_t).
struct MdlFlex {
  int desc = 0;       // index into MdlFile::flexDescs
  double target[4] = {0, 0, 0, 0};
  int partner = 0;    // other flex desc of a flex pair, 0 when there is none
  std::vector<MdlVertAnim> verts;
};
struct MdlEyeball {
  std::string name;
  int bone = 0;
  Vec3 org;
  double zOffset = 0, radius = 0, irisScale = 1;
  int upperFlexDesc[3] = {0, 0, 0}, lowerFlexDesc[3] = {0, 0, 0};
  double upperTarget[3] = {0, 0, 0}, lowerTarget[3] = {0, 0, 0};
  int upperLidFlexDesc = 0, lowerLidFlexDesc = 0;
};
struct MdlFlexController {
  std::string type, name;
  double min = 0, max = 1;
};
struct MdlFlexRule {
  struct Op {
    enum { Const = 1, Fetch1, Fetch2, Add, Sub, Mul, Div, Neg, Exp, Open, Close, Comma, Max, Min, TwoWay0, TwoWay1, NWay, Combo, Dominate, DmeLowerEyelid, DmeUpperEyelid };
    int op = 0;
    int index = 0;      // operand for the fetch and DMX operations
    double value = 0;   // operand for Const
  };
  int flexDesc = 0;
  std::vector<Op> ops;  // reverse Polish
};
struct MdlMouth {
  int bone = 0;
  Vec3 forward;
  int flexDesc = 0;
};

struct MdlMesh {
  int material = 0;
  int vertexCount = 0;
  int vertexOffset = 0;  // into the owning model's vertices
  int materialType = 0;   // 1 = eyeball mesh
  int materialParam = 0;  // which eyeball
  std::vector<MdlFlex> flexes;
};
struct MdlModel {
  std::string name;
  int vertexCount = 0;
  int vertexIndex = 0;  // byte offset into the VVD vertex array
  std::vector<MdlEyeball> eyeballs;
  std::vector<MdlMesh> meshes;
};
struct MdlBodyPart {
  std::string name;
  int base = 0;
  std::vector<MdlModel> models;
};

struct MdlMovement {
  int endFrame = 0;
  uint32_t motionFlags = 0;
  double v0 = 0, v1 = 0, angle = 0;
  Vec3 vector, position;
};

// One bone's compressed track inside an animation section (mstudioanim_t).
struct MdlAnimTrack {
  enum : uint8_t { RawPos = 0x01, RawRot = 0x02, AnimPos = 0x04, AnimRot = 0x08, Delta = 0x10, RawRot2 = 0x20 };
  int bone = 0;
  uint8_t flags = 0;
  Quat rawRot;
  Vec3 rawPos;
  // RLE value streams (mstudioanimvalue_t as raw int16 words); empty when the axis has none.
  std::array<std::vector<int16_t>, 3> rotValues, posValues;
};

struct MdlAnimDesc {
  enum : uint32_t { Looping = 0x1, Snap = 0x2, DeltaFlag = 0x4, Autoplay = 0x8, Post = 0x10, AllZeros = 0x20,
                    FrameAnim = 0x40, CyclePose = 0x80, Realtime = 0x100, Local = 0x200, Hidden = 0x400,
                    Override = 0x800, Activity = 0x1000, Event = 0x2000, World = 0x4000 };
  std::string name;
  double fps = 30;
  uint32_t flags = 0;
  int frameCount = 0;
  int animBlock = 0;
  int sectionFrames = 0;
  std::vector<MdlMovement> movements;
  std::vector<MdlIkRule> ikRules;
  int ikRuleCount = 0;  // what the file says; larger than ikRules.size() when they sit in an .ani block
  std::vector<std::vector<MdlAnimTrack>> sections;  // at least one once loaded
  bool loaded = false;                              // false: data lives in an .ani block we could not read
};

struct MdlEvent {
  double cycle = 0;
  int event = 0;
  uint32_t type = 0;
  std::string options;
  std::string name;
};

struct MdlSequence {
  std::string label, activity;
  uint32_t flags = 0;
  int activityWeight = 0;
  std::vector<MdlEvent> events;
  int groupSize[2] = {0, 0};
  int paramIndex[2] = {-1, -1};
  double paramStart[2] = {0, 0}, paramEnd[2] = {0, 0};
  double fadeIn = 0.2, fadeOut = 0.2;
  int entryNode = 0, exitNode = 0, nodeFlags = 0;
  std::vector<int> anims;  // animdesc indices, groupSize[0] * groupSize[1]
  std::vector<float> boneWeights;
  struct Layer {  // mstudioautolayer_t: addlayer / blendlayer
    enum : uint32_t { Spline = 0x40, XFade = 0x80, NoBlend = 0x200, Local = 0x1000, Pose = 0x4000 };
    int sequence = 0, pose = 0;
    uint32_t flags = 0;
    double start = 0, peak = 0, tail = 0, end = 0;
  };
  std::vector<Layer> layers;
  std::vector<MdlIkLock> ikLocks;
  std::string keyValues;
};

struct MdlPoseParam {
  std::string name;
  uint32_t flags = 0;
  double start = 0, end = 0, loop = 0;
};

struct MdlFile {
  enum : uint32_t { AutoHitbox = 0x1, ForceOpaque = 0x4, TranslucentTwoPass = 0x8, StaticProp = 0x10,
                    Obsolete = 0x200, NoForcedFade = 0x800, ForcePhonemeCrossfade = 0x1000,
                    ConstantDirectionalLight = 0x2000, AmbientBoost = 0x10000, DoNotCastShadows = 0x20000,
                    CastTextureShadows = 0x40000 };
  int version = 0;
  int checksum = 0;
  std::string name;  // $modelname
  Vec3 eyePos, illumPos, hullMin, hullMax, viewMin, viewMax;
  uint32_t flags = 0;
  std::vector<MdlBone> bones;
  std::vector<MdlHitboxSet> hitboxSets;
  std::vector<MdlAnimDesc> anims;
  std::vector<MdlSequence> sequences;
  std::vector<std::string> textures;
  std::vector<std::string> texturePaths;
  int skinRefCount = 0;
  std::vector<std::vector<int>> skinFamilies;
  std::vector<MdlBodyPart> bodyParts;
  std::vector<MdlAttachment> attachments;
  std::vector<std::string> nodeNames;
  std::vector<MdlPoseParam> poseParams;
  std::vector<MdlIkChain> ikChains;
  std::vector<MdlIkLock> ikAutoplayLocks;
  std::vector<std::pair<std::string, std::string>> includeModels;  // label, file
  std::string surfaceProp;
  std::string keyValues;
  double mass = 0;
  int contents = 0;
  int constDirectionalLightDot = 0;
  int illumAttachment = 0;  // 1-based, 0 = none
  // Counts of things the decompiler does not write yet (reported as warnings).
  std::vector<std::string> flexDescs;
  std::vector<MdlFlexController> flexControllers;
  std::vector<MdlFlexRule> flexRules;
  std::vector<MdlMouth> mouths;
  int animBlockCount = 0;
  std::string animBlockName;
  std::string aniFile;     // name of the .ani file that was read, if any
  std::string aniProblem;  // why animation blocks could not be (fully) trusted, for a warning
};

// ---- VVD / VTX ---------------------------------------------------------------

struct Vertex {
  float weight[3] = {0, 0, 0};
  uint8_t bone[3] = {0, 0, 0};
  uint8_t boneCount = 0;
  float pos[3] = {0, 0, 0};
  float normal[3] = {0, 0, 0};
  float uv[2] = {0, 0};
};

struct VvdFile {
  int version = 0;
  int checksum = 0;
  int lodCount = 0;
  std::vector<Vertex> vertices;  // already run through the fixup table for LOD 0
};

struct VtxStripGroup {
  std::vector<uint16_t> meshVertex;  // original mesh vertex id per strip-group vertex
  std::vector<uint16_t> indices;
};
struct VtxMesh {
  std::vector<VtxStripGroup> stripGroups;
};
struct VtxLod {
  float switchPoint = 0;
  std::vector<VtxMesh> meshes;
};
struct VtxModel {
  std::vector<VtxLod> lods;
};
struct VtxBodyPart {
  std::vector<VtxModel> models;
};
struct VtxFile {
  int version = 0;
  int checksum = 0;
  int lodCount = 0;
  std::vector<VtxBodyPart> bodyParts;
};

// ---- PHY ---------------------------------------------------------------------

struct PhyConvex {
  int bone = 0;  // bone index the piece was built for
  std::vector<std::array<uint16_t, 3>> faces;
  std::vector<Vec3> vertices;  // metres, in the physics engine's axes
};
struct PhySolid {
  std::vector<PhyConvex> pieces;
};
// One block of the text section, e.g. solid { "name" "x" ... }.
struct PhyBlock {
  std::string name;
  std::vector<std::pair<std::string, std::string>> values;
  std::string raw;  // the block re-serialised, for $collisiontext
  std::string get(const std::string& key, const std::string& fallback = {}) const {
    for (const auto& [k, v] : values)
      if (k == key) return v;
    return fallback;
  }
  bool has(const std::string& key) const {
    for (const auto& kv : values)
      if (kv.first == key) return true;
    return false;
  }
};
struct PhyFile {
  int checksum = 0;
  std::vector<PhySolid> solids;
  std::vector<PhyBlock> blocks;
};

// Readers throw std::runtime_error with a readable message on malformed input.
MdlFile readMdl(const std::filesystem::path& file);
VvdFile readVvd(const std::filesystem::path& file);
VtxFile readVtx(const std::filesystem::path& file);
PhyFile readPhy(const std::filesystem::path& file);

}  // namespace czd
