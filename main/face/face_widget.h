#ifndef FACE_WIDGET_H
#define FACE_WIDGET_H

// A procedurally drawn female face for the 240x320 panel.
// The face lives on LVGL's top layer, so it does not depend on the display theme:
// it blinks while idle, widens its eyes while listening and moves its lips while the
// assistant speaks.
void InitializeFace();

#endif  // FACE_WIDGET_H
