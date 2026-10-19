// Firebrand's collision world: a list of shapes (track piece meshes, planes) and a batch of segments (the car's
// wheel and body probes) tested against them once per physics step. Singleton *0x020c8e78, vtable 0x020bd390:
//   +0x20 TestShape 0x01ffb61c   +0x24 SetMasks 0x01ffb614   +0x28 AddSegment 0x01ffb478
//   +0x2c TestSegments 0x01ffb28c   +0x34 AddShape 0x01ffb144   +0x38 RemoveShape 0x01ffb034
// The hot paths run from ITCM on the DS.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "collision/dsCollisionMesh.h"
#include "nitro.h"

class dsCollisionShape;

// One hit, 0x28 bytes.
struct dsCollisionHit {
    fx32 t = -1;                    // +0x00 distance along the segment
    s32 unk04 = 0;                  // +0x04
    VecFx32 point = {};             // +0x08 world position
    VecFx32 normal = {};            // +0x14 world face normal (unit)
    u8 material = 0xff;             // +0x20 surface of the triangle (0xff: none); the car ignores 12
    dsCollisionShape *shape = nullptr; // +0x24 (reference counted in the original)
};

// One probe, 0x20 bytes: origin + dir * length, dir of unit length.
struct dsCollisionSegment {
    VecFx32 origin;                 // +0x00
    VecFx32 dir;                    // +0x0c
    fx32 length;                    // +0x18
    std::vector<dsCollisionHit> *hits; // +0x1c: where hits go (NULL: only "is there a hit")
};

class dsCollisionShape {
public:
    enum Type { MESH = 2, PLANE = 3 };
    dsCollisionShape(s32 type, u32 mask);
    virtual ~dsCollisionShape() {}
    // +0x00: nearest hit of one segment; hit may be NULL.
    virtual bool TestSegment(const dsCollisionSegment &seg, dsCollisionHit *hit) = 0;
    // +0x08: places the shape (keeps the matrix and its inverse).
    virtual void SetMatrix(const MtxFx43 &m);              // 0x0205b16c / 0x0205b03c
    // +0x1c: every segment of a batch at once (meshes only); min/max = the batch's bounding box.
    virtual bool TestSegmentBatch(std::vector<dsCollisionSegment> &segs, const VecFx32 &min, const VecFx32 &max)
    {
        return false;
    }

    s32 type;                       // +0x04
    u32 mask;                       // +0x08
    MtxFx43 matrix;                 // +0x0c
    MtxFx43 inverse;                // +0x3c
};

// Type 2 (constructor 0x02067e20, vtable 0x020be1ec): a shared dsCollisionMesh placed with a matrix.
class dsCollisionMeshShape : public dsCollisionShape {
public:
    dsCollisionMeshShape(std::shared_ptr<dsCollisionMesh> mesh, u32 mask);
    bool TestSegment(const dsCollisionSegment &seg, dsCollisionHit *hit) override;          // 0x020673c4 (partial)
    bool TestSegmentBatch(std::vector<dsCollisionSegment> &segs, const VecFx32 &min,
                          const VecFx32 &max) override;                                     // 0x01ffbb3c

    std::shared_ptr<dsCollisionMesh> mesh; // +0x70

private:
    // +0x74: per-segment scratch of the batch test, 12 entries of 0x4c bytes.
    struct Scratch {
        s32 best;                   // index in the gathered triangle list, -1 = none
        fx32 bestT;
        s32 normal[3];
        dsCollisionSegment local;   // segment in the mesh's space
        fx32 *tOut;
    };
    Scratch m_scratch[12];
};

// Type 3 (constructor 0x0205b510, vtable 0x020bd3f0): an infinite plane n.p = d (the sea level on Coast).
class dsCollisionPlaneShape : public dsCollisionShape {
public:
    dsCollisionPlaneShape(const VecFx32 &normal, fx32 d, u32 mask, u8 material);
    bool TestSegment(const dsCollisionSegment &seg, dsCollisionHit *hit) override;          // 0x0205b19c

    VecFx32 planeNormal;            // +0x6c
    fx32 planeD;                    // +0x78 (stored negated: n.p + planeD = 0)
    VecFx32 hitNormal;              // +0x7c
    u8 material;                    // +0x88
};

class dsCollisionWorld {
public:
    static dsCollisionWorld *Get();

    void SetMasks(u32 include, u32 exclude);           // 0x01ffb614
    void AddSegment(const VecFx32 &origin, const VecFx32 &dir, fx32 length,
                    std::vector<dsCollisionHit> *hits); // 0x01ffb478
    bool TestSegments();                               // 0x01ffb28c: tests and clears the batch
    void AddShape(dsCollisionShape *s);                // 0x01ffb144
    void RemoveShape(dsCollisionShape *s);             // 0x01ffb034
    void Clear();

    // Mesh resources shared by name (the resource cache 0x0205bcb0 + 0x0206822c in the original).
    std::shared_ptr<dsCollisionMesh> LoadMesh(const std::string &name);

    const std::vector<dsCollisionShape *> &Shapes() const { return m_shapes; }

private:
    std::vector<dsCollisionShape *> m_shapes;          // +0x04
    std::vector<dsCollisionSegment> m_segments;        // +0x10
    u32 m_include = 0, m_exclude = 0;                  // +0x1c +0x20
    VecFx32 m_min = {}, m_max = {};                    // +0x24 +0x30: bounding box of the segments
    std::map<std::string, std::shared_ptr<dsCollisionMesh>> m_meshes;
};

// Ray / triangle test (itcm 0x01ffc3c4): seg in the triangle's space, e1 = b - a, e2 = c - a, s = origin - a,
// n = e1 x e2 (rounded). Front faces only. Writes t to *t (when not NULL) once the hit is inside the segment's
// range, and returns true when t >= 0.
bool dsCollision_RayTriangle(const dsCollisionSegment &seg, const VecFx32 &e1, const VecFx32 &e2, const VecFx32 &s,
                             const VecFx32 &n, fx32 *t);

// Point * matrix with the translation, sums of 64-bit products shifted without rounding (itcm 0x01ffa924).
void dsVec_MulMtx43(const VecFx32 *v, const MtxFx43 *m, VecFx32 *out);
