#ifndef __UMAPINFO__
#define __UMAPINFO__

#include "doomtype.h"

#define UMAPINFO_MAX_EPISODES 8
#define UMAPINFO_MAX_MAPS 64

typedef struct
{
    int episode;
    char patch[9];
    char name[64];
    char key;
} umapinfo_episode_t;

typedef struct
{
    char lump[9];
    int episode;
    int map;
    char level_name[64];
    char level_pic[9];
    char next[9];
    char next_secret[9];
    char music[9];
    char sky_texture[9];
    char exit_pic[9];
    char end_pic[9];
    char inter_text[768];
    int partime;
} umapinfo_map_t;

void UMAPINFO_LoadLumps(void);

const umapinfo_map_t *UMAPINFO_GetMap(int episode, int map);
const umapinfo_episode_t *UMAPINFO_GetEpisode(int episode);

boolean UMAPINFO_HasEpisode(int episode);
int UMAPINFO_MaxEpisode(void);
boolean UMAPINFO_ResolveMap(const char *marker, int *episode, int *map);
boolean UMAPINFO_GetNextMap(int episode, int map, boolean secret,
                            int *next_episode, int *next_map);

const char *UMAPINFO_LevelName(int episode, int map);
const char *UMAPINFO_LevelPic(int episode, int map);
const char *UMAPINFO_Music(int episode, int map);
const char *UMAPINFO_SkyTexture(int episode, int map);
const char *UMAPINFO_ExitPic(int episode, int map);
const char *UMAPINFO_EndPic(int episode, int map);
const char *UMAPINFO_InterText(int episode, int map);
int UMAPINFO_ParTime(int episode, int map);

#endif
