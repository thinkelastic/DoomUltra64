/*
 * mkpvs -- bake a per-subsector potentially-visible set for each map.
 *
 * The renderer culls by frustum and by solid-segment occlusion, but both
 * cost projection arithmetic per node before they can reject anything. A
 * PVS answers "can the subsector I am standing in see anything under this
 * subtree" with one bit, before any of that.
 *
 * The geometry this rests on is the subsector's true convex cell, rebuilt
 * the same way the renderer rebuilds it at load time (r_ssdata.c): start
 * from a polygon larger than the map, walk the BSP from the root to the
 * leaf clipping by every partition line on the way, then clip by the
 * subsector's own segs. A Doom subsector is NOT closed by its segs -- the
 * edges a BSP split creates carry no seg at all -- so anything that works
 * from segs alone (a centroid of seg vertices, points pushed off a seg
 * face) can land outside the cell or inside solid space. That mistake made
 * an earlier version of this tool blind from whole subsectors.
 *
 * Only one-sided linedefs block sight. A two-sided line can always open
 * later, so treating a closed door as opaque would bake in a state the
 * game does not promise to keep.
 *
 * Output: filesystem/pvs_<MAP>.bin
 *   uint16 numsubsectors, uint16 numnodes (big endian), then per subsector
 *   a bitset over subsectors followed by a bitset over nodes; a node bit is
 *   set when any subsector in its subtree is visible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define DIE(...) do { fprintf(stderr, "mkpvs: " __VA_ARGS__); \
                      fputc('\n', stderr); exit(1); } while (0)

/* ------------------------------------------------------------------ WAD */
typedef struct { char name[9]; const uint8_t *data; int size; } lump_t;
static uint8_t *wadbuf;
static lump_t  *lumps;
static int      numlumps;

static int rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static int rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }

static void wad_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) DIE("cannot open %s", path);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    wadbuf = malloc(sz);
    if (!wadbuf || fread(wadbuf, 1, sz, f) != (size_t)sz) DIE("short read on %s", path);
    fclose(f);

    numlumps = rd32(wadbuf + 4);
    int dofs = rd32(wadbuf + 8);
    lumps = calloc(numlumps, sizeof *lumps);
    for (int i = 0; i < numlumps; i++) {
        const uint8_t *d = wadbuf + dofs + 16 * i;
        lumps[i].data = wadbuf + rd32(d);
        lumps[i].size = rd32(d + 4);
        memcpy(lumps[i].name, d + 8, 8);
        lumps[i].name[8] = 0;
    }
}

/* --------------------------------------------------------------- level */
typedef struct { double x, y; } vec2_t;
typedef struct { int v1, v2, line, side; } seg_t;
typedef struct { int count, first; } ssec_t;
typedef struct { double x, y, dx, dy; int child[2]; } node_t;
typedef struct { int v1, v2, right, left; } line_t;

static vec2_t *verts;  static int numverts;
static seg_t  *segs;   static int numsegs;
static ssec_t *ssecs;  static int numssecs;
static node_t *nodes;  static int numnodes;
static line_t *lines;  static int numlines;

typedef struct { double x1, y1, x2, y2; } wall_t;
static wall_t *walls; static int numwalls;      /* one-sided: permanent */

#define NF_SUBSECTOR 0x8000

static int load_level(int mapidx)
{
    numverts = numsegs = numssecs = numnodes = numlines = 0;
    for (int i = mapidx + 1; i < numlumps && i <= mapidx + 10; i++) {
        const lump_t *l = &lumps[i];
        if (!strcmp(l->name, "VERTEXES")) {
            numverts = l->size / 4;
            verts = malloc(numverts * sizeof *verts);
            for (int k = 0; k < numverts; k++) {
                verts[k].x = rd16(l->data + k*4);
                verts[k].y = rd16(l->data + k*4 + 2);
            }
        } else if (!strcmp(l->name, "SEGS")) {
            numsegs = l->size / 12;
            segs = malloc(numsegs * sizeof *segs);
            for (int k = 0; k < numsegs; k++) {
                segs[k].v1   = (uint16_t)rd16(l->data + k*12);
                segs[k].v2   = (uint16_t)rd16(l->data + k*12 + 2);
                segs[k].line = (uint16_t)rd16(l->data + k*12 + 6);
                segs[k].side = (uint16_t)rd16(l->data + k*12 + 8);
            }
        } else if (!strcmp(l->name, "SSECTORS")) {
            numssecs = l->size / 4;
            ssecs = malloc(numssecs * sizeof *ssecs);
            for (int k = 0; k < numssecs; k++) {
                ssecs[k].count = (uint16_t)rd16(l->data + k*4);
                ssecs[k].first = (uint16_t)rd16(l->data + k*4 + 2);
            }
        } else if (!strcmp(l->name, "NODES")) {
            numnodes = l->size / 28;
            nodes = malloc(numnodes * sizeof *nodes);
            for (int k = 0; k < numnodes; k++) {
                const uint8_t *d = l->data + k*28;
                nodes[k].x  = rd16(d);    nodes[k].y  = rd16(d+2);
                nodes[k].dx = rd16(d+4);  nodes[k].dy = rd16(d+6);
                nodes[k].child[0] = (uint16_t)rd16(d+24);
                nodes[k].child[1] = (uint16_t)rd16(d+26);
            }
        } else if (!strcmp(l->name, "LINEDEFS")) {
            numlines = l->size / 14;
            lines = malloc(numlines * sizeof *lines);
            for (int k = 0; k < numlines; k++) {
                const uint8_t *d = l->data + k*14;
                lines[k].v1    = (uint16_t)rd16(d);
                lines[k].v2    = (uint16_t)rd16(d+2);
                lines[k].right = rd16(d+10);
                lines[k].left  = rd16(d+12);
            }
        }
    }
    if (!numssecs || !numsegs || !numverts || !numnodes) return 0;

    numwalls = 0;
    walls = malloc((numlines ? numlines : 1) * sizeof *walls);
    for (int k = 0; k < numlines; k++) {
        if (lines[k].left >= 0) continue;
        walls[numwalls].x1 = verts[lines[k].v1].x;
        walls[numwalls].y1 = verts[lines[k].v1].y;
        walls[numwalls].x2 = verts[lines[k].v2].x;
        walls[numwalls].y2 = verts[lines[k].v2].y;
        numwalls++;
    }
    return 1;
}

/* ------------------------------------------------ subsector polygons */
/* Generous next to the renderer's 24: a wrong polygon here is a wrong
 * visibility answer, and the memory is a host-side rounding error. */
#define MAXP 64

typedef struct { vec2_t p[MAXP]; int n; } poly_t;
static poly_t *cell;                       /* one convex polygon per subsector */

static double side_of(double x, double y, double px, double py,
                      double dx, double dy)
{
    return (x - px) * dy - (y - py) * dx;
}

static int clip_half(const vec2_t *in, int n, vec2_t *out,
                     double px, double py, double dx, double dy, int keep_front)
{
    if (n == 0) return 0;
    const double sgn = keep_front ? 1.0 : -1.0;

    int m = 0;
    for (int i = 0; i < n; i++) {
        const vec2_t a = in[i], b = in[(i + 1) % n];
        const double sa = sgn * side_of(a.x, a.y, px, py, dx, dy);
        const double sb = sgn * side_of(b.x, b.y, px, py, dx, dy);

        if (sa >= 0.0 && m < MAXP) out[m++] = a;
        if ((sa > 0.0 && sb < 0.0) || (sa < 0.0 && sb > 0.0)) {
            const double t = sa / (sa - sb);
            if (m < MAXP) {
                out[m].x = a.x + (b.x - a.x) * t;
                out[m].y = a.y + (b.y - a.y) * t;
                m++;
            }
        }
    }
    return m;
}

static int poly_truncated;

/* Same descent the renderer uses: clip the running polygon by each
 * partition on the way down, then by the leaf's own segs. */
static void build_cell(int num, const vec2_t *poly, int n)
{
    if (num & NF_SUBSECTOR) {
        const int ss = num & ~NF_SUBSECTOR;
        if (ss >= numssecs) return;

        vec2_t a[MAXP], b[MAXP];
        int cn = n > MAXP ? MAXP : n;
        memcpy(a, poly, cn * sizeof *a);

        for (int i = 0; i < ssecs[ss].count && cn >= 3; i++) {
            const seg_t *sg = &segs[ssecs[ss].first + i];
            const vec2_t p1 = verts[sg->v1], p2 = verts[sg->v2];
            cn = clip_half(a, cn, b, p1.x, p1.y, p2.x - p1.x, p2.y - p1.y, 1);
            memcpy(a, b, cn * sizeof *a);
        }

        if (cn >= MAXP) poly_truncated++;
        cell[ss].n = cn < 3 ? 0 : cn;
        if (cell[ss].n) memcpy(cell[ss].p, a, cn * sizeof *a);
        return;
    }

    if (num >= numnodes) return;
    const node_t *nd = &nodes[num];
    vec2_t child[MAXP] = {{0, 0}};
    int cn;

    cn = clip_half(poly, n, child, nd->x, nd->y, nd->dx, nd->dy, 1);
    build_cell(nd->child[0], child, cn);
    cn = clip_half(poly, n, child, nd->x, nd->y, nd->dx, nd->dy, 0);
    build_cell(nd->child[1], child, cn);
}

static double gminx, gminy, gmaxx, gmaxy;

static void build_cells(void)
{
    gminx = gminy = 1e18; gmaxx = gmaxy = -1e18;
    for (int i = 0; i < numverts; i++) {
        if (verts[i].x < gminx) gminx = verts[i].x;
        if (verts[i].y < gminy) gminy = verts[i].y;
        if (verts[i].x > gmaxx) gmaxx = verts[i].x;
        if (verts[i].y > gmaxy) gmaxy = verts[i].y;
    }

    const double m = 64.0;
    const vec2_t root[4] = {
        { gminx - m, gminy - m }, { gmaxx + m, gminy - m },
        { gmaxx + m, gmaxy + m }, { gminx - m, gmaxy + m },
    };
    cell = calloc(numssecs, sizeof *cell);
    poly_truncated = 0;
    build_cell(numnodes - 1, root, 4);
}

/* Convexity lets a point test be a sign check against every edge. The
 * polygons come out of clip_half wound consistently, so one sign serves. */
static int point_inside(const poly_t *c, double x, double y, double margin)
{
    if (c->n < 3) return 0;
    int neg = 0, pos = 0;
    for (int i = 0; i < c->n; i++) {
        const vec2_t a = c->p[i], b = c->p[(i + 1) % c->n];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double len = sqrt(dx*dx + dy*dy);
        if (len < 1e-9) continue;
        const double s = side_of(x, y, a.x, a.y, dx, dy) / len;
        if (s >  margin) pos = 1;
        if (s < -margin) neg = 1;
    }
    return !(pos && neg);                  /* strictly one side of every edge */
}

/* Distance from a point to a segment, for the boundary checks below. */
static double dist_point_seg(double px, double py,
                             double ax, double ay, double bx, double by)
{
    const double dx = bx - ax, dy = by - ay;
    const double dd = dx*dx + dy*dy;
    double t = dd > 1e-12 ? ((px-ax)*dx + (py-ay)*dy) / dd : 0.0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return hypot(px - (ax + dx*t), py - (ay + dy*t));
}

static double dist_point_poly_edge(const poly_t *c, double x, double y)
{
    double best = 1e18;
    for (int i = 0; i < c->n; i++) {
        const vec2_t a = c->p[i], b = c->p[(i + 1) % c->n];
        const double d = dist_point_seg(x, y, a.x, a.y, b.x, b.y);
        if (d < best) best = d;
    }
    return best;
}

/* Every seg endpoint must lie on its cell's boundary, and the cell must
 * hold an interior point. A failure means the reconstruction is wrong and
 * everything downstream is visibility computed for the wrong shape. */
static void verify_cells(const char *map)
{
    int bad_bound = 0, bad_inside = 0, empty = 0;
    double worst = 0.0;

    for (int i = 0; i < numssecs; i++) {
        const poly_t *c = &cell[i];
        if (c->n < 3) { empty++; continue; }

        for (int j = 0; j < ssecs[i].count; j++) {
            const seg_t *sg = &segs[ssecs[i].first + j];
            for (int e = 0; e < 2; e++) {
                const vec2_t v = verts[e ? sg->v2 : sg->v1];
                const double d = dist_point_poly_edge(c, v.x, v.y);
                if (d > worst) worst = d;
                if (d > 0.5) { bad_bound++; break; }
            }
        }

        double cx = 0, cy = 0;
        for (int k = 0; k < c->n; k++) { cx += c->p[k].x; cy += c->p[k].y; }
        if (!point_inside(c, cx / c->n, cy / c->n, 1e-6)) bad_inside++;
    }

    if (bad_bound || bad_inside || empty || poly_truncated)
        printf("  [PVS  ] %s: cell check -- %d off-boundary (worst %.2f u), "
               "%d bad centroid, %d empty, %d truncated\n",
               map, bad_bound, worst, bad_inside, empty, poly_truncated);
}

/* ------------------------------------------------------------- samples */
#define MAXSAMP 40
typedef struct { double x[MAXSAMP], y[MAXSAMP]; int n; double cx, cy; } samp_t;
static samp_t *samples;

/* Interior points only: the centroid, then each vertex and each edge
 * midpoint drawn a little toward it. Convexity makes every one of those
 * inside, and each is checked before it is kept. */
static void build_samples(void)
{
    samples = calloc(numssecs, sizeof *samples);

    for (int i = 0; i < numssecs; i++) {
        const poly_t *c = &cell[i];
        samp_t *s = &samples[i];
        s->n = 0;
        if (c->n < 3) continue;

        double cx = 0, cy = 0;
        for (int k = 0; k < c->n; k++) { cx += c->p[k].x; cy += c->p[k].y; }
        cx /= c->n; cy /= c->n;
        s->cx = cx; s->cy = cy;

        if (point_inside(c, cx, cy, 1e-6)) {
            s->x[s->n] = cx; s->y[s->n] = cy; s->n++;
        }

        for (int k = 0; k < c->n && s->n + 2 <= MAXSAMP; k++) {
            const vec2_t a = c->p[k], b = c->p[(k + 1) % c->n];
            const double mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5;
            const double pts[2][2] = {
                { a.x + (cx - a.x) * 0.08, a.y + (cy - a.y) * 0.08 },
                { mx + (cx - mx) * 0.08,   my + (cy - my) * 0.08   },
            };
            for (int q = 0; q < 2; q++)
                if (point_inside(c, pts[q][0], pts[q][1], 1e-6)) {
                    s->x[s->n] = pts[q][0];
                    s->y[s->n] = pts[q][1];
                    s->n++;
                }
        }
    }
}

/* ------------------------------------------------------- blocker grid */
#define GRID 64

/* Every wall is registered in EVERY grid cell its bounding box covers.
 *
 * The first version linked each wall into the single cell containing its
 * lower corner, which only ever worked by accident: the bake asks about
 * long rays whose own bounding box happens to sweep that cell too. Ask
 * about a short segment -- as the ray-fan validator does, marching eight
 * units at a time -- and the wall is simply invisible, so rays walk through
 * solid geometry. The validator found this immediately, which is the whole
 * reason for having a second, differently-shaped check. */
static int *grid_off;        /* GRID*GRID + 1 offsets into grid_idx */
static int *grid_idx;        /* wall indices, grouped by cell */
static int *wall_stamp;      /* dedupe within one query */
static int  query_stamp;

static void grid_cellrange(double x0, double y0, double x1, double y1,
                           int *c0, int *c1, int *r0, int *r1)
{
    const double sx = GRID / (gmaxx - gminx + 1.0);
    const double sy = GRID / (gmaxy - gminy + 1.0);
    *c0 = (int)((fmin(x0, x1) - gminx) * sx);
    *c1 = (int)((fmax(x0, x1) - gminx) * sx);
    *r0 = (int)((fmin(y0, y1) - gminy) * sy);
    *r1 = (int)((fmax(y0, y1) - gminy) * sy);
    /* Both ends of both ranges, not just the near ones: the validator
     * marches rays past the map edge, where an unclamped low index runs
     * off the end of the offset table. */
    if (*c0 < 0) *c0 = 0;
    if (*c0 >= GRID) *c0 = GRID - 1;
    if (*r0 < 0) *r0 = 0;
    if (*r0 >= GRID) *r0 = GRID - 1;
    if (*c1 < 0) *c1 = 0;
    if (*c1 >= GRID) *c1 = GRID - 1;
    if (*r1 < 0) *r1 = 0;
    if (*r1 >= GRID) *r1 = GRID - 1;
    if (*c1 < *c0) *c1 = *c0;
    if (*r1 < *r0) *r1 = *r0;
}

static void grid_build(void)
{
    grid_off = calloc(GRID * GRID + 1, sizeof(int));
    wall_stamp = calloc(numwalls ? numwalls : 1, sizeof(int));
    query_stamp = 0;

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int total = 0;
            for (int i = 0; i < GRID * GRID; i++) {
                const int n = grid_off[i];
                grid_off[i] = total;
                total += n;
            }
            grid_off[GRID * GRID] = total;
            grid_idx = malloc((total ? total : 1) * sizeof(int));
        }
        for (int i = 0; i < numwalls; i++) {
            int c0, c1, r0, r1;
            grid_cellrange(walls[i].x1, walls[i].y1, walls[i].x2, walls[i].y2,
                           &c0, &c1, &r0, &r1);
            for (int r = r0; r <= r1; r++)
                for (int c = c0; c <= c1; c++) {
                    const int k = r * GRID + c;
                    if (pass == 0) grid_off[k]++;
                    else           grid_idx[grid_off[k]++] = i;
                }
        }
        if (pass == 1) {
            /* Undo the fill cursor: shift the offsets back into place. */
            for (int i = GRID * GRID; i > 0; i--) grid_off[i] = grid_off[i-1];
            grid_off[0] = 0;
        }
    }
}

static int seg_cross(double ax, double ay, double bx, double by,
                     double cx, double cy, double dx, double dy)
{
    const double d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    const double d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
    if ((d1 > 0) == (d2 > 0)) return 0;
    const double d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
    const double d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);
    return (d3 > 0) != (d4 > 0);
}

static int blocked(double ax, double ay, double bx, double by)
{
    int c0, c1, r0, r1;
    grid_cellrange(ax, ay, bx, by, &c0, &c1, &r0, &r1);
    ++query_stamp;

    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            const int k = r * GRID + c;
            for (int e = grid_off[k]; e < grid_off[k + 1]; e++) {
                const int i = grid_idx[e];
                if (wall_stamp[i] == query_stamp) continue;
                wall_stamp[i] = query_stamp;
                if (seg_cross(ax, ay, bx, by,
                              walls[i].x1, walls[i].y1, walls[i].x2, walls[i].y2))
                    return 1;
            }
        }
    return 0;
}

/* ----------------------------------------------------------- adjacency */
/* Exact, from the reconstructed cells: two subsectors are adjacent when
 * their boundaries touch. That covers portals through two-sided linedefs
 * and, just as importantly, the edges a BSP split leaves with no linedef
 * at all -- the previous "samples within 12 units" guess covered neither
 * reliably. */
#define ADJ_EPS 0.25

static uint8_t *adj;                       /* numssecs x subbytes */
static int subbytes;

static int  bit(const uint8_t *row, int i) { return (row[i >> 3] >> (i & 7)) & 1; }
static void setbit(uint8_t *row, int i)    { row[i >> 3] |= 1 << (i & 7); }

/* The collinear overlap of two edges, if they share boundary at all.
 *
 * This must catch a PARTIAL overlap -- edges that run into each other
 * without either containing the other -- which is the ordinary case where
 * two cells meet along part of a wall. Requiring one edge to contain the
 * other, as this first did, found barely one neighbour per cell and left
 * the portal graph too sparse to propagate through at all. */
static int edges_share(vec2_t a0, vec2_t a1, vec2_t b0, vec2_t b1,
                       vec2_t *o0, vec2_t *o1)
{
    const vec2_t da = { a1.x - a0.x, a1.y - a0.y };
    const double la = hypot(da.x, da.y);
    if (la < 1e-6) return 0;
    const vec2_t ua = { da.x / la, da.y / la };

    /* Both ends of b must lie on a's infinite line, or the edges cross
     * rather than share. */
    if (fabs(ua.x * (b0.y - a0.y) - ua.y * (b0.x - a0.x)) > ADJ_EPS) return 0;
    if (fabs(ua.x * (b1.y - a0.y) - ua.y * (b1.x - a0.x)) > ADJ_EPS) return 0;

    double t0 = (b0.x - a0.x) * ua.x + (b0.y - a0.y) * ua.y;
    double t1 = (b1.x - a0.x) * ua.x + (b1.y - a0.y) * ua.y;
    if (t0 > t1) { const double t = t0; t0 = t1; t1 = t; }
    if (t0 < 0.0) t0 = 0.0;
    if (t1 > la)  t1 = la;
    if (t1 - t0 < 0.05) return 0;              /* a corner touch, not a way through */

    if (o0) { o0->x = a0.x + ua.x * t0; o0->y = a0.y + ua.y * t0; }
    if (o1) { o1->x = a0.x + ua.x * t1; o1->y = a0.y + ua.y * t1; }
    return 1;
}

static int cells_touch(const poly_t *A, const poly_t *B)
{
    for (int i = 0; i < A->n; i++)
        for (int j = 0; j < B->n; j++)
            if (edges_share(A->p[i], A->p[(i + 1) % A->n],
                            B->p[j], B->p[(j + 1) % B->n], NULL, NULL))
                return 1;
    return 0;
}

static void build_adjacency(void)
{
    adj = calloc((size_t)numssecs * subbytes, 1);

    double *bx0 = malloc(numssecs * sizeof(double));
    double *by0 = malloc(numssecs * sizeof(double));
    double *bx1 = malloc(numssecs * sizeof(double));
    double *by1 = malloc(numssecs * sizeof(double));
    for (int i = 0; i < numssecs; i++) {
        bx0[i] = by0[i] = 1e18; bx1[i] = by1[i] = -1e18;
        for (int k = 0; k < cell[i].n; k++) {
            if (cell[i].p[k].x < bx0[i]) bx0[i] = cell[i].p[k].x;
            if (cell[i].p[k].y < by0[i]) by0[i] = cell[i].p[k].y;
            if (cell[i].p[k].x > bx1[i]) bx1[i] = cell[i].p[k].x;
            if (cell[i].p[k].y > by1[i]) by1[i] = cell[i].p[k].y;
        }
    }

    for (int a = 0; a < numssecs; a++) {
        if (cell[a].n < 3) continue;
        for (int b = a + 1; b < numssecs; b++) {
            if (cell[b].n < 3) continue;
            if (bx1[a] < bx0[b] - ADJ_EPS || bx1[b] < bx0[a] - ADJ_EPS ||
                by1[a] < by0[b] - ADJ_EPS || by1[b] < by0[a] - ADJ_EPS)
                continue;
            if (cells_touch(&cell[a], &cell[b])) {
                setbit(adj + (size_t)a * subbytes, b);
                setbit(adj + (size_t)b * subbytes, a);
            }
        }
    }
    free(bx0); free(by0); free(bx1); free(by1);
}

/* ------------------------------------------------- portal visibility */
/* Conservative by construction, rather than sampled and widened.
 *
 * Portals are the shared boundaries between adjacent cells that solid
 * geometry does not close. From a source cell S, sight can only leave
 * through one of its portals, and beyond that only through a portal of the
 * cell it entered, and so on. What survives at each step is the
 * anti-penumbra: the set of points reachable by a line that starts
 * somewhere in S and passes through every aperture on the way.
 *
 * For a convex source and a segment aperture the anti-penumbra is bounded
 * by two lines, each through one end of the aperture and tangent to the
 * source on the far side -- the crossed pairing, which is what makes it a
 * penumbra rather than a shadow. Clipping the next portal by those two
 * half-planes can only ever keep MORE than is truly visible, never less,
 * because a genuine sight line through the aperture satisfies both
 * constraints by definition. That is the property the sampled bake could
 * not offer at any density. */

typedef struct { vec2_t a, b; int cell[2]; } portal_t;
static portal_t *portals; static int numportals;
static int *cell_portal_head, *cell_portal_next;   /* per-cell portal lists */

static double cross2(vec2_t u, vec2_t v) { return u.x * v.y - u.y * v.x; }
static vec2_t sub2(vec2_t u, vec2_t v) { vec2_t r = { u.x - v.x, u.y - v.y }; return r; }

/* Does solid geometry close this boundary? Step across it and ask. */
static int boundary_is_solid(vec2_t p0, vec2_t p1)
{
    const vec2_t d = sub2(p1, p0);
    const double len = hypot(d.x, d.y);
    if (len < 1e-6) return 1;
    const vec2_t n = { -d.y / len, d.x / len };
    const double mx = (p0.x + p1.x) * 0.5, my = (p0.y + p1.y) * 0.5;
    return blocked(mx - n.x * 1.5, my - n.y * 1.5,
                   mx + n.x * 1.5, my + n.y * 1.5);
}

static void build_portals(void)
{
    numportals = 0;
    portals = malloc((size_t)numssecs * 16 * sizeof *portals);
    cell_portal_head = malloc(numssecs * sizeof(int));
    cell_portal_next = malloc((size_t)numssecs * 32 * sizeof(int));
    for (int i = 0; i < numssecs; i++) cell_portal_head[i] = -1;

    for (int a = 0; a < numssecs; a++) {
        if (cell[a].n < 3) continue;
        const uint8_t *arow = adj + (size_t)a * subbytes;
        for (int b = a + 1; b < numssecs; b++) {
            if (!bit(arow, b) || cell[b].n < 3) continue;

            vec2_t o0, o1;
            int found = 0;
            for (int i = 0; i < cell[a].n && !found; i++)
                for (int j = 0; j < cell[b].n && !found; j++)
                    if (edges_share(cell[a].p[i], cell[a].p[(i+1) % cell[a].n],
                                    cell[b].p[j], cell[b].p[(j+1) % cell[b].n],
                                    &o0, &o1))
                        found = 1;
            if (!found || boundary_is_solid(o0, o1)) continue;
            if (numportals >= numssecs * 16) continue;

            portals[numportals].a = o0;
            portals[numportals].b = o1;
            portals[numportals].cell[0] = a;
            portals[numportals].cell[1] = b;
            for (int s = 0; s < 2; s++) {
                const int c = portals[numportals].cell[s];
                cell_portal_next[numportals * 2 + s] = cell_portal_head[c];
                cell_portal_head[c] = numportals * 2 + s;
            }
            numportals++;
        }
    }
}

/* Clip segment [p0,p1] to the half-plane through org whose interior side
 * is the one holding `inside`. Returns 0 when nothing survives. */
static int clip_seg_half(vec2_t *p0, vec2_t *p1, vec2_t org, vec2_t dir,
                         vec2_t inside)
{
    const double si = cross2(dir, sub2(inside, org));
    const double sgn = si >= 0 ? 1.0 : -1.0;
    const double s0 = sgn * cross2(dir, sub2(*p0, org));
    const double s1 = sgn * cross2(dir, sub2(*p1, org));

    if (s0 < 0 && s1 < 0) return 0;
    if (s0 < 0) {
        const double t = s0 / (s0 - s1);
        p0->x += (p1->x - p0->x) * t;
        p0->y += (p1->y - p0->y) * t;
    } else if (s1 < 0) {
        const double t = s1 / (s1 - s0);
        p1->x += (p0->x - p1->x) * t;
        p1->y += (p0->y - p1->y) * t;
    }
    /* Any surviving sliver counts: discarding thin beams would be an
     * under-report, the one direction this may not err in. */
    return hypot(p1->x - p0->x, p1->y - p0->y) > 1e-9;
}

/* The anti-penumbra of one aperture seen through the next, clipped onto a
 * candidate portal.
 *
 * The beam is defined by the last TWO apertures rather than by the source
 * cell. That is deliberate: on the first hop the aperture lies ON the
 * source cell's own boundary, and "the tangent from a point to the
 * polygon" is degenerate there -- which is what made the first attempt
 * clip away almost everything and report a quarter of the map when the
 * truth is more than a third. Two segments have no such degeneracy, and
 * bounding by the last two apertures is still conservative: every sight
 * line through the whole chain passes through both of them, so it
 * satisfies these constraints by construction.
 *
 * Which endpoints pair with which is not a guess: order both apertures
 * across the direction of travel, then cross them -- the left end of the
 * first with the right end of the second and vice versa. That crossing is
 * what makes the region a penumbra rather than an umbra. */
static int beam_clip_pair(vec2_t p0, vec2_t p1, vec2_t a0, vec2_t a1,
                          vec2_t *q0, vec2_t *q1)
{
    const vec2_t midp = { (p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5 };
    const vec2_t mida = { (a0.x + a1.x) * 0.5, (a0.y + a1.y) * 0.5 };
    const vec2_t dir = sub2(mida, midp);
    const double dlen = hypot(dir.x, dir.y);
    if (dlen < 1e-9) return 1;                         /* nothing to bound by */

    /* The aperture midpoint is the reference for which side of each
     * boundary to keep. Pushing it further along the travel direction was
     * tried and is wrong: the boundary lines pass through the aperture's
     * own ends, so a point beyond it can fall on the far side of one of
     * them and invert the test, which loosened E1M1 from 71% to 94%
     * visible. The midpoint is separated from both boundaries by the
     * aperture's own half-width, which is exactly the margin available. */
    (void)dlen;
    const vec2_t ref = mida;

    /* Sort each aperture's ends across the travel direction. */
    const double sp0 = cross2(dir, sub2(p0, midp));
    const double sa0 = cross2(dir, sub2(a0, mida));
    const vec2_t p_lo = sp0 <= 0 ? p0 : p1, p_hi = sp0 <= 0 ? p1 : p0;
    const vec2_t a_lo = sa0 <= 0 ? a0 : a1, a_hi = sa0 <= 0 ? a1 : a0;

    if (!clip_seg_half(q0, q1, a_hi, sub2(a_hi, p_lo), ref)) return 0;
    if (!clip_seg_half(q0, q1, a_lo, sub2(a_lo, p_hi), ref)) return 0;
    return 1;
}

/* Propagate from one source cell and mark everything its sight can reach.
 *
 * NOT YET CONSERVATIVE, and off by default. Three real faults have been
 * found and fixed along the way, each caught by the ray-fan check:
 *
 *   - adjacency missed partially overlapping edges, requiring one to
 *     contain the other, which left barely 1.6 neighbours per cell and a
 *     portal graph too sparse to propagate through;
 *   - apertures were pruned by a single spanning interval per portal, so a
 *     beam falling in the gap between two earlier ones was discarded
 *     unprocessed;
 *   - the beam was built from tangents to the source POLYGON, which is
 *     degenerate on the first hop because the aperture lies on that
 *     polygon's own boundary. It is now built from the last two apertures,
 *     which is both well defined and still conservative: every sight line
 *     through the chain passes through both.
 *
 * Between them those took E1M1 from 2.3M ray-fan violations to 23.5k, and
 * what remains has now been localised. The validator reports the portal
 * hop count for each violation, and they are uniformly DEEP: 6, 8, 19
 * hops, never a neighbour. So the graph reaches these cells comfortably
 * and the beam is being thrown away somewhere along a long chain.
 *
 * Three candidate fixes were tried against that and none moved the number,
 * which rules them out: recording apertures as separate intervals rather
 * than widening one (SEEN_MAX), counting queue overflow, and bounding the
 * first hop by its own aperture plane. All three are kept because each is
 * right on its own terms, but none is the cause.
 *
 * The clipping is NOT the remaining problem, and that is now settled
 * rather than argued: --check traces the shortest portal chain to the
 * first violating cell and prints the aperture at every hop. On E1M1 the
 * eight-hop chain from subsector 0 to 204 keeps its FULL aperture at each
 * one -- 8.0, 120.0, 184.0, 160.0, 160.0, 105.8, 258.0, 224.0, not a
 * single unit lost -- and 204 is still missing from the set. The beam
 * never narrows; the walk simply never takes that chain.
 *
 * So what is left is the propagation's own bookkeeping, not its geometry.
 * Keying the pruning on the arriving portal as well as the interval, and
 * then on the arriving span too, took the WAD from 3.96M violations to
 * 2.65M and E1M1 from 70.5% to 76.9% visible -- real progress, and proof
 * the direction is right, but the walk is still dropping chains it should
 * follow. The next thing to instrument is not the beam but the QUEUE:
 * log every entry pushed and processed for one source, and find which hop
 * of that traced chain never gets enqueued. */
long pvs_queue_overflow;

/* When set, every portal side the walk enqueues is recorded here, so a
 * chain that the geometry says is clear can be checked hop by hop for the
 * one that never entered the queue. */
static uint8_t *trace_pushed;

static void propagate(int src, uint8_t *row)
{
    typedef struct { int pside; vec2_t a, b, pa, pb; int has_prev; } work_t;
    static work_t *queue; static int qcap;
    if (!queue) { qcap = 65536; queue = malloc(qcap * sizeof *queue); }
    /* The queue GROWS. A fixed cap silently truncated the walk -- 16k
     * dropped entries on E1M1 and 2.7M on E1M8 -- and a truncated walk
     * simply never reaches cells, which is an under-report no amount of
     * correct geometry can undo. This was the real limiter behind
     * violations that survived every fix to the beam and the pruning. */
    #define QGROW(need) do {                                              \
        if ((need) >= qcap) {                                             \
            while ((need) >= qcap) qcap *= 2;                             \
            queue = realloc(queue, (size_t)qcap * sizeof *queue);         \
            if (!queue) DIE("out of memory growing the portal queue");    \
        }                                                                 \
    } while (0)

    /* Which parts of each portal have already been let through.
     *
     * Kept as a small list of intervals rather than one spanning range: a
     * single min/max would swallow the gap between two separate apertures
     * and prune a third that falls in it but was never actually processed.
     * That is an under-report, so the intervals are held separately and a
     * new aperture is skipped only when one of them truly contains it. */
    /* Keyed on WHERE THE BEAM CAME FROM as well as how much of the portal
     * it lights.
     *
     * Pruning on the interval alone is wrong, and this is the bug that
     * survived every geometric fix: what propagates onward is decided by
     * the PAIR (previous aperture, this aperture), so two chains arriving
     * at the same portal through different predecessors carry different
     * beams beyond it even when they light the same span. Keying on the
     * interval let whichever chain arrived first suppress the other, and
     * the suppressed one was sometimes the only route to a cell. Traced on
     * E1M1: the eight-hop chain from subsector 0 to 204 keeps its full
     * aperture at every hop -- the beam never narrows at all -- yet 204 was
     * absent from the set, because the walk never took that chain. */
    enum { SEEN_MAX = 12 };
    typedef struct {
        int    prev[SEEN_MAX];
        double pt0[SEEN_MAX], pt1[SEEN_MAX];   /* how much of prev was open */
        double t0[SEEN_MAX], t1[SEEN_MAX];     /* how much of this one is */
        int    n;
    } seen_t;
    seen_t *seen = calloc((size_t)numportals * 2, sizeof *seen);

    int qn = 0;
    setbit(row, src);

    for (int e = cell_portal_head[src]; e >= 0; e = cell_portal_next[e]) {
        const portal_t *p = &portals[e >> 1];
        const int other = p->cell[(e & 1) ^ 1];
        setbit(row, other);
        {
            QGROW(qn);
            if (trace_pushed) trace_pushed[e] = 1;
            queue[qn].pside = e;
            queue[qn].a = p->a;
            queue[qn].b = p->b;
            queue[qn].has_prev = 0;
            seen[e].prev[0] = -1;
            seen[e].pt0[0] = 0.0;
            seen[e].pt1[0] = 0.0;
            seen[e].t0[0] = 0.0;
            seen[e].t1[0] = hypot(p->b.x - p->a.x, p->b.y - p->a.y);
            seen[e].n = 1;
            qn++;
        }
    }

    for (int qi = 0; qi < qn; qi++) {
        const work_t w = queue[qi];
        const portal_t *p = &portals[w.pside >> 1];
        const int into = p->cell[(w.pside & 1) ^ 1];

        for (int e = cell_portal_head[into]; e >= 0; e = cell_portal_next[e]) {
            if ((e >> 1) == (w.pside >> 1)) continue;      /* came from here */
            const portal_t *q = &portals[e >> 1];
            if (q->cell[e & 1] != into) continue;          /* wrong side */

            vec2_t q0 = q->a, q1 = q->b;
            /* The first hop out of the source cell is unclipped: its
             * aperture is part of that cell's own boundary, so every point
             * beyond it is reachable from somewhere inside. Constraints
             * start at the second aperture, where two distinct segments
             * exist to define a beam. */
            if (w.has_prev) {
                if (!beam_clip_pair(w.pa, w.pb, w.a, w.b, &q0, &q1)) continue;
            } else {
                /* Straight out of the source cell there is no earlier
                 * aperture to form a beam with, and none is needed: this
                 * aperture lies on the source's own boundary, so every
                 * point beyond it is reachable from somewhere inside. But
                 * only what is BEYOND it -- the cell being entered supplies
                 * a reference point on that side. */
                const vec2_t into_ref = { samples[into].cx, samples[into].cy };
                if (samples[into].n &&
                    !clip_seg_half(&q0, &q1, w.a, sub2(w.b, w.a), into_ref))
                    continue;
            }

            /* Where the surviving aperture sits along this portal. */
            const vec2_t qd = sub2(q->b, q->a);
            const double ql = hypot(qd.x, qd.y);
            if (ql < 1e-6) continue;
            const vec2_t qu = { qd.x / ql, qd.y / ql };
            double t0 = (q0.x - q->a.x) * qu.x + (q0.y - q->a.y) * qu.y;
            double t1 = (q1.x - q->a.x) * qu.x + (q1.y - q->a.y) * qu.y;
            if (t0 > t1) { const double t = t0; t0 = t1; t1 = t; }

            setbit(row, q->cell[(e & 1) ^ 1]);

            /* Continue only when this aperture reaches somewhere none of
             * the earlier ones did. */
            /* How much of the aperture we arrived through was open, which
             * together with this portal's span is what actually determines
             * the beam beyond. Recording only this portal's span let a
             * wider arrival suppress a narrower one whose beam pointed
             * somewhere else entirely. */
            /* WHERE the arriving aperture sits on its portal, not merely
             * how long it is. Recording only the length made every shorter
             * arrival look contained by any longer one regardless of
             * position, so a beam entering through the far end of a portal
             * was pruned by one that had entered through the near end and
             * pointed somewhere else entirely. */
            double w0 = 0.0, w1 = 0.0;
            if (w.has_prev || 1) {
                const portal_t *wp = &portals[w.pside >> 1];
                const vec2_t pd = sub2(wp->b, wp->a);
                const double pl = hypot(pd.x, pd.y);
                if (pl > 1e-9) {
                    const vec2_t pu = { pd.x / pl, pd.y / pl };
                    w0 = (w.a.x - wp->a.x) * pu.x + (w.a.y - wp->a.y) * pu.y;
                    w1 = (w.b.x - wp->a.x) * pu.x + (w.b.y - wp->a.y) * pu.y;
                    if (w0 > w1) { const double t = w0; w0 = w1; w1 = t; }
                }
            }

            /* Dropped only when an already-processed state contains this
             * one in BOTH apertures: same arriving portal, its open span
             * covered, and this portal's span covered. The beam beyond is
             * decided by that pair, so a state contained in both can reach
             * nowhere new.
             *
             * Pruning on this portal's span alone was wrong and cost real
             * routes -- traced on E1M1, that is how subsector 33 went
             * missing from 32. Dropping containment entirely and keeping
             * only exact duplicates is correct but combinatorial: the walk
             * stopped finishing in any useful time. This is the middle that
             * terminates and keeps the routes. */
            int covered = 0;
            for (int k = 0; k < seen[e].n && !covered; k++)
                if (seen[e].prev[k] == w.pside &&
                    w0 >= seen[e].pt0[k] - 1e-4 && w1 <= seen[e].pt1[k] + 1e-4 &&
                    t0 >= seen[e].t0[k] - 1e-4 && t1 <= seen[e].t1[k] + 1e-4)
                    covered = 1;
            if (covered) continue;
            if (seen[e].n < SEEN_MAX) {
                seen[e].prev[seen[e].n] = w.pside;
                seen[e].pt0[seen[e].n] = w0;
                seen[e].pt1[seen[e].n] = w1;
                seen[e].t0[seen[e].n] = t0;
                seen[e].t1[seen[e].n] = t1;
                seen[e].n++;
            }
            /* Out of slots: record nothing and let it through. Widening an
             * existing interval, as this first did, manufactures a spanning
             * range that swallows the gap between two real apertures and
             * prunes a third that falls in it -- the same mistake as the
             * single-interval version, reintroduced by the back door. */

            {
                QGROW(qn);
                if (trace_pushed) trace_pushed[e] = 1;
                queue[qn].pside = e;
                queue[qn].a = q0;
                queue[qn].b = q1;
                queue[qn].pa = w.a;
                queue[qn].pb = w.b;
                queue[qn].has_prev = 1;
                qn++;
            }
        }
    }
    free(seen);
}

/* ---------------------------------------------------------- validation */
/* Locate a point the way the game does: descend the BSP taking the side
 * each partition puts it on. */
static int point_in_ss(double x, double y)
{
    int num = numnodes - 1;
    while (!(num & NF_SUBSECTOR)) {
        if (num >= numnodes) return -1;
        const node_t *nd = &nodes[num];
        const int side = side_of(x, y, nd->x, nd->y, nd->dx, nd->dy) > 0 ? 0 : 1;
        num = nd->child[side];
    }
    const int ss = num & ~NF_SUBSECTOR;
    return ss < numssecs ? ss : -1;
}

/* An independent check on the finished set, sampling visibility a different
 * way than the bake did: fan rays out from interior points in every
 * direction, march each until solid geometry stops it, and require that
 * every cell a ray passes through is one the bitset admits. The bake tests
 * point-to-point between cell pairs; this tests direction-by-direction from
 * one cell, so a systematic hole in the first does not hide in the second.
 *
 * Marching can step over a very thin cell, which costs coverage, never a
 * false alarm: a violation reported here is always a real one. */
static long validate_fan(const uint8_t *pvs, const char *map)
{
    const int   DIRS = 180;
    const double STEP = 8.0;
    const double maxlen = hypot(gmaxx - gminx, gmaxy - gminy);
    long violations = 0, rays = 0;

    for (int a = 0; a < numssecs; a++) {
        const samp_t *s = &samples[a];
        const uint8_t *row = pvs + (size_t)a * subbytes;
        const int nuse = s->n < 6 ? s->n : 6;

        for (int i = 0; i < nuse; i++)
            for (int d = 0; d < DIRS; d++) {
                const double th = d * (2.0 * M_PI / DIRS);
                const double cx = cos(th), cy = sin(th);
                double px = s->x[i], py = s->y[i];
                rays++;

                for (double t = STEP; t < maxlen; t += STEP) {
                    const double qx = s->x[i] + cx * t, qy = s->y[i] + cy * t;
                    if (blocked(px, py, qx, qy)) break;
                    const int ss = point_in_ss(qx, qy);
                    if (ss >= 0 && !bit(row, ss)) {
                        /* The chain diagnostics walk the portal graph,
                         * which only the portal bake builds. */
                        if (++violations <= 3 && portals && cell_portal_head) {
                            /* How far is the target through portals alone,
                             * ignoring every beam? A short hop count means
                             * the graph reaches it easily and the clipping
                             * threw the beam away; a long one means the
                             * chain is genuinely deep. */
                            int *dist = malloc(numssecs * sizeof(int));
                            int *bfs  = malloc(numssecs * sizeof(int));
                            for (int k = 0; k < numssecs; k++) dist[k] = -1;
                            int head = 0, tail = 0;
                            dist[a] = 0; bfs[tail++] = a;
                            while (head < tail) {
                                const int c = bfs[head++];
                                if (c == ss) break;
                                for (int pe = cell_portal_head[c]; pe >= 0;
                                     pe = cell_portal_next[pe]) {
                                    const int nb =
                                        portals[pe >> 1].cell[(pe & 1) ^ 1];
                                    if (dist[nb] < 0) {
                                        dist[nb] = dist[c] + 1;
                                        bfs[tail++] = nb;
                                    }
                                }
                            }
                            printf("  [PVS  ] %s: VIOLATION ss=%d from ss=%d "
                                   "(%d portal hops, ray %.0f,%.0f)\n",
                                   map, ss, a, dist[ss], s->x[i], s->y[i]);

                            /* Walk the shortest portal chain to the target
                             * and apply the beam clip step by step, so the
                             * hop where it collapses names itself. */
                            if (violations == 1 && dist[ss] > 0) {
                                int *par = malloc(numssecs * sizeof(int));
                                for (int k = 0; k < numssecs; k++) par[k] = -1;
                                for (int k = 0; k < numssecs; k++) dist[k] = -1;
                                head = tail = 0; dist[a] = 0; bfs[tail++] = a;
                                while (head < tail) {
                                    const int c = bfs[head++];
                                    for (int pe = cell_portal_head[c]; pe >= 0;
                                         pe = cell_portal_next[pe]) {
                                        const int nb = portals[pe >> 1].cell[(pe & 1) ^ 1];
                                        if (dist[nb] < 0) {
                                            dist[nb] = dist[c] + 1;
                                            par[nb] = pe;
                                            bfs[tail++] = nb;
                                        }
                                    }
                                }
                                int chain[64], n = 0;
                                for (int c = ss; c != a && n < 64; ) {
                                    const int pe = par[c];
                                    if (pe < 0) break;
                                    chain[n++] = pe;
                                    c = portals[pe >> 1].cell[pe & 1];
                                }
                                printf("        chain (source first):\n");
                                vec2_t pa = {0,0}, pb = {0,0};
                                int have_prev = 0;
                                for (int k = n - 1; k >= 0; k--) {
                                    const portal_t *pp = &portals[chain[k] >> 1];
                                    vec2_t c0 = pp->a, c1 = pp->b;
                                    const double full = hypot(c1.x-c0.x, c1.y-c0.y);
                                    int kept = 1;
                                    if (have_prev)
                                        kept = beam_clip_pair(pa, pb, c0, c1, &c0, &c1);
                                    const double left = kept ? hypot(c1.x-c0.x, c1.y-c0.y) : 0.0;
                                    printf("          hop %d portal %d  full %.1f -> %.1f%s\n",
                                           n - k, chain[k] >> 1, full, left,
                                           left < 0.05 ? "   <-- BEAM DIES HERE" : "");
                                    if (!kept) break;
                                    pa = c0; pb = c1; have_prev = 1;
                                }
                                /* Re-run the walk with the queue recorded,
                                 * and say which hop of that clear chain
                                 * never got enqueued. */
                                trace_pushed = calloc((size_t)numportals * 2, 1);
                                uint8_t *tmp = calloc(subbytes, 1);
                                propagate(a, tmp);
                                printf("        enqueued along the chain:\n");
                                for (int k = n - 1; k >= 0; k--) {
                                    const int pe = chain[k];
                                    const int hit = trace_pushed[pe] ||
                                                    trace_pushed[pe ^ 1];
                                    printf("          hop %d portal %d  %s\n",
                                           n - k, pe >> 1,
                                           hit ? "enqueued"
                                               : "NEVER ENQUEUED  <--");
                                    if (!hit) break;
                                }
                                free(tmp);
                                free(trace_pushed);
                                trace_pushed = NULL;
                                free(par);
                            }
                            free(dist); free(bfs);
                        } else violations += 0;
                    }
                    px = qx; py = qy;
                }
            }
    }
    if (violations)
        printf("  [PVS  ] %s: %ld ray-fan violations over %ld rays\n",
               map, violations, rays);
    return violations;
}

/* ------------------------------------------------------------- subtrees */
static uint8_t *node_sub;

static void mark_subtree(int nodenum, uint8_t *out)
{
    if (nodenum & NF_SUBSECTOR) {
        const int ss = nodenum & ~NF_SUBSECTOR;
        if (ss < numssecs) setbit(out, ss);
        return;
    }
    if (nodenum >= numnodes) return;
    mark_subtree(nodes[nodenum].child[0], out);
    mark_subtree(nodes[nodenum].child[1], out);
}

/* --------------------------------------------------------------- main */
extern long pvs_queue_overflow;

int main(int argc, char **argv)
{
    if (argc < 3) DIE("usage: mkpvs <wad> <outdir> [--check]");
    wad_open(argv[1]);
    /* The sampled bake is the default. The portal propagation below it is
     * tighter -- 11% of E1M1 visible against 37% -- but does not yet pass
     * the ray-fan check, so it stays behind a flag until it does. See the
     * note above propagate(). */
    int do_check = 0, sampled = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--check"))  do_check = 1;
        if (!strcmp(argv[i], "--portal")) sampled = 0;
    }
    long total_violations = 0;

    /* One dilation pass along the exact adjacency graph. The samples are
     * now guaranteed interior and the adjacency is exact, so this is not
     * papering over sampling gaps: it covers the camera standing close
     * enough to a boundary that which cell it is "in" is a rounding
     * decision. One hop is that neighbourhood; more would only inflate. */
    const int DILATE_PASSES = 1;

    for (int m = 0; m < numlumps; m++) {
        const char *nm = lumps[m].name;
        if (!(strlen(nm) == 4 && nm[0] == 'E' && nm[2] == 'M')) continue;
        if (!load_level(m)) continue;

        subbytes = (numssecs + 7) / 8;
        const int nodebytes = (numnodes + 7) / 8;

        build_cells();
        verify_cells(nm);
        build_samples();
        grid_build();
        build_adjacency();
        if (!sampled) {
            build_portals();
            long adjpairs = 0;
            for (int a = 0; a < numssecs; a++)
                for (int b = a + 1; b < numssecs; b++)
                    if (bit(adj + (size_t)a * subbytes, b)) adjpairs++;
            printf("  [PVS  ] %s: %d portals from %ld adjacent pairs\n",
                   nm, numportals, adjpairs);
        }

        node_sub = calloc((size_t)numnodes * subbytes, 1);
        for (int i = 0; i < numnodes; i++)
            mark_subtree(i, node_sub + (size_t)i * subbytes);

        uint8_t *pvs = calloc((size_t)numssecs * subbytes, 1);

        /* A degenerate cell -- no polygon, so no interior sample -- cannot
         * be reasoned about. Treat it as seeing everything and as seen by
         * everything: over-reporting costs a little culling, under-reporting
         * would hide it and whatever stands in it. */
        int nosample = 0;
        for (int a = 0; a < numssecs; a++) {
            if (!samples[a].n) {
                nosample++;
                memset(pvs + (size_t)a * subbytes, 0xFF, subbytes);
                for (int b = 0; b < numssecs; b++)
                    setbit(pvs + (size_t)b * subbytes, a);
            }
        }

        if (!sampled) {
            /* Conservative-by-construction: propagate the anti-penumbra
             * from every cell. No dilation follows -- there is nothing to
             * paper over, and adding any would only loosen the set. */
            for (int a = 0; a < numssecs; a++)
                propagate(a, pvs + (size_t)a * subbytes);

            /* Visibility is mutual: if a can see b then b can see a. The
             * propagation can reach one direction and not the other when a
             * cell's own portals are degenerate, so symmetrise. */
            for (int a = 0; a < numssecs; a++)
                for (int b = a + 1; b < numssecs; b++)
                    if (bit(pvs + (size_t)a * subbytes, b) !=
                        bit(pvs + (size_t)b * subbytes, a)) {
                        setbit(pvs + (size_t)a * subbytes, b);
                        setbit(pvs + (size_t)b * subbytes, a);
                    }
            goto emit;
        }

        for (int a = 0; a < numssecs; a++) {
            setbit(pvs + (size_t)a * subbytes, a);
            if (!samples[a].n) continue;
            for (int b = a + 1; b < numssecs; b++) {
                if (!samples[b].n) continue;
                int seen = 0;
                for (int i = 0; i < samples[a].n && !seen; i++)
                    for (int j = 0; j < samples[b].n && !seen; j++)
                        if (!blocked(samples[a].x[i], samples[a].y[i],
                                     samples[b].x[j], samples[b].y[j]))
                            seen = 1;
                if (seen) {
                    setbit(pvs + (size_t)a * subbytes, b);
                    setbit(pvs + (size_t)b * subbytes, a);
                }
            }
        }

        for (int pass = 0; pass < DILATE_PASSES; pass++) {
            uint8_t *wide = malloc((size_t)numssecs * subbytes);
            memcpy(wide, pvs, (size_t)numssecs * subbytes);
            for (int a = 0; a < numssecs; a++) {
                const uint8_t *arow = adj + (size_t)a * subbytes;
                for (int b = 0; b < numssecs; b++) {
                    /* what a neighbour of the source sees */
                    if (bit(arow, b))
                        for (int k = 0; k < subbytes; k++)
                            wide[(size_t)a*subbytes + k] |= pvs[(size_t)b*subbytes + k];
                    /* neighbours of what the source sees */
                    if (bit(pvs + (size_t)a * subbytes, b))
                        for (int k = 0; k < subbytes; k++)
                            wide[(size_t)a*subbytes + k] |= adj[(size_t)b*subbytes + k];
                }
            }
            free(pvs); pvs = wide;
        }

emit:;
        uint8_t *pvsnode = calloc((size_t)numssecs * nodebytes, 1);
        long vis_total = 0;
        for (int a = 0; a < numssecs; a++) {
            const uint8_t *row = pvs + (size_t)a * subbytes;
            for (int b = 0; b < numssecs; b++) if (bit(row, b)) vis_total++;
            for (int nd = 0; nd < numnodes; nd++) {
                const uint8_t *sub = node_sub + (size_t)nd * subbytes;
                for (int k = 0; k < subbytes; k++)
                    if (row[k] & sub[k]) {
                        setbit(pvsnode + (size_t)a * nodebytes, nd);
                        break;
                    }
            }
        }

        if (do_check) total_violations += validate_fan(pvs, nm);

        char path[512];
        snprintf(path, sizeof path, "%s/pvs_%s.bin", argv[2], nm);
        FILE *f = fopen(path, "wb");
        if (!f) DIE("cannot write %s", path);
        const uint8_t hdr[4] = { numssecs >> 8, numssecs & 255,
                                 numnodes >> 8, numnodes & 255 };
        fwrite(hdr, 1, 4, f);
        for (int a = 0; a < numssecs; a++) {
            fwrite(pvs + (size_t)a * subbytes, 1, subbytes, f);
            fwrite(pvsnode + (size_t)a * nodebytes, 1, nodebytes, f);
        }
        fclose(f);

        if (pvs_queue_overflow)
            printf("  [PVS  ] %s: QUEUE OVERFLOW %ld times -- propagation "
                   "truncated\n", nm, pvs_queue_overflow);
        pvs_queue_overflow = 0;
        printf("  [PVS  ] %s: %d subsectors, %.1f%% visible, %ld bytes%s\n",
               nm, numssecs, 100.0 * vis_total / ((double)numssecs * numssecs),
               (long)(4 + (long)numssecs * (subbytes + nodebytes)),
               nosample ? " (some cells had no interior sample)" : "");

        free(pvs); free(pvsnode); free(node_sub); free(adj);
        free(samples); free(cell);
        free(grid_off); free(grid_idx); free(wall_stamp); free(walls);
        if (!sampled) { free(portals); free(cell_portal_head); free(cell_portal_next); }
    }

    if (do_check)
        printf("  [PVS  ] ray-fan check: %ld violations\n", total_violations);
    return total_violations != 0 && do_check ? 1 : 0;
}
