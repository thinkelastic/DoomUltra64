#include "mem.h"

#include <libdragon.h>
#include <malloc.h>
#include <string.h>

typedef struct {
    const char *name;
    uint8_t    *base;
    size_t      used;
    size_t      capacity;
    size_t      peak;       /* high-water mark, so headroom can be judged */
} arena_t;

static arena_t arenas[MEM_ARENA_COUNT] = {
    [MEM_ARENA_TEXTURE] = { .name = "texture", .capacity = MEM_TEXTURE_ARENA },
    [MEM_ARENA_SPRITE]  = { .name = "sprite",  .capacity = MEM_SPRITE_ARENA  },
    [MEM_ARENA_LEVEL]   = { .name = "level",   .capacity = MEM_LEVEL_ARENA   },
};

void mem_init(void)
{
    /* The Expansion Pak is a hard requirement, so say so plainly rather than
     * failing later with a confusing out-of-memory in the middle of a level
     * load. On a 4 MB console the texture arena alone would not fit. */
    assertf(is_memory_expanded(),
            "This build requires the Expansion Pak (8 MB).\n"
            "Detected %d MB of RDRAM.\n"
            "The per-level texture working set reaches ~1.3 MB, which does not\n"
            "fit 4 MB alongside the zone heap and framebuffers.",
            get_memory_size() / (1024 * 1024));

    for (int i = 0; i < MEM_ARENA_COUNT; i++) {
        arenas[i].base = memalign(16, arenas[i].capacity);
        assertf(arenas[i].base, "cannot reserve %s arena (%u KB)",
                arenas[i].name, (unsigned)(arenas[i].capacity / 1024));
        arenas[i].used = 0;
        arenas[i].peak = 0;
    }

    mem_report("after mem_init");
}

void *mem_alloc(mem_arena_id_t id, size_t size)
{
    arena_t *a = &arenas[id];

    /* Keep every allocation 16-byte aligned: the RDP reads texture data
     * straight out of these buffers and requires 8-byte alignment. */
    const size_t aligned = (size + 15u) & ~(size_t)15u;

    if (a->used + aligned > a->capacity) {
        debugf("mem: %s arena exhausted (need %u KB, %u KB free of %u KB)\n",
               a->name, (unsigned)(aligned / 1024),
               (unsigned)((a->capacity - a->used) / 1024),
               (unsigned)(a->capacity / 1024));
        return NULL;
    }

    void *p = a->base + a->used;
    a->used += aligned;
    if (a->used > a->peak) a->peak = a->used;
    return p;
}

void mem_reset(mem_arena_id_t id)
{
    arenas[id].used = 0;
}

size_t mem_used(mem_arena_id_t id)     { return arenas[id].used; }
size_t mem_capacity(mem_arena_id_t id) { return arenas[id].capacity; }

void mem_report(const char *when)
{
    debugf("mem [%s] RDRAM %d MB\n", when, get_memory_size() / (1024 * 1024));

    for (int i = 0; i < MEM_ARENA_COUNT; i++) {
        const arena_t *a = &arenas[i];
        debugf("  %-8s %5u / %5u KB  (peak %u KB)\n",
               a->name, (unsigned)(a->used / 1024),
               (unsigned)(a->capacity / 1024), (unsigned)(a->peak / 1024));
    }

    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("  sysheap %5u KB used, %u KB total\n",
           (unsigned)(hs.used / 1024), (unsigned)(hs.total / 1024));
}
