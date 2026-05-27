// system/actor.c — minimal actor + (synchronous) message delivery.
//
// See include/actor.h.  Two demo actors are created at boot:
//   id 0  "counter"  — bump:+1, add:+arg, set:=arg, get/reset
//   id 1  "store"    — set:=arg, add:+arg, get/reset
// Both expose field[0] as their primary value.  HTTP requests of the
// form GET /send?to=<id>&m=<method>&arg=<n> land here via tcp_server.c.

#include "actor.h"

struct actor {
    int  used;
    char name[ACTOR_NAMELEN];
    int  field[ACTOR_FIELDS];
};

static struct actor   g_actors[ACTOR_MAX];
static int            g_nactors;
static unsigned long  g_msgs;

static void set_name(char *dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < ACTOR_NAMELEN - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int actor_new(const char *name)
{
    if (g_nactors >= ACTOR_MAX) return -1;
    int id = g_nactors++;
    g_actors[id].used = 1;
    set_name(g_actors[id].name, name);
    for (int i = 0; i < ACTOR_FIELDS; i++) g_actors[id].field[i] = 0;
    return id;
}

void actor_init(void)
{
    g_nactors = 0;
    g_msgs    = 0;
    for (int i = 0; i < ACTOR_MAX; i++) g_actors[i].used = 0;
    actor_new("counter");   /* id 0 */
    actor_new("store");     /* id 1 */
}

int actor_message(int id, const char *method, int arg, int *out)
{
    int dummy;
    if (!out) out = &dummy;
    if (id < 0 || id >= g_nactors || !g_actors[id].used) return -1;

    struct actor *a = &g_actors[id];

    /* Method set shared by both demo actors; primary value is field[0]. */
    if (str_eq(method, "bump")) {
        a->field[0] += 1;
    } else if (str_eq(method, "add")) {
        a->field[0] += arg;
    } else if (str_eq(method, "set")) {
        a->field[0] = arg;
    } else if (str_eq(method, "reset")) {
        a->field[0] = 0;
    } else if (str_eq(method, "get")) {
        /* no-op; just read back below */
    } else {
        return -1;   /* unknown method */
    }

    a->field[1] += 1;          /* per-actor message count in field[1] */
    g_msgs++;
    *out = a->field[0];
    return 0;
}

int           actor_count(void)          { return g_nactors; }
unsigned long actor_msg_count(void)      { return g_msgs;    }

const char *actor_name(int id)
{
    if (id < 0 || id >= g_nactors || !g_actors[id].used) return 0;
    return g_actors[id].name;
}

int actor_field(int id, int idx)
{
    if (id < 0 || id >= g_nactors || !g_actors[id].used) return 0;
    if (idx < 0 || idx >= ACTOR_FIELDS) return 0;
    return g_actors[id].field[idx];
}
