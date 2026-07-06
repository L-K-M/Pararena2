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

ShimInput shimInput;

static SDL_Gamepad *pad;              /* first pad (merged single-player input) */
static SDL_Gamepad *padList[4];       /* all pads by connect order (4P seats) */
static int inPlayMode;
static float keyAxisX, keyAxisY;      /* ramped keyboard deflection -1..1 */
static Uint64 lastPresentNS;

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
}

static void handleEvent (const SDL_Event *ev)
{
	switch (ev->type)
	{
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
			break;
		case SDL_EVENT_KEY_DOWN:
			if (ev->key.key == SDLK_F11 ||
			    (ev->key.key == SDLK_RETURN && (ev->key.mod & SDL_KMOD_ALT)))
				PortVideoSetFullscreen(!PortVideoIsFullscreen());
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

	if (inPlayMode)
	{
		int cx = screenBits.bounds.right / 2;
		int cy = screenBits.bounds.bottom / 2;
		if (padActive)
		{
			shimInput.virtualMouse.h = (short)(cx + (int)(px * DEFLECT_HALF));
			shimInput.virtualMouse.v = (short)(cy + (int)(py * DEFLECT_HALF));
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
	if (pad)
	{
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))
			setKeyBit(km, kTab, 1);
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
			setKeyBit(km, kR, 1);
	}
	/* Esc = pause (Tab) — the pause screen then offers to end the game;
	 * window close = quit (Cmd+Q) */
	if (ks[SDL_SCANCODE_ESCAPE])
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
