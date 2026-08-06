/*
 * Precomputed visibility, baked per map by tools/mkpvs.c.
 *
 * The renderer's own culling is good but not free: every node costs a
 * two-corner projection, a solid-segment range test and a window lookup
 * before it can reject anything, and a subsector that survives all that
 * still pays for its seg loop. The PVS answers the same question -- can
 * anything under this child be seen from where the camera stands -- with a
 * single bit, consulted first.
 *
 * A wrong bit here hides geometry, which is the worst failure this renderer
 * has. So PVS=2 builds keep the full traversal and only report subsectors
 * that were drawn from outside the recorded set: run the route, see no
 * violations, then trust it. PVS=0 removes it entirely.
 */
#include "r_pvs.h"

#include <libdragon.h>
#include <string.h>

#include "doom/doomtype.h"
#include "doom/r_defs.h"
#include "doom/r_state.h"
#include "doom/p_local.h"
#include "doom/z_zone.h"
#include "mem.h"

#if R_PVS

static uint8_t *pvs_data;         /* numssecs rows of (subbytes + nodebytes) */
static int      pvs_subbytes, pvs_nodebytes, pvs_stride;
static int      pvs_numss, pvs_numnodes;
static const uint8_t *pvs_row;    /* current camera subsector's row */
static int      pvs_cur_ss = -1;

int r_pvs_violations;

void r_pvs_reset(void)
{
    pvs_data = NULL;
    pvs_row  = NULL;
    pvs_cur_ss = -1;
}

void r_pvs_load(int episode, int map)
{
    char path[32];
    snprintf(path, sizeof path, "/pvs_E%dM%d.bin", episode, map);

    pvs_data = NULL;
    pvs_row  = NULL;
    pvs_cur_ss = -1;

    int fd = dfs_open(path);
    if (fd < 0) { debugf("pvs: %s absent, culling unaided\n", path); return; }

    const int size = dfs_size(fd);
    if (size < 4) { dfs_close(fd); return; }

    uint8_t *buf = Z_Malloc(size, PU_LEVEL, NULL);
    if (!buf || dfs_read(buf, 1, size, fd) != size) { dfs_close(fd); return; }
    dfs_close(fd);

    pvs_numss    = (buf[0] << 8) | buf[1];
    pvs_numnodes = (buf[2] << 8) | buf[3];
    pvs_subbytes  = (pvs_numss + 7) / 8;
    pvs_nodebytes = (pvs_numnodes + 7) / 8;
    pvs_stride    = pvs_subbytes + pvs_nodebytes;

    /* The set is baked against one build of the map. If the counts do not
     * match what the level actually loaded, the rows would be read at the
     * wrong stride -- refuse rather than cull by noise. */
    if (pvs_numss != numsubsectors || pvs_numnodes != numnodes ||
        4 + (long)pvs_numss * pvs_stride > size) {
        debugf("pvs: %s does not match level (%d/%d ss, %d/%d nodes)\n",
               path, pvs_numss, numsubsectors, pvs_numnodes, numnodes);
        return;
    }

    pvs_data = buf + 4;
    debugf("pvs: %s loaded (%d subsectors, %d KB)\n",
           path, pvs_numss, size / 1024);
}

/* Latch the row for the subsector the camera occupies. Called once a frame,
 * so the per-node test is a bare load and mask. */
void r_pvs_frame(float x, float y)
{
    if (!pvs_data) { pvs_row = NULL; return; }

    const subsector_t *ss = R_PointInSubsector((fixed_t)(x * 65536.0f),
                                               (fixed_t)(y * 65536.0f));
    const int idx = ss ? (int)(ss - subsectors) : -1;
    if (idx < 0 || idx >= pvs_numss) { pvs_row = NULL; return; }

    pvs_cur_ss = idx;
    pvs_row    = pvs_data + (size_t)idx * pvs_stride;
}

bool r_pvs_active(void) { return pvs_row != NULL; }

bool r_pvs_ss_visible(int ssnum)
{
    if (!pvs_row || ssnum < 0 || ssnum >= pvs_numss) return true;
    return (pvs_row[ssnum >> 3] >> (ssnum & 7)) & 1;
}

bool r_pvs_node_visible(int nodenum)
{
    if (!pvs_row || nodenum < 0 || nodenum >= pvs_numnodes) return true;
    const uint8_t *nb = pvs_row + pvs_subbytes;
    return (nb[nodenum >> 3] >> (nodenum & 7)) & 1;
}

/* Validation build: the traversal is left unpruned, and anything drawn from
 * outside the set is a bit the bake missed. */
void r_pvs_check_drawn(int ssnum)
{
    if (!pvs_row) return;
    if (r_pvs_ss_visible(ssnum)) return;
    if (++r_pvs_violations <= 200)
        debugf("pvs VIOLATION: drew ss=%d from ss=%d\n", ssnum, pvs_cur_ss);
}

#endif /* R_PVS */
