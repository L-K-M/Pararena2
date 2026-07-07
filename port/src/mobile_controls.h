/* mobile_controls.h — layout of the on-screen touch controls (640x480 screen
 * coords). Shared by the input hit-testing (shim_input.c) and the on-screen
 * drawing (port_shell.c). The pause button is always shown on a touch device;
 * the stick + action buttons only in the "on-screen" control mode. */
#ifndef MOBILE_CONTROLS_H
#define MOBILE_CONTROLS_H

/* analog stick (bottom-left): a fixed base ring with a thumb that follows the
 * finger; deflection maps to the same skate vector the swipe joystick uses. */
#define MC_STICK_CX   86
#define MC_STICK_CY   386
#define MC_STICK_R    54     /* base radius */
#define MC_STICK_TR   26     /* thumb radius (max travel = R - TR) */

/* action buttons (bottom-right) */
#define MC_CATCH_CX   566    /* catch / throw / crouch */
#define MC_CATCH_CY   398
#define MC_CATCH_R    42
#define MC_BRAKE_CX   596    /* brake */
#define MC_BRAKE_CY   316
#define MC_BRAKE_R    30

/* pause button (top-centre, clear of the corner scoreboards/chips) */
#define MC_PAUSE_CX   320
#define MC_PAUSE_CY   24
#define MC_PAUSE_R    20

/* true if normalized touch (nx,ny in 0..1) is within radius r of (cx,cy) */
#define MC_HIT(nx, ny, cx, cy, r) \
	(((nx) * 640 - (cx)) * ((nx) * 640 - (cx)) + \
	 ((ny) * 480 - (cy)) * ((ny) * 480 - (cy)) <= (r) * (r))

#endif
