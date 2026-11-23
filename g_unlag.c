#include "g_local.h"

/* 250 ms at the largest server rate, plus the write slot and one spare. */
enum { HISTORY_COUNT = 2 + 250 * UNLAG_MAX_FPS / 1000 };

typedef struct {
    uint64_t life;
    uint64_t command_life;
    int life_frame;
    int command_frame;
    unlag_pose_t poses[HISTORY_COUNT];
} unlag_slot_t;

static unlag_slot_t *slots;
static cvar_t *g_unlag, *g_unlag_max_ms;

static unlag_slot_t *UnlagSlot(edict_t *ent)
{
    return &slots[ent - g_edicts - 1];
}

static bool UnlagLivePlayer(const edict_t *ent)
{
    return ent->inuse && ent->client && UNLAG_ACTIVE_PLAYER(ent) &&
           ent->health > 0 && !ent->deadflag && ent->solid == SOLID_BBOX;
}

void G_UnlagInit(void)
{
    g_unlag = gi.cvar("g_unlag", "0", 0);
    g_unlag_max_ms = gi.cvar("g_unlag_max_ms", "200", 0);
    if (!unlag_api)
        return;
    slots = gi.TagMalloc(game.maxclients * sizeof(*slots), TAG_GAME);
}

void G_UnlagShutdown(void)
{
    slots = NULL;
}

void G_UnlagNewLife(edict_t *ent)
{
    if (slots) {
        unlag_slot_t *slot = UnlagSlot(ent);
        slot->life++;
        slot->life_frame = unlag_api->WorldFrame();
    }
}

void G_UnlagClear(void)
{
    if (!slots)
        return;
    for (int i = 0; i < game.maxclients; i++) {
        memset(slots[i].poses, 0, sizeof(slots[i].poses));
        slots[i].command_life = 0;
    }
}

void G_UnlagCommand(edict_t *ent)
{
    if (!slots)
        return;
    unlag_command_t context = unlag_api->Command(ent);
    if (context.kind == UNLAG_REPEAT)
        return;
    unlag_slot_t *slot = UnlagSlot(ent);
    slot->command_life = context.kind == UNLAG_ESTIMATE ? slot->life : 0;
    slot->command_frame = context.frame;
}

void G_UnlagRecord(void)
{
    if (!slots)
        return;
    int frame = unlag_api->WorldFrame();
    for (int i = 0; i < game.maxclients; i++) {
        edict_t *ent = &g_edicts[i + 1];
        if (!UnlagLivePlayer(ent))
            continue;
        unlag_pose_t *pose = &slots[i].poses[frame % HISTORY_COUNT];
        pose->frame = frame;
        pose->life = slots[i].life;
        VectorCopy(ent->s.origin, pose->origin);
        VectorCopy(ent->mins, pose->mins);
        VectorCopy(ent->maxs, pose->maxs);
    }
}

const unlag_pose_t *G_UnlagHistory(edict_t *ent, int frame)
{
    if (!slots || frame < 0 || !UnlagLivePlayer(ent))
        return NULL;
    const unlag_slot_t *slot = UnlagSlot(ent);
    const unlag_pose_t *pose = &slot->poses[frame % HISTORY_COUNT];
    return pose->life && pose->life == slot->life && pose->frame == frame ? pose : NULL;
}

unlag_shot_t G_UnlagBeginShot(edict_t *ent)
{
    unlag_shot_t shot = { .log = g_unlag->value == 2 };
    if ((!shot.log && g_unlag->value != 1) || !slots || !unlag_game_ex->clip)
        return shot;

    const unlag_slot_t *slot = UnlagSlot(ent);
    if (!slot->command_life || slot->command_life != slot->life)
        return shot;
    int frame = unlag_api->WorldFrame();
    if (slot->command_frame < slot->life_frame || slot->command_frame > frame)
        return shot;

    float ms = min(max(g_unlag_max_ms->value, 0.0f), 250.0f);
    int limit = (double)ms * unlag_api->FrameRate() / 1000;
    shot.estimated = true;
    shot.estimate_age = frame - slot->command_frame;
    shot.age = min(shot.estimate_age, limit);
    shot.frame = frame - shot.age;
    return shot;
}

void G_UnlagEndShot(edict_t *ent, const unlag_shot_t *shot)
{
    if (!shot->log)
        return;
    int slot = ent - g_edicts - 1;
    if (shot->estimated)
        gi.dprintf("unlag slot=%d estimate_age=%d age=%d estimated=1 hits=%d ping=%d\n",
                   slot, shot->estimate_age, shot->age, shot->hits, ent->client->ping);
    else
        gi.dprintf("unlag slot=%d estimate_age= age= estimated=0 hits=0 ping=%d\n",
                   slot, ent->client->ping);
}

typedef struct {
    edict_t *ent;
    const unlag_pose_t *pose;
} unlag_target_t;

static int UnlagHideTargets(unlag_target_t *targets, const edict_t *shooter, const edict_t *ignore, int frame)
{
    int count = 0;
    for (int i = 1; i <= game.maxclients; i++) {
        edict_t *ent = &g_edicts[i];
        if (ent == shooter || ent == ignore || (ignore && (ent->owner == ignore || ignore->owner == ent)))
            continue;

        const unlag_pose_t *pose = G_UnlagHistory(ent, frame);
        if (!pose || (VectorCompare(pose->origin, ent->s.origin) &&
                      VectorCompare(pose->mins, ent->mins) && VectorCompare(pose->maxs, ent->maxs)))
            continue;

        targets[count++] = (unlag_target_t){ ent, pose };
        ent->solid = SOLID_NOT;
    }
    return count;
}

static bool UnlagMayIntersect(const unlag_pose_t *pose, const vec3_t start, const vec3_t end)
{
    /* Use the engine's local coordinates, with padding larger than its trace epsilon. */
    const float pad = 0.125f;
    float enter = 0, leave = 1;
    for (int i = 0; i < 3; i++) {
        float s = start[i] - pose->origin[i], e = end[i] - pose->origin[i];
        for (int side = 0; side < 2; side++) {
            float a = side ? s - pose->maxs[i] : pose->mins[i] - s;
            float b = side ? e - pose->maxs[i] : pose->mins[i] - e;
            if (!isfinite(a) || !isfinite(b))
                return true;
            if (a > pad && b > pad)
                return false;
            if (a > pad || b > pad) {
                float span = a - b;
                if (!isfinite(span))
                    return true;
                float fraction = (a - pad) / span;
                if (a > pad)
                    enter = max(enter, fraction);
                else
                    leave = min(leave, fraction);
                if (enter > leave)
                    return false;
            }
        }
    }
    return true;
}

static const unlag_pose_t *UnlagClipTargets(trace_t *result, const unlag_target_t *targets, int count,
                                           const vec3_t start, const vec3_t end, int mask)
{
    if (result->fraction == 0 && result->ent == world)
        return NULL;

    edict_t box = { .solid = SOLID_BBOX };
    const unlag_pose_t *hit = NULL;
    for (int i = 0; i < count && !result->allsolid; i++) {
        const unlag_pose_t *pose = targets[i].pose;
        if (!UnlagMayIntersect(pose, start, end))
            continue;
        VectorCopy(pose->origin, box.s.origin);
        VectorCopy(pose->mins, box.mins);
        VectorCopy(pose->maxs, box.maxs);
        trace_t clip = unlag_game_ex->clip(start, NULL, NULL, end, &box, mask);

        /* Same merge as CM_ClipEntity. The current result wins ties. */
        clip.allsolid |= result->allsolid;
        clip.startsolid |= result->startsolid;
        if (clip.fraction < result->fraction) {
            clip.ent = targets[i].ent;
            *result = clip;
            hit = pose;
        } else {
            result->allsolid = clip.allsolid;
            result->startsolid = clip.startsolid;
        }
    }
    return hit;
}

static void UnlagDamagePoint(vec3_t point, const edict_t *ent, const unlag_pose_t *pose)
{
    for (int i = 0; i < 3; i++) {
        point[i] += ent->s.origin[i] - pose->origin[i];
        if (ent->mins[i] != pose->mins[i])
            point[i] = max(point[i], ent->s.origin[i] + ent->mins[i]);
        if (ent->maxs[i] != pose->maxs[i])
            point[i] = min(point[i], ent->s.origin[i] + ent->maxs[i]);
    }
}

trace_t G_UnlagTrace(unlag_shot_t *shot, edict_t *shooter, vec3_t start,
                     vec3_t end, edict_t *ignore, int mask, vec3_t damage_point)
{
    if (shot->age <= 0) {
        trace_t tr = gi.trace(start, NULL, NULL, end, ignore, mask);
        VectorCopy(tr.endpos, damage_point);
        return tr;
    }

    unlag_target_t targets[MAX_CLIENTS];
    int count = UnlagHideTargets(targets, shooter, ignore, shot->frame);
    trace_t tr = gi.trace(start, NULL, NULL, end, ignore, mask);
    for (int i = 0; i < count; i++)
        targets[i].ent->solid = SOLID_BBOX;

    const unlag_pose_t *hit = UnlagClipTargets(&tr, targets, count, start, end, mask);
    VectorCopy(tr.endpos, damage_point);
    if (hit) {
        UnlagDamagePoint(damage_point, tr.ent, hit);
        shot->hits++;
    }
    return tr;
}

trace_t G_UnlagRailTrace(unlag_shot_t *shot, unlag_rail_t *rail, edict_t *shooter,
                         vec3_t start, vec3_t end, edict_t *ignore, int mask, vec3_t damage_point)
{
    if (shot->age <= 0)
        return G_UnlagTrace(shot, shooter, start, end, ignore, mask, damage_point);

    if (ignore != shooter && (!rail->count || rail->passed[rail->count - 1].ent != ignore))
        rail->passed[rail->count++].ent = ignore;

    /* A continued ray can still start in front of any of the earlier entry planes. */
    for (int i = 0; i < rail->count; i++) {
        edict_t *ent = rail->passed[i].ent;
        rail->passed[i].solid = ent->solid;
        ent->solid = SOLID_NOT;
    }
    trace_t tr = G_UnlagTrace(shot, shooter, start, end, ignore, mask, damage_point);
    for (int i = 0; i < rail->count; i++)
        rail->passed[i].ent->solid = rail->passed[i].solid;
    return tr;
}
