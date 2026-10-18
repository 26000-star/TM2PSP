// Interned, reference-counted strings: an object holds a slot index (-1 = empty) into a global table of
// { char data, refcount } entries; equal strings share a slot.
// The game has two independent tables with the same code:
//   pool A: ctor 0x02044258, release 0x020442cc, c_str 0x0204422c (script names)
//   pool B: ctor 0x020444b4, assign 0x02044304, release 0x02044528 (car names, ...)
// The original keeps the characters in one block and finds slots by hash (0x020451b8 / 0x02044e64);
// the port keeps the behaviour (interning, counting, slot reuse) with standard containers.
// @port main:02044258 partial
// @port main:020442cc partial
// @port main:0204422c partial
// @port main:020444b4 partial
// @port main:02044304 partial
// @port main:02044528 partial
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

template <int Pool>
class dsStringId {
public:
    dsStringId() {}
    explicit dsStringId(const char *s) { Set(s); }
    dsStringId(const dsStringId &o) : m_slot(o.m_slot) { AddRef(); }
    dsStringId &operator=(const dsStringId &o)
    {
        if (o.m_slot != m_slot) {
            Release();
            m_slot = o.m_slot;
            AddRef();
        }
        return *this;
    }
    dsStringId &operator=(const char *s)
    {
        Set(s);
        return *this;
    }
    ~dsStringId() { Release(); }

    const char *c_str() const { return m_slot < 0 ? nullptr : Table().slots[m_slot].text.c_str(); }
    bool IsEmpty() const { return m_slot < 0 || Table().slots[m_slot].text.empty(); }
    int Slot() const { return m_slot; }
    void Release()
    {
        if (m_slot < 0)
            return;
        Slot_ &e = Table().slots[m_slot];
        if (--e.refs == 0) {
            Table().index.erase(e.text);
            e.text.clear();
            Table().freeSlots.push_back(m_slot);
        }
        m_slot = -1;
    }

private:
    struct Slot_ {
        std::string text;
        int refs = 0;
    };
    struct Table_ {
        std::vector<Slot_> slots;
        std::unordered_map<std::string, int> index;
        std::vector<int> freeSlots;
    };
    static Table_ &Table()
    {
        static Table_ t;
        return t;
    }
    void AddRef()
    {
        if (m_slot >= 0)
            Table().slots[m_slot].refs++;
    }
    void Set(const char *s)
    {
        if (!s) {
            Release();
            return;
        }
        Table_ &t = Table();
        auto it = t.index.find(s);
        int slot;
        if (it != t.index.end()) {
            slot = it->second;
        } else {
            if (!t.freeSlots.empty()) {
                slot = t.freeSlots.back();
                t.freeSlots.pop_back();
            } else {
                slot = (int)t.slots.size();
                t.slots.emplace_back();
            }
            t.slots[slot].text = s;
            t.index[s] = slot;
        }
        if (slot != m_slot) {
            t.slots[slot].refs++;
            Release();
            m_slot = slot;
        }
    }

    int m_slot = -1;
};

typedef dsStringId<0> dsStringIdA;
typedef dsStringId<1> dsStringIdB;
