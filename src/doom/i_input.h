/* i_input.h — the source fork port (no SDL). */
#ifndef __I_INPUT__
#define __I_INPUT__

#include "doomtype.h"

#define MAX_MOUSE_BUTTONS 8

extern float mouse_acceleration;
extern int   mouse_threshold;

void I_BindInputVariables(void);
void I_ReadMouse(void);

void I_StartTextInput(int x1, int y1, int x2, int y2);
void I_StopTextInput(void);

/* the source fork: controller poll — translates buttons to doom key events. */
void I_PollInput(void);

#endif
