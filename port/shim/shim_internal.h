/* shim_internal.h — shared state between the shim translation units and the
 * port layer. Not visible to the original game sources. */

#ifndef SHIM_INTERNAL_H
#define SHIM_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MacShim.h"

#define SHIM_FOURCC(a,b,c,d) ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (uint32_t)(d))

/* ---------------- asset pack ---------------- */
typedef struct ShimAsset {
	uint32_t fourcc;      /* 'PIX ', 'SND ', 'FORC', 'VERT', 'SPTS', 'CLUT', 'STR#' */
	int32_t  id;
	uint32_t size;
	uint32_t w, h;
	uint32_t flags;       /* bit0 = mask image */
	const uint8_t *data;
} ShimAsset;

int  ShimLoadPack (const char *path);
const ShimAsset *ShimFindAsset (uint32_t fourcc, int id);

/* ---------------- video / present ---------------- */
extern uint8_t  shimPaletteRGBA[16][4];
extern GrafPort *shimCurPort;
extern GrafPort  shimScreenPort;      /* the "main window" */
extern int       shimScreenDirty;

void ShimVideoInit (int w, int h);            /* creates screenBits + screen port */
void ShimPresentIfDirty (void);               /* upload + present (rate-limited) */
void ShimForcePresent (void);

/* implemented by port_video.c (SDL side) */
void PortVideoPresent (const uint8_t *pix, int w, int h);
void PortVideoSetFullscreen (int on);
int  PortVideoIsFullscreen (void);
/* window-normalized touch (0..1) -> logical render coords (0..640, 0..480),
 * undoing the letterbox; 0 if headless/no renderer. */
int  PortVideoTouchToLogical (float nx, float ny, float *lx, float *ly);

/* ---------------- timing / events ---------------- */
void ShimPumpEvents (void);      /* poll SDL, update Ticks/keymap/mouse; may sleep ~0.5ms */
void ShimAdvanceTicks (void);    /* recompute Ticks from wall clock */
int  ShimAnyPadButton (int button);  /* is an SDL_GamepadButton down on any connected pad? */

/* ---------------- input state (written by pump, read by shims) ---------------- */
typedef struct ShimInput {
	Point   virtualMouse;        /* global screen coords, clamped by game's SetMouse */
	int     buttonDown;          /* action: LMB / pad south / X key */
	int     brakeDown;           /* space / pad trigger or east */
	int     bashDown;            /* B,N,M / pad west */
	int     padStart;            /* pad Start (pause) */
	int     keyBits[4];          /* raw Mac keymap words, see shim_input.c */
	int     quitRequested;
	int     padConnected;
	float   padX, padY;          /* -1..1 left stick */
	int     splitInputs;         /* 4P: keep pads out of the merged kb/mouse state */
	/* one-shot raw touch-down (any mode): the menu hit-tests it directly against
	 * its rows, and the pause screen uses it for tap-to-resume / tap-to-end.
	 * tapX/tapY are normalized 0..1; the consumer clears tapFresh. */
	int     tapFresh; float tapX, tapY;
	/* latched Android Back press (SDL_HINT_ANDROID_TRAP_BACK_BUTTON): set on the
	 * key-down event so a momentary press is never missed by state polling.
	 * Consumed by the pause key (enter pause) / the pause loops (a fresh press
	 * ends the game) / the menu (acts like Escape). */
	int     backEdge;
	/* latched on-screen pause-button tap: set on the finger-down event (a quick
	 * tap can fall between the polled-finger snapshots and be missed), consumed
	 * by the pause key and cleared on pause entry. */
	int     pauseTap;
	/* on-screen touch controls (mobile): pressed states + stick deflection for
	 * the overlay drawing, and the pause button (mapped to the pause key) */
	int     mcCatch, mcBrake, mcPause;
	int     mcStickActive; float mcThumbX, mcThumbY;   /* -1..1 */
} ShimInput;

/* 1 on a touch device (Android, or --mobile for testing): show the pause button
 * and, in on-screen mode, the analog stick + action buttons */
extern int shimMobile;
extern ShimInput shimInput;

/* per-pad access for multi-seat play (idx 0..3 = connect order) */
int ShimPadRead (int idx, float *x, float *y, int *btn, int *brake, int *bash);

/* ---------------- sound ---------------- */
void ShimSoundInit (void);
void ShimSoundQuit (void);

/* ---------------- shell hooks ---------------- */
void PortShellHandleEvent (void);   /* the port's idle-mode UI (HandleEvent) */

/* ---------------- debug / headless ---------------- */
extern int   shimHeadless;
extern long  shimAutoQuitTicks;     /* >0: quit after this many ticks (testing) */
extern const char *shimFrameDumpDir;/* non-NULL: dump screen PPM every N ticks */
extern int   shimFrameDumpEvery;
void ShimMaybeDumpFrame (void);
void ShimLog (const char *fmt, ...);

#endif
