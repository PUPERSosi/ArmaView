#pragma once
// Proxies ("proxy:<path>.NNN" triangles) with their models placed on the triangle (bone animation included),
// and the crew preview: a character model on the crew proxies, posed by the RTM of the seat's action from config.
//   seat action (driverAction / gunnerAction / cargoAction[] ...) -> CfgMovesMaleSdr >> ManActions >> <action> = state
//   -> CfgMovesMaleSdr >> States >> <state> >> file -> RTM
// Proxy transform (own implementation of the engine rule, CWR ShapeLOD.cpp:942-970): the corner between the two
// shortest edges is the origin, the shortest edge the direction, the second shortest the up vector.
#include "ArmaConfig.h"
#include "P3D.h"
#include "RTM.h"
#include "Mesh.h"
#include "Skeleton.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// CfgMovesMaleSdr >> States >> <name>: RTM, timing and links to other states.
struct MovesStateInfo
{
    std::string name, file, actions;
    float speed = 0, interpolationSpeed = 6;
    bool looped = true;
    std::vector<std::string> connectTo, interpolateTo; // state names (costs dropped)
    std::vector<std::pair<std::string, std::string>> actionEntries; // CfgMovesMaleSdr >> Actions >> <actions>: entry -> state
    std::filesystem::path cfgFile;
    int cfgLine = 0;
    bool found = false;
};
MovesStateInfo LookupMovesState(const ArmaConfig* cfg, const std::string& name);
// Action -> state name ("" if unknown): CfgMovesMaleSdr >> ManActions, then the Actions class of the state the man
// is in (actionsClass; e.g. GetInLow from RifleStandActions, GetOutLow from the seat state's actions); an action may
// also name a state directly.
std::string StateOfAction(const ArmaConfig* cfg, const std::string& action, const std::string& actionsClass = "RifleStandActions");

// A model loaded for a proxy or as the crew character (first LOD as drawable sections).
struct AttachedModel
{
    enum class Status { Ok, Missing, Binarized, Error };
    std::filesystem::path file;
    Status status = Status::Missing;
    std::string message;
    P3DModel model;
    std::vector<MeshVertex> verts;   // first LOD, local (MLOD) coordinates
    std::vector<uint32_t> vertPoint; // vertex -> point of that LOD
    struct Section { std::string texture, material; uint32_t first = 0, count = 0; };
    std::vector<Section> sections;
    // character rig (crew model): skeleton from its model.cfg
    bool rigged = false;
    Skeleton skeleton;
    SkinData skin;
    std::filesystem::path modelCfg;
    // nested proxies of the first LOD (weapon -> optic -> ...), baked into verts / sections: their files (hot reload)
    std::vector<std::filesystem::path> nested;
    int nestedCount = 0;
};

// Proxy paths the game fills itself (gear slots, muzzle flashes, light volumes): placeholders, not models to show.
bool IsPlaceholderProxy(const std::string& armaPathLower, const std::string& stemLower);

// One load per file, shared by every proxy that uses it.
class ModelCache
{
public:
    std::shared_ptr<AttachedModel> Get(const std::filesystem::path& file, bool rig, const std::filesystem::path& projectRoot);
    void Invalidate(const std::filesystem::path& file); // hot reload: next Get loads it again
    void Clear() { cache_.clear(); }
    void Merge(ModelCache&& other);                    // models loaded elsewhere (a worker) join this cache
    // the model files of a LOD's proxies (what BuildSlots will ask for), for loading them ahead on a worker
    static std::vector<std::filesystem::path> ProxyFiles(const P3DLod& lod, const std::filesystem::path& projectRoot);
    std::vector<std::filesystem::path> Files() const; // loaded files (+ model.cfg of rigs) to watch
    static constexpr int kMaxNesting = 4;             // character -> weapon -> optic -> ... (proxies inside proxies)

private:
    std::shared_ptr<AttachedModel> Load(const std::filesystem::path& file, bool rig, const std::filesystem::path& projectRoot, int depth);
    void ExpandNested(AttachedModel& m, const std::filesystem::path& projectRoot, int depth);
    std::map<std::wstring, std::shared_ptr<AttachedModel>> cache_; // key: lower-case absolute path
    std::vector<std::wstring> loading_;                              // files being loaded (proxy cycles)
};

enum class SeatRole { None, Driver, Gunner, Commander, Cargo };
const char* SeatRoleName(SeatRole r);

struct ProxySlot
{
    std::string selection;   // proxy:\a3\data_f\proxies\crew_tank01\driver.001
    std::string armaPath;    // \a3\data_f\proxies\crew_tank01\driver
    std::string stem;        // driver
    int index = 0;           // .001 -> 1
    uint32_t points[3] = {};
    SeatRole role = SeatRole::None;
    std::shared_ptr<AttachedModel> model; // null for crew proxies (the crew preview draws there)
    int seat = -1;                        // crew seat placed here
    glm::mat4 xf{ 1.0f };                 // current transform (posed)
    glm::mat4 xfRest{ 1.0f };             // transform in the vehicle's rest pose (IK reference)
    bool valid = false;                   // non-degenerate triangle (a hide animation collapses it)
    bool destroyed = false;               // destroyed-state proxy (wreck model): shown only with showDestroyed
    bool effect = false;                  // muzzle flash / light volume: shown only with showEffects
    // weapon slots (Optics mode): the chosen item's model drawn here instead of the placeholder, with a temporary
    // viewer-only correction (weapon space: adjust * xf)
    std::shared_ptr<AttachedModel> item;
    glm::mat4 adjust{ 1.0f };
};

// Proxy triangle -> proxy model space to parent space (engine rule, see the file comment). valid: non-degenerate.
glm::mat4 ProxyMatrix(const glm::vec3 p[3], bool* valid = nullptr);
// Rebuilds a model's drawable geometry (verts / sections) from its first LOD; faceOverride may change a face's texture /
// material (hiddenSelectionsTextures of the class wearing it) and returns false for a face that is not drawn (texture
// ""). Nested proxies baked into the model are dropped.
using FaceOverride = std::function<bool(size_t face, std::string& texture, std::string& material)>;
void RebuildModelGeometry(AttachedModel& m, const FaceOverride& faceOverride = {});
// The hidden selections of the class wearing a model (sels / texs / mats of its config) on its first LOD: re-textured,
// "" faces left out. Returns the faces left out; found: hidden selections present in the model.
int RebuildWithHiddenSelections(AttachedModel& m, const std::vector<std::string>& sels, const std::vector<std::string>& texs,
                                const std::vector<std::string>& mats, int* found = nullptr);
// Skinned copy of a model's first-LOD vertices (point matrices of that LOD, e.g. from SkinMatrices).
void SkinModelVerts(const AttachedModel& m, const std::vector<glm::mat4>& pointMats, std::vector<MeshVertex>& out);

struct ConfigRef
{
    std::filesystem::path file;
    int line = 0;
    std::string Where() const;
};

struct CrewSeat
{
    std::string title;          // driver / turret MainTurret / cargo 3
    SeatRole role = SeatRole::None;
    int proxyIndex = 1;
    std::string actionOut, actionIn; // *Action / *InAction (cargo: actionOut only)
    ConfigRef actionRef;
    // resolved for the current In/Out choice
    std::string action, state, rtmArma;
    ConfigRef manActionRef, stateRef;
    std::filesystem::path rtmFile;
    std::string problem;        // shown in Validation (missing action / state / RTM / proxy)
    bool problemIsWarning = false; // e.g. action not set at all (the game uses its default)
    bool binarized = false;     // its pose RTM is a binarized file (not supported): shown as a frame
    std::string overrideRtm;    // manual RTM (path on disk), empty = from config
    int slot = -1;              // proxy slot index
    std::shared_ptr<RtmAnimation> rtm;
    std::vector<MeshVertex> posed; // crew model posed by the RTM (local coordinates)
    // playback
    float duration = 1.0f;         // seconds per cycle (CfgMoves speed)
    float interpolationSpeed = 6.0f;
    bool looped = true;
    bool playing = false;          // this seat plays even if "play all" is off
    float speed = 1.0f;
    double time = 0.0;
    std::vector<std::pair<std::string, std::string>> related; // label -> state (actions of the state, get in/out, in/out)
    // transition chain: states played one after another, blended over 1 / interpolationSpeed of the next one
    struct ChainItem { std::string state; std::shared_ptr<RtmAnimation> rtm; float duration = 1, interpolationSpeed = 6; bool looped = false; bool atGetIn = false; };
    std::vector<ChainItem> chain;
    int chainIndex = -1;          // >= 0: chain playing
    double chainTime = 0.0;
    // get in / out position: memoryPointsGetIn<Driver|Gunner|Cargo>(Dir) in the Memory LOD
    std::string getInPoint, getInDir;
    bool hasGetIn = false;
    glm::mat4 getInXf{ 1.0f };
    bool atGetInNow = false;      // the playing chain item is a get in / out (placed at getInXf)
    // IK: vehicle bones the left hand, right hand, left leg, right leg follow (driverLeftHandAnimName, ...)
    std::string ikBone[4];
    // bound evaluation stack for the current animation pair (re-bound only when it changes)
    AnimStack poseStack;
    const RtmAnimation* poseA = nullptr;
    const RtmAnimation* poseB = nullptr;
    bool poseFollow = false;
};

// Two-bone IK on deformation matrices (model space): the end joint of a -> b -> c moves to target, the bend plane is
// kept (pole = current middle joint). Returns the rotation the chain applied to the end bone.
glm::quat TwoBoneIK(const Skeleton& sk, std::vector<glm::mat4>& bones, int a, int b, int c, const glm::vec3& target);
// Turns bone b and its children by q about pivot (model space).
void RotateSubtree(const Skeleton& sk, std::vector<glm::mat4>& bones, int b, const glm::vec3& pivot, const glm::quat& q);

class Attachments
{
public:
    std::vector<uint8_t> hiddenSlot; // per slot: hidden by the viewer's hide / isolate
    bool showProxies = true;
    bool showCrew = true;
    bool showDestroyed = false;
    bool showEffects = false;      // muzzle flashes, light volumes (the game shows them only when firing / lights on)
    bool realGetIn = true;         // get in / out states play at the get-in memory point instead of the seat proxy    // wreck proxies (the game shows them when the vehicle is destroyed)
    bool turnedOut = false;
    bool followParent = true;  // crew bones without RTM data follow the parent (false: identity, not confirmed for Arma 3)
    bool ik = false;               // hands / legs follow the vehicle bones of *LeftHandAnimName ... (two-bone IK)
    bool playAll = false;          // crew playback (all seats)
    float speedAll = 1.0f;
    bool loopAll = true; // crew RTM without data for a bone: follow the parent instead of the bind pose
    std::string crewModel; // set at startup: the Arma 3 Samples man (armaview.local.ini: samples=)
    // extra models drawn after slots and crew, vertices already in the scene's space (weapon modes: the character)
    struct ExtraModel { std::shared_ptr<AttachedModel> model; std::vector<MeshVertex> posed; };
    std::vector<ExtraModel> extras;
    std::vector<ExtraModel> unitParts; // parts of the assembled unit (Unit: head, headgear, vest ...), posed by Unit::Pose

    // Proxy slots of a LOD; loads MLOD proxy models (binarized -> stub box, logged once per file).
    void BuildSlots(const P3DLod& lod, const ArmaConfig* cfg, const std::filesystem::path& projectRoot);
    // Seats from CfgVehicles (class using modelPath), matched to the slots; actions resolved to RTMs.
    void BuildCrew(const ArmaConfig* cfg, const std::string& modelPath, const std::filesystem::path& projectRoot);
    // Re-resolve actions / RTMs / poses (In/Out switch, RTM override, hot reload).
    void ResolveCrew(const ArmaConfig* cfg, const std::filesystem::path& projectRoot);

    // Advances seat playback / chains; true if any seat pose changed (geometry must be re-transformed).
    bool Advance(double dt);
    // Adds a state to the seat's chain (resolves its RTM); false with a log line if it has no usable RTM.
    bool AddToChain(CrewSeat& s, const std::string& state, const ArmaConfig* cfg, const std::filesystem::path& projectRoot, bool atGetIn = false);
    void SetMemoryLod(const P3DLod* memory) { memory_ = memory; }
    void PlayChain(CrewSeat& s) { if (!s.chain.empty()) { s.chainIndex = 0; s.chainTime = 0.0; } }
    // Re-poses a seat at a phase (and optionally blended towards a second animation).
    void PoseSeatAt(CrewSeat& s, const RtmAnimation& a, float phase, const RtmAnimation* b = nullptr, float phaseB = 0, float weightB = 0);

    // Vehicle skeleton and its current deformation matrices (empty = rest): IK targets.
    void SetVehiclePose(const Skeleton* skel, const std::vector<glm::mat4>& bones) { vehSkel_ = skel; vehBones_ = bones; }
    // Re-poses the seats that have IK bones (after the vehicle pose changed). True if any changed.
    bool RefreshIK();

    // Posed slot transforms from the current point positions of the LOD (pointMats empty = rest).
    void UpdateTransforms(const P3DLod& lod, const std::vector<glm::mat4>& pointMats);

    // Geometry appended after the vehicle mesh: sections (texture/material/range) and their local vertices.
    struct Section { std::string texture, material; uint32_t first = 0, count = 0; };
    void AppendGeometry(std::vector<MeshVertex>& verts, std::vector<Section>& sections);
    // Rewrites the appended vertices in place with the current transforms (verts as returned by AppendGeometry).
    void TransformGeometry(std::vector<MeshVertex>& verts, size_t base = 0) const; // verts[0] = mesh vertex `base`
    // Frames for proxies without a drawable model (binarized or missing file, binarized seat pose): lines with depth test.
    void StubLines(std::vector<LineVertex>& out) const;
    struct Label { glm::vec3 pos; std::string text; bool crew; };
    std::vector<Label> StubLabels() const;

    const std::vector<ProxySlot>& Slots() const { return slots_; }
    std::vector<ProxySlot>& SlotsMut() { return slots_; }
    std::vector<CrewSeat>& Seats() { return seats_; }
    const std::vector<CrewSeat>& Seats() const { return seats_; }
    const std::string& VehicleClass() const { return vehicleClass_; }
    std::vector<std::filesystem::path> WatchFiles() const;
    void Invalidate(const std::filesystem::path& file);
    ModelCache& Cache() { return cache_; }

private:
    std::shared_ptr<AttachedModel> CrewModel(const std::filesystem::path& projectRoot);
    void PoseSeat(CrewSeat& s, const std::filesystem::path& projectRoot);
    std::filesystem::path projectRoot_;
    const Skeleton* vehSkel_ = nullptr;
    const P3DLod* memory_ = nullptr;
    void ResolveGetIn(CrewSeat& s) const;
    std::vector<glm::mat4> vehBones_;
    void ApplyIK(const CrewSeat& s, const Skeleton& crew, std::vector<glm::mat4>& bones) const;
    void SkinSeat(CrewSeat& s, const AttachedModel& model, std::vector<glm::mat4>& bones);
    const ArmaConfig* lastCfg_ = nullptr;

    ModelCache cache_;
    bool deferPose_ = false;              // ResolveCrew: PoseSeat only loads, the posing runs in parallel after
    std::vector<CrewSeat*> posePending_;
    std::map<std::wstring, std::shared_ptr<RtmAnimation>> rtmCache_; // crew RTMs by lower-case path (seats share them)
    std::shared_ptr<RtmAnimation> LoadCachedRtm(const std::filesystem::path& file, std::string& err);
    std::vector<ProxySlot> slots_;
    std::vector<CrewSeat> seats_;
    std::string vehicleClass_;
    struct Instance { int slot; int seat; uint32_t first; uint32_t count; int extra = -1; int unit = -1; };
    std::vector<Instance> instances_; // ranges of AppendGeometry
    std::vector<std::string> loggedStubs_;
};
