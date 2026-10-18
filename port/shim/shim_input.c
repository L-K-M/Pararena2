/* shim_input.c — the virtual analog stick and Mac keymap synthesis.
 *
 * The original reads the mouse's deflection from screen center as a 2-axis
 * force, warping the pointer back into a clamp box each frame (SetMouse).
 * Here a *virtual* cursor provides that deflection, fed by whichever source
 * is active: relative mouse motion, gamepad left stick, or ramped arrow/WASD
 * keys. The game's own clamp logic (ForcePointInRect + SetMouse) runs
 * unchanged against it.
 *
 * Keyboard bindings (original in parentheses):
 *   move: mouse / left stick / arrows / WASD    (mouse)
 *   catch/throw/crouch: LMB / pad South / X     (mouse button)
 *   brake: Space / pad East / right trigger     (Space)
 *   bash: B,N,M / pad West                      (B,N,M)
 *   pause: Tab / pad Start                      (Tab)
 *   replay: R / pad North                       (R)
 *   sound toggle: S                             (S)
 *   end game: Esc or Ctrl+E                     (Cmd+E)
 *   quit: Ctrl+Q / window close                 (Cmd+Q)
 *   fullscreen: F11 or Alt+Enter
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "mobile_controls.h"

ShimInput shimInput;
extern int mobileOnScreen;            /* Options toggle (port_shell.c) */

static SDL_Gamepad *pad;              /* first pad (merged single-player input) */
static SDL_Gamepad *padList[4];       /* all pads by connect order (4P seats) */
static int inPlayMode;
static float keyAxisX, keyAxisY;      /* ramped keyboard deflection -1..1 */
static Uint64 lastPresentNS;

/* ---- touchscreen (Android) virtual controls ----
 * Play mode: the left half is a relative "skate" joystick (drag from wherever
 * the finger lands), the right half is CATCH/THROW (upper) and BRAKE (lower).
 * Menu mode: the raw tap is recorded and HandleEvent hit-tests it against the
 * on-screen menu rows. SDL's touch->mouse synthesis is disabled (a hint in
 * port_main.c) so this is the sole owner of the deflection while touching. */
#define TOUCH_MAX 8
#define TOUCH_RADIUS 0.16f            /* normalized drag distance for full tilt */
static SDL_FingerID tfId[TOUCH_MAX];  /* button fingers (right half) */
static int          tfKind[TOUCH_MAX];/* 1 = catch, 2 = brake */
static int          tfN;
static int          moveOn;           /* a movement finger is down (left half) */
static SDL_FingerID moveId;
static float        moveAnchorX, moveAnchorY, moveDefX, moveDefY;

extern void ShimTickSleep (void);
extern void ShimTimeInit (void);

#define DEFLECT_HALF 214              /* mouseFrame half-width, large arena */

static void setKeyBit (unsigned char *km, int bit, int on)
{
	if (on)
		km[bit >> 3] |= (unsigned char)(0x80 >> (bit & 7));
}

/* Mac keymap bit numbers from Headers/UnivUtilities.h */
enum { kZ = 1, kS = 6, kR = 8, kE = 9, kQ = 11, kB = 12, kPeriod = 40,
       kM = 41, kN = 42, kCommand = 48, kSpace = 54, kTab = 55, kOption = 61 };

void PortInputSetPlayMode (int playing)
{
	inPlayMode = playing;
	if (!shimHeadless)
	{
		SDL_Window *w = SDL_GetKeyboardFocus();
		if (w)
			SDL_SetWindowRelativeMouseMode(w, playing ? true : false);
	}
	shimInput.virtualMouse.h = screenBits.bounds.right / 2;
	shimInput.virtualMouse.v = screenBits.bounds.bottom / 2;
	keyAxisX = keyAxisY = 0;
	moveOn = 0; tfN = 0; moveDefX = moveDefY = 0;
	/* a tap or pause latch from the mode we're leaving must not fire into the
	 * one we're entering (e.g. the last in-game tap clicking a menu row) */
	shimInput.tapFresh = 0;
	shimInput.pauseTap = 0;
}

int ShimInPlayMode (void)
{
	return inPlayMode;
}

/* SDL reports finger coords normalized to the whole window; on a phone the 4:3
 * image is letterboxed inside a wider window, so convert to content-normalized
 * (0..1 across the visible 640x480 image) before any hit-test. Identity on a
 * 4:3 desktop window (and headless), so --mobile testing is unaffected. */
static void touchToContent (float wx, float wy, float *cx, float *cy)
{
	float lx, ly;
	if (PortVideoTouchToLogical(wx, wy, &lx, &ly))
	{
		*cx = lx / 640.0f;
		*cy = ly / 480.0f;
	}
	else
	{
		*cx = wx; *cy = wy;                    /* no renderer: nothing to undo */
	}
}

static void touchDown (SDL_FingerID id, float x, float y)
{
	touchToContent(x, y, &x, &y);              /* window-normalized -> content-normalized */

	/* record the raw tap (any mode): the menu hit-tests it against its rows and
	 * the pause screen uses it for tap-to-resume / tap-to-end */
	shimInput.tapFresh = 1; shimInput.tapX = x; shimInput.tapY = y;

	if (!inPlayMode)
		return;                                /* menu taps handled in HandleEvent */

	/* during play, the pause button and the on-screen pad are handled by polling,
	 * not the swipe machinery — keep their fingers out of it. The pause button is
	 * also latched here on the down-event so a quick tap can't slip between the
	 * finger-state polls and be lost (the reported "tapping pause does nothing"). */
	if (shimMobile && MC_HIT(x, y, MC_PAUSE_CX, MC_PAUSE_CY, MC_PAUSE_R + 8))
	{
		shimInput.pauseTap = 1;
		return;
	}
	if (shimMobile && mobileOnScreen)
		return;

	if (x < 0.5f)                              /* left half: movement joystick */
	{
		if (!moveOn) { moveOn = 1; moveId = id; moveAnchorX = x; moveAnchorY = y; moveDefX = moveDefY = 0; }
		return;
	}
	if (tfN < TOUCH_MAX)                        /* right half: a button finger */
	{
		tfId[tfN] = id;
		tfKind[tfN] = (y < 0.5f) ? 1 : 2;      /* upper = catch, lower = brake */
		tfN++;
	}
}

static void touchMotion (SDL_FingerID id, float x, float y)
{
	touchToContent(x, y, &x, &y);              /* window-normalized -> content-normalized */
	if (moveOn && id == moveId)
	{
		moveDefX = (x - moveAnchorX) / TOUCH_RADIUS;
		moveDefY = (y - moveAnchorY) / TOUCH_RADIUS;
		if (moveDefX >  1) moveDefX =  1;
		if (moveDefX < -1) moveDefX = -1;
		if (moveDefY >  1) moveDefY =  1;
		if (moveDefY < -1) moveDefY = -1;
	}
}

static void touchUp (SDL_FingerID id)
{
	if (moveOn && id == moveId) { moveOn = 0; moveDefX = moveDefY = 0; return; }
	for (int i = 0; i < tfN; i++)
		if (tfId[i] == id)
		{
			tfId[i] = tfId[tfN - 1];
			tfKind[i] = tfKind[tfN - 1];
			tfN--;
			break;
		}
}

static void handleEvent (const SDL_Event *ev)
{
	switch (ev->type)
	{
		case SDL_EVENT_FINGER_DOWN:
			touchDown(ev->tfinger.fingerID, ev->tfinger.x, ev->tfinger.y);
			break;
		case SDL_EVENT_FINGER_MOTION:
			touchMotion(ev->tfinger.fingerID, ev->tfinger.x, ev->tfinger.y);
			break;
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
			touchUp(ev->tfinger.fingerID);
			break;
		case SDL_EVENT_QUIT:
			shimInput.quitRequested = 1;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			if (inPlayMode)
			{
				shimInput.virtualMouse.h += (short)ev->motion.xrel;
				shimInput.virtualMouse.v += (short)ev->motion.yrel;
			}
			else
			{
				shimInput.virtualMouse.h = (short)ev->motion.x;
				shimInput.virtualMouse.v = (short)ev->motion.y;
			}
			break;
		case SDL_EVENT_GAMEPAD_ADDED:
			for (int i = 0; i < 4; i++)
			{
				if (padList[i])
					continue;
				padList[i] = SDL_OpenGamepad(ev->gdevice.which);
				if (padList[i])
					ShimLog("gamepad %d connected: %s", i + 1, SDL_GetGamepadName(padList[i]));
				break;
			}
			pad = padList[0];
			shimInput.padConnected = (pad != NULL);
			/* the side panels reflect pad presence; repaint even if the
			 * 8bpp screen itself hasn't changed */
			shimScreenDirty = 1;
			lastPresentNS = 0;
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			for (int i = 0; i < 4; i++)
			{
				if (padList[i] && ev->gdevice.which == SDL_GetGamepadID(padList[i]))
				{
					SDL_CloseGamepad(padList[i]);
					padList[i] = NULL;
				}
			}
			pad = padList[0];
			shimInput.padConnected = (pad != NULL);
			shimScreenDirty = 1;      /* repaint the side panels here too */
			lastPresentNS = 0;
			break;
		case SDL_EVENT_KEY_DOWN:
			if (ev->key.key == SDLK_F11 ||
			    (ev->key.key == SDLK_RETURN && (ev->key.mod & SDL_KMOD_ALT)))
				PortVideoSetFullscreen(!PortVideoIsFullscreen());
			/* Android Back (trapped as a key): latch it here so a momentary
			 * press can't slip between GetKeys' keyboard-state snapshots */
			if (ev->key.key == SDLK_AC_BACK || ev->key.scancode == SDL_SCANCODE_AC_BACK)
				shimInput.backEdge = 1;
			break;
		case SDL_EVENT_WINDOW_EXPOSED:
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			/* the compositor may not retain our contents; repaint even if the
			 * 8bpp screen hasn't changed (idle splash draws nothing for long
			 * stretches), bypassing the present throttle */
			shimScreenDirty = 1;
			lastPresentNS = 0;
			break;
		default:
			break;
	}
}

void ShimPumpEvents (void)
{
	static int pumping;
	if (pumping)
		return;
	pumping = 1;

	SDL_Event ev;
	while (SDL_PollEvent(&ev))
		handleEvent(&ev);

	/* keyboard state */
	const bool *ks = SDL_GetKeyboardState(NULL);

	/* ramped keyboard steering (arrows only — S/B/N/M/R/E are game keys) */
	float tx = (float)(ks[SDL_SCANCODE_RIGHT] ? 1 : 0) - (float)(ks[SDL_SCANCODE_LEFT] ? 1 : 0);
	float ty = (float)(ks[SDL_SCANCODE_DOWN] ? 1 : 0) - (float)(ks[SDL_SCANCODE_UP] ? 1 : 0);
	float rate = 0.15f;
	keyAxisX += (tx - keyAxisX) * rate;
	keyAxisY += (ty - keyAxisY) * rate;

	/* gamepad stick (absolute deflection); in split-input (4P) mode the pads
	 * belong to their own seats and stay out of the merged state */
	float px = 0, py = 0;
	int padActive = 0;
	if (pad && !shimInput.splitInputs)
	{
		px = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
		py = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  px = -1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) px = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))    py = -1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  py = 1;
		if (px > 0.12f || px < -0.12f || py > 0.12f || py < -0.12f)
			padActive = 1;
		shimInput.padX = px;
		shimInput.padY = py;
	}

	int touchActive = moveOn && (moveDefX > 0.06f || moveDefX < -0.06f ||
	                             moveDefY > 0.06f || moveDefY < -0.06f);

	/* mobile on-screen controls + pause button, polled from all active fingers.
	 * Only while actually playing: the widgets are drawn (and meaningful) only
	 * then, and this keeps menu taps from registering as catch/brake/pause. */
	shimInput.mcCatch = shimInput.mcBrake = shimInput.mcBash = shimInput.mcPause = 0;
	shimInput.mcStickActive = 0;
	if (shimMobile && inPlayMode)
	{
		float mdx = 0, mdy = 0; int stick = 0, ndev = 0;
		SDL_TouchID *devs = SDL_GetTouchDevices(&ndev);
		for (int di = 0; di < ndev; di++)
		{
			int nf = 0;
			SDL_Finger **fs = SDL_GetTouchFingers(devs ? devs[di] : 0, &nf);
			for (int fi = 0; fs && fi < nf; fi++)
			{
				float fx, fy;
				touchToContent(fs[fi]->x, fs[fi]->y, &fx, &fy);
				if (MC_HIT(fx, fy, MC_PAUSE_CX, MC_PAUSE_CY, MC_PAUSE_R + 8)) { shimInput.mcPause = 1; continue; }
				if (!mobileOnScreen) continue;
				int sxp = (int)(fx * 640), syp = (int)(fy * 480);
				if (MC_HIT(fx, fy, MC_STICK_CX, MC_STICK_CY, MC_STICK_R + 20))
				{
					int travel = MC_STICK_R - MC_STICK_TR;
					mdx = (float)(sxp - MC_STICK_CX) / travel;
					mdy = (float)(syp - MC_STICK_CY) / travel;
					if (mdx >  1) mdx =  1; if (mdx < -1) mdx = -1;
					if (mdy >  1) mdy =  1; if (mdy < -1) mdy = -1;
					stick = 1;
				}
				else if (MC_HIT(fx, fy, MC_CATCH_CX, MC_CATCH_CY, MC_CATCH_R + 8)) shimInput.mcCatch = 1;
				else if (MC_HIT(fx, fy, MC_BRAKE_CX, MC_BRAKE_CY, MC_BRAKE_R + 8)) shimInput.mcBrake = 1;
				else if (MC_HIT(fx, fy, MC_BASH_CX, MC_BASH_CY, MC_BASH_R + 8)) shimInput.mcBash = 1;
			}
			SDL_free(fs);
		}
		SDL_free(devs);
		shimInput.mcStickActive = stick;
		shimInput.mcThumbX = stick ? mdx : 0;
		shimInput.mcThumbY = stick ? mdy : 0;
	}

	if (inPlayMode)
	{
		int cx = screenBits.bounds.right / 2;
		int cy = screenBits.bounds.bottom / 2;
		if (padActive)
		{
			shimInput.virtualMouse.h = (short)(cx + (int)(px * DEFLECT_HALF));
			shimInput.virtualMouse.v = (short)(cy + (int)(py * DEFLECT_HALF));
		}
		else if (shimInput.mcStickActive)
		{
			shimInput.virtualMouse.h = (short)(cx + (int)(shimInput.mcThumbX * DEFLECT_HALF));
			shimInput.virtualMouse.v = (short)(cy + (int)(shimInput.mcThumbY * DEFLECT_HALF));
		}
		else if (touchActive)
		{
			shimInput.virtualMouse.h = (short)(cx + (int)(moveDefX * DEFLECT_HALF));
			shimInput.virtualMouse.v = (short)(cy + (int)(moveDefY * DEFLECT_HALF));
		}
		else if (keyAxisX > 0.02f || keyAxisX < -0.02f || keyAxisY > 0.02f || keyAxisY < -0.02f)
		{
			shimInput.virtualMouse.h = (short)(cx + (int)(keyAxisX * DEFLECT_HALF));
			shimInput.virtualMouse.v = (short)(cy + (int)(keyAxisY * DEFLECT_HALF));
		}
	}

	/* buttons */
	shimInput.buttonDown = 0;
	shimInput.brakeDown = 0;
	shimInput.bashDown = 0;
	shimInput.padStart = 0;
	if ((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0) shimInput.buttonDown = 1;
	if (ks[SDL_SCANCODE_X]) shimInput.buttonDown = 1;
	if (ks[SDL_SCANCODE_SPACE]) shimInput.brakeDown = 1;
	if (ks[SDL_SCANCODE_B] || ks[SDL_SCANCODE_N] || ks[SDL_SCANCODE_M]) shimInput.bashDown = 1;
	if (pad && !shimInput.splitInputs)
	{
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) shimInput.buttonDown = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST)) shimInput.brakeDown = 1;
		if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) shimInput.brakeDown = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST)) shimInput.bashDown = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) shimInput.padStart = 1;
	}
	for (int i = 0; i < tfN; i++)                 /* right-half swipe-mode buttons */
	{
		if (tfKind[i] == 1) shimInput.buttonDown = 1;
		if (tfKind[i] == 2) shimInput.brakeDown = 1;
	}
	if (shimInput.mcCatch) shimInput.buttonDown = 1;   /* on-screen action buttons */
	if (shimInput.mcBrake) shimInput.brakeDown = 1;
	if (shimInput.mcBash) shimInput.bashDown = 1;

	ShimAdvanceTicks();
	ShimPresentIfDirty();
	pumping = 0;
}

void GetKeys (KeyMap theKeys)
{
	ShimPumpEvents();
	ShimTickSleep();

	memset(theKeys, 0, 16);
	const bool *ks = SDL_GetKeyboardState(NULL);
	unsigned char *km = (unsigned char *)theKeys;

	setKeyBit(km, kZ, ks[SDL_SCANCODE_Z]);
	setKeyBit(km, kS, ks[SDL_SCANCODE_S]);
	setKeyBit(km, kR, ks[SDL_SCANCODE_R]);
	setKeyBit(km, kE, ks[SDL_SCANCODE_E]);
	setKeyBit(km, kQ, ks[SDL_SCANCODE_Q]);
	setKeyBit(km, kB, ks[SDL_SCANCODE_B]);
	setKeyBit(km, kPeriod, ks[SDL_SCANCODE_PERIOD]);
	setKeyBit(km, kM, ks[SDL_SCANCODE_M]);
	setKeyBit(km, kN, ks[SDL_SCANCODE_N]);
	setKeyBit(km, kCommand, ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]
	                      || ks[SDL_SCANCODE_LGUI] || ks[SDL_SCANCODE_RGUI]);
	setKeyBit(km, kSpace, ks[SDL_SCANCODE_SPACE] || shimInput.brakeDown);
	setKeyBit(km, kTab, ks[SDL_SCANCODE_TAB]);
	setKeyBit(km, kOption, ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT]);

	if (shimInput.bashDown)
		setKeyBit(km, kB, 1);
	/* ANY pad's Start pauses (not just pad 1): in a four-player game the
	 * other seats hold pads 2..4 and deserve a working pause button too —
	 * matching the side panels' "START = PAUSE" and the pause screens'
	 * any-pad Back = end game */
	if (ShimAnyPadButton(SDL_GAMEPAD_BUTTON_START))
		setKeyBit(km, kTab, 1);
	if (pad)
	{
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
			setKeyBit(km, kR, 1);
	}
	/* Esc = pause (Tab) — the pause screen then offers to end the game;
	 * window close = quit (Cmd+Q). On Android the hardware Back button (latched
	 * from its key event) and the on-screen pause button pause too. backEdge
	 * stays latched until a pause loop / the menu consumes it, so the press is
	 * never lost between snapshots. */
	if (ks[SDL_SCANCODE_ESCAPE] || shimInput.backEdge || shimInput.mcPause || shimInput.pauseTap)
		setKeyBit(km, kTab, 1);
	if (shimInput.quitRequested)
	{
		setKeyBit(km, kCommand, 1);
		setKeyBit(km, kQ, 1);
	}
}

/* per-pad state for the 4P seats (deadzone + d-pad override like the merged path) */
int ShimPadRead (int idx, float *x, float *y, int *btn, int *brake, int *bash)
{
	*x = *y = 0;
	*btn = *brake = *bash = 0;
	if (idx < 0 || idx >= 4 || !padList[idx])
		return 0;
	SDL_Gamepad *p = padList[idx];
	float px = (float)SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
	float py = (float)SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  px = -1;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) px = 1;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP))    py = -1;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  py = 1;
	if (px < 0.12f && px > -0.12f) px = 0;
	if (py < 0.12f && py > -0.12f) py = 0;
	*x = px;
	*y = py;
	*btn = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH);
	*brake = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST) ||
	         SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192;
	*bash = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST);
	return 1;
}

Boolean Button (void)
{
	ShimPumpEvents();
	return (Boolean)shimInput.buttonDown;
}

/* is <button> (an SDL_GamepadButton) down on any connected pad? */
int ShimAnyPadButton (int button)
{
	for (int i = 0; i < 4; i++)
		if (padList[i] && SDL_GetGamepadButton(padList[i], (SDL_GamepadButton)button))
			return 1;
	return 0;
}

void GetMouse (Point *mouseLoc)
{
	ShimPumpEvents();
	*mouseLoc = shimInput.virtualMouse;
}

/* project function from UnivUtilities.c: warps the pointer; here it just
 * moves the virtual cursor (the game calls it to clamp into mouseFrame) */
void SetMouse (Point where)
{
	shimInput.virtualMouse = where;
}

Boolean BitTst (const void *bytePtr, long bitNum)
{
	const unsigned char *p = (const unsigned char *)bytePtr;
	return (Boolean)((p[bitNum >> 3] >> (7 - (bitNum & 7))) & 1);
}

/* ---------------- present pacing ---------------- */

void ShimPresentIfDirty (void)
{
	if (!shimScreenDirty)
		return;
	Uint64 now = SDL_GetTicksNS();
	if (now - lastPresentNS < 8000000ull)     /* max ~120 presents/s */
		return;
	lastPresentNS = now;
	shimScreenDirty = 0;
	PortVideoPresent((const uint8_t *)screenBits.baseAddr,
	                 screenBits.bounds.right, screenBits.bounds.bottom);
}

void ShimForcePresent (void)
{
	shimScreenDirty = 1;
	lastPresentNS = 0;
	ShimPresentIfDirty();
}
