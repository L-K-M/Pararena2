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

static SDL_Gamepad *pad;
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
			if (!pad)
			{
				pad = SDL_OpenGamepad(ev->gdevice.which);
				if (pad)
				{
					shimInput.padConnected = 1;
					ShimLog("gamepad connected: %s", SDL_GetGamepadName(pad));
				}
			}
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			if (pad && ev->gdevice.which == SDL_GetGamepadID(pad))
			{
				SDL_CloseGamepad(pad);
				pad = NULL;
				shimInput.padConnected = 0;
			}
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

	/* gamepad stick (absolute deflection) */
	float px = 0, py = 0;
	int padActive = 0;
	if (pad)
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
	if ((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0) shimInput.buttonDown = 1;
	if (ks[SDL_SCANCODE_X]) shimInput.buttonDown = 1;
	if (ks[SDL_SCANCODE_SPACE]) shimInput.brakeDown = 1;
	if (ks[SDL_SCANCODE_B] || ks[SDL_SCANCODE_N] || ks[SDL_SCANCODE_M]) shimInput.bashDown = 1;
	if (pad)
	{
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) shimInput.buttonDown = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST)) shimInput.brakeDown = 1;
		if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) shimInput.brakeDown = 1;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST)) shimInput.bashDown = 1;
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
	/* Esc = end game (Cmd+E); window close = quit (Cmd+Q) */
	if (ks[SDL_SCANCODE_ESCAPE])
	{
		setKeyBit(km, kCommand, 1);
		setKeyBit(km, kE, 1);
	}
	if (shimInput.quitRequested)
	{
		setKeyBit(km, kCommand, 1);
		setKeyBit(km, kQ, 1);
	}
}

Boolean Button (void)
{
	ShimPumpEvents();
	return (Boolean)shimInput.buttonDown;
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
