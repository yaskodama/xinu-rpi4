// include/softkbd.h — on-screen soft keyboard window.
//
// Renders a QWERTY keyboard inside one wm window.  Input dispatch
// happens later (UI-K1) once a mouse cursor exists to click with;
// for now the window is purely visual.
//
// Layout: five rows × variable columns.  Each key is a rounded
// rectangle with a centred glyph.  Modifier state (caps/shift) is
// tracked internally so the same rendering routine can paint the
// uppercase variant later.

#ifndef XINU_RPI4_SOFTKBD_H
#define XINU_RPI4_SOFTKBD_H

#include "wm.h"

void softkbd_draw(window_t *self, unsigned int frame);
void softkbd_on_click(window_t *self, int lx, int ly);   /* click a key -> type it */

extern window_t softkbd_win;

#endif /* XINU_RPI4_SOFTKBD_H */
