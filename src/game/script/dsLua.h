// Firebrand's C++ layer over Lua 5.0.2 (main 0x02054bb4..0x02054e58, 0x0205c838.., 0x02068f28..0x020694e8).
// Scripts are data: each config read creates a lua_State, runs the chunk, reads the globals through
// dsLuaObject handles (registry references) and closes the state.
// Inline members: @port main:02069440 done  @port main:0205ccd8 done
#pragma once

#include <list>

#include "core/dsStringId.h"
#include "nitro.h"

extern "C" {
#include "lua.h"
}

// A Lua value pinned in the registry. 12 bytes: { type, lua_State*, ref }.
class dsLuaObject {
public:
    enum Type { NIL = 0, BOOLEAN = 1, NUMBER = 2, STRING = 3, TABLE = 4, FUNCTION = 5, OTHER = 6 };

    dsLuaObject(lua_State *L, int index);             // 0x020694b4
    dsLuaObject(const dsLuaObject &o);                // 0x02069450
    dsLuaObject &operator=(const dsLuaObject &o);     // 0x02069450 used as assignment
    ~dsLuaObject();                                   // 0x0206949c

    Type GetType() const { return m_type; }
    bool IsNil() const { return m_type == NIL; }     // 0x02069440
    const char *ToString() const;                     // 0x02069410
    int Count() const;                                // 0x02069300

    dsLuaObject Field(const char *name) const;        // 0x02069050
    bool HasField(const char *name) const;            // 0x020692d0
    int FieldNumber(const char *name) const;          // 0x020691e4
    int IndexNumber(int i) const;                     // 0x02069230: element i (0-based) of an array
    dsLuaObject Value(const dsLuaObject &key) const;  // 0x02069274

    // Table iteration: Begin returns the first key, Next the key after it; a NIL key ends the loop.
    dsLuaObject Begin() const;                        // 0x02068fe4
    dsLuaObject Next(const dsLuaObject &key) const;   // 0x02068f74

private:
    dsLuaObject(lua_State *L) : m_type(OTHER), m_L(L), m_ref(-1) {}
    void PinTop();                                    // type of the top value + luaL_ref (pops it)
    static Type TypeOfTop(lua_State *L);              // 0x02068f28

    Type m_type;      // +0
    lua_State *m_L;   // +4
    int m_ref;        // +8
};

class dsScript;

// Refcounted owner of a lua_State with the base, table and fixed-point math libraries. 0x24 bytes.
class dsLuaState {
public:
    dsLuaState();                                     // 0x02054d0c
    virtual ~dsLuaState();                            // 0x02054c94 / 0x02054cd4
    void Close();                                     // 0x02054c40
    bool Run(const dsScript &script);                 // 0x02054bec
    dsLuaObject Global(const char *name);             // 0x02054bb4
    void CollectGarbage();                            // 0x02054ba8
    lua_State *L() const { return m_L; }

    void AddRef() { m_refs++; }
    void Release()
    {
        if (--m_refs == 0)
            delete this;
    }

private:
    static int Panic(lua_State *L);                   // 0x02054ba4

    int m_refs = 0;           // +0x04
    lua_State *m_L = nullptr; // +0x08
    bool m_ownsState = true;  // +0x0c
    u32 m_unk10 = 0;          // +0x10
    bool m_unk14 = false;     // +0x14
    std::list<dsLuaState *> m_children; // +0x18: objects released with the state (empty in the ported paths)
};

// Script file contents ("scripts/<name>.lua[c]" or "scenes/<name>.dcs[c]").
class dsScript {
public:
    explicit dsScript(const char *name) : m_name(name) {}
    ~dsScript() { delete[] m_data; }
    bool Load();                                      // 0x020572b0
    const char *Name() const { return m_name.c_str(); }
    const char *Data() const { return m_data; }
    u32 Size() const { return m_size; }

private:
    dsStringIdA m_name;       // +0x04
    char *m_data = nullptr;   // +0x14
    u32 m_size = 0;           // +0x18
};

// Smart pointer returned by dsScriptManager::Run (the game uses an intrusive count at +4).
class dsLuaStateRef {
public:
    dsLuaStateRef() {}
    explicit dsLuaStateRef(dsLuaState *s) : m_p(s) { if (m_p) m_p->AddRef(); }
    dsLuaStateRef(const dsLuaStateRef &o) : m_p(o.m_p) { if (m_p) m_p->AddRef(); }
    dsLuaStateRef &operator=(const dsLuaStateRef &o)
    {
        if (o.m_p) o.m_p->AddRef();
        if (m_p) m_p->Release();
        m_p = o.m_p;
        return *this;
    }
    ~dsLuaStateRef() { if (m_p) m_p->Release(); }
    dsLuaState *operator->() const { return m_p; }
    dsLuaState *get() const { return m_p; }

private:
    dsLuaState *m_p = nullptr;
};

// Empty object (1 byte) of which several modules keep their own instance. Run loads a script through the
// script cache, executes it in a new dsLuaState and returns that state.
class dsScriptManager {
public:
    dsLuaStateRef Run(const char *name);              // 0x0205c838
};

// Firebrand's replacement of the Lua math library (fixed point), registered by dsLuaState.
extern "C" int luaopen_fxmath(lua_State *L);
