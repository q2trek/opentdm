#ifndef G_UNLAG_H
#define G_UNLAG_H

#include <stdbool.h>

typedef struct {
    int frame;
    int hits;
    int estimate_age;
    int age;
    bool rewind;
    bool estimated;
    bool log;
} unlag_shot_t;

#ifdef USE_UNLAG
typedef struct {
    int frame;
    uint64_t life;
    vec3_t origin, mins, maxs;
} unlag_pose_t;

void G_UnlagInit(void);
void G_UnlagShutdown(void);
void G_UnlagClear(void);
void G_UnlagRecord(void);
void G_UnlagNewLife(edict_t *ent);
void G_UnlagCommand(edict_t *ent);
const unlag_pose_t *G_UnlagHistory(edict_t *ent, int frame);
unlag_shot_t G_UnlagBeginShot(edict_t *ent);
void G_UnlagEndShot(edict_t *ent, const unlag_shot_t *shot);
trace_t G_UnlagTrace(unlag_shot_t *shot, edict_t *shooter, vec3_t start,
                     vec3_t end, edict_t *ignore, int mask, vec3_t damage_point);
#else
#define G_UnlagInit() ((void)0)
#define G_UnlagShutdown() ((void)0)
#define G_UnlagClear() ((void)0)
#define G_UnlagRecord() ((void)0)
#define G_UnlagNewLife(ent) ((void)0)
#define G_UnlagCommand(ent) ((void)0)
#define G_UnlagBeginShot(ent) ((unlag_shot_t){ 0 })
#define G_UnlagEndShot(ent, shot) ((void)0)

static inline trace_t G_UnlagTrace(unlag_shot_t *shot, edict_t *shooter, vec3_t start,
                                   vec3_t end, edict_t *ignore, int mask, vec3_t damage_point)
{
    trace_t tr = gi.trace(start, NULL, NULL, end, ignore, mask);
    VectorCopy(tr.endpos, damage_point);
    return tr;
}
#endif

#endif
