/*
 * mem -- RDRAM budget and arena allocation.
 *
 * This project targets an 8 MB console: the Expansion Pak is required, not
 * optional. The deciding number is the per-level texture working set, which
 * reaches ~1.3 MB on the heaviest Ultimate Doom maps (E2M2: 88 wall textures
 * plus 49 flats). Fitting that into 4 MB alongside Doom's zone heap and the
 * framebuffers would mean an LRU that evicts mid-level, and eviction means PI
 * DMA stalls during play -- ~5 MB/s is fine at a level transition and ruinous
 * inside a frame. Paying for the Expansion Pak buys a working set that is
 * loaded once per level and then never touched again.
 *
 * Budgets below are reservations, not measurements. mem_report() prints what
 * is actually in use so the two can be compared rather than assumed.
 */
#ifndef MEM_H
#define MEM_H

#include <stddef.h>
#include <stdbool.h>

/* --- budget ------------------------------------------------------------- */

#define MEM_TOTAL_BYTES     (8 * 1024 * 1024)

/* Composed wall textures, flats AND sprite frames for the current level.
 *
 * The worst observed working set is 1691 KB (E2M2, measured in play rather
 * than at load -- see below), leaving ~350 KB of headroom for a heavier PWAD.
 * The 1334 KB this comment used to quote was a load-time reading, taken
 * before the level's textures and every demand-loaded sprite frame had
 * arrived; it understated the real peak by a quarter. */
/* Overridable so a build can reproduce a tighter budget than this
 * console has: the intermission loads its art while the finished
 * level's is still resident, and how much room is left decides
 * whether it draws at all. */
#ifndef MEM_TEXTURE_ARENA
#define MEM_TEXTURE_ARENA   (2 * 1024 * 1024)
#endif

/* Reserved for sprite frames, and never used: dt64_load puts every texture,
 * sprites included, in MEM_ARENA_TEXTURE. Nothing in the tree references
 * MEM_ARENA_SPRITE, and its high-water mark is 0 across a full demo route.
 * A megabyte of an 8 MB machine sat here for a split that never happened.
 *
 * Zero rather than deleted: the arena id stays valid, so if sprites ever do
 * get their own lifetime the reservation is one number away. */
#define MEM_SPRITE_ARENA    0

/* One level's lumps. The largest Ultimate Doom map is 186 KB. */
#define MEM_LEVEL_ARENA     (256 * 1024)

/* --- arenas -------------------------------------------------------------
 *
 * Bump allocators, reset wholesale at level load. Doom's asset lifetime is
 * strictly per-level, so individual frees are never needed -- and avoiding
 * them avoids fragmenting a heap that has to survive hours of play without a
 * compaction pass. */

typedef enum {
    MEM_ARENA_TEXTURE,
    MEM_ARENA_SPRITE,
    MEM_ARENA_LEVEL,
    MEM_ARENA_COUNT,
} mem_arena_id_t;

/* Reserve the arenas. Call once, after display and RDP init so their
 * allocations are already accounted for. Halts with a clear message if the
 * Expansion Pak is absent. */
void mem_init(void);

/* Allocate from an arena. Returns NULL when exhausted, having logged which
 * arena overflowed and by how much -- silent truncation would show up much
 * later as missing textures. Allocations are 16-byte aligned so the result can
 * be handed straight to the RDP. */
void *mem_alloc(mem_arena_id_t arena, size_t size);

/* Drop everything in an arena. Used at level transitions. */
void mem_reset(mem_arena_id_t arena);

/* Bytes used and reserved, for reporting and tests. */
size_t mem_used(mem_arena_id_t arena);
size_t mem_capacity(mem_arena_id_t arena);

/* Print arena occupancy and remaining system heap to the debug channel. */
void mem_report(const char *when);

#endif /* MEM_H */
