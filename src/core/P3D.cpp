#include "P3D.h"
#include "Perf.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "CoreGuard.h"

namespace {

struct ParseError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Bounds-checked little-endian reader over a byte range. Offsets in errors are absolute file offsets.
class BinReader
{
public:
    BinReader(const uint8_t* data, size_t size, size_t base = 0) : d_(data), n_(size), base_(base) {}

    template <class T> T Read()
    {
        Need(sizeof(T));
        T v;
        std::memcpy(&v, d_ + o_, sizeof(T));
        o_ += sizeof(T);
        return v;
    }
    void Need(size_t k) const
    {
        if (k > n_ - o_)
            throw ParseError(std::format("unexpected end of data at offset {} (need {} bytes, {} left)", Offset(), k, n_ - o_));
    }
    void Skip(size_t k) { Need(k); o_ += k; }
    std::string Asciiz()
    {
        const void* z = std::memchr(d_ + o_, 0, n_ - o_);
        if (!z) throw ParseError(std::format("unterminated string at offset {}", Offset()));
        size_t len = static_cast<const uint8_t*>(z) - (d_ + o_);
        std::string s(reinterpret_cast<const char*>(d_ + o_), len);
        o_ += len + 1;
        return s;
    }
    std::string Fixed(size_t len)
    {
        Need(len);
        const char* p = reinterpret_cast<const char*>(d_ + o_);
        o_ += len;
        return std::string(p, strnlen(p, len));
    }
    std::string_view AsciizView()
    {
        const void* z = std::memchr(d_ + o_, 0, n_ - o_);
        if (!z) throw ParseError(std::format("unterminated string at offset {}", Offset()));
        const size_t len = static_cast<const uint8_t*>(z) - (d_ + o_);
        std::string_view s(reinterpret_cast<const char*>(d_ + o_), len);
        o_ += len + 1;
        return s;
    }
    std::string_view Sig4()
    {
        Need(4);
        std::string_view s(reinterpret_cast<const char*>(d_ + o_), 4);
        o_ += 4;
        return s;
    }
    BinReader Sub(size_t len)
    {
        Need(len);
        BinReader r(d_ + o_, len, base_ + o_);
        o_ += len;
        return r;
    }
    const uint8_t* Ptr() const { return d_ + o_; }
    size_t Offset() const { return base_ + o_; }
    size_t Left() const { return n_ - o_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t base_;
    size_t o_ = 0;
};

std::string Printable(std::string_view s)
{
    std::string r;
    for (char c : s) r += (c >= 32 && c < 127) ? c : '?';
    return r;
}

bool IsKnownIgnoredTagg(std::string_view name)
{
    // Editor-only state in Object Builder; not needed by the viewer.
    return name == "#Lock#" || name == "#Selected#" || name == "#Hide#" || name == "#Hidden#";
}

void ParseTagg(const std::string& name, BinReader data, P3DLod& lod, int lodIndex)
{
    const size_t nPoints = lod.points.size();
    const size_t nFaces = lod.faces.size();
    const size_t size = data.Left();

    if (name == "#Property#")
    {
        if (size != 128) Log::Get().Warn("LOD {}: #Property# has {} bytes (expected 128)", lodIndex, size);
        P3DProperty p;
        p.name = AnsiToUtf8(data.Fixed(64));
        p.value = AnsiToUtf8(data.Fixed(64));
        lod.properties.push_back(std::move(p));
    }
    else if (name == "#Mass#")
    {
        if (size != nPoints * 4) Log::Get().Warn("LOD {}: #Mass# has {} bytes, expected {} (4 x {} points)", lodIndex, size, nPoints * 4, nPoints);
        size_t n = std::min(size / 4, nPoints);
        lod.mass.resize(n);
        for (size_t i = 0; i < n; ++i) lod.mass[i] = data.Read<float>();
    }
    else if (name == "#SharpEdges#")
    {
        if (size % 8) Log::Get().Warn("LOD {}: #SharpEdges# size {} is not a multiple of 8", lodIndex, size);
        size_t n = size / 8;
        lod.sharpEdges.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            uint32_t a = data.Read<uint32_t>(), b = data.Read<uint32_t>();
            lod.sharpEdges.emplace_back(a, b);
        }
    }
    else if (name == "#UVSet#")
    {
        P3DUVSet set;
        set.stage = data.Read<uint32_t>();
        size_t corners = 0;
        for (const auto& f : lod.faces) corners += f.count;
        if (data.Left() != corners * 8)
            Log::Get().Warn("LOD {}: #UVSet# stage {} has {} UV bytes, expected {} ({} face corners)", lodIndex, set.stage, data.Left(), corners * 8, corners);
        size_t n = std::min(data.Left() / 8, corners);
        set.uv.resize(n);
        for (size_t i = 0; i < n; ++i) { set.uv[i].x = data.Read<float>(); set.uv[i].y = data.Read<float>(); }
        lod.uvSets.push_back(std::move(set));
    }
    else if (name == "#Animation#")
    {
        ++lod.animationFrames; // float time + vec3 per point; used for vertex animation, not needed yet
    }
    else if (name.starts_with("#"))
    {
        if (!IsKnownIgnoredTagg(name))
        {
            Log::Get().Warn("LOD {}: skipped unknown TAGG '{}' ({} bytes)", lodIndex, Printable(name), size);
            lod.skippedTaggs.push_back(std::format("{} ({} bytes)", Printable(name), size));
        }
    }
    else
    {
        // Named selection: nPoints weight bytes + nFaces selection bytes.
        if (size != nPoints + nFaces)
            Log::Get().Warn("LOD {}: selection '{}' has {} bytes, expected {} (points {} + faces {})",
                lodIndex, AnsiToUtf8(name), size, nPoints + nFaces, nPoints, nFaces);
        P3DSelection sel;
        sel.name = AnsiToUtf8(name);
        sel.engineIgnored = !name.empty() && (name[0] == '-' || name[0] == '.');
        size_t np = std::min(size, nPoints);
        sel.pointBytes.assign(data.Ptr(), data.Ptr() + np);
        data.Skip(np);
        size_t nf = std::min(data.Left(), nFaces);
        sel.faceBytes.assign(data.Ptr(), data.Ptr() + nf);
        for (uint8_t b : sel.pointBytes)
        {
            if (b) ++sel.pointCount;
            if (b > 1) ++sel.partialCount;
        }
        for (uint8_t b : sel.faceBytes) if (b) ++sel.faceCount;
        lod.selections.push_back(std::move(sel));
    }
}

void ParseLod(BinReader& r, P3DLod& lod, int lodIndex)
{
    size_t start = r.Offset();
    auto sig = r.Sig4();
    if (sig != "P3DM")
    {
        if (sig == "SP3X") throw ParseError(std::format("LOD {} uses old SP3X format (resave in Object Builder)", lodIndex));
        throw ParseError(std::format("LOD {} at offset {}: expected 'P3DM', got '{}'", lodIndex, start, Printable(sig)));
    }
    lod.versionMajor = r.Read<uint32_t>();
    lod.versionMinor = r.Read<uint32_t>();
    uint32_t nPoints = r.Read<uint32_t>();
    uint32_t nNormals = r.Read<uint32_t>();
    uint32_t nFaces = r.Read<uint32_t>();
    lod.flags = r.Read<uint32_t>();
    if (lod.versionMajor != 0x1C || lod.versionMinor != 0x100)
        Log::Get().Warn("LOD {}: unexpected P3DM version {}.{} (expected 28.256), trying anyway", lodIndex, lod.versionMajor, lod.versionMinor);

    r.Need(size_t(nPoints) * 16 + size_t(nNormals) * 12); // sanity before allocating
    lod.points.resize(nPoints);
    for (auto& p : lod.points)
    {
        p.pos.x = r.Read<float>(); p.pos.y = r.Read<float>(); p.pos.z = r.Read<float>();
        p.flags = r.Read<uint32_t>();
    }
    lod.normals.resize(nNormals);
    for (auto& n : lod.normals) { n.x = r.Read<float>(); n.y = r.Read<float>(); n.z = r.Read<float>(); }

    r.Need(size_t(nFaces) * 72);
    lod.faces.resize(nFaces);
    // texture / material strings repeat face after face: convert once per change (AnsiToUtf8 is not free)
    std::string_view lastTexRaw, lastMatRaw;
    std::string lastTex, lastMat;
    bool first = true;
    for (uint32_t i = 0; i < nFaces; ++i)
    {
        P3DFace& f = lod.faces[i];
        f.count = r.Read<uint32_t>();
        if (f.count != 3 && f.count != 4)
            throw ParseError(std::format("LOD {} face {} at offset {}: vertex count {} (expected 3 or 4)", lodIndex, i, r.Offset() - 4, f.count));
        for (auto& v : f.v)
        {
            v.point = r.Read<uint32_t>(); v.normal = r.Read<uint32_t>();
            v.u = r.Read<float>(); v.v = r.Read<float>();
        }
        f.flags = r.Read<uint32_t>();
        const std::string_view tex = r.AsciizView(), mat = r.AsciizView();
        if (first || tex != lastTexRaw) { lastTexRaw = tex; lastTex = AnsiToUtf8(tex); }
        if (first || mat != lastMatRaw) { lastMatRaw = mat; lastMat = AnsiToUtf8(mat); }
        first = false;
        f.texture = lastTex;
        f.material = lastMat;
    }

    auto tagSig = r.Sig4();
    if (tagSig != "TAGG")
        throw ParseError(std::format("LOD {} at offset {}: expected 'TAGG', got '{}'", lodIndex, r.Offset() - 4, Printable(tagSig)));

    for (;;)
    {
        size_t taggOffset = r.Offset();
        r.Read<uint8_t>(); // "active" flag, always 1
        std::string name = r.Asciiz();
        uint32_t size = r.Read<uint32_t>();
        if (size > r.Left())
            throw ParseError(std::format("LOD {}: TAGG '{}' at offset {} claims {} bytes, only {} left", lodIndex, Printable(name), taggOffset, size, r.Left()));
        BinReader data = r.Sub(size);
        if (name == "#EndOfFile#") break;
        try
        {
            ParseTagg(name, data, lod, lodIndex);
        }
        catch (const ParseError& e) // a broken tagg is skipped by its size, the rest of the LOD is still fine
        {
            Log::Get().Error("LOD {}: TAGG '{}' at offset {} is malformed: {}", lodIndex, Printable(name), taggOffset, e.what());
        }
    }
    lod.resolution = r.Read<float>();

    // Validate indices so the renderer can trust them.
    size_t badFaces = 0;
    for (const auto& f : lod.faces)
        for (uint32_t c = 0; c < f.count; ++c)
            if (f.v[c].point >= nPoints || f.v[c].normal >= nNormals) { ++badFaces; break; }
    if (badFaces) Log::Get().Error("LOD {}: {} faces reference points/normals out of range (they will not be drawn)", lodIndex, badFaces);
}

// The byte size of the LOD at r's position without parsing it (faces: fixed part + two strings; TAGGs by their sizes).
size_t SkipLod(BinReader r)
{
    const size_t start = r.Offset();
    if (r.Sig4() != "P3DM") throw ParseError(std::format("expected 'P3DM' at offset {}", start));
    r.Skip(8);
    const uint32_t nPoints = r.Read<uint32_t>(), nNormals = r.Read<uint32_t>(), nFaces = r.Read<uint32_t>();
    r.Skip(4);
    r.Skip(size_t(nPoints) * 16 + size_t(nNormals) * 12);
    for (uint32_t i = 0; i < nFaces; ++i)
    {
        r.Skip(4 + 64 + 4);
        r.AsciizView();
        r.AsciizView();
    }
    if (r.Sig4() != "TAGG") throw ParseError(std::format("expected 'TAGG' at offset {}", r.Offset() - 4));
    for (;;)
    {
        r.Skip(1);
        const std::string_view name = r.AsciizView();
        const uint32_t size = r.Read<uint32_t>();
        r.Skip(size);
        if (name == "#EndOfFile#") break;
    }
    r.Skip(4); // resolution
    return r.Offset() - start;
}

} // namespace

std::string LodResolutionName(float res)
{
    struct Named { double value; const char* name; };
    static const Named table[] = {
        { 1.0e3, "View Gunner" }, { 1.1e3, "View Pilot" }, { 1.2e3, "View Cargo" },
        { 1.0e13, "Geometry" }, { 2.0e13, "Geometry Buoyancy" }, { 4.0e13, "Geometry PhysX" },
        { 1.0e15, "Memory" }, { 2.0e15, "Land Contact" }, { 3.0e15, "Roadway" }, { 4.0e15, "Paths" },
        { 5.0e15, "Hit-points" }, { 6.0e15, "View Geometry" }, { 7.0e15, "Fire Geometry" },
        { 8.0e15, "View Cargo Geometry" }, { 9.0e15, "View Cargo Fire Geometry" }, { 1.0e16, "View Commander" },
        { 1.1e16, "View Commander Geometry" }, { 1.2e16, "View Commander Fire Geometry" },
        { 1.3e16, "View Pilot Geometry" }, { 1.4e16, "View Pilot Fire Geometry" },
        { 1.5e16, "View Gunner Geometry" }, { 1.6e16, "View Gunner Fire Geometry" }, { 1.7e16, "Sub Parts" },
        { 1.8e16, "Shadow Volume - View Cargo" }, { 1.9e16, "Shadow Volume - View Pilot" },
        { 2.0e16, "Shadow Volume - View Gunner" }, { 2.1e16, "Wreck" },
    };
    double r = res;
    for (const auto& n : table)
        if (std::abs(r - n.value) <= n.value * 1e-5) return n.name;
    if (r >= 1.0e4 && r < 2.0e4) return std::format("Shadow Volume {:g}", r - 1.0e4);
    if (r >= 1.2e3 && r < 1.3e3) return std::format("View Cargo {:g}", r - 1.2e3);
    if (r < 1.0e3) return std::format("{:.3f}", r);
    return std::format("Unknown {:g}", r);
}

std::string P3DLod::Name() const { return LodResolutionName(resolution); }

std::vector<glm::vec3> ComputeCornerNormals(const P3DLod& lod, bool useSharpEdges)
{
    const size_t nFaces = lod.faces.size(), nPoints = lod.points.size();
    std::vector<glm::vec3> result(nFaces * 4, glm::vec3(0.0f));

    auto valid = [&](const P3DFace& f) {
        for (uint32_t c = 0; c < f.count; ++c)
            if (f.v[c].point >= nPoints) return false;
        return true;
    };
    // Area-weighted face normals (unnormalized cross product).
    std::vector<glm::vec3> faceN(nFaces, glm::vec3(0.0f));
    for (size_t i = 0; i < nFaces; ++i)
    {
        const P3DFace& f = lod.faces[i];
        if (!valid(f)) continue;
        auto P = [&](int c) { return lod.points[f.v[c].point].pos; };
        faceN[i] = f.count == 4 ? glm::cross(P(2) - P(0), P(3) - P(1)) : glm::cross(P(1) - P(0), P(2) - P(0));
    }

    // Union-find over corners (face*4 + corner).
    std::vector<uint32_t> uf(nFaces * 4);
    for (uint32_t i = 0; i < uf.size(); ++i) uf[i] = i;
    auto find = [&](uint32_t x) {
        while (uf[x] != x) x = uf[x] = uf[uf[x]];
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b) { uf[find(a)] = find(b); };
    auto key = [](uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };

    if (useSharpEdges)
    {
        std::unordered_set<uint64_t> sharp;
        for (auto [a, b] : lod.sharpEdges) sharp.insert(key(a, b));
        // edge -> first (face, cornerA, cornerB) seen; later faces on the same edge are joined to it
        struct EdgeRef { uint32_t face, ca, cb; };
        std::unordered_map<uint64_t, std::vector<EdgeRef>> edges;
        edges.reserve(nFaces * 4);
        for (uint32_t i = 0; i < nFaces; ++i)
        {
            const P3DFace& f = lod.faces[i];
            if (!valid(f)) continue;
            for (uint32_t c = 0; c < f.count; ++c)
            {
                uint32_t d = (c + 1) % f.count;
                uint32_t pa = f.v[c].point, pb = f.v[d].point;
                uint64_t k = key(pa, pb);
                if (sharp.count(k)) continue;
                auto& list = edges[k];
                for (const EdgeRef& e : list)
                {
                    const P3DFace& g = lod.faces[e.face];
                    // match corners by point index
                    uint32_t ga = g.v[e.ca].point == pa ? e.ca : e.cb;
                    uint32_t gb = g.v[e.ca].point == pa ? e.cb : e.ca;
                    unite(i * 4 + c, e.face * 4 + ga);
                    unite(i * 4 + d, e.face * 4 + gb);
                }
                list.push_back({ i, c, d });
            }
        }
    }
    else
    {
        // Fully smooth: every corner on the same point joins one group.
        std::vector<int64_t> first(nPoints, -1);
        for (uint32_t i = 0; i < nFaces; ++i)
        {
            const P3DFace& f = lod.faces[i];
            if (!valid(f)) continue;
            for (uint32_t c = 0; c < f.count; ++c)
            {
                auto& fp = first[f.v[c].point];
                if (fp < 0) fp = i * 4 + c;
                else unite(i * 4 + c, uint32_t(fp));
            }
        }
    }

    std::vector<glm::vec3> acc(nFaces * 4, glm::vec3(0.0f));
    for (uint32_t i = 0; i < nFaces; ++i)
        for (uint32_t c = 0; c < lod.faces[i].count; ++c) acc[find(i * 4 + c)] += faceN[i];
    for (uint32_t i = 0; i < nFaces; ++i)
        for (uint32_t c = 0; c < lod.faces[i].count; ++c)
        {
            glm::vec3 n = acc[find(i * 4 + c)];
            float len = glm::length(n);
            result[i * 4 + c] = len > 1e-12f ? n / len : glm::vec3(0, 1, 0);
        }
    return result;
}

size_t P3DLod::TriangleCount() const
{
    size_t n = 0;
    for (const auto& f : faces) n += f.count - 2;
    return n;
}

float P3DLod::TotalMass() const
{
    float m = 0;
    for (float v : mass) m += v;
    return m;
}

bool LoadP3D(const std::filesystem::path& path, P3DModel& out)
{
    std::vector<uint8_t> bytes;
    {
        PerfNoteScope ps("load:   P3D file read");
        std::ifstream f;
        f.rdbuf()->pubsetbuf(nullptr, 0); // unbuffered: the whole file in one read
        f.open(path, std::ios::binary | std::ios::ate);
        if (!f)
        {
            out = {};
            out.path = path;
            Log::Get().Error("Cannot open '{}'", PathToUtf8(path));
            return false;
        }
        bytes.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f)
        {
            out = {};
            out.path = path;
            Log::Get().Error("Read error on '{}'", PathToUtf8(path));
            return false;
        }
    }
    return LoadP3D(path, bytes, out);
}

bool LoadP3D(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, P3DModel& out)
{
    out = {};
    out.path = path;
    auto& log = Log::Get();
    const std::string pathStr = PathToUtf8(path);
    BinReader r(bytes.data(), bytes.size());
    uint32_t nLods = 0;
    try
    {
        auto sig = r.Sig4();
        if (sig != "MLOD")
        {
            log.Error("'{}': not an MLOD P3D (signature '{}'). {}", pathStr, Printable(sig), kBinarizedNotSupported);
            return false;
        }
        out.version = r.Read<uint32_t>();
        nLods = r.Read<uint32_t>();
        if (out.version != 0x101) log.Warn("MLOD version {:#x} (expected 0x101), trying anyway", out.version);
        if (nLods == 0 || nLods > 4096)
        {
            log.Error("'{}': implausible LOD count {}", pathStr, nLods);
            return false;
        }
    }
    catch (const ParseError& e)
    {
        log.Error("'{}': {}", pathStr, e.what());
        return false;
    }

    // where each LOD starts (a fast scan), then the LODs in parallel; a LOD that does not scan or parse ends the model
    // there, the ones before it are kept (as before)
    std::vector<size_t> starts;
    {
        size_t o = r.Offset();
        for (uint32_t i = 0; i < nLods; ++i)
        {
            starts.push_back(o);
            try
            {
                o += SkipLod(BinReader(bytes.data() + o, bytes.size() - o, o));
            }
            catch (const std::exception&)
            {
                break; // parsed below, it reports the error
            }
        }
    }
    struct Parsed { P3DLod lod; std::string error; size_t end = 0; std::vector<LogEntry> log; };
    std::vector<Parsed> parsed(starts.size());
    {
        std::atomic<size_t> next{ 0 };
        std::vector<std::jthread> pool;
        const unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(starts.size())));
        for (unsigned w = 0; w < n; ++w)
            pool.emplace_back([&] {
                for (size_t i; (i = next.fetch_add(1)) < starts.size();)
                {
                    BinReader lr(bytes.data() + starts[i], bytes.size() - starts[i], starts[i]);
                    Log::CaptureScope capture(parsed[i].log); // replayed below on the calling thread, in LOD order
                    try
                    {
                        ParseLod(lr, parsed[i].lod, int(i));
                        parsed[i].end = lr.Offset();
                    }
                    catch (const ParseError& e) { parsed[i].error = e.what(); }
                    catch (const std::exception& e) { parsed[i].error = std::format("LOD {}: {}", i, e.what()); } // bad_alloc from a corrupt count
                }
            });
    }
    size_t end = r.Offset();
    for (auto& p : parsed)
    {
        for (auto& e : p.log) log.Add(e.level, std::move(e.text)); // the caller's capture (Mod check, fuzz) applies
        if (!p.error.empty())
        {
            log.Error("'{}': {} — keeping {} LOD(s) read before the error", pathStr, p.error, out.lods.size());
            break;
        }
        end = p.end;
        out.lods.push_back(std::move(p.lod));
    }
    if (out.lods.size() == starts.size() && starts.size() < nLods)
        log.Error("'{}': LOD {} could not be located — keeping {} LOD(s) read before it", pathStr, starts.size(), out.lods.size());
    r = BinReader(bytes.data() + end, bytes.size() - end, end);
    out.complete = out.lods.size() == nLods;
    if (r.Left() > 0 && out.lods.size() == nLods)
        log.Debug("'{}': {} trailing bytes after last LOD (ignored)", pathStr, r.Left());

    return !out.lods.empty();
}
