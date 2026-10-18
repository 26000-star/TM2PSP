#include "dsLua.h"

#include <stdio.h>
#include <string.h>

#include <unordered_map>

#include "fs/dsFileSystem.h"

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

// ---- dsLuaObject ----

// @port main:02068f28 done
dsLuaObject::Type dsLuaObject::TypeOfTop(lua_State *L)
{
    switch (lua_type(L, -1)) {
    case LUA_TNIL: return NIL;
    case LUA_TBOOLEAN: return BOOLEAN;
    case LUA_TNUMBER: return NUMBER;
    case LUA_TSTRING: return STRING;
    case LUA_TTABLE: return TABLE;
    case LUA_TFUNCTION: return FUNCTION;
    default: return OTHER;
    }
}

void dsLuaObject::PinTop()
{
    m_type = TypeOfTop(m_L);
    m_ref = luaL_ref(m_L, LUA_REGISTRYINDEX);
}

// @port main:020694b4 done
dsLuaObject::dsLuaObject(lua_State *L, int index) : m_type(OTHER), m_L(L), m_ref(-1)
{
    if (index >= 0)
        index++;
    lua_pushvalue(m_L, index);
    PinTop();
}

// @port main:02069450 done
dsLuaObject::dsLuaObject(const dsLuaObject &o) : m_type(o.m_type), m_L(o.m_L)
{
    lua_rawgeti(o.m_L, LUA_REGISTRYINDEX, o.m_ref);
    lua_pushvalue(m_L, -1);
    PinTop();
    lua_settop(o.m_L, -2);
}

dsLuaObject &dsLuaObject::operator=(const dsLuaObject &o)
{
    // The game assigns with the copy constructor's code, which does not unref the previous value: loops leak one
    // registry slot per step. Harmless because the state is closed after loading; kept for fidelity.
    m_L = o.m_L;
    m_type = o.m_type;
    lua_rawgeti(o.m_L, LUA_REGISTRYINDEX, o.m_ref);
    lua_pushvalue(m_L, -1);
    PinTop();
    lua_settop(o.m_L, -2);
    return *this;
}

// @port main:0206949c done
dsLuaObject::~dsLuaObject()
{
    luaL_unref(m_L, LUA_REGISTRYINDEX, m_ref);
}

// @port main:02069410 done
const char *dsLuaObject::ToString() const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    const char *s = lua_tostring(m_L, -1);
    lua_settop(m_L, -2);
    return s;
}

// @port main:02069300 done
int dsLuaObject::Count() const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    int n = luaL_getn(m_L, -1);
    if (n == 0) {
        lua_pushnil(m_L);
        while (lua_next(m_L, -2) != 0) {
            n++;
            lua_settop(m_L, -2);
        }
    }
    lua_settop(m_L, -2);
    return n;
}

// @port main:02069050 done
dsLuaObject dsLuaObject::Field(const char *name) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_pushstring(m_L, name);
    lua_gettable(m_L, -2);
    dsLuaObject r(m_L);
    lua_pushvalue(m_L, -1);
    r.PinTop();
    lua_settop(m_L, -3);
    return r;
}

// @port main:020692d0 done
bool dsLuaObject::HasField(const char *name) const
{
    return Field(name).m_type != NIL;
}

// @port main:020691e4 done
int dsLuaObject::FieldNumber(const char *name) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_pushstring(m_L, name);
    lua_gettable(m_L, -2);
    int v = (int)lua_tonumber(m_L, -1);
    lua_settop(m_L, -3);
    return v;
}

// @port main:02069230 done
int dsLuaObject::IndexNumber(int i) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_rawgeti(m_L, -1, i + 1);
    int v = (int)lua_tonumber(m_L, -1);
    lua_settop(m_L, -3);
    return v;
}

// The string stays valid after the pop because the table still references it.
// @port main:02069154 done
const char *dsLuaObject::FieldString(const char *name) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_pushstring(m_L, name);
    lua_gettable(m_L, -2);
    const char *v = lua_tostring(m_L, -1);
    lua_settop(m_L, -3);
    return v;
}

// @port main:020691a0 done
const char *dsLuaObject::IndexString(int i) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_rawgeti(m_L, -1, i + 1);
    const char *v = lua_tostring(m_L, -1);
    lua_settop(m_L, -3);
    return v;
}

// @port main:02069274 done
dsLuaObject dsLuaObject::Value(const dsLuaObject &key) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_rawgeti(key.m_L, LUA_REGISTRYINDEX, key.m_ref);
    lua_gettable(m_L, -2);
    dsLuaObject r(m_L);
    lua_pushvalue(m_L, -1);
    r.PinTop();
    lua_settop(m_L, -3);
    return r;
}

// @port main:02068fe4 done (port fix: pushes nil at the end instead of copying the slot below the table)
dsLuaObject dsLuaObject::Begin() const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_pushnil(m_L);
    if (lua_next(m_L, -2) == 0)
        lua_pushnil(m_L); // the original reads stack[-2] here, i.e. below the table
    else
        lua_pushvalue(m_L, -2);
    dsLuaObject r(m_L);
    r.PinTop();
    lua_settop(m_L, r.m_type == NIL ? -2 : -4);
    return r;
}

// @port main:02068f74 done (same port fix as Begin)
dsLuaObject dsLuaObject::Next(const dsLuaObject &key) const
{
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_ref);
    lua_rawgeti(key.m_L, LUA_REGISTRYINDEX, key.m_ref);
    if (lua_next(m_L, -2) == 0)
        lua_pushnil(m_L);
    else
        lua_pushvalue(m_L, -2);
    dsLuaObject r(m_L);
    r.PinTop();
    lua_settop(m_L, r.m_type == NIL ? -2 : -4);
    return r;
}

// ---- dsLuaState ----

// @port main:02054ba4 done
int dsLuaState::Panic(lua_State *)
{
    return 0;
}

// @port main:02054d0c done
dsLuaState::dsLuaState()
{
    m_L = lua_open();
    luaopen_base(m_L);
    lua_settop(m_L, 0);
    luaopen_table(m_L);
    lua_settop(m_L, 0);
    luaopen_fxmath(m_L);
    lua_settop(m_L, 0);
    lua_atpanic(m_L, Panic);
}

// @port main:02054cd4 done
dsLuaState::~dsLuaState()
{
    Close();
}

// @port main:02054c40 done
void dsLuaState::Close()
{
    if (m_L && m_ownsState)
        lua_close(m_L);
    m_L = nullptr;
    for (dsLuaState *c : m_children)
        if (c)
            delete c;
    m_children.clear();
}

// @port main:02054bec done
bool dsLuaState::Run(const dsScript &script)
{
    int err = luaL_loadbuffer(m_L, script.Data(), script.Size(), script.Name());
    if (err == 0)
        err = lua_pcall(m_L, 0, 0, 0);
    if (err == 0)
        return true;
    // The message is fetched and dropped (the release build prints nothing); the port reports it.
    OS_Printf("lua: %s\n", lua_tostring(m_L, -1));
    lua_settop(m_L, -2);
    return false;
}

// @port main:02054bb4 done
dsLuaObject dsLuaState::Global(const char *name)
{
    lua_pushstring(m_L, name);
    lua_gettable(m_L, LUA_GLOBALSINDEX);
    dsLuaObject r(m_L, -1);
    lua_settop(m_L, -2);
    return r;
}

// @port main:02054ba8 done
void dsLuaState::CollectGarbage()
{
    lua_setgcthreshold(m_L, 0);
}

// ---- dsScript / dsScriptManager ----

// @port main:020572b0 done
bool dsScript::Load()
{
    char path[0x100 + 4];
    dsFileSystem *fs = dsFileSystem::Get();
    int n = snprintf(path, 0x100, "scripts/%s.lua", m_name.c_str());
    if (!fs->Exists(path)) {
        path[n] = 'c';
        path[n + 1] = '\0';
        if (!fs->Exists(path)) {
            n = snprintf(path, 0x100, "scenes/%s.dcs", m_name.c_str());
            if (!fs->Exists(path)) {
                path[n] = 'c';
                path[n + 1] = '\0';
                if (!fs->Exists(path))
                    return false;
            }
        }
    }
    dsFile *f = fs->Open(path);
    if (f) {
        if (f->IsOpen()) {
            m_size = (u32)f->GetSize();
            m_data = new char[m_size + 1];
            int size = (int)m_size;
            if (f->Read(m_data, m_size) == size) {
                // Source text (not "\x1bLua" bytecode) gets a terminating NUL counted in the size.
                if (memcmp(m_data, "\x1bLua", 4) != 0) {
                    m_data[size] = '\0';
                    m_size++;
                }
                delete f;
                return true;
            }
        }
        delete f;
    }
    m_size = 0;
    return false;
}

namespace {
// Script cache of 0x0205ce70: one entry per interned name slot { use count, name, script }. Run() drops the script
// data again once the chunk has executed, so in practice nothing stays cached between reads.
struct ScriptCacheEntry {
    int uses = 0;
    dsScript *script = nullptr;
};
std::unordered_map<int, ScriptCacheEntry> g_scriptCache;
}

// @port main:0205c838 partial (cache simplified to a map keyed by the name's slot; 0x0205c9dc, 0x0205c920 folded in)
dsLuaStateRef dsScriptManager::Run(const char *name)
{
    dsStringIdA id(name);
    ScriptCacheEntry &e = g_scriptCache[id.Slot()];
    if (!e.script) {
        e.script = new dsScript(name);
        e.script->Load();
    }
    e.uses++;
    dsLuaStateRef state(new dsLuaState());
    state->Run(*e.script);
    if (--e.uses == 0 && !id.IsEmpty()) {
        delete e.script;
        e.script = nullptr;
    }
    state->CollectGarbage();
    return state;
}
