#include "ninja.h"
#include <unordered_set>
#include <functional>

namespace sonic {

static inline u32 argbToRgba(u32 c) {
    // NJS_COLOR is stored as B,G,R,A bytes (u32 ARGB little endian)
    return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000);
}

bool NinjaReader::looksLikeModel(u32 va) const {
    if (!pe_.valid(va, 0x28)) return false;
    u32 pts = pe_.u32at(va), nrm = pe_.u32at(va + 4);
    s32 npt = pe_.read<s32>(va + 8);
    u32 ms = pe_.u32at(va + 12), mats = pe_.u32at(va + 16);
    u16 nms = pe_.read<u16>(va + 20), nmat = pe_.read<u16>(va + 22);
    if (npt <= 0 || npt > 65535 || !pe_.valid(pts, u32(npt) * 12)) return false;
    if (nrm && !pe_.valid(nrm, u32(npt) * 12)) return false;
    if (nms > 4096 || (nms && !pe_.valid(ms, u32(nms) * 0x1C))) return false;
    if (nmat > 4096 || (nmat && !pe_.valid(mats, u32(nmat) * 0x14))) return false;
    return true;
}

bool NinjaReader::looksLikeObject(u32 va) const {
    if (!pe_.valid(va, 0x34)) return false;
    u32 fl = pe_.u32at(va);
    if (fl > 0xFFFF) return false;
    u32 mdl = pe_.u32at(va + 4);
    if (mdl && !looksLikeModel(mdl)) return false;
    for (int i = 0; i < 3; i++) {
        float f = pe_.read<float>(va + 8 + i * 4);
        if (!std::isfinite(f) || std::fabs(f) > 1e7f) return false;
    }
    u32 ch = pe_.u32at(va + 0x2C), sib = pe_.u32at(va + 0x30);
    if (ch && !pe_.valid(ch, 0x34)) return false;
    if (sib && !pe_.valid(sib, 0x34)) return false;
    return true;
}

std::shared_ptr<NjModel> NinjaReader::readModel(u32 va) {
    auto it = modelCache_.find(va);
    if (it != modelCache_.end()) return it->second;
    if (!looksLikeModel(va)) return nullptr;
    auto m = std::make_shared<NjModel>();
    m->va = va;
    u32 ptsVA = pe_.u32at(va), nrmVA = pe_.u32at(va + 4);
    s32 npt = pe_.read<s32>(va + 8);
    u32 msVA = pe_.u32at(va + 12), matVA = pe_.u32at(va + 16);
    u16 nms = pe_.read<u16>(va + 20), nmat = pe_.read<u16>(va + 22);
    m->center = {pe_.read<float>(va + 24), pe_.read<float>(va + 28), pe_.read<float>(va + 32)};
    m->radius = pe_.read<float>(va + 36);

    const u8* pts = pe_.ptr(ptsVA);
    const u8* nrms = nrmVA ? pe_.ptr(nrmVA) : nullptr;
    auto point = [&](int i) { return Vec3(rd<float>(pts + i * 12), rd<float>(pts + i * 12 + 4), rd<float>(pts + i * 12 + 8)); };
    auto normal = [&](int i) { return nrms ? Vec3(rd<float>(nrms + i * 12), rd<float>(nrms + i * 12 + 4), rd<float>(nrms + i * 12 + 8)) : Vec3(0, 1, 0); };

    for (u16 i = 0; i < nmat; i++) {
        u32 a = matVA + i * 0x14;
        NjMaterial mt;
        mt.diffuse = pe_.u32at(a);
        mt.specular = pe_.u32at(a + 4);
        mt.exponent = pe_.read<float>(a + 8);
        mt.texId = pe_.u32at(a + 12) & 0x3FFF;
        mt.flags = pe_.u32at(a + 16);
        m->mats.push_back(mt);
    }
    if (m->mats.empty()) m->mats.push_back(NjMaterial());

    m->points.resize(npt);
    m->normals.resize(npt);
    for (int i = 0; i < npt; i++) {
        m->points[i] = point(i);
        m->normals[i] = normal(i);
    }
    m->bmin = Vec3(1e30f, 1e30f, 1e30f);
    m->bmax = Vec3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < npt; i++) {
        m->bmin = vmin(m->bmin, point(i));
        m->bmax = vmax(m->bmax, point(i));
    }

    for (u16 s = 0; s < nms; s++) {
        u32 a = msVA + s * 0x1C;
        u16 typeMat = pe_.read<u16>(a);
        u16 nbMesh = pe_.read<u16>(a + 2);
        u32 meshesVA = pe_.u32at(a + 4);
        u32 vcVA = pe_.u32at(a + 16);
        u32 uvVA = pe_.u32at(a + 20);
        int type = typeMat >> 14;
        int matId = typeMat & 0x3FFF;
        if (!meshesVA || !nbMesh) continue;

        NjMeshPart part;
        part.material = matId < (int)m->mats.size() ? matId : 0;
        part.firstIndex = (u32)m->indices.size();
        part.hasVColor = vcVA != 0;
        part.hasUV = uvVA != 0;

        u32 corner = 0;
        u32 cursor = meshesVA;
        auto readIdx = [&](u32& c) -> int {
            int v = pe_.read<u16>(c, 0xFFFF);
            c += 2;
            return v;
        };
        auto makeVert = [&](int pi, u32 cn) -> u32 {
            NjVertex v;
            if (pi >= 0 && pi < npt) {
                v.pos = point(pi);
                v.nrm = normal(pi);
            }
            if (uvVA && pe_.valid(uvVA + cn * 4, 4)) {
                v.u = float(pe_.read<s16>(uvVA + cn * 4)) / 255.0f;
                v.v = float(pe_.read<s16>(uvVA + cn * 4 + 2)) / 255.0f;
            }
            if (vcVA && pe_.valid(vcVA + cn * 4, 4)) v.color = argbToRgba(pe_.u32at(vcVA + cn * 4));
            m->verts.push_back(v);
            m->pointOf.push_back(pi);
            return u32(m->verts.size() - 1);
        };
        auto tri = [&](u32 a0, u32 a1, u32 a2) {
            m->indices.push_back(a0);
            m->indices.push_back(a1);
            m->indices.push_back(a2);
        };
        bool bad = false;
        for (u16 p = 0; p < nbMesh && !bad; p++) {
            if (type == 0 || type == 1) {
                int n = type == 0 ? 3 : 4;
                u32 vi[4];
                for (int k = 0; k < n; k++) {
                    int pi = readIdx(cursor);
                    if (pi >= npt) { bad = true; break; }
                    vi[k] = makeVert(pi, corner++);
                }
                if (bad) break;
                tri(vi[0], vi[1], vi[2]);
                if (n == 4) tri(vi[2], vi[1], vi[3]);
            } else {
                int hdr = readIdx(cursor);
                int n = hdr & 0x7FFF;
                bool flip = (hdr & 0x8000) != 0;
                if (n > 4096) { bad = true; break; }
                std::vector<u32> vi(n);
                std::vector<int> pi(n);
                for (int k = 0; k < n; k++) {
                    pi[k] = readIdx(cursor);
                    if (pi[k] >= npt) { bad = true; break; }
                    vi[k] = makeVert(pi[k], corner++);
                }
                if (bad) break;
                if (type == 3) {
                    for (int k = 0; k + 2 < n; k++) {
                        if (pi[k] == pi[k + 1] || pi[k + 1] == pi[k + 2] || pi[k] == pi[k + 2]) continue;
                        bool odd = ((k & 1) != 0) != flip;
                        if (odd) tri(vi[k + 1], vi[k], vi[k + 2]);
                        else tri(vi[k], vi[k + 1], vi[k + 2]);
                    }
                } else {
                    for (int k = 1; k + 1 < n; k++) tri(vi[0], vi[k], vi[k + 1]);
                }
            }
        }
        part.indexCount = (u32)m->indices.size() - part.firstIndex;
        if (part.indexCount) m->parts.push_back(part);
    }
    modelCache_[va] = m;
    return m;
}

bool NinjaReader::readSkeleton(u32 rootVA, NjSkeleton& out, int maxNodes) {
    out.nodes.clear();
    out.models.clear();
    if (!looksLikeObject(rootVA)) return false;
    std::unordered_map<u32, int> modelIndex;
    std::unordered_set<u32> visited;
    // iterative depth-first (node, child subtree, then siblings)
    std::function<void(u32, int)> walk = [&](u32 va, int parent) {
        while (va && (int)out.nodes.size() < maxNodes) {
            if (visited.count(va) || !looksLikeObject(va)) return;
            visited.insert(va);
            NjNode n;
            n.va = va;
            n.parent = parent;
            n.flags = pe_.u32at(va);
            u32 mdl = pe_.u32at(va + 4);
            n.pos = {pe_.read<float>(va + 8), pe_.read<float>(va + 12), pe_.read<float>(va + 16)};
            n.ang[0] = pe_.read<s32>(va + 20);
            n.ang[1] = pe_.read<s32>(va + 24);
            n.ang[2] = pe_.read<s32>(va + 28);
            n.scl = {pe_.read<float>(va + 32), pe_.read<float>(va + 36), pe_.read<float>(va + 40)};
            if (mdl) {
                auto it = modelIndex.find(mdl);
                if (it != modelIndex.end()) n.model = it->second;
                else {
                    auto m = readModel(mdl);
                    if (m) {
                        n.model = (int)out.models.size();
                        out.models.push_back(m);
                        modelIndex[mdl] = n.model;
                    }
                }
            }
            int me = (int)out.nodes.size();
            out.nodes.push_back(n);
            u32 child = pe_.u32at(va + 0x2C);
            if (child) walk(child, me);
            va = pe_.u32at(va + 0x30);
        }
    };
    walk(rootVA, -1);
    return !out.nodes.empty();
}

bool NinjaReader::readMotion(u32 va, size_t nodeCount, NjMotion& out) {
    out = NjMotion();
    if (!pe_.valid(va, 12)) return false;
    u32 mdata = pe_.u32at(va);
    u32 frames = pe_.u32at(va + 4);
    u16 type = pe_.read<u16>(va + 8);
    u16 inp = pe_.read<u16>(va + 10);
    int elems = inp & 0xF;
    if (!mdata || frames == 0 || frames > 100000 || elems < 1 || elems > 8) return false;
    out.va = va;
    out.frames = frames;
    out.tracks.resize(nodeCount);
    // element order follows the bit order of `type`
    std::vector<u32> kinds;
    for (u32 b = 0; b < 16 && (int)kinds.size() < elems; b++)
        if (type & (1u << b)) kinds.push_back(1u << b);
    u32 stride = u32(elems) * 8;
    for (size_t n = 0; n < nodeCount; n++) {
        u32 md = mdata + u32(n) * stride;
        if (!pe_.valid(md, stride)) break;
        for (int e = 0; e < (int)kinds.size(); e++) {
            u32 keys = pe_.u32at(md + e * 4);
            u32 cnt = pe_.u32at(md + elems * 4 + e * 4);
            if (!keys || !cnt || cnt > 100000 || !pe_.valid(keys, cnt * 16)) continue;
            for (u32 k = 0; k < cnt; k++) {
                u32 a = keys + k * 16;
                if (kinds[e] == 1 || kinds[e] == 4) {
                    NjKey3f key{pe_.u32at(a), Vec3(pe_.read<float>(a + 4), pe_.read<float>(a + 8), pe_.read<float>(a + 12))};
                    (kinds[e] == 1 ? out.tracks[n].pos : out.tracks[n].scl).push_back(key);
                } else if (kinds[e] == 2) {
                    NjKey3i key{pe_.u32at(a), {pe_.read<s32>(a + 4), pe_.read<s32>(a + 8), pe_.read<s32>(a + 12)}};
                    out.tracks[n].ang.push_back(key);
                }
            }
        }
    }
    return true;
}

std::vector<std::string> NinjaReader::readTexlistNames(u32 tl) const {
    std::vector<std::string> r;
    if (!pe_.valid(tl, 8)) return r;
    u32 tex = pe_.u32at(tl), n = pe_.u32at(tl + 4);
    if (n > 4096 || !pe_.valid(tex, n * 12)) return r;
    for (u32 i = 0; i < n; i++) {
        u32 nm = pe_.u32at(tex + i * 12);
        r.push_back(pe_.valid(nm) ? pe_.cstr(nm, 64) : std::string());
    }
    return r;
}

// ---- pose evaluation ---------------------------------------------------------

template <typename K>
static bool findKeys(const std::vector<K>& keys, float frame, u32 total, const K*& a, const K*& b, float& t) {
    if (keys.empty()) return false;
    if (keys.size() == 1) { a = b = &keys[0]; t = 0; return true; }
    // keys are sorted by frame; wrap from last to first for looping
    size_t i = 0;
    while (i + 1 < keys.size() && float(keys[i + 1].frame) <= frame) i++;
    a = &keys[i];
    if (i + 1 < keys.size()) {
        b = &keys[i + 1];
        float span = float(b->frame - a->frame);
        t = span > 0 ? (frame - float(a->frame)) / span : 0;
    } else {
        b = &keys[0];
        float span = float(total) - float(a->frame);
        t = span > 0 ? (frame - float(a->frame)) / span : 0;
    }
    t = clampf(t, 0, 1);
    return true;
}

void evalLocal(const NjSkeleton& sk, const NjMotion* mot, float frame, std::vector<NjLocal>& out) {
    out.resize(sk.nodes.size());
    for (size_t i = 0; i < sk.nodes.size(); i++) {
        const NjNode& n = sk.nodes[i];
        NjLocal& L = out[i];
        L.pos = n.pos;
        L.scl = n.scl;
        for (int k = 0; k < 3; k++) L.ang[k] = n.ang[k];
        bool hasPos = false, hasScl = false;
        if (mot && i < mot->tracks.size()) {
            const NjTrack& tr = mot->tracks[i];
            const NjKey3f *pa, *pb;
            const NjKey3i *aa, *ab;
            float t;
            if (findKeys(tr.pos, frame, mot->frames, pa, pb, t)) { L.pos = lerp(pa->v, pb->v, t); hasPos = true; }
            if (findKeys(tr.scl, frame, mot->frames, pa, pb, t)) { L.scl = lerp(pa->v, pb->v, t); hasScl = true; }
            if (findKeys(tr.ang, frame, mot->frames, aa, ab, t)) {
                for (int k = 0; k < 3; k++) {
                    int d = ab->a[k] - aa->a[k];
                    d = ((d + 0x8000) & 0xFFFF) - 0x8000;  // shortest path
                    L.ang[k] = aa->a[k] + int(float(d) * t);
                }
            }
        }
        if ((n.flags & NJD_EVAL_UNIT_POS) && !hasPos) L.pos = Vec3();
        if ((n.flags & NJD_EVAL_UNIT_SCL) && !hasScl) L.scl = Vec3(1, 1, 1);
    }
}

void blendLocal(std::vector<NjLocal>& a, const std::vector<NjLocal>& b, float t) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        a[i].pos = lerp(a[i].pos, b[i].pos, t);
        a[i].scl = lerp(a[i].scl, b[i].scl, t);
        for (int k = 0; k < 3; k++) {
            int d = b[i].ang[k] - a[i].ang[k];
            d = ((d + 0x8000) & 0xFFFF) - 0x8000;
            a[i].ang[k] += int(float(d) * t);
        }
    }
}

void composePose(const NjSkeleton& sk, const std::vector<NjLocal>& L, const Mat4& root, std::vector<Mat4>& out) {
    out.resize(sk.nodes.size());
    for (size_t i = 0; i < sk.nodes.size(); i++) {
        const NjNode& n = sk.nodes[i];
        Mat4 local = ninjaTransform(L[i].pos, L[i].ang, L[i].scl, (n.flags & NJD_EVAL_ZXY_ANG) != 0);
        out[i] = (n.parent >= 0 ? out[n.parent] : root) * local;
    }
}

void evalPose(const NjSkeleton& sk, const NjMotion* mot, float frame, const Mat4& root, std::vector<Mat4>& out) {
    std::vector<NjLocal> L;
    evalLocal(sk, mot, frame, L);
    composePose(sk, L, root, out);
}

}  // namespace sonic
