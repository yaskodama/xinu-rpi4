// include/basicwin.h — wm-managed BASIC interpreter window.
//
// A self-contained on-screen BASIC: the interpreter core lives in
// device/video/basic.c (ported from the Pi 3 desktop, decoupled via
// callback seams); this window provides the text ring, the line
// editor (basicwin_handle_key) and the output sink (bw_emit), and
// drives the interpreter one typed line at a time via basic_exec_line().

#ifndef XINU_RPI4_BASICWIN_H
#define XINU_RPI4_BASICWIN_H

#include "wm.h"

/* Up to BASICWIN_N on-screen BASIC windows, each with a FULLY INDEPENDENT
 * interpreter instance (program / variables / text buffer).  Window 0 is the
 * primary `basic_win` wired at boot; the right-click menu's "New BASIC window"
 * builds the rest on demand via basicwin_new().  Must be <= NBASIC in basic.c. */
#define BASICWIN_N 4

extern window_t basic_win;

void basicwin_init(void);                                   /* set seams + greet  */
void basicwin_draw(window_t *self, unsigned int frame);     /* wm draw_content    */
void basicwin_handle_key(char c);                           /* keyboard -> primary REPL */
void basicwin_new(void);                                    /* spawn another BASIC window */
int  basicwin_route_key(window_t *aw, char c);              /* key -> extra BASIC window  */

#endif /* XINU_RPI4_BASICWIN_H */
