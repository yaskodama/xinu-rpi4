// include/actor.h — minimal actor + mailbox layer for xinu-rpi5.
//
// This is the delivery target for remote messages arriving over HTTP
// (system/tcp_server.c routes GET /send?to=N&m=method&arg=X here).  It
// is deliberately a small, self-contained object table so that the
// future AIPL->C codegen port (c_translator.ml --xinu retargeted for
// xinu-rpi5) can grow into it: each actor is an object with a handful
// of integer fields and a name, and a message is (method, arg) applied
// to it.  For now delivery is synchronous (actor_message runs the
// handler inline); a real mailbox + actor process can be layered on
// later without changing this surface.

#ifndef XINU_RPI5_ACTOR_H
#define XINU_RPI5_ACTOR_H

#define ACTOR_MAX     8
#define ACTOR_FIELDS  4
#define ACTOR_NAMELEN 16

/* Register the built-in demo actors.  Call once at boot. */
void actor_init(void);

/* Deliver one message to actor `id`: apply `method` (a short name like
 * "bump"/"get"/"set"/"add") with integer `arg`.  Writes the resulting
 * value (e.g. the counter's new count) to *out.  Returns 0 on success,
 * -1 if the actor id is invalid or the method is unknown. */
int actor_message(int id, const char *method, int arg, int *out);

/* Introspection for the HTTP /api/actors endpoint and the shell. */
int          actor_count(void);
const char  *actor_name(int id);          /* 0 if id invalid */
int          actor_field(int id, int idx); /* field value, 0 if invalid */
unsigned long actor_msg_count(void);       /* total messages handled */

#endif /* XINU_RPI5_ACTOR_H */
