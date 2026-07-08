/* port_shell.c — the port's replacement for the Mac menu bar and dialogs:
 * a simple overlay menu driven by keyboard/gamepad, drawn with the shim's
 * QuickDraw onto the game screen. RootLoop() (verbatim Main.c) calls
 * HandleEvent() whenever the game is idle or paused; everything here hangs
 * off that hook.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "Globals.h"
#include "UnivUtilities.h"
#include "IdleRoutines.h"
#include "TeamSetUp.h"
#include "MainWindow.h"
#include "PlayUtils.h"
#include "SoundUtils.h"
#include "Render.h"
#include "controller_img.h"   /* embedded 1-bit dithered gamepad art */
#ifdef __ANDROID__
#include "pause_screen_mobile_img.h"  /* touch (P800) swipe-scheme pause art */
#else
#include "pause_screen_img.h"         /* gamepad pause art */
#endif
/* on-screen-scheme mobile pause art (unique symbols, so it coexists with either
 * of the above); shown when the touch UI is in on-screen mode */
#include "pause_screen_mobile_onscreen_img.h"
#include "mobile_controls.h"          /* on-screen touch control layout */

void PortInputSetPlayMode (int playing);
void PortFourRun (int mode, const int personas[4]);   /* port_four.c */

extern short soundVolume;     /* declared in ConfigureSound.h (not included here) */

int portCpuDemo = 0;          /* --cpu-demo: auto-start George vs Mara */
int portFourDemo = 0;         /* --four-demo N: auto-run one 4P AI game */

/* Classic mode: when on, the port's HUD enhancements (player-number plates and
 * the ball-owner caret, in every play mode) are hidden for a pure-1992 look.
 * Off by default; persisted in the port settings file. A future gate for any
 * further gameplay tweaks we want kept out of the classic experience. */
int classicMode = 0;
/* Options toggle (mobile): 1 = draw the on-screen analog stick + action buttons,
 * 0 = the invisible swipe scheme. The pause button is shown either way. */
int mobileOnScreen = 1;
void PortLoadSettings (int *classicMode, int *mobileOnScreen);   /* port_prefs.c */
void PortSaveSettings (int classicMode, int mobileOnScreen);

/* menu model: a dynamic main page (items shown/hidden by the selected mode)
 * plus an options page. */
enum { PAGE_MAIN, PAGE_OPTIONS };
enum { MI_START, MI_MODE, MI_GAME, MI_OPPONENT, MI_LEAGUE, MI_SIDE,
       MI_P2, MI_P3, MI_P4, MI_OPTIONS, MI_CONTROLS, MI_QUIT, MI_ITEMS };
enum { OI_VOLUME, OI_SOUND, OI_CURSOR, OI_ANNOUNCER, OI_RGOALS, OI_RFOULS, OI_RKEY,
       OI_FULLSCREEN, OI_CLASSIC, OI_ONSCREEN, OI_BACK, OI_COUNT };
static int menuPage = PAGE_MAIN;
static int menuSel = 0;        /* index into the visible-item list on the main page */
static int optSel = OI_VOLUME;
static int selMode = 0;        /* 0 = classic 1v1, 1 = 2v2, 2 = FFA-2, 3 = FFA-4 */
static int selGame = 0;        /* 0 standard, 1 tournament, 2 boardin', 3 scorin' */
static int selOpponent = 0;    /* persona-1: default Simple George (the game's own default) */
static int selLeague = 4;      /* professional */
static int selSide = 0;        /* 0 = defend left goal */
static int selSeat[3] = { 1, 2, 3 };  /* P2..P4: 0 = human on a pad, 1..6 = persona */
static int wasPlaying = 0;
static int menuDirty = 1;

static const char *gameNames[4] = { "STANDARD GAME", "TOURNAMENT", "PRACTICE BOARDIN'", "PRACTICE SCORIN'" };
static const short gameConsts[4] = { kStandardGame, kTournament, kPracticeBoardin, kPracticeScoring };
static const char *oppNames[6] = { "SIMPLE GEORGE", "MAD MARA", "HEAVY OTTO", "CLEVER CLAIRE", "MR. EAZE", "MS. TEAK" };
static const char *leagueNames[5] = { "LITTLE LEAGUE", "JUNIOR VARSITY", "VARSITY", "MINOR LEAGUE", "PROFESSIONAL" };
static const char *modeNames[4] = { "1 VS 1", "2 VS 2", "FFA - 2 GOALS", "FFA - 4 GOALS" };
static const char *seatNames[7] = { "HUMAN (PAD)", "SIMPLE GEORGE", "MAD MARA", "HEAVY OTTO",
                                    "CLEVER CLAIRE", "MR. EAZE", "MS. TEAK" };

static int visItems[MI_ITEMS];
static int visCount;

static void buildMenuItems (void)
{
	int n = 0;
	visItems[n++] = MI_START;
	visItems[n++] = MI_MODE;
	if (selMode == 0)
	{
		visItems[n++] = MI_GAME;
		visItems[n++] = MI_OPPONENT;
		visItems[n++] = MI_LEAGUE;
		visItems[n++] = MI_SIDE;
	}
	else
	{
		visItems[n++] = MI_P2;
		visItems[n++] = MI_P3;
		visItems[n++] = MI_P4;
		visItems[n++] = MI_LEAGUE;
	}
	visItems[n++] = MI_OPTIONS;
	visItems[n++] = MI_CONTROLS;
	visItems[n++] = MI_QUIT;
	visCount = n;
	if (menuSel >= visCount)
		menuSel = visCount - 1;
}

/* seed the menu from the loaded preferences so game/opponent/league/side
 * actually persist across launches (startGame writes them back to the
 * globals, and DumpTheDefaults saves the globals on quit) */
void PortShellSyncFromPrefs (void)
{
	for (int i = 0; i < 4; i++)
		if (gameConsts[i] == whichGame)
			selGame = i;
	if (theOpponent.persona >= kSimpleGeorge && theOpponent.persona <= kMissTeak)
		selOpponent = theOpponent.persona - 1;
	if (isLeague >= kLittleLeague && isLeague <= kProfessional)
		selLeague = isLeague;
	selSide = leftGoalIsPlayers ? 0 : 1;
	PortLoadSettings(&classicMode, &mobileOnScreen);
}

/* ------------------------------------------------------------------ */
/* App lifecycle (Android). SDL delivers these on the OS's activity thread
 * through the event watch — by the time the normal event pump would see
 * them, the main thread is already blocked by SDL_ANDROID_BLOCK_ON_PAUSE.
 * Going to the background: auto-pause a live game (exactly what DoPausing
 * does, so the player returns to the pause screen, not a live goal against
 * them), persist prefs + settings (on Android the process is killed without
 * ever running the desktop quit path, so this is the only save point), and
 * freeze the tick clock so the game clock doesn't fast-forward by the whole
 * background stay. The main thread is quiescent (blocked or about to block)
 * during the save; these globals are plain flags/structs it only rereads
 * after resume. */

void DumpTheDefaults (void);                  /* Sources/Main.c */

static void portSaveEverything (void)
{
	DumpTheDefaults();
	PortSaveSettings(classicMode, mobileOnScreen);
}

static bool SDLCALL portAppLifeWatch (void *userdata, SDL_Event *ev)
{
	(void)userdata;
	switch (ev->type)
	{
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			if (primaryMode == kPlayMode && !pausing)
			{
				pausing = TRUE;               /* == DoPausing() (1v1/practice) */
				timePaused = Ticks;
			}
			if (ShimInPlayMode())
				shimInput.pauseTap = 1;       /* the 4P loop pauses via the key path */
			portSaveEverything();
			ShimTimeFreezeBegin();
			break;
		case SDL_EVENT_WILL_ENTER_FOREGROUND:
			ShimTimeFreezeEnd();
			break;
		case SDL_EVENT_TERMINATING:           /* mobile: the only quit notice we get */
			portSaveEverything();
			break;
		default:
			break;
	}
	return true;
}

void PortLifecycleInit (void)
{
	SDL_AddEventWatch(portAppLifeWatch, NULL);
}

/* ------------------------------------------------------------------ */
/* WhosOnFirst — transcribed verbatim from Sources/TeamSetUp.c (the rest
 * of that file is the drag-and-drop Toolbox dialog, replaced by this menu) */

void WhosOnFirst (void)
{
	if (leftPlayerNumber == kHumanPlayer)
	{
		leftGoalIsPlayers = TRUE;
		thePlayer.persona = kHumanPlayer;
		theOpponent.persona = rightPlayerNumber;
		thePlayer.whichGoal = kLeftGoal;
		theOpponent.whichGoal = kRightGoal;
		disableBoardCursor = FALSE;
		leftGoalLeague = isLeague;
		if (netGameInSession)
			rightGoalLeague = theirLeague;
		else
			rightGoalLeague = kProfessional;

		if (arenaSize == kLargeArena)
		{
			thePlayer.initXPos = kLgInitLeftXPos;
			thePlayer.initZPos = kLgInitLeftZPos;
			theOpponent.initXPos = kLgInitRightXPos;
			theOpponent.initZPos = kLgInitRightZPos;
		}
		else
		{
			thePlayer.initXPos = kSmInitLeftXPos;
			thePlayer.initZPos = kSmInitLeftZPos;
			theOpponent.initXPos = kSmInitRightXPos;
			theOpponent.initZPos = kSmInitRightZPos;
		}
	}
	else if (rightPlayerNumber == kHumanPlayer)
	{
		leftGoalIsPlayers = FALSE;
		thePlayer.persona = kHumanPlayer;
		theOpponent.persona = leftPlayerNumber;
		thePlayer.whichGoal = kRightGoal;
		theOpponent.whichGoal = kLeftGoal;
		disableBoardCursor = FALSE;

		rightGoalLeague = isLeague;
		if (netGameInSession)
			leftGoalLeague = theirLeague;
		else
			leftGoalLeague = kProfessional;

		if (arenaSize == kLargeArena)
		{
			thePlayer.initXPos = kLgInitRightXPos;
			thePlayer.initZPos = kLgInitRightZPos;
			theOpponent.initXPos = kLgInitLeftXPos;
			theOpponent.initZPos = kLgInitLeftZPos;
		}
		else
		{
			thePlayer.initXPos = kSmInitRightXPos;
			thePlayer.initZPos = kSmInitRightZPos;
			theOpponent.initXPos = kSmInitLeftXPos;
			theOpponent.initZPos = kSmInitLeftZPos;
		}
	}
	else
	{
		leftGoalIsPlayers = TRUE;
		thePlayer.persona = leftPlayerNumber;
		theOpponent.persona = rightPlayerNumber;
		thePlayer.whichGoal = kLeftGoal;
		theOpponent.whichGoal = kRightGoal;
		leftGoalLeague = kProfessional;
		rightGoalLeague = kProfessional;
		disableBoardCursor = TRUE;

		if (arenaSize == kLargeArena)
		{
			thePlayer.initXPos = kLgInitLeftXPos;
			thePlayer.initZPos = kLgInitLeftZPos;
			theOpponent.initXPos = kLgInitRightXPos;
			theOpponent.initZPos = kLgInitRightZPos;
		}
		else
		{
			thePlayer.initXPos = kSmInitLeftXPos;
			thePlayer.initZPos = kSmInitLeftZPos;
			theOpponent.initXPos = kSmInitRightXPos;
			theOpponent.initZPos = kSmInitRightZPos;
		}
	}
}

/* ------------------------------------------------------------------ */

static void drawText (short x, short y, const char *s, unsigned char colorIdx)
{
	Str255 ps;
	size_t n = strlen(s);
	if (n > 255) n = 255;
	ps[0] = (unsigned char)n;
	memcpy(ps + 1, s, n);
	RGBColor c;
	Index2Color(colorIdx, &c);
	RGBForeColor(&c);
	MoveTo(x, y);
	DrawString(ps);
}

#define IDX_WHITE 0
#define IDX_YELLOW 1
#define IDX_BLACK 15
#define IDX_GRAY 13
#define IDX_DGRAY 14

/* ---- 8bpp pixel helpers (screen coords): the gamepad-bitmap blit and the
 *      little button glyphs in the controls-card legend ---- */
static void pxPlot (const BitMap *bm, unsigned char idx, int x, int y)
{
	if (x < bm->bounds.left || x >= bm->bounds.right ||
	    y < bm->bounds.top  || y >= bm->bounds.bottom) return;
	*((uint8_t *)bm->baseAddr + (long)(y - bm->bounds.top) * bm->rowBytes
	  + (x - bm->bounds.left)) = idx;
}

static void pxFillCircle (const BitMap *bm, unsigned char idx, int cx, int cy, int rad)
{
	for (int dy = -rad; dy <= rad; dy++)
		for (int dx = -rad; dx <= rad; dx++)
			if (dx * dx + dy * dy <= rad * rad)
				pxPlot(bm, idx, cx + dx, cy + dy);
}

static void pxFillRect (const BitMap *bm, unsigned char idx, int l, int t, int r, int b)
{
	for (int y = t; y <= b; y++)
		for (int x = l; x <= r; x++)
			pxPlot(bm, idx, x, y);
}

/* filled circle with a 1px border */
static void pxDisc (const BitMap *bm, int cx, int cy, int rad, unsigned char fill, unsigned char border)
{
	pxFillCircle(bm, border, cx, cy, rad);
	pxFillCircle(bm, fill, cx, cy, rad - 1);
}

/* one glyph centered at (cx,cy) in colour fg, via the shim font (mainWndo port) */
static void pxChar (int cx, int cy, char ch, unsigned char fg)
{
	RGBColor c;
	Str255 ps;
	ps[0] = 1; ps[1] = (unsigned char)ch;
	Index2Color(fg, &c);
	RGBForeColor(&c);
	MoveTo((short)(cx - 4), (short)(cy + 3));
	DrawString(ps);
}

/* Blit the user-supplied dithered controller bitmap (controller_img.h) at its
 * top-left (left,top), pixel-for-pixel — no scaling. The art is 1-bit on a
 * white ground, so we paint only the black pixels and let the white System-7
 * panel show through the rest, reproducing the image exactly. */
static void drawController (const BitMap *scr, int left, int top)
{
	for (int y = 0; y < CONTROLLER_IMG_H; y++)
	{
		const unsigned char *row = controllerImgBits + (long)y * CONTROLLER_IMG_ROWBYTES;
		for (int x = 0; x < CONTROLLER_IMG_W; x++)
			if (row[x >> 3] & (0x80 >> (x & 7)))
				pxPlot(scr, IDX_BLACK, left + x, top + y);
	}
}

/* Draw the controls card (panel + controller + legend) onto mainWndo. Leaves the
 * bottom ~20px for a caller-supplied prompt. Shared by the menu and the pause. */
void DrawControlsCard (const char *title)
{
	GrafPtr wasPort;
	RGBColor c;
	const BitMap *scr = &(((GrafPtr)mainWndo)->portBits);
	Rect panel;
	int imgLeft, imgTop, lx, ly;

	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);

	/* the panel is tall enough to hold the full-size gamepad bitmap (drawn 1:1,
	 * never scaled) plus a title above and the legend below. */
	SetRect(&panel, (short)(screenWide / 2 - 290), (short)(screenHigh / 2 - 226),
	                (short)(screenWide / 2 + 290), (short)(screenHigh / 2 + 226));
	/* a plain white System-7 dialog: white fill, black double frame */
	PmForeColor(IDX_WHITE);
	PaintRect(&panel);
	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	FrameRect(&panel);
	Rect inner = panel; InsetRect(&inner, 2, 2); FrameRect(&inner);

	drawText((short)(screenWide / 2 - (short)(strlen(title) * 4)),
	         (short)(panel.top + 22), title, IDX_BLACK);

	/* the gamepad bitmap, blitted pixel-for-pixel and centred horizontally */
	imgLeft = screenWide / 2 - CONTROLLER_IMG_W / 2;
	imgTop  = panel.top + 28;
	drawController(scr, imgLeft, imgTop);

	/* legend: a button glyph (or its name), then the action — all black-on-white */
	lx = panel.left + 70;
	ly = imgTop + CONTROLLER_IMG_H + 16;
	#define ROW 22
	pxDisc(scr, lx, ly - 4, 8, IDX_WHITE, IDX_BLACK); pxChar(lx, ly - 4, 'A', IDX_BLACK);
	drawText((short)(lx + 156), (short)(ly), "CATCH / THROW / CROUCH", IDX_BLACK);

	pxDisc(scr, lx, ly + ROW - 4, 8, IDX_WHITE, IDX_BLACK); pxChar(lx, ly + ROW - 4, 'X', IDX_BLACK);
	drawText((short)(lx + 156), (short)(ly + ROW), "BASH  (dash / tackle)", IDX_BLACK);

	pxDisc(scr, lx, ly + 2 * ROW - 4, 8, IDX_WHITE, IDX_BLACK); pxChar(lx, ly + 2 * ROW - 4, 'B', IDX_BLACK);
	drawText((short)(lx + 18), (short)(ly + 2 * ROW), "/  RT", IDX_BLACK);
	drawText((short)(lx + 156), (short)(ly + 2 * ROW), "BRAKE", IDX_BLACK);

	drawText((short)(lx - 6), (short)(ly + 3 * ROW), "STICK / D-PAD", IDX_BLACK);
	drawText((short)(lx + 156), (short)(ly + 3 * ROW), "SKATE  (thrust)", IDX_BLACK);

	drawText((short)(lx - 6), (short)(ly + 4 * ROW), "START", IDX_BLACK);
	drawText((short)(lx + 156), (short)(ly + 4 * ROW), "PAUSE  (also Esc)", IDX_BLACK);

	drawText((short)(panel.left + 24), (short)(ly + 5 * ROW + 4),
	         "KEYBOARD:  MOUSE=SKATE  X=CATCH  SPACE=BRAKE  B/N/M=BASH", IDX_BLACK);
	#undef ROW

	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	SetPort(wasPort);
	shimScreenDirty = 1;
}

/* blit a bit-packed (MSB-first, 1 = black) full-screen 1-bit image over white */
static void blitPauseBits (const BitMap *scr, const unsigned char *bits,
                           int w, int h, int rowBytes)
{
	for (int y = 0; y < h; y++)
	{
		const unsigned char *row = bits + (long)y * rowBytes;
		for (int x = 0; x < w; x++)
			if (row[x >> 3] & (0x80 >> (x & 7)))
				pxPlot(scr, IDX_BLACK, x, y);
	}
}

/* The pause screen: a hand-drawn 640x480 1-bit image blitted over the whole
 * window — white ground, black line-art + labels. On a touch device it shows the
 * P800 art for the *active* control scheme (on-screen stick+buttons vs swipe);
 * on desktop, the gamepad card. Shared by the 1v1 and four-player pause loops. */
void DrawPauseScreen (void)
{
	GrafPtr wasPort;
	RGBColor c;
	const BitMap *scr = &(((GrafPtr)mainWndo)->portBits);
	Rect full;

	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);
	SetRect(&full, 0, 0, (short)screenWide, (short)screenHigh);
	PmForeColor(IDX_WHITE);
	PaintRect(&full);
	if (shimMobile && mobileOnScreen)
		blitPauseBits(scr, pauseOnscreenImgBits, PAUSE_ONSCREEN_IMG_W,
		              PAUSE_ONSCREEN_IMG_H, PAUSE_ONSCREEN_IMG_ROWBYTES);
	else
		blitPauseBits(scr, pauseImgBits, PAUSE_IMG_W, PAUSE_IMG_H, PAUSE_IMG_ROWBYTES);
	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	SetPort(wasPort);
	shimScreenDirty = 1;
}

/* Draw the on-screen touch controls over the current game frame (mobile only).
 * Called every frame from the render hooks after the scene is composited, so
 * the opaque stick base covers the previous thumb position (no trails). */
void PortDrawMobileControls (void)
{
	GrafPtr wasPort;
	RGBColor c;
	const BitMap *scr;

	if (!shimMobile)
		return;
	scr = &(((GrafPtr)mainWndo)->portBits);
	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);

	/* pause button (always): a ring with two bars */
	pxDisc(scr, MC_PAUSE_CX, MC_PAUSE_CY, MC_PAUSE_R,
	       shimInput.mcPause ? IDX_YELLOW : IDX_WHITE, IDX_BLACK);
	pxFillRect(scr, IDX_BLACK, MC_PAUSE_CX - 6, MC_PAUSE_CY - 8, MC_PAUSE_CX - 2, MC_PAUSE_CY + 8);
	pxFillRect(scr, IDX_BLACK, MC_PAUSE_CX + 2, MC_PAUSE_CY - 8, MC_PAUSE_CX + 6, MC_PAUSE_CY + 8);

	if (mobileOnScreen)
	{
		/* analog stick: opaque dark base + white ring, white thumb on top */
		pxFillCircle(scr, IDX_WHITE, MC_STICK_CX, MC_STICK_CY, MC_STICK_R);
		pxFillCircle(scr, IDX_DGRAY, MC_STICK_CX, MC_STICK_CY, MC_STICK_R - 3);
		int travel = MC_STICK_R - MC_STICK_TR;
		int tx = MC_STICK_CX + (int)(shimInput.mcThumbX * travel);
		int ty = MC_STICK_CY + (int)(shimInput.mcThumbY * travel);
		pxDisc(scr, tx, ty, MC_STICK_TR, IDX_WHITE, IDX_BLACK);
		pxFillCircle(scr, IDX_GRAY, tx, ty, 4);

		/* catch button: white disc, filled dot (the ball); lit when pressed */
		pxDisc(scr, MC_CATCH_CX, MC_CATCH_CY, MC_CATCH_R,
		       shimInput.mcCatch ? IDX_YELLOW : IDX_WHITE, IDX_BLACK);
		pxFillCircle(scr, IDX_BLACK, MC_CATCH_CX, MC_CATCH_CY, 9);
		pxFillCircle(scr, IDX_WHITE, MC_CATCH_CX - 3, MC_CATCH_CY - 3, 2);

		/* brake button: white disc with a bold bar */
		pxDisc(scr, MC_BRAKE_CX, MC_BRAKE_CY, MC_BRAKE_R,
		       shimInput.mcBrake ? IDX_YELLOW : IDX_WHITE, IDX_BLACK);
		pxFillRect(scr, IDX_BLACK, MC_BRAKE_CX - 12, MC_BRAKE_CY - 4, MC_BRAKE_CX + 12, MC_BRAKE_CY + 4);

		/* bash button: white disc with a bold X (the lunge / tackle) */
		pxDisc(scr, MC_BASH_CX, MC_BASH_CY, MC_BASH_R,
		       shimInput.mcBash ? IDX_YELLOW : IDX_WHITE, IDX_BLACK);
		for (int d = -8; d <= 8; d++)
			for (int t = -1; t <= 1; t++)
			{
				pxPlot(scr, IDX_BLACK, MC_BASH_CX + d, MC_BASH_CY + d + t);
				pxPlot(scr, IDX_BLACK, MC_BASH_CX + d, MC_BASH_CY - d + t);
			}
	}

	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	SetPort(wasPort);
	shimScreenDirty = 1;
}

int ShimPadRead (int idx, float *x, float *y, int *btn, int *brake, int *bash);  /* shim_input.c */

/* any keyboard/gamepad press that should dismiss the controls screen */
static int controlsDismiss (void)
{
	const bool *ks = SDL_GetKeyboardState(NULL);
	if (ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] ||
	    ks[SDL_SCANCODE_SPACE]  || ks[SDL_SCANCODE_ESCAPE]) return 1;
	for (int i = 0; i < 4; i++)
	{
		float px, py; int b = 0, bk = 0, bs = 0;
		if (ShimPadRead(i, &px, &py, &b, &bk, &bs) && (b || bk || bs)) return 1;
	}
	return 0;
}

/* modal controls screen opened from the main menu */
static void ShowControlsScreen (void)
{
	GrafPtr wp;
	RGBColor c;
	int armed = 0;

	DrawControlsCard("CONTROLS");
	GetPort(&wp);
	SetPort((GrafPtr)mainWndo);
	drawText((short)(screenWide / 2 - (shimMobile ? 104 : 92)),
	         (short)(screenHigh / 2 + 178),
	         shimMobile ? "TAP OR PRESS ANY KEY TO RETURN"
	                    : "PRESS ANY KEY TO RETURN", IDX_BLACK);
	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	SetPort(wp);
	shimScreenDirty = 1;
	ShimForcePresent();

	if (shimHeadless) { menuDirty = 1; return; }
	/* the tap/Back that opened this screen may still be latched — spend it,
	 * or a touch-only device would dismiss the card the instant it opens */
	shimInput.tapFresh = 0;
	shimInput.backEdge = 0;
	for (;;)
	{
		ShimPumpEvents();
		if (shimInput.quitRequested) { quitting = TRUE; break; }
		if (shimInput.tapFresh)              /* touch: a fresh tap returns */
		{
			shimInput.tapFresh = 0;
			break;
		}
		if (shimInput.backEdge)              /* Android Back returns too */
		{
			shimInput.backEdge = 0;
			break;
		}
		if (!controlsDismiss()) armed = 1;   /* wait for release, then any press */
		else if (armed) break;
		SDL_Delay(10);
	}
	menuDirty = 1;
}

static void menuValueText (int item, char *buf, size_t bufsz)
{
	switch (item)
	{
		case MI_MODE: snprintf(buf, bufsz, "%s", modeNames[selMode]); break;
		case MI_GAME: snprintf(buf, bufsz, "%s", gameNames[selGame]); break;
		case MI_OPPONENT: snprintf(buf, bufsz, "%s", oppNames[selOpponent]); break;
		case MI_LEAGUE: snprintf(buf, bufsz, "%s", leagueNames[selLeague]); break;
		case MI_SIDE: snprintf(buf, bufsz, "%s", selSide == 0 ? "LEFT GOAL" : "RIGHT GOAL"); break;
		case MI_P2: snprintf(buf, bufsz, "%s", seatNames[selSeat[0]]); break;
		case MI_P3: snprintf(buf, bufsz, "%s", seatNames[selSeat[1]]); break;
		case MI_P4: snprintf(buf, bufsz, "%s", seatNames[selSeat[2]]); break;
		default: buf[0] = 0; break;
	}
}

static void optionValueText (int item, char *buf, size_t bufsz)
{
	switch (item)
	{
		case OI_VOLUME: snprintf(buf, bufsz, "%d", soundVolume); break;
		case OI_SOUND: snprintf(buf, bufsz, "%s", soundOn ? "ON" : "OFF"); break;
		case OI_CURSOR: snprintf(buf, bufsz, "%s", showBoardCursor ? "ON" : "OFF"); break;
		case OI_ANNOUNCER: snprintf(buf, bufsz, "%s", enableAnnouncer ? "ON" : "OFF"); break;
		case OI_RGOALS: snprintf(buf, bufsz, "%s", replayGoals ? "ON" : "OFF"); break;
		case OI_RFOULS: snprintf(buf, bufsz, "%s", replayFouls ? "ON" : "OFF"); break;
		case OI_RKEY: snprintf(buf, bufsz, "%s", replayOnR ? "ON" : "OFF"); break;
		case OI_FULLSCREEN: snprintf(buf, bufsz, "%s", PortVideoIsFullscreen() ? "ON" : "OFF"); break;
		case OI_CLASSIC: snprintf(buf, bufsz, "%s", classicMode ? "ON" : "OFF"); break;
		case OI_ONSCREEN: snprintf(buf, bufsz, "%s", mobileOnScreen ? "ON-SCREEN" : "SWIPE"); break;
		default: buf[0] = 0; break;
	}
}

static const char *menuItemLabel (int item)
{
	switch (item)
	{
		case MI_START: return "START GAME";
		case MI_MODE: return "MODE";
		case MI_GAME: return "GAME";
		case MI_OPPONENT: return "OPPONENT";
		case MI_LEAGUE: return "LEAGUE";
		case MI_SIDE: return "PLAY SIDE";
		/* in 2v2 the seats pair up left {P1,P3} vs right {P2,P4} */
		case MI_P2: return selMode == 1 ? "RIVAL 1" : "PLAYER 2";
		case MI_P3: return selMode == 1 ? "TEAMMATE" : "PLAYER 3";
		case MI_P4: return selMode == 1 ? "RIVAL 2" : "PLAYER 4";
		case MI_OPTIONS: return "OPTIONS...";
		case MI_CONTROLS: return "CONTROLS";
		case MI_QUIT: return "QUIT";
		default: return "";
	}
}

static void drawMenu (void)
{
	GrafPtr wasPort;
	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);

	Rect panel;
	panel.left = (short)(screenWide / 2 - 190);
	panel.right = (short)(screenWide / 2 + 190);
	panel.top = (short)(screenHigh / 2 - 120);
	panel.bottom = (short)(screenHigh / 2 + 120);

	PmForeColor(IDX_BLACK);
	PaintRect(&panel);
	RGBColor c;
	Index2Color(IDX_WHITE, &c);
	RGBForeColor(&c);
	FrameRect(&panel);
	Rect inner = panel;
	InsetRect(&inner, 2, 2);
	FrameRect(&inner);

	drawText((short)(panel.left + 120), (short)(panel.top + 24), "P A R A R E N A  2", IDX_YELLOW);

	static const char *optLabels[OI_COUNT] = {
		"VOLUME", "SOUND", "BOARD CURSOR", "ANNOUNCER", "REPLAY GOALS",
		"REPLAY FOULS", "REPLAY KEY R", "FULLSCREEN", "CLASSIC MODE",
		"TOUCH CONTROLS", "BACK"
	};
	char val[48];
	if (menuPage == PAGE_MAIN)
	{
		buildMenuItems();
		for (int i = 0; i < visCount; i++)
		{
			int item = visItems[i];
			short y = (short)(panel.top + 48 + i * 19);
			unsigned char col = (i == menuSel) ? IDX_YELLOW : IDX_WHITE;
			drawText((short)(panel.left + 28), y, i == menuSel ? ">" : " ", col);
			drawText((short)(panel.left + 44), y, menuItemLabel(item), col);
			menuValueText(item, val, sizeof val);
			if (val[0])
				drawText((short)(panel.left + 170), y, val, col);
		}
	}
	else
	{
		for (int i = 0; i < OI_COUNT; i++)
		{
			short y = (short)(panel.top + 42 + i * 17);
			unsigned char col = (i == optSel) ? IDX_YELLOW : IDX_WHITE;
			drawText((short)(panel.left + 28), y, i == optSel ? ">" : " ", col);
			drawText((short)(panel.left + 44), y, optLabels[i], col);
			optionValueText(i, val, sizeof val);
			if (val[0])
				drawText((short)(panel.left + 190), y, val, col);
		}
	}
	drawText((short)(panel.left + 16), (short)(panel.bottom - 14),
	         shimMobile ? "TAP A ROW TO SELECT   TAP AGAIN TO CHOOSE"
	                    : "ARROWS: MOVE  RETURN: SELECT  F11: FULLSCREEN", IDX_GRAY);

	Index2Color(IDX_BLACK, &c);
	RGBForeColor(&c);
	SetPort(wasPort);
}

static void startGame (void)
{
	isLeague = (short)selLeague;

	if (selMode > 0)
	{
		/* four-player modes run their own loop and return to the menu.
		 * seat order: 0 = P1 (kb/mouse), 1..3 = the P2..P4 menu rows */
		int personas[4];
		personas[0] = kHumanPlayer;
		for (int i = 0; i < 3; i++)
			personas[1 + i] = selSeat[i] == 0 ? kHumanPlayer : selSeat[i];
		whichHumanNumber = 1;
		PortFourRun(selMode, personas);
		menuDirty = 1;
		return;
	}

	whichGame = gameConsts[selGame];
	if (whichGame == kTournament)
	{
		selLeague = 4;                     /* tournaments are Professional-league */
		isLeague = (short)selLeague;
	}
	if (portCpuDemo)
	{
		leftPlayerNumber = kSimpleGeorge;
		rightPlayerNumber = kMadMara;
	}
	else if (selSide == 0)
	{
		leftPlayerNumber = kHumanPlayer;
		rightPlayerNumber = (short)(selOpponent + 1);
	}
	else
	{
		rightPlayerNumber = kHumanPlayer;
		leftPlayerNumber = (short)(selOpponent + 1);
	}
	whichHumanNumber = 1;
	autoTeamsDialog = FALSE;
	WhosOnFirst();
	UpdateGoalPicts(!splashIsUp);
	newGame = TRUE;
	primaryMode = kPlayMode;
	PortInputSetPlayMode(1);
	menuDirty = 1;
}

static void menuMoveSel (int dir)
{
	if (menuPage == PAGE_MAIN)
	{
		buildMenuItems();
		menuSel = (menuSel + visCount + dir) % visCount;
	}
	else
		optSel = (optSel + OI_COUNT + dir) % OI_COUNT;
	menuDirty = 1;
}

/* edge-detected shell key handling */
static int keyPressedOnce (int scancode)
{
	static Uint8 prev[SDL_SCANCODE_COUNT];
	const bool *ks = SDL_GetKeyboardState(NULL);
	int now = ks[scancode] ? 1 : 0;
	int was = prev[scancode];
	prev[scancode] = (Uint8)now;
	return now && !was;
}

static void menuAdjust (int dir)
{
	if (menuPage == PAGE_MAIN)
	{
		buildMenuItems();
		switch (visItems[menuSel])
		{
			case MI_MODE: selMode = (selMode + dir + 4) % 4; buildMenuItems(); break;
			case MI_GAME: selGame = (selGame + dir + 4) % 4; break;
			case MI_OPPONENT: selOpponent = (selOpponent + dir + 6) % 6; break;
			case MI_LEAGUE: selLeague = (selLeague + dir + 5) % 5; break;
			case MI_SIDE: selSide ^= 1; break;
			case MI_P2: selSeat[0] = (selSeat[0] + dir + 7) % 7; break;
			case MI_P3: selSeat[1] = (selSeat[1] + dir + 7) % 7; break;
			case MI_P4: selSeat[2] = (selSeat[2] + dir + 7) % 7; break;
			default: break;
		}
	}
	else
	{
		switch (optSel)
		{
			case OI_VOLUME:
				soundVolume = (short)(soundVolume + dir);
				if (soundVolume < 0) soundVolume = 0;
				if (soundVolume > 7) soundVolume = 7;
				SetSoundVol(soundVolume);
				if (soundOn)
				{
					extern void SMSSTART (short);
					SMSSTART(kBallPickUpSound);      /* audible level check */
				}
				break;
			case OI_SOUND: soundOn = !soundOn; break;
			case OI_CURSOR: showBoardCursor = !showBoardCursor; break;
			case OI_ANNOUNCER: enableAnnouncer = !enableAnnouncer; break;
			case OI_RGOALS: replayGoals = !replayGoals; break;
			case OI_RFOULS: replayFouls = !replayFouls; break;
			case OI_RKEY: replayOnR = !replayOnR; break;
			case OI_FULLSCREEN: PortVideoSetFullscreen(!PortVideoIsFullscreen()); break;
			case OI_CLASSIC: classicMode = !classicMode; PortSaveSettings(classicMode, mobileOnScreen); break;
			case OI_ONSCREEN: mobileOnScreen = !mobileOnScreen; PortSaveSettings(classicMode, mobileOnScreen); break;
			default: break;
		}
		replaySomething = replayGoals || replayFouls || replayOnR;
	}
	menuDirty = 1;
}

static void menuActivate (void)
{
	if (menuPage == PAGE_MAIN)
	{
		buildMenuItems();
		switch (visItems[menuSel])
		{
			case MI_START: startGame(); break;
			case MI_OPTIONS: menuPage = PAGE_OPTIONS; optSel = OI_VOLUME; menuDirty = 1; break;
			case MI_CONTROLS: ShowControlsScreen(); break;
			case MI_QUIT: quitting = TRUE; break;
			default: menuAdjust(1); break;
		}
	}
	else
	{
		if (optSel == OI_BACK)
		{
			menuPage = PAGE_MAIN;
			menuDirty = 1;
		}
		else
			menuAdjust(1);
	}
}

/* Touchscreen menu: hit-test a normalized tap (nx,ny) against the on-screen
 * rows using the exact geometry drawMenu() lays out. A tap highlights the row it
 * lands on; a second tap on that same row activates it (start / open / cycle).
 * Activation is gated on the row having been highlighted BY A TAP — the startup
 * default (START pre-selected) is not enough — so the first tap anywhere can only
 * move the highlight, never launch a game. The (page,row) pair is remembered so
 * switching pages or navigating by key/pad re-requires a selecting tap first. */
static void menuTapHit (float nx, float ny)
{
	static int armedPage = -1, armedRow = -1;
	int px = (int)(nx * screenWide);
	int py = (int)(ny * screenHigh);
	short pLeft   = (short)(screenWide / 2 - 190);
	short pRight  = (short)(screenWide / 2 + 190);
	short pTop    = (short)(screenHigh / 2 - 120);
	short pBottom = (short)(screenHigh / 2 + 120);
	int count, hit = -1;
	int *selp;

	if (px < pLeft || px > pRight || py < pTop || py > pBottom)
		return;                                /* outside the panel: ignore */

	if (menuPage == PAGE_MAIN) { buildMenuItems(); count = visCount; selp = &menuSel; }
	else                       { count = OI_COUNT;                  selp = &optSel;  }

	for (int i = 0; i < count; i++)
	{
		int y    = (menuPage == PAGE_MAIN) ? pTop + 48 + i * 19 : pTop + 42 + i * 17;
		int band = (menuPage == PAGE_MAIN) ? 16 : 14;
		if (py >= y - band && py <= y + 3) { hit = i; break; }
	}
	if (hit < 0)
		return;                                /* between rows: ignore */

	if (armedPage == menuPage && armedRow == hit && *selp == hit)
		menuActivate();                        /* second tap on the same row */
	else
	{
		*selp = hit; armedPage = menuPage; armedRow = hit; menuDirty = 1;
	}
}

void HandleEvent (void)
{
	ShimPumpEvents();

	if (shimInput.quitRequested)
	{
		quitting = TRUE;
		return;
	}

	/* paused mid-game (1v1): show the same full-screen pause art the four-player
	 * modes use, and resume on Esc / Tab / pad Start or end the game
	 * with E / pad Back. The game sets pausing while the pause key is still held,
	 * so wait for a release ("armed") before a fresh press counts — otherwise the
	 * entry press would instantly resume. Ending a game is just primaryMode =
	 * kIdleMode (what the original's Cmd+E abort does for a standard game). */
	if (primaryMode == kPlayMode && pausing)
	{
		int armed = 0;

		DrawPauseScreen();
		ShimForcePresent();
		shimInput.tapFresh = 0;                  /* ignore any tap still pending */
		shimInput.backEdge = 0;                  /* the Back press that opened this is spent */
		shimInput.pauseTap = 0;

		for (;;)
		{
			ShimPumpEvents();
			if (shimInput.quitRequested) { quitting = TRUE; pausing = FALSE; primaryMode = kIdleMode; break; }
			if (shimInput.backEdge)             /* Android: a second Back press ends the game */
			{
				shimInput.backEdge = 0;
				pausing = FALSE; primaryMode = kIdleMode;
				break;
			}
			if (shimInput.tapFresh)              /* touch: tap anywhere resumes (Back ends) */
			{
				shimInput.tapFresh = 0;
				pausing = FALSE;
				break;
			}
			const bool *ks = SDL_GetKeyboardState(NULL);
			int resumeHeld = ks[SDL_SCANCODE_TAB] || ks[SDL_SCANCODE_ESCAPE] || shimInput.padStart;
			int endHeld    = ks[SDL_SCANCODE_E] || ShimAnyPadButton(SDL_GAMEPAD_BUTTON_BACK);
			if (armed && endHeld)    { pausing = FALSE; primaryMode = kIdleMode; break; }  /* end game */
			if (armed && resumeHeld)
			{
				/* wait for the resume key to release before handing control back,
				 * or RunStandardGame's CheckAbortiveInput sees it still down and
				 * instantly re-pauses. */
				while (resumeHeld && !shimInput.quitRequested)
				{
					ShimPumpEvents();
					ks = SDL_GetKeyboardState(NULL);
					resumeHeld = ks[SDL_SCANCODE_TAB] || ks[SDL_SCANCODE_ESCAPE] || shimInput.padStart;
					SDL_Delay(10);
				}
				pausing = FALSE;                                                          /* resume */
				break;
			}
			if (!resumeHeld && !endHeld) armed = 1;
			SDL_Delay(10);
		}
		/* drop any pause latch the resume tap itself set (e.g. a tap that landed
		 * on the top-centre pause column), so play doesn't instantly re-pause */
		shimInput.pauseTap = 0;
		shimInput.backEdge = 0;
		shimInput.tapFresh = 0;
		return;
	}

	if (wasPlaying)
	{
		/* a game just ended: back to menu input mode */
		wasPlaying = 0;
		PortInputSetPlayMode(0);
		menuDirty = 1;
		if (portCpuDemo)
		{
			quitting = TRUE;      /* demo run ends after one game */
			return;
		}
	}

	if (portFourDemo)
	{
		static const int demoSeats[4] = { kSimpleGeorge, kMadMara, kHeavyOtto, kCleverClaire };
		PortFourRun(portFourDemo, demoSeats);
		quitting = TRUE;              /* demo run ends after one game */
		return;
	}

	if (portCpuDemo)
	{
		startGame();
		wasPlaying = 1;
		return;
	}

	buildMenuItems();
	if (keyPressedOnce(SDL_SCANCODE_UP)) menuMoveSel(-1);
	if (keyPressedOnce(SDL_SCANCODE_DOWN)) menuMoveSel(1);
	if (keyPressedOnce(SDL_SCANCODE_LEFT)) menuAdjust(-1);
	if (keyPressedOnce(SDL_SCANCODE_RIGHT)) menuAdjust(1);
	if (keyPressedOnce(SDL_SCANCODE_RETURN) || keyPressedOnce(SDL_SCANCODE_KP_ENTER)
	    || keyPressedOnce(SDL_SCANCODE_SPACE))
		menuActivate();
	if (keyPressedOnce(SDL_SCANCODE_ESCAPE) && !splashIsUp)
	{
		if (menuPage == PAGE_OPTIONS)
			menuPage = PAGE_MAIN;
		else
			menuSel = visCount - 1;            /* QUIT is always last */
		menuDirty = 1;
	}

	/* gamepad menu navigation via polled edges */
	{
		static int prevS, prevU, prevD, prevL, prevR;
		int u = 0, d = 0, l = 0, r = 0, s = 0;
		if (shimInput.padConnected)
		{
			u = shimInput.padY < -0.5f;
			d = shimInput.padY > 0.5f;
			l = shimInput.padX < -0.5f;
			r = shimInput.padX > 0.5f;
			s = shimInput.buttonDown;
		}
		if (u && !prevU) menuMoveSel(-1);
		if (d && !prevD) menuMoveSel(1);
		if (l && !prevL) menuAdjust(-1);
		if (r && !prevR) menuAdjust(1);
		if (s && !prevS) menuActivate();
		prevU = u; prevD = d; prevL = l; prevR = r; prevS = s;
	}

	/* touchscreen menu navigation: tap a row to select it, tap it again to use it */
	if (shimInput.tapFresh)
	{
		menuTapHit(shimInput.tapX, shimInput.tapY);
		shimInput.tapFresh = 0;
	}

	/* Android Back in the menu behaves like Esc: leave the options page, else
	 * jump the highlight to QUIT. Always consumed so it can't leak into play. */
	if (shimInput.backEdge)
	{
		shimInput.backEdge = 0;
		if (!splashIsUp)
		{
			if (menuPage == PAGE_OPTIONS)
				menuPage = PAGE_MAIN;
			else
				menuSel = visCount - 1;        /* QUIT is always last */
			menuDirty = 1;
		}
	}

	if (primaryMode == kPlayMode)
	{
		wasPlaying = 1;
		return;
	}

	if (menuDirty)
	{
		menuDirty = 0;
		/* repaint backdrop then the panel: splash if it's still up, else arena */
		if (splashIsUp)
			DoSplashScreen();
		else
			RefreshMainWindow();
		drawMenu();
	}
	SDL_Delay(10);
}

/* ------------------------------------------------------------------ */
/* replacements for IdleRoutines.c functions referenced by compiled code */

void DoNew (void)
{
	primaryMode = kPlayMode;
	newGame = TRUE;
}

Boolean DoTeamsSetUp (void)
{
	return TRUE;                  /* teams are chosen in the port menu */
}

void DoOpeningAnnouncer (void)
{
	/* simplified from IdleRoutines.c: announcer art + the six voice clips */
	static const short clipIDs[6] = { 100, 101, 102, 103, 104, 105 };
	static const short clipTicks[6] = { 90, 90, 90, 100, 90, 120 };
	GrafPtr wasPort;
	PicHandle pict;
	long dummy;

	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);
	pict = GetPicture(isColor ? kAnnouncerPict4ID : kAnnouncerPict1ID);
	if (pict)
	{
		Rect dest = replayRect;
		DrawPicture(pict, &dest);
		ReleaseResource((Handle)pict);
	}
	for (int i = 0; i < 6; i++)
	{
		if (soundOn && ShimFindAsset(SHIM_FOURCC('S','N','D',' '), clipIDs[i]))
		{
			extern void SMSSTART (short);
			SMSSTART(clipIDs[i]);
			Delay(clipTicks[i], &dummy);
		}
	}
	/* restore what the announcer covered */
	CopyBits(&((GrafPtr)offCWorkPtr)->portBits, &(((GrafPtr)mainWndo)->portBits),
	         &replayRect, &replayRect, srcCopy, NULL);
	SetPort(wasPort);
}
