#include "dsCollisionWorld.h"

#include <algorithm>

#include "math/dsMath.h"

// Rounded fx multiply, written out like the ARM code: (a * b + 0x800) >> 12 on 64 bits.
static inline fx32 Mul(fx32 a, fx32 b)
{
    return (fx32)(((s64)a * b + 0x800) >> 12);
}

// @port itcm:01ffa924 done
void dsVec_MulMtx43(const VecFx32 *v, const MtxFx43 *m, VecFx32 *out)
{
    s64 x = v->x, y = v->y, z = v->z;
    fx32 rx = (fx32)((x * m->m[0][0] + y * m->m[1][0] + z * m->m[2][0]) >> 12) + m->m[3][0];
    fx32 ry = (fx32)((x * m->m[0][1] + y * m->m[1][1] + z * m->m[2][1]) >> 12) + m->m[3][1];
    fx32 rz = (fx32)((x * m->m[0][2] + y * m->m[1][2] + z * m->m[2][2]) >> 12) + m->m[3][2];
    out->x = rx;
    out->y = ry;
    out->z = rz;
}

// High 32 bits of a * b modulo 2^64 (the ARM umull/mla sequence keeps only the low 64 bits of the product).
static inline s32 MulHi64(s64 a, s64 b)
{
    return (s32)(u32)(((u64)a * (u64)b) >> 32);
}

static inline s64 Dot64(const VecFx32 &a, const VecFx32 &b)
{
    return (s64)a.x * b.x + (s64)a.y * b.y + (s64)a.z * b.z;
}

// @port itcm:01ffc3c4 done
bool dsCollision_RayTriangle(const dsCollisionSegment &seg, const VecFx32 &e1, const VecFx32 &e2, const VecFx32 &s,
                             const VecFx32 &n, fx32 *t)
{
    s64 det = Dot64(n, seg.dir);
    if (det >= -16) // back face, or (nearly) parallel
        return false;
    s64 neg = -det;
    s64 q = (s64)(((u64)1 << 56) / (u64)neg); // 64/64 divider, started here and read at the end
    VecFx32 Q, R;
    VEC_CrossProduct(&s, &e2, &Q);
    s64 v = -Dot64(Q, seg.dir);
    if (v < 0)
        return false;
    VEC_CrossProduct(&e1, &s, &R);
    s64 u = -Dot64(R, seg.dir);
    if (u < 0)
        return false;
    if (neg - (u + v) < 0)
        return false;
    s64 tn = Dot64(n, s);
    s64 lim = (s64)((u64)neg * (u64)(s64)seg.length) >> 12; // 64 x 32 product, low 64 bits, arithmetic shift
    if (tn < -lim || tn > lim)
        return false;
    fx32 r = MulHi64(tn, q) >> 12;
    if (t)
        *t = r;
    return r >= 0;
}

// ---- shapes ----

dsCollisionShape::dsCollisionShape(s32 type_, u32 mask_) : type(type_), mask(mask_)
{
    dsMtx43_Identity(&matrix);
    dsMtx43_Identity(&inverse);
}

// @port main:0205b16c done  @port main:0205b03c done
void dsCollisionShape::SetMatrix(const MtxFx43 &m)
{
    dsMtx43_Copy(&m, &matrix);
    dsMtx43_Copy(&m, &inverse);
    MTX_Inverse43(&inverse, &inverse);
}

// @port main:02067e20 partial (the resource lookup by name is dsCollisionWorld::LoadMesh)
dsCollisionMeshShape::dsCollisionMeshShape(std::shared_ptr<dsCollisionMesh> m, u32 mask_)
    : dsCollisionShape(MESH, mask_), mesh(std::move(m))
{
}

// Nearest hit of one segment through the grid walk (0x0206614c + the DDA 0x01ffe3ec). Not ported yet: the race
// physics only uses the batch test. Returns no hit.
// @port main:020673c4 stub
bool dsCollisionMeshShape::TestSegment(const dsCollisionSegment &seg, dsCollisionHit *hit)
{
    return false;
}

// Triangles of the cells overlapping the box min..max (mesh space), each once, in y, x, z cell order; every
// triangle when the mesh has no grid. itcm 0x01ffb834.
// @port itcm:01ffb834 done
static void GatherBox(const dsCollisionMesh &m, const VecFx32 &mn, const VecFx32 &mx, std::vector<u32> &out)
{
    if (!m.HasGrid()) {
        for (u32 i = 0; i < m.tris.size(); i++)
            out.push_back(i);
        return;
    }
    const dsCollisionGrid &g = m.grid;
    // (p - min) * cellsPerUnit (32.32): the high word, rounded with the low word's top bit, then >> 12.
    auto cellOf = [&](fx32 p, int axis) {
        s64 d = (s32)(p - g.min[axis]);
        u64 prod = (u64)d * (u64)g.cellsPerUnit[axis];
        s32 hi = (s32)(u32)(prod >> 32) + (((u32)prod & 0x80000000u) ? 1 : 0);
        return hi >> 12;
    };
    s32 lo[3], hi[3];
    const fx32 a[3] = {mn.x, mn.y, mn.z}, b[3] = {mx.x, mx.y, mx.z};
    for (int i = 0; i < 3; i++) {
        lo[i] = cellOf(a[i], i);
        hi[i] = cellOf(b[i], i);
        if (lo[i] < 0)
            lo[i] = 0;
        if (lo[i] >= (s32)g.dim[i])
            lo[i] = (s32)g.dim[i] - 1;
        if (hi[i] < 0)
            hi[i] = 0;
        if (hi[i] >= (s32)g.dim[i])
            hi[i] = (s32)g.dim[i] - 1;
    }
    for (s32 y = lo[1]; y <= hi[1]; y++)
        for (s32 x = lo[0]; x <= hi[0]; x++)
            for (s32 z = lo[2]; z <= hi[2]; z++) {
                u32 c[3] = {(u32)x, (u32)y, (u32)z};
                m.GatherCell(c, out);
            }
}

// Every segment of the batch against every gathered triangle, in the mesh's space; the nearest hit of each
// segment is appended to its hit list (point and normal back in world space).
// @port itcm:01ffbb3c done
bool dsCollisionMeshShape::TestSegmentBatch(std::vector<dsCollisionSegment> &segs, const VecFx32 &wmin,
                                            const VecFx32 &wmax)
{
    if (!mesh || segs.empty())
        return false;
    VecFx32 mn = wmin, mx = wmax;
    dsVec_MulMtx43(&mn, &inverse, &mn);
    dsVec_MulMtx43(&mx, &inverse, &mx);
    if (mx.x < mn.x)
        std::swap(mn.x, mx.x);
    if (mx.y < mn.y)
        std::swap(mn.y, mx.y);
    if (mx.z < mn.z)
        std::swap(mn.z, mx.z);
    std::vector<u32> tris;
    GatherBox(*mesh, mn, mx, tris);
    if (tris.empty())
        return false;

    size_t n = std::min(segs.size(), (size_t)12);
    fx32 t = -1; // one shared output, like local_a4 in the original
    for (size_t i = 0; i < n; i++) {
        Scratch &sc = m_scratch[i];
        sc.best = -1;
        sc.tOut = segs[i].hits ? &t : nullptr;
        sc.local = segs[i];
        dsVec_MulMtx43(&sc.local.origin, &inverse, &sc.local.origin);
        dsVec_MulMtx33(&sc.local.dir, &inverse, &sc.local.dir);
    }
    bool any = false;
    for (u32 k = 0; k < tris.size(); k++) {
        const dsCollisionTri &tr = mesh->tris[tris[k]];
        const VecFx32 &a = mesh->verts[tr.v[0]];
        const VecFx32 &b = mesh->verts[tr.v[1]];
        const VecFx32 &c = mesh->verts[tr.v[2]];
        VecFx32 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
        VecFx32 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
        VecFx32 nrm;
        VEC_CrossProduct(&e1, &e2, &nrm);
        const fx16 *fn = &mesh->normals[tris[k] * 3];
        for (size_t i = 0; i < n; i++) {
            Scratch &sc = m_scratch[i];
            VecFx32 s = {sc.local.origin.x - a.x, sc.local.origin.y - a.y, sc.local.origin.z - a.z};
            if (!dsCollision_RayTriangle(sc.local, e1, e2, s, nrm, sc.tOut))
                continue;
            if (sc.tOut && (sc.best < 0 || t < sc.bestT)) {
                sc.best = (s32)k;
                sc.bestT = t;
                sc.normal[0] = fn[0];
                sc.normal[1] = fn[1];
                sc.normal[2] = fn[2];
            }
            any = true;
        }
    }
    if (!any)
        return false;
    for (size_t i = 0; i < n; i++) {
        Scratch &sc = m_scratch[i];
        if (sc.best < 0)
            continue;
        const dsCollisionSegment &sg = segs[i];
        dsCollisionHit h;
        h.t = sc.bestT;
        h.unk04 = 0;
        h.point.x = Mul(sg.dir.x, sc.bestT) + sg.origin.x;
        h.point.y = Mul(sg.dir.y, sc.bestT) + sg.origin.y;
        h.point.z = Mul(sg.dir.z, sc.bestT) + sg.origin.z;
        VecFx32 nl = {sc.normal[0], sc.normal[1], sc.normal[2]};
        dsVec_MulMtx33(&nl, &matrix, &h.normal);
        h.shape = this;
        h.material = mesh->materials.empty() ? 0xff : mesh->materials[tris[sc.best]];
        sg.hits->push_back(h);
    }
    return true;
}

// @port main:0205b510 done
dsCollisionPlaneShape::dsCollisionPlaneShape(const VecFx32 &normal, fx32 d, u32 mask_, u8 mat)
    : dsCollisionShape(PLANE, mask_), planeNormal(normal), planeD(-d), hitNormal(normal), material(mat)
{
}

// t of origin + t * dir on the plane: FX_Div(d + n.p, -n.dir), the products rounded one by one. 0x02068d9c
// @port main:02068d9c done
static bool PlaneRay(const VecFx32 &n, fx32 d, const VecFx32 &p, const VecFx32 &dir, fx32 *t)
{
    fx32 den = Mul(n.z, -dir.z) + Mul(n.x, -dir.x) + Mul(n.y, -dir.y);
    if (den == 0)
        return false;
    *t = FX_Div(d + Mul(n.z, p.z) + Mul(n.x, p.x) + Mul(n.y, p.y), den);
    return true;
}

// @port main:0205b19c done (with 0x02068d08: the segment's end point, t in 0..1, then the distance)
bool dsCollisionPlaneShape::TestSegment(const dsCollisionSegment &seg, dsCollisionHit *hit)
{
    VecFx32 end = {seg.origin.x + Mul(seg.dir.x, seg.length), seg.origin.y + Mul(seg.dir.y, seg.length),
                   seg.origin.z + Mul(seg.dir.z, seg.length)};
    VecFx32 d = {end.x - seg.origin.x, end.y - seg.origin.y, end.z - seg.origin.z};
    fx32 f;
    if (!PlaneRay(planeNormal, planeD, seg.origin, d, &f) || f < 0 || f > FX32_ONE)
        return false;
    if (hit) {
        hit->normal = hitNormal;
        hit->unk04 = 0;
        hit->point = {seg.origin.x + Mul(f, d.x), seg.origin.y + Mul(f, d.y), seg.origin.z + Mul(f, d.z)};
        hit->material = material;
        hit->shape = this;
        fx32 t;
        if (PlaneRay(planeNormal, planeD, seg.origin, seg.dir, &t))
            hit->t = t;
    }
    return true;
}

// ---- world ----

dsCollisionWorld *dsCollisionWorld::Get()
{
    static dsCollisionWorld s_instance;
    return &s_instance;
}

// @port itcm:01ffb614 done
void dsCollisionWorld::SetMasks(u32 include, u32 exclude)
{
    m_include = include;
    m_exclude = exclude;
}

// @port itcm:01ffb478 done
void dsCollisionWorld::AddSegment(const VecFx32 &origin, const VecFx32 &dir, fx32 length,
                                  std::vector<dsCollisionHit> *hits)
{
    bool first = m_segments.empty();
    m_segments.push_back({origin, dir, length, hits});
    fx32 *mn = &m_min.x, *mx = &m_max.x;
    const fx32 o[3] = {origin.x, origin.y, origin.z};
    if (first) {
        m_min = m_max = origin;
    } else {
        for (int i = 0; i < 3; i++) {
            if (o[i] < mn[i])
                mn[i] = o[i];
            else if (mx[i] < o[i])
                mx[i] = o[i];
        }
    }
    const fx32 e[3] = {Mul(dir.x, length) + origin.x, Mul(dir.y, length) + origin.y, Mul(dir.z, length) + origin.z};
    for (int i = 0; i < 3; i++) {
        if (e[i] < mn[i])
            mn[i] = e[i];
        else if (mx[i] < e[i])
            mx[i] = e[i];
    }
}

// Shapes are tested from the last added to the first; meshes take the whole batch, other shapes one segment at a
// time. The batch is cleared afterwards.
// @port itcm:01ffb28c done
bool dsCollisionWorld::TestSegments()
{
    bool any = false;
    if (!m_shapes.empty() && !m_segments.empty()) {
        dsCollisionHit hit; // shared like the original's stack record (t is not reset between segments)
        for (size_t i = m_shapes.size(); i-- > 0;) {
            dsCollisionShape *s = m_shapes[i];
            if (!(s->mask & m_include) || (s->mask & m_exclude))
                continue;
            if (s->type == dsCollisionShape::MESH) {
                if (s->TestSegmentBatch(m_segments, m_min, m_max))
                    any = true;
            } else {
                for (dsCollisionSegment &sg : m_segments) {
                    if (s->TestSegment(sg, sg.hits ? &hit : nullptr)) {
                        if (sg.hits)
                            sg.hits->push_back(hit);
                        any = true;
                    }
                }
            }
        }
    }
    m_segments.clear();
    return any;
}

// @port itcm:01ffb144 done
void dsCollisionWorld::AddShape(dsCollisionShape *s)
{
    m_shapes.push_back(s);
}

// Swap with the last entry and shrink, as the original.
// @port itcm:01ffb034 done
void dsCollisionWorld::RemoveShape(dsCollisionShape *s)
{
    for (size_t i = 0; i < m_shapes.size(); i++) {
        if (m_shapes[i] == s) {
            m_shapes[i] = m_shapes.back();
            m_shapes.pop_back();
            return;
        }
    }
}

void dsCollisionWorld::Clear()
{
    m_shapes.clear();
    m_segments.clear();
    m_meshes.clear();
}

std::shared_ptr<dsCollisionMesh> dsCollisionWorld::LoadMesh(const std::string &name)
{
    auto it = m_meshes.find(name);
    if (it != m_meshes.end())
        return it->second;
    auto m = std::make_shared<dsCollisionMesh>();
    if (!m->Load(name.c_str()))
        m.reset();
    m_meshes[name] = m;
    return m;
}
