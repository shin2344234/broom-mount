#include "game/gamefile.h"

#include <Windows.h>
#include <intrin.h>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

using namespace bm::sig;

namespace
{
    // What Read fills: the file's bytes, the space allocated for them (two
    // more than the file, zeroed), the file's size, and a memory tag (-1 for
    // none). The table loader allocates one with data 0 and tag -1 and
    // passes it in.
    struct alignas(16) Buffer
    {
        uint8_t* data;
        uint32_t capacity;
        uint32_t size;
        int8_t   tag;
    };

    // Read(loader, path, buffer, name, three u32 on the stack).
    typedef uint64_t (*FnRead)(uintptr_t loader, uintptr_t path, Buffer* buffer, uintptr_t name, uintptr_t a5,
                               uintptr_t a6, uintptr_t a7);
    typedef uintptr_t (*FnMakePath)(uintptr_t* out, const char* text, uintptr_t flag);
    typedef void (*FnReleasePath)(uintptr_t* path);
    typedef void* (*FnAlloc)(size_t size, size_t align);
    typedef void (*FnFree)(void* p);
    typedef void (*FnBufferRelease)(Buffer* buffer);
    typedef uintptr_t (*FnAppearanceLoad)(uintptr_t task);
    // The async load takes nine arguments; twelve pass through untouched.
    typedef uintptr_t (*FnAsync)(uintptr_t loader, uintptr_t handle, uintptr_t status, uintptr_t path, uintptr_t a5,
                                 uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11,
                                 uintptr_t a12);

    // Find (slot 3) picks the pack source for a path; exists (slot 14) asks
    // whether one has it, and index-of (slot 13) which one does.
    typedef uintptr_t (*FnFind)(uintptr_t loader, uintptr_t out, uintptr_t path, uintptr_t flags);
    typedef uint64_t (*FnExists)(uintptr_t loader, uintptr_t path);

    FnRead           g_readOriginal = nullptr;
    FnAsync          g_asyncOriginal = nullptr;
    FnFind           g_findOriginal = nullptr;
    FnExists         g_existsOriginal = nullptr;
    FnExists         g_indexOfOriginal = nullptr;
    FnMakePath       g_makePath = nullptr;
    FnReleasePath    g_releasePath = nullptr;
    FnAlloc          g_allocSet = nullptr, g_allocClear = nullptr;
    FnFree           g_freeSet = nullptr, g_freeClear = nullptr;
    FnBufferRelease  g_bufferRelease = nullptr;
    FnAppearanceLoad g_appearanceOriginal = nullptr;
    // Bytes held per memory tag; the release takes a buffer's capacity off
    // its tag's count, so a swap moves the count by the difference.
    volatile LONG*   g_tagBytes = nullptr;

    bm::gamefile::Wants       g_wants = nullptr;
    bm::gamefile::Serve       g_serve = nullptr;
    bm::gamefile::Appearance  g_appearance = nullptr;
    bm::gamefile::Redirect    g_redirect = nullptr;
    bm::gamefile::ReadInstead g_readInstead = nullptr;
    bm::gamefile::Patch       g_patch = nullptr;
    bm::gamefile::Seen volatile g_seen = nullptr;
    bm::gamefile::Supply volatile g_supply = nullptr;

    // The thread block's job pointer (+0x1E0): set on the loader's own
    // threads, where the async slot queues a task instead of calling Read.
    uintptr_t* JobSlot()
    {
        const uintptr_t slots = __readgsqword(0x58);
        const uintptr_t block = slots ? *reinterpret_cast<uintptr_t*>(slots) : 0;
        return block ? reinterpret_cast<uintptr_t*>(block + 0x1E0) : nullptr;
    }

    // The loader of the read Serve is looking at, for ReadFile.
    thread_local uintptr_t t_loader = 0;

    // The game picks one of two allocators by a byte in its thread block
    // (+0x1FE; +0x1FD on 2976), and frees with the matching one.
    bool AllocatorFlag()
    {
        const uintptr_t slots = __readgsqword(0x58);
        const uintptr_t block = slots ? *reinterpret_cast<uintptr_t*>(slots) : 0;
        return block && *reinterpret_cast<uint8_t*>(block + 0x1FE) != 0;
    }

    // The game's buffer, now holding `bytes`, in the game's own allocation.
    bool Replace(Buffer* b, const std::string& bytes, const char* path)
    {
        const bool flag = AllocatorFlag();
        FnAlloc alloc = flag ? g_allocSet : g_allocClear;
        FnFree release = flag ? g_freeSet : g_freeClear;
        const size_t space = bytes.size() + 2;
        if (space > 0x7FFFFFF0u) return false;
        void* fresh = alloc(space, 16);
        if (!fresh)
        {
            LOG_ERR("[files] the game's allocator refused %zu bytes for %s, so the game keeps its own.", space, path);
            return false;
        }
        memcpy(fresh, bytes.data(), bytes.size());
        memset(static_cast<uint8_t*>(fresh) + bytes.size(), 0, 2);
        uint8_t* old = b->data;
        const uint32_t oldCapacity = old ? b->capacity : 0;
        b->data = static_cast<uint8_t*>(fresh);
        b->size = static_cast<uint32_t>(bytes.size());
        b->capacity = static_cast<uint32_t>(space);
        if (b->tag != -1 && g_tagBytes)
            InterlockedExchangeAdd(g_tagBytes + static_cast<uint8_t>(b->tag),
                                   static_cast<LONG>(static_cast<int64_t>(space) - static_cast<int64_t>(oldCapacity)));
        if (old) release(old);
        return true;
    }

    // Read, for another path the game's own way.
    uint64_t ReadAs(uintptr_t loader, const std::string& other, Buffer* buffer, uintptr_t name, uintptr_t a5,
                    uintptr_t a6, uintptr_t a7)
    {
        uintptr_t obj = 0;
        g_makePath(&obj, other.c_str(), 0);
        const uint64_t r = g_readOriginal(loader, reinterpret_cast<uintptr_t>(&obj), buffer, name, a5, a6, a7);
        g_releasePath(&obj);
        return r;
    }

    uint64_t ReadDetour(uintptr_t loader, uintptr_t path, Buffer* buffer, uintptr_t name, uintptr_t a5, uintptr_t a6,
                        uintptr_t a7)
    {
        char text[300];
        if (const bm::gamefile::Supply supply = g_supply)
        {
            const std::string* bytes =
                buffer && bm::mem::ReadEngineString(path, text, sizeof text) ? supply(text, nullptr) : nullptr;
            if (bytes)
            {
                // The find hook has already sent a new name to its stand-in.
                const uint64_t r = g_readOriginal(loader, path, buffer, name, a5, a6, a7);
                if (bytes->empty()) return r;
                const bool ok = Replace(buffer, *bytes, text);
                static volatile LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 8)
                    LOG("[anim] Read %s: the game's read gave %u bytes (result %llX); the plugin's %zu bytes %s.", text,
                        (r & 0xFF) && buffer->data ? buffer->size : 0, static_cast<unsigned long long>(r), bytes->size(),
                        ok ? "are in its buffer now" : "could not be placed");
                return ok ? (r & ~0xFFull) | 1 : r;
            }
        }
        const bool wanted = buffer && bm::mem::ReadEngineString(path, text, sizeof text) && g_wants(text);
        if (wanted && g_readInstead)
        {
            std::string instead, fallback;
            const uintptr_t outer = t_loader;
            t_loader = loader;
            const bool swap = g_readInstead(text, instead, fallback);
            t_loader = outer;
            if (swap)
            {
                uint64_t r = ReadAs(loader, instead, buffer, name, a5, a6, a7);
                bool ok = (r & 0xFF) && buffer->data;
                if (!ok && !fallback.empty())
                {
                    r = ReadAs(loader, fallback, buffer, name, a5, a6, a7);
                    ok = (r & 0xFF) && buffer->data;
                }
                if (ok && g_patch) g_patch(text, buffer->data, buffer->size);
                static volatile LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 8)
                    LOG("[files] %s read as %s%s.", text, ok ? instead.c_str() : "nothing", ok ? "" : " (it did not read)");
                return r;
            }
        }
        uint64_t r = g_readOriginal(loader, path, buffer, name, a5, a6, a7);
        if (wanted)
        {
            const bool found = (r & 0xFF) && buffer->data;
            std::string game, ours;
            if (found) game.assign(reinterpret_cast<const char*>(buffer->data), buffer->size);
            const uintptr_t outer = t_loader;
            t_loader = loader;
            const bool serve = g_serve(text, found, game, ours);
            t_loader = outer;
            if (serve && Replace(buffer, ours, text) && !found) r = (r & ~0xFFull) | 1;
        }
        // After Serve, so an observer sees the bytes the game keeps.
        if (const bm::gamefile::Seen seen = g_seen)
        {
            char seenText[300];
            if ((r & 0xFF) && buffer && buffer->data && bm::mem::ReadEngineString(path, seenText, sizeof seenText))
                seen(seenText, buffer->data, buffer->size);
        }
        return r;
    }

    // The loader reads the task's buffer (+0x28) twice at its start, the
    // null check and the parse, and nothing else does, so it holds ours for
    // the call and the game's again after.
    uintptr_t AppearanceDetour(uintptr_t task)
    {
        char path[300];
        const std::string* text = nullptr;
        if (bm::mem::ReadEngineString(task + kOff_AppTask_Path, path, sizeof path)) text = g_appearance(path);
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(task + kOff_AppTask_Buffer);
        if (!text || !bm::mem::Readable(reinterpret_cast<uintptr_t>(slot), 8)) return g_appearanceOriginal(task);

        std::string copy = *text;   // the parse may write into it
        copy.push_back('\0');
        Buffer ours{ reinterpret_cast<uint8_t*>(&copy[0]), static_cast<uint32_t>(text->size() + 2),
                     static_cast<uint32_t>(text->size()), -1 };
        const uintptr_t game = *slot;
        *slot = reinterpret_cast<uintptr_t>(&ours);
        const uintptr_t r = g_appearanceOriginal(task);
        *slot = game;
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 4) LOG("[files] the broom's appearance went to the loader for %s.", path);
        return r;
    }

    // The load goes ahead with a path of the game's own making for the file
    // to load instead. The loader takes its own reference to the path when it
    // queues the load, so ours is released as soon as it returns.
    uintptr_t AsyncDetour(uintptr_t loader, uintptr_t handle, uintptr_t status, uintptr_t path, uintptr_t a5,
                          uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11,
                          uintptr_t a12)
    {
        char text[300];
        std::string to;
        const bm::gamefile::Supply supply = g_supply;
        uintptr_t* job = supply ? JobSlot() : nullptr;
        if (job && path && bm::mem::ReadEngineString(path, text, sizeof text) && supply(text, nullptr))
        {
            const uintptr_t was = *job;
            *job = 0;
            const uintptr_t r = g_asyncOriginal(loader, handle, status, path, a5, a6, a7, a8, a9, a10, a11, a12);
            *job = was;
            static volatile LONG logged = 0;
            if (InterlockedIncrement(&logged) <= 8)
                LOG("[anim] async %s on a %s thread: read through Read, status %u, result %llX.", text,
                    was ? "loader" : "plain", status ? *reinterpret_cast<uint8_t*>(status) : 255,
                    static_cast<unsigned long long>(r));
            return r;
        }
        if (!path || !bm::mem::ReadEngineString(path, text, sizeof text) || !g_redirect(text, to))
            return g_asyncOriginal(loader, handle, status, path, a5, a6, a7, a8, a9, a10, a11, a12);
        uintptr_t obj = 0;
        g_makePath(&obj, to.c_str(), 0);
        const uintptr_t r =
            g_asyncOriginal(loader, handle, status, reinterpret_cast<uintptr_t>(&obj), a5, a6, a7, a8, a9, a10, a11, a12);
        g_releasePath(&obj);
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 6) LOG("[files] %s loads %s.", text, to.c_str());
        return r;
    }

    // A redirected path held for one call, and a note of it in the log.
    struct Swapped
    {
        uintptr_t obj = 0;
        bool on = false;
        Swapped(uintptr_t path, const char* where)
        {
            char text[300];
            std::string to;
            if (!path || !bm::mem::ReadEngineString(path, text, sizeof text)) return;
            const bm::gamefile::Supply supply = g_supply;
            if (!(supply && supply(text, &to) && !to.empty()) && !g_redirect(text, to)) return;
            g_makePath(&obj, to.c_str(), 0);
            on = true;
            static volatile LONG logged = 0;
            if (InterlockedIncrement(&logged) <= 24) LOG("[files] %s of %s goes to %s.", where, text, to.c_str());
        }
        ~Swapped()
        {
            if (on) g_releasePath(&obj);
        }
        uintptr_t Path(uintptr_t game) const { return on ? reinterpret_cast<uintptr_t>(&obj) : game; }
    };

    uintptr_t FindDetour(uintptr_t loader, uintptr_t out, uintptr_t path, uintptr_t flags)
    {
        Swapped s(path, "find");
        return g_findOriginal(loader, out, s.Path(path), flags);
    }

    uint64_t ExistsDetour(uintptr_t loader, uintptr_t path)
    {
        Swapped s(path, "exists");
        return g_existsOriginal(loader, s.Path(path));
    }

    uint64_t IndexOfDetour(uintptr_t loader, uintptr_t path)
    {
        Swapped s(path, "index");
        return g_indexOfOriginal(loader, s.Path(path));
    }

    uintptr_t CallTarget(uintptr_t call)
    {
        uint8_t op = 0;
        return bm::mem::Read8(call, &op) && op == 0xE8 ? bm::mem::RipAt(call, 5) : 0;
    }

    // The release whose empty-string global is the path builder's.
    bool SameEmpty(uintptr_t hit, void* ctx)
    {
        return bm::mem::RipAt(hit + kOff_ReleasePath_EmptyLea, 7) == *static_cast<uintptr_t*>(ctx);
    }
}

namespace bm::gamefile
{
    void Observe(Seen seen) { g_seen = seen; }
    void SupplyLoads(Supply supply) { g_supply = supply; }

    bool Install(const Hooks& hooks)
    {
        g_wants = hooks.wants;
        g_serve = hooks.serve;
        g_appearance = hooks.appearance;
        g_redirect = hooks.redirect;
        g_readInstead = hooks.readInstead;
        g_patch = hooks.patch;
        const uintptr_t read = mem::FindUnique(kSig_ResourceRead);
        const uintptr_t make = mem::FindUnique(kSig_MakePath);
        uintptr_t empty = make ? mem::RipAt(make + kOff_MakePath_EmptyLea, 7) : 0;
        const uintptr_t release = empty ? mem::FindIf(kSig_ReleasePath, &SameEmpty, &empty) : 0;
        const uintptr_t idiom = mem::Find(kSig_AllocIdiom);
        const uintptr_t bufRelease = mem::FindUnique(kSig_BufferRelease);
        const uintptr_t appLoad = mem::FindUnique(kSig_AppearanceLoad);
        if (idiom)
        {
            g_allocSet = reinterpret_cast<FnAlloc>(CallTarget(idiom + kOff_AllocIdiom_Set));
            g_allocClear = reinterpret_cast<FnAlloc>(CallTarget(idiom + kOff_AllocIdiom_Clear));
        }
        if (bufRelease)
        {
            g_freeSet = reinterpret_cast<FnFree>(CallTarget(bufRelease + kOff_BufferRelease_FreeSet));
            g_freeClear = reinterpret_cast<FnFree>(CallTarget(bufRelease + kOff_BufferRelease_FreeClear));
            g_tagBytes = reinterpret_cast<volatile LONG*>(mem::RipAt(bufRelease + kOff_BufferRelease_TagBytes, 7));
            g_bufferRelease = reinterpret_cast<FnBufferRelease>(bufRelease);
        }
        g_makePath = reinterpret_cast<FnMakePath>(make);
        g_releasePath = reinterpret_cast<FnReleasePath>(release);
        if (!read || !make || !release || !appLoad || !g_allocSet || !g_allocClear || !g_freeSet || !g_freeClear ||
            !g_tagBytes)
        {
            LOG_ERR("[files] the loader's pieces were not all found (read %s, path %s and %s, allocator %s, release %s, "
                    "appearance loader %s), so every file is the game's own and Broomy does not exist this session.",
                    read ? "found" : "missing", make ? "found" : "missing", release ? "found" : "missing",
                    g_allocSet && g_allocClear ? "found" : "missing",
                    g_freeSet && g_freeClear && g_tagBytes ? "found" : "missing", appLoad ? "found" : "missing");
            return false;
        }
        char why[96] = {}, why2[96] = {};
        const bool a = farhook::Install("resource read", read, reinterpret_cast<void*>(&ReadDetour),
                                        reinterpret_cast<void**>(&g_readOriginal), why, sizeof why);
        const bool b = a && farhook::Install("appearance loader", appLoad, reinterpret_cast<void*>(&AppearanceDetour),
                                             reinterpret_cast<void**>(&g_appearanceOriginal), why2, sizeof why2);
        if (!a || !b)
        {
            LOG_ERR("[files] the loader could not be hooked (%s%s), so every file is the game's own. A hook that did go "
                    "in comes out on unload.", why, why2);
            return false;
        }
        LOG("[files] serving files: read at +0x%llX, appearance loader at +0x%llX.",
            static_cast<unsigned long long>(mem::Rva(read)), static_cast<unsigned long long>(mem::Rva(appLoad)));

        uintptr_t vt[2] = {};
        uintptr_t async = 0;
        char why3[96] = "the loader's vtable was not found";
        if (mem::FindVtablesByName(kRtti_ResourceLoader, vt, 2) == 1 &&
            mem::ReadPtr(vt[0] + kSlot_LoaderAsync * 8, &async) && mem::InImage(async) &&
            farhook::Install("async load", async, reinterpret_cast<void*>(&AsyncDetour),
                             reinterpret_cast<void**>(&g_asyncOriginal), why3, sizeof why3))
            LOG("[files] loads redirected at the async load, +0x%llX.", static_cast<unsigned long long>(mem::Rva(async)));
        else
        {
            g_asyncOriginal = nullptr;
            LOG_ERR("[files] the async load could not be hooked (%s), so Broomy keeps the Wyvern's charts.", why3);
            return true;
        }
        const uintptr_t find = mem::FindUnique(kSig_LoaderFindSource);
        const uintptr_t exists = mem::FindUnique(kSig_LoaderExists);
        const uintptr_t indexOf = mem::FindUnique(kSig_LoaderIndexOf);
        char why4[96] = "not found", why5[96] = "not found", why6[96] = "not found";
        const bool f = find && farhook::Install("loader find", find, reinterpret_cast<void*>(&FindDetour),
                                                reinterpret_cast<void**>(&g_findOriginal), why4, sizeof why4);
        const bool e = exists && farhook::Install("loader exists", exists, reinterpret_cast<void*>(&ExistsDetour),
                                                  reinterpret_cast<void**>(&g_existsOriginal), why5, sizeof why5);
        const bool x = indexOf && farhook::Install("loader index of", indexOf, reinterpret_cast<void*>(&IndexOfDetour),
                                                   reinterpret_cast<void**>(&g_indexOfOriginal), why6, sizeof why6);
        if (f && e && x)
            LOG("[files] finds and existence checks redirected too, +0x%llX, +0x%llX and +0x%llX.",
                static_cast<unsigned long long>(mem::Rva(find)), static_cast<unsigned long long>(mem::Rva(exists)),
                static_cast<unsigned long long>(mem::Rva(indexOf)));
        else
            LOG_ERR("[files] find (%s), exists (%s) or index of (%s) could not be hooked.", f ? "hooked" : why4,
                    e ? "hooked" : why5, x ? "hooked" : why6);
        return true;
    }

    bool Redirecting() { return g_asyncOriginal != nullptr; }

    bool ReadFile(const char* path, std::string& out)
    {
        out.clear();
        if (!t_loader || !g_readOriginal) return false;
        static const char kNoName[1] = "";
        uintptr_t obj = 0;
        g_makePath(&obj, path, 0);
        Buffer b{ nullptr, 0, 0, -1 };
        const uint64_t r = g_readOriginal(t_loader, reinterpret_cast<uintptr_t>(&obj), &b,
                                          reinterpret_cast<uintptr_t>(kNoName), 0, 0, 0);
        g_releasePath(&obj);
        const bool ok = (r & 0xFF) && b.data;
        if (ok) out.assign(reinterpret_cast<const char*>(b.data), b.size);
        if (b.data) g_bufferRelease(&b);
        return ok;
    }
}
