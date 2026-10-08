#include "character.h"
#include <cmath>

namespace sonic {

bool CharacterModel::load(const Assets& assets, const std::string& prefix) {
    unload();
    const PeImage* pe = assets.chrModels();
    if (!pe) return false;
    NinjaReader nr(*pe);
    u32 objs = pe->exportVA("___" + prefix + "_OBJECTS");
    u32 acts = pe->exportVA("___" + prefix + "_ACTIONS");
    if (!objs) return false;
    auto skelFor = [&](u32 objVA) -> int {
        for (size_t i = 0; i < skelVA_.size(); i++)
            if (skelVA_[i] == objVA) return (int)i;
        NjSkeleton sk;
        if (!nr.readSkeleton(objVA, sk)) return -1;
        skels_.push_back(std::move(sk));
        skelVA_.push_back(objVA);
        return (int)skels_.size() - 1;
    };
    // remember every OBJECTS entry (welds refer to them by index)
    {
        u32 end = objs + 4 * 256;
        for (auto& kv : pe->exports())
            if (kv.second > objs && kv.second < end) end = kv.second;
        for (u32 a = objs; a < end; a += 4) objectVAs_.push_back(pe->u32at(a));
    }
    // main model is OBJECTS[0]
    u32 main = pe->u32at(objs);
    if (skelFor(main) < 0) return false;
    if (acts) {
        // the actions array runs until the next export (with NULL holes)
        u32 end = acts + 4 * 400;
        for (auto& kv : pe->exports())
            if (kv.second > acts && kv.second < end) end = kv.second;
        for (u32 a = acts; a < end; a += 4) {
            Action act;
            u32 ap = pe->u32at(a);
            if (pe->valid(ap, 8)) {
                u32 obj = pe->u32at(ap), mot = pe->u32at(ap + 4);
                int s = skelFor(obj);
                if (s >= 0 && nr.readMotion(mot, skels_[s].nodes.size(), act.motion)) act.skel = s;
            }
            actions_.push_back(std::move(act));
        }
    }
    welds_.resize(skels_.size());
    gameWelds_.resize(skels_.size());
    if (prefix == "SONIC") setupGameWelds(kSonicWelds);
    for (size_t i = 0; i < skels_.size(); i++)
        if (gameWelds_[i].empty()) setupWelds((int)i);
    // measure bind pose height
    std::vector<Mat4> w;
    evalPose(skels_[0], nullptr, 0, Mat4(), w);
    Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (size_t n = 0; n < skels_[0].nodes.size(); n++) {
        int mi = skels_[0].nodes[n].model;
        if (mi < 0) continue;
        for (auto& v : skels_[0].models[mi]->verts) {
            Vec3 p = w[n].transformPoint(v.pos);
            mn = vmin(mn, p);
            mx = vmax(mx, p);
        }
    }
    if (mx.y > mn.y) {
        height_ = mx.y - mn.y;
        footOffset_ = mn.y;
    }
    SONIC_LOGI("Character %s: %zu skeletons, %zu actions, height %.1f (feet at %.1f)", prefix.c_str(), skels_.size(), actions_.size(), height_, footOffset_);
    return true;
}

void CharacterModel::setupGameWelds(const std::vector<WeldDef>& defs) {
    auto objVA = [&](int i) -> u32 { return i >= 0 && i < (int)objectVAs_.size() ? objectVAs_[i] : 0; };
    for (size_t si = 0; si < skels_.size(); si++) {
        const NjSkeleton& sk = skels_[si];
        auto nodeOf = [&](u32 va) -> int {
            if (!va) return -1;
            for (size_t n = 0; n < sk.nodes.size(); n++)
                if (sk.nodes[n].va == va) return (int)n;
            return -1;
        };
        for (auto& d : defs) {
            if (objVA(d.base) != skelVA_[si]) continue;
            GameWeld w;
            w.nodeA = nodeOf(objVA(d.a));
            w.nodeB = nodeOf(objVA(d.b));
            if (w.nodeA < 0 || w.nodeB < 0) continue;
            int ma = sk.nodes[w.nodeA].model, mb = sk.nodes[w.nodeB].model;
            if (ma < 0 || mb < 0) continue;
            int na = (int)sk.models[ma]->points.size(), nb = (int)sk.models[mb]->points.size();
            for (size_t k = 0; k + 1 < d.pairs.size(); k += 2)
                if (d.pairs[k] < na && d.pairs[k + 1] < nb) w.pairs.push_back({d.pairs[k], d.pairs[k + 1]});
            if (!w.pairs.empty()) gameWelds_[si].push_back(std::move(w));
        }
        if (!gameWelds_[si].empty()) SONIC_LOGI("Skeleton %zu: %zu welds from the game's weld table", si, gameWelds_[si].size());
    }
}

void CharacterModel::setupWelds(int si) {
    NjSkeleton& sk = skels_[si];
    std::vector<Mat4> bind;
    evalPose(sk, nullptr, 0, Mat4(), bind);
    auto& out = welds_[si];
    int n = (int)sk.nodes.size();
    auto hasChildren = [&](int k) {
        for (int c = 0; c < n; c++)
            if (sk.nodes[c].parent == k) return true;
        return false;
    };
    for (int w = 0; w < n; w++) {
        int mi = sk.nodes[w].model;
        int par = sk.nodes[w].parent;
        if (mi < 0 || par < 0) continue;
        NjModel& m = *sk.models[mi];
        if (m.verts.empty() || m.bmax.x - m.bmin.x > 3.0f) continue;
        // count distinct points; welds are tiny tubes (<= 12 points)
        if (m.verts.size() > 64) continue;
        // the far joint: a sibling (child of the same parent) that is a model-less joint with children
        int other = -1;
        for (int c = 0; c < n; c++)
            if (c != w && sk.nodes[c].parent == par && sk.nodes[c].model < 0 && hasChildren(c)) {
                other = c;
                break;
            }
        if (other < 0) continue;
        Vec3 jA = bind[par].translation(), jB = bind[other].translation();
        if (length(jB - jA) < 0.05f) continue;
        Weld wd;
        wd.node = w;
        wd.other = other;
        Mat4 invB = bind[other].affineInverse();
        int nFollow = 0;
        for (auto& v : m.verts) {
            Vec3 wp = bind[w].transformPoint(v.pos);
            bool f = length2(wp - jB) < length2(wp - jA);
            wd.follow.push_back(f);
            nFollow += f;
            wd.localPos.push_back(invB.transformPoint(wp));
            wd.localNrm.push_back(invB.transformDir(bind[w].transformDir(v.nrm)));
            wd.origPos.push_back(v.pos);
            wd.origNrm.push_back(v.nrm);
        }
        if (nFollow == 0 || nFollow == (int)m.verts.size()) continue;
        out.push_back(std::move(wd));
    }
    SONIC_LOGI("Skeleton %d: %zu weld meshes", si, out.size());
}

void CharacterModel::unload() {
    skels_.clear();
    skelVA_.clear();
    actions_.clear();
    objectVAs_.clear();
    welds_.clear();
    gameWelds_.clear();
    locals_.clear();
    prevLocals_.clear();
    animId_ = -1;
}

std::vector<NjModel*> CharacterModel::allModels() {
    std::vector<NjModel*> r;
    for (auto& s : skels_)
        for (auto& m : s.models)
            if (m) r.push_back(m.get());
    return r;
}

void CharacterModel::draw(DrawList& r, int action, float frame, const Mat4& root) {
    if (skels_.empty()) return;
    int si = 0;
    const NjMotion* mot = nullptr;
    if (hasAction(action)) {
        si = actions_[action].skel;
        mot = &actions_[action].motion;
    }
    evalLocal(skels_[si], mot, frame, locals_);
    drawPose(r, si, locals_, root);
}

void CharacterModel::drawAction(DrawList& r, int action, float frame, const Mat4& root) {
    if (!hasAction(action)) return;
    const Action& a = actions_[action];
    float n = float(std::max<u32>(a.motion.frames, 1));
    frame = std::fmod(std::max(frame, 0.0f), n);
    evalLocal(skels_[a.skel], &a.motion, frame, actionScratch_);
    boost_ = 1.0f;
    drawPose(r, a.skel, actionScratch_, root);
    boost_ = 1.55f;
}

bool CharacterModel::actionLocals(int action, float frame, std::vector<NjLocal>& out) const {
    if (!hasAction(action)) return false;
    const Action& a = actions_[action];
    float n = float(std::max<u32>(a.motion.frames, 1));
    frame = std::fmod(std::max(frame, 0.0f), n);
    evalLocal(skels_[a.skel], &a.motion, frame, out);
    return true;
}

void CharacterModel::drawLocals(DrawList& r, int skel, const std::vector<NjLocal>& locals, const Mat4& root) {
    if (skel < 0 || skel >= (int)skels_.size() || locals.size() != skels_[skel].nodes.size()) return;
    drawPose(r, skel, locals, root);
}

void CharacterModel::playAnim(int id, bool restart) {
    const AnimDef* d = animDef(id);
    if (!d || !hasAction(d->action)) return;
    if (id == animId_ && !restart) return;
    // remember the current pose so we can blend out of it
    if (animId_ >= 0 && !locals_.empty()) {
        prevLocals_ = locals_;
        prevSkel_ = hasAction(animDef(animId_)->action) ? actions_[animDef(animId_)->action].skel : 0;
        blend_ = 0.0f;
        blendRate_ = d->transition > 0 ? d->transition : 1.0f;
    } else {
        blend_ = 1.0f;
    }
    animId_ = id;
    animFrame_ = 0;
    animDone_ = false;
}

void CharacterModel::tickAnim(float speed) {
    const AnimDef* d = animDef(animId_);
    if (!d) return;
    if (blend_ < 1.0f) blend_ = std::min(1.0f, blend_ + blendRate_);
    float frames = actionFrames(d->action);
    float inc = d->speed;
    if (d->property == 9 || d->property == 10) inc = d->speed * std::max(speed, 0.6f);
    animFrame_ += inc;
    if (animFrame_ >= frames) {
        bool once = d->property == 4 || d->property == 6;
        if (once && d->next != animId_) {
            animDone_ = true;
            playAnim(d->next);
            return;
        }
        if (d->property == 5 || d->property == 12) {
            animFrame_ = frames - 0.001f;  // hold last frame
            animDone_ = true;
        } else {
            animFrame_ = std::fmod(animFrame_, frames);
        }
    }
}

void CharacterModel::drawAnimated(DrawList& r, const Mat4& root) {
    const AnimDef* d = animDef(animId_);
    if (!d || !hasAction(d->action)) {
        draw(r, -1, 0, root);
        return;
    }
    const Action& act = actions_[d->action];
    evalLocal(skels_[act.skel], &act.motion, animFrame_, locals_);
    if (blend_ < 1.0f && prevSkel_ >= 0 && skels_[prevSkel_].nodes.size() == skels_[act.skel].nodes.size()) {
        blendScratch_ = prevLocals_;
        // smoothstep for a softer transition
        float t = blend_ * blend_ * (3 - 2 * blend_);
        blendLocal(blendScratch_, locals_, t);
        drawPose(r, act.skel, blendScratch_, root);
        return;
    }
    drawPose(r, act.skel, locals_, root);
}

void CharacterModel::drawPose(DrawList& r, int si, const std::vector<NjLocal>& locals, const Mat4& root) {
    const NjSkeleton* sk = &skels_[si];
    composePose(*sk, locals, root, pose_);
    if (!gameWelds_[si].empty()) {
        // exact SADX welding: move A's points onto B's points, in table order
        workPts_.resize(sk->models.size());
        workNrm_.resize(sk->models.size());
        std::vector<bool> touched(sk->models.size(), false);
        for (auto& w : gameWelds_[si]) {
            int ma = sk->nodes[w.nodeA].model, mb = sk->nodes[w.nodeB].model;
            for (int m : {ma, mb})
                if (!touched[m]) {
                    workPts_[m] = sk->models[m]->points;
                    workNrm_[m] = sk->models[m]->normals;
                    touched[m] = true;
                }
        }
        for (auto& w : gameWelds_[si]) {
            int ma = sk->nodes[w.nodeA].model, mb = sk->nodes[w.nodeB].model;
            Mat4 rel = pose_[w.nodeA].affineInverse() * pose_[w.nodeB];
            for (auto& pr : w.pairs) {
                workPts_[ma][pr.first] = rel.transformPoint(workPts_[mb][pr.second]);
                workNrm_[ma][pr.first] = normalize(rel.transformDir(workNrm_[mb][pr.second]));
            }
        }
        for (size_t m = 0; m < touched.size(); m++) {
            if (!touched[m]) continue;
            NjModel& md = *sk->models[m];
            for (size_t v = 0; v < md.verts.size(); v++) {
                int pi = md.pointOf[v];
                if (pi < 0 || pi >= (int)workPts_[m].size()) continue;
                md.verts[v].pos = workPts_[m][pi];
                md.verts[v].nrm = workNrm_[m][pi];
            }
            md.version++;
        }
    }
    for (auto& wd : welds_[si]) {
        NjModel& m = *sk->models[sk->nodes[wd.node].model];
        Mat4 inv = pose_[wd.node].affineInverse();
        Mat4 rel = inv * pose_[wd.other];
        for (size_t i = 0; i < m.verts.size(); i++) {
            if (wd.follow[i]) {
                m.verts[i].pos = rel.transformPoint(wd.localPos[i]);
                m.verts[i].nrm = normalize(rel.transformDir(wd.localNrm[i]));
            }
        }
        m.version++;
    }
    for (size_t n = 0; n < sk->nodes.size(); n++) {
        int mi = sk->nodes[n].model;
        if (mi < 0 || (sk->nodes[n].flags & NJD_EVAL_HIDE)) continue;
        DrawItem it;
        it.model = sk->models[mi].get();
        it.world = pose_[n];
        it.textureSet = TEXSET_SONIC;
        it.lightBoost = boost_;
        r.items.push_back(it);
    }
}

}  // namespace sonic
