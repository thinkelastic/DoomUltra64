/* Precomputed visibility. See r_pvs.c. R_PVS: 0 off, 1 prune, 2 validate. */
#ifndef R_PVS_H
#define R_PVS_H

#include <stdbool.h>

#ifndef R_PVS
#define R_PVS 1
#endif

#if R_PVS
void r_pvs_reset(void);
void r_pvs_load(int episode, int map);
void r_pvs_frame(float x, float y);
bool r_pvs_active(void);
bool r_pvs_ss_visible(int ssnum);
bool r_pvs_node_visible(int nodenum);
void r_pvs_check_drawn(int ssnum);
extern int r_pvs_violations;
#else
static inline void r_pvs_reset(void) { }
static inline void r_pvs_load(int e, int m) { (void)e; (void)m; }
static inline void r_pvs_frame(float x, float y) { (void)x; (void)y; }
static inline bool r_pvs_ss_visible(int s) { (void)s; return true; }
static inline bool r_pvs_node_visible(int n) { (void)n; return true; }
static inline void r_pvs_check_drawn(int s) { (void)s; }
#endif

#endif
