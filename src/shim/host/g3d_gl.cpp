// G3D stand-in back-end on OpenGL 1.x (host), see g3d_model.h.
#include "g3d_gl.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

G3Model::~G3Model()
{
    for (G3Texture &t : textures)
        if (t.gl)
            glDeleteTextures(1, &t.gl);
}

void G3Model::Upload()
{
    for (G3Texture &t : textures) {
        if (t.gl || t.rgba.empty())
            continue;
        glGenTextures(1, &t.gl);
        glBindTexture(GL_TEXTURE_2D, t.gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        !t.repeatS ? GL_CLAMP_TO_EDGE : t.flipS ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        !t.repeatT ? GL_CLAMP_TO_EDGE : t.flipT ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
    }
}

static float curAlpha = 1;
#ifndef G3_FRONT_FACE
#define G3_FRONT_FACE GL_CCW
#endif

// Display-list primitives replayed as GL immediate-mode primitives.
struct GlSink : G3ShapeSink {
    float tw, th;
    // Lighting (G3SetDrawLight): a NORMAL command computes the vertex colour, as on the DS.
    const G3DrawLight *light = nullptr;
    const G3Lit *lit = nullptr;
    const float *node = nullptr;
    explicit GlSink(const G3Texture *t)
        : tw(t && t->w ? 1.0f / (16.0f * t->w) : 0), th(t && t->h ? 1.0f / (16.0f * t->h) : 0) {}
    void Begin(int mode) override
    {
        static const GLenum modes[4] = {GL_TRIANGLES, GL_QUADS, GL_TRIANGLE_STRIP, GL_QUAD_STRIP};
        glBegin(modes[mode & 3]);
    }
    void End() override { glEnd(); }
    void Color(u32 c) override
    {
        glColor4f((c & 31) / 31.0f, ((c >> 5) & 31) / 31.0f, ((c >> 10) & 31) / 31.0f, curAlpha);
    }
    void Normal(float x, float y, float z) override
    {
        if (!light || !lit->lit)
            return;
        float n[3] = {x, y, z}, w[3];
        G3WorldNormal(*light, node, n, w);
        Color(G3LightVertex(*lit, *light->env, w, light->eye, light->specular));
    }
    void TexCoord(s16 s, s16 t) override { glTexCoord2f(s * tw, t * th); }
    void Vertex(const float *p) override { glVertex3f(p[0], p[1], p[2]); }
};

void G3Model::Prepare(const G3Model *) const {}

void G3Model::Draw(const G3Model *batch) const
{
    DrawOps(ops, batch);
}

void G3Model::DrawPart(int material, int shape, const G3Model *batch) const
{
    DrawOps({{1, material}, {2, shape}}, batch);
}

void G3Model::DrawOps(const std::vector<Op> &ops, const G3Model *batch) const
{
    glFrontFace(G3_FRONT_FACE);
    glPushMatrix();
    float scale = 1;
    const G3Texture *tex = nullptr;
    u32 dif = 0x7fff;
    float alpha = 1;
    const G3DrawLight *light = G3GetDrawLight();
    G3Lit lit;
    const float *node = nullptr;
    for (const Op &o : ops) {
        switch (o.kind) {
        case 0:
            glPopMatrix();
            glPushMatrix();
            node = o.a < (int)nodes.size() ? nodes[o.a].m : nullptr;
            if (o.a < (int)nodes.size())
                glMultMatrixf(nodes[o.a].m);
            if (scale != 1)
                glScalef(scale, scale, scale);
            break;
        case 1:
            tex = nullptr;
            dif = 0x7fff;
            {
                const G3Model *src = batchMaterials ? batch : this;
                if (src && o.a < (int)src->materials.size()) {
                    const Material &m = src->materials[o.a];
                    dif = m.difAmb & 0x7fff;
                    if (m.texture >= 0)
                        tex = &src->textures[m.texture];
                    // Polygon alpha 0 is wireframe on the DS; 1..30 translucent.
                    // POLYGON_ATTR bit 6 = draw back faces, bit 7 = draw front faces.
                    bool back = (m.polyAttr >> 6) & 1, front = (m.polyAttr >> 7) & 1;
                    if (back && front) {
                        glDisable(GL_CULL_FACE);
                    } else {
                        glEnable(GL_CULL_FACE);
                        glCullFace(front ? GL_BACK : back ? GL_FRONT : GL_FRONT_AND_BACK);
                    }
                    u32 a = (m.polyAttr >> 16) & 31;
                    if (light) {
                        lit = G3ResolveMaterial(m, *light->env, light->set);
                        dif = lit.diffuse;
                        if (lit.alpha >= 0)
                            a = (u32)lit.alpha;
                        if (light->env->fog && lit.fog)
                            glEnable(GL_FOG);
                        else
                            glDisable(GL_FOG);
                    }
                    alpha = a == 0 ? 1.0f : a / 31.0f;
                }
            }
            if (tex && tex->gl) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, tex->gl);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
            glColor4f((dif & 31) / 31.0f, ((dif >> 5) & 31) / 31.0f, ((dif >> 10) & 31) / 31.0f, alpha);
            curAlpha = alpha;
            break;
        case 2:
            if (o.a < (int)shapes.size()) {
                GlSink sink(tex);
                if (light) {
                    sink.light = light;
                    sink.lit = &lit;
                    sink.node = node;
                }
                G3DecodeShape(shapes[o.a], sink);
            }
            break;
        case 3:
            scale = upScale;
            glScalef(upScale, upScale, upScale);
            break;
        case 4:
            if (scale != 1)
                glScalef(1 / scale, 1 / scale, 1 / scale);
            scale = 1;
            break;
        }
    }
    glPopMatrix();
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
}
