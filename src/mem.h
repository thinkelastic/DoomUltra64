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

/* Composed wall textures and flats for the current level. Sized off the
 * worst observed working set (1334 KB) with room for a heavier PWAD. */
#define MEM_TEXTURE_ARENA   (2 * 1024 * 1024)

/* Sprite frames referenced by the current level. The full set across the IWAD
 * is 3.4 MB, but any one level needs a small fraction of it. */
#define MEM_SPRITE_ARENA    (1 * 1024 * 1024)

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
