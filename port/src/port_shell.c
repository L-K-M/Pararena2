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

/* ---- per-player identity, chosen on the pre-match setup screen ----
 * Indexed by seat (0 = P1 — the 1v1 human or four-player seat 0 — then 1..3).
 * Each human picks a roster character, a badge colour, and a 3-char name; the
 * name + colour are drawn on their in-game head plate. */
static const char *rosterNames[6] = { "GEORGE", "MARA", "OTTO", "CLAIRE", "EAZE", "TEAK" };
/* badge colours the player can pick, as Apple-palette indices */
static const unsigned char colorChoices[8] = { 6, 7, 8, 1, 2, 3, 4, 5 };
static const char *colorNames[8] = { "BLUE", "CYAN", "GREEN", "YELLOW",
                                     "ORANGE", "RED", "MAGENTA", "PURPLE" };
#define ROSTER_COUNT 6
#define COLOR_COUNT  8

static int  portPlayerChar[4]  = { 0, 1, 2, 3 };   /* roster index 0..5 */
static int  portPlayerColorI[4] = { 0, 5, 2, 6 };  /* index into colorChoices */
static char portPlayerName[4][4];                  /* up to 3 chars + NUL */

static void defaultNameFor (int roster, char *out)   /* first 3 letters, upper */
{
	const char *s = rosterNames[roster % ROSTER_COUNT];
	int i = 0;
	for (; i < 3 && s[i]; i++)
	{
		char c = s[i];
		out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	for (; i < 3; i++) out[i] = ' ';
	out[3] = 0;
}

/* seed any still-empty names from the seat's default character */
void PortPlayersInit (void)
{
	for (int s = 0; s < 4; s++)
		if (!portPlayerName[s][0])
			defaultNameFor(portPlayerChar[s], portPlayerName[s]);
}

/* accessors for the in-game head plates (port_four.c) */
const char *PortPlayerName (int seat)
{
	if (seat < 0 || seat >= 4) return NULL;
	if (!portPlayerName[seat][0]) defaultNameFor(portPlayerChar[seat], portPlayerName[seat]);
	return portPlayerName[seat];
}

unsigned char PortPlayerColor (int seat)
{
	int ci = (seat >= 0 && seat < 4) ? portPlayerColorI[seat] : 0;
	return colorChoices[ci % COLOR_COUNT];
}

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
		/* PLAY SIDE now lives on the pre-match player-setup card */
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
	PortPlayersInit();
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

/* ================= pre-match player setup ================= */
/* Fields on a setup card, in navigation order. SF_SIDE is present only in 1v1. */
enum { SF_CHAR, SF_COLOR, SF_NAME0, SF_NAME1, SF_NAME2, SF_SIDE };
#define SC_CHAR_Y   54
#define SC_COLOR_Y  78
#define SC_NAME_Y   108
#define SC_SIDE_Y   138

/* cycle a name character through {space, A..Z} */
static char cycleChar (char c, int dir)
{
	int idx = (c >= 'A' && c <= 'Z') ? (c - 'A' + 1) : 0;
	idx = (idx + dir + 27) % 27;
	return idx == 0 ? ' ' : (char)('A' + idx - 1);
}

/* when the character changes, follow the default name only while it is unedited */
static void setupSetChar (int seat, int dir)
{
	char oldDef[4], newDef[4];
	defaultNameFor(portPlayerChar[seat], oldDef);
	portPlayerChar[seat] = (portPlayerChar[seat] + dir + ROSTER_COUNT) % ROSTER_COUNT;
	defaultNameFor(portPlayerChar[seat], newDef);
	if (strcmp(portPlayerName[seat], oldDef) == 0)
		strcpy(portPlayerName[seat], newDef);
}

static void setupButtonRects (Rect *ready, Rect *back)
{
	short by = (short)(screenHigh / 2 + 120 - 38);
	SetRect(ready, (short)(screenWide / 2 - 150), by, (short)(screenWide / 2 - 30), (short)(by + 24));
	SetRect(back,  (short)(screenWide / 2 + 30),  by, (short)(screenWide / 2 + 150), (short)(by + 24));
}

static void drawSetupButton (const BitMap *scr, const Rect *r, const char *label, int lit)
{
	RGBColor c;
	pxFillRect(scr, IDX_BLACK, r->left, r->top, r->right, r->bottom);
	Index2Color(lit ? IDX_YELLOW : IDX_WHITE, &c); RGBForeColor(&c);
	FrameRect((Rect *)r);
	drawText((short)((r->left + r->right) / 2 - (short)(strlen(label) * 4)),
	         (short)(r->bottom - 7), label, lit ? IDX_YELLOW : IDX_WHITE);
}

static void drawSetupCard (int seat, int focus, int nFields, int labelNum,
                           int hasSide, const char *ctrl)
{
	GrafPtr wasPort; RGBColor c;
	const BitMap *scr = &(((GrafPtr)mainWndo)->portBits);
	Rect panel; char buf[16];
	int lx, vx, top;
	unsigned char badge = colorChoices[portPlayerColorI[seat]];

	GetPort(&wasPort); SetPort((GrafPtr)mainWndo);
	SetRect(&panel, (short)(screenWide / 2 - 190), (short)(screenHigh / 2 - 120),
	                (short)(screenWide / 2 + 190), (short)(screenHigh / 2 + 120));
	PmForeColor(IDX_BLACK); PaintRect(&panel);
	Index2Color(IDX_WHITE, &c); RGBForeColor(&c);
	FrameRect(&panel); { Rect in = panel; InsetRect(&in, 2, 2); FrameRect(&in); }

	top = panel.top; lx = panel.left + 28; vx = panel.left + 150;
	snprintf(buf, sizeof buf, "PLAYER %d", labelNum);
	drawText((short)(screenWide / 2 - (short)(strlen(buf) * 4)), (short)(top + 22), buf, IDX_YELLOW);

	/* CHARACTER (drawn in the badge colour) */
	drawText((short)lx, (short)(top + SC_CHAR_Y), focus == SF_CHAR ? ">" : " ",
	         focus == SF_CHAR ? IDX_YELLOW : IDX_WHITE);
	drawText((short)(lx + 16), (short)(top + SC_CHAR_Y), "CHARACTER",
	         focus == SF_CHAR ? IDX_YELLOW : IDX_WHITE);
	drawText((short)vx, (short)(top + SC_CHAR_Y), rosterNames[portPlayerChar[seat]], badge);

	/* COLOUR (name + swatch) */
	drawText((short)lx, (short)(top + SC_COLOR_Y), focus == SF_COLOR ? ">" : " ",
	         focus == SF_COLOR ? IDX_YELLOW : IDX_WHITE);
	drawText((short)(lx + 16), (short)(top + SC_COLOR_Y), "COLOUR",
	         focus == SF_COLOR ? IDX_YELLOW : IDX_WHITE);
	drawText((short)vx, (short)(top + SC_COLOR_Y), colorNames[portPlayerColorI[seat]],
	         focus == SF_COLOR ? IDX_YELLOW : IDX_WHITE);
	pxFillRect(scr, IDX_WHITE, vx + 118, top + SC_COLOR_Y - 9, vx + 140, top + SC_COLOR_Y + 1);
	pxFillRect(scr, badge, vx + 119, top + SC_COLOR_Y - 8, vx + 139, top + SC_COLOR_Y);

	/* NAME (three slots) */
	drawText((short)lx, (short)(top + SC_NAME_Y),
	         (focus >= SF_NAME0 && focus <= SF_NAME2) ? ">" : " ",
	         (focus >= SF_NAME0 && focus <= SF_NAME2) ? IDX_YELLOW : IDX_WHITE);
	drawText((short)(lx + 16), (short)(top + SC_NAME_Y), "NAME",
	         (focus >= SF_NAME0 && focus <= SF_NAME2) ? IDX_YELLOW : IDX_WHITE);
	for (int i = 0; i < 3; i++)
	{
		int sx = vx + i * 30, active = (focus == SF_NAME0 + i);
		char ch[2];
		pxFillRect(scr, active ? IDX_YELLOW : IDX_GRAY, sx - 2, top + SC_NAME_Y - 14, sx + 20, top + SC_NAME_Y + 4);
		pxFillRect(scr, IDX_BLACK, sx, top + SC_NAME_Y - 12, sx + 18, top + SC_NAME_Y + 2);
		ch[0] = portPlayerName[seat][i] ? portPlayerName[seat][i] : ' '; ch[1] = 0;
		drawText((short)(sx + 5), (short)(top + SC_NAME_Y), ch, active ? IDX_YELLOW : IDX_WHITE);
	}

	/* PLAY SIDE (1v1 only) */
	if (hasSide)
	{
		drawText((short)lx, (short)(top + SC_SIDE_Y), focus == SF_SIDE ? ">" : " ",
		         focus == SF_SIDE ? IDX_YELLOW : IDX_WHITE);
		drawText((short)(lx + 16), (short)(top + SC_SIDE_Y), "PLAY SIDE",
		         focus == SF_SIDE ? IDX_YELLOW : IDX_WHITE);
		drawText((short)vx, (short)(top + SC_SIDE_Y), selSide == 0 ? "LEFT GOAL" : "RIGHT GOAL",
		         focus == SF_SIDE ? IDX_YELLOW : IDX_WHITE);
	}

	/* control hint */
	drawText((short)lx, (short)(top + (hasSide ? 166 : 150)), "CONTROLS:", IDX_GRAY);
	drawText((short)(lx + 84), (short)(top + (hasSide ? 166 : 150)), ctrl, IDX_GRAY);

	if (shimMobile)
	{
		Rect ready, back;
		setupButtonRects(&ready, &back);
		drawSetupButton(scr, &ready, "READY", 1);
		drawSetupButton(scr, &back, "BACK", 0);
		drawText((short)(screenWide / 2 - 92), (short)(top + 190),
		         "TAP A FIELD TO CHANGE IT", IDX_GRAY);
	}
	else
		drawText((short)(panel.left + 14), (short)(panel.bottom - 12),
		         "MOVE <>   CHANGE ^v   TYPE NAME   ENTER READY   ESC BACK", IDX_GRAY);
	(void)nFields;
	Index2Color(IDX_BLACK, &c); RGBForeColor(&c);
	SetPort(wasPort); shimScreenDirty = 1;
}

/* merged gamepad direction/buttons across all pads (any pad can drive a card) */
static void setupPadState (int *u, int *d, int *l, int *r, int *conf, int *back)
{
	float mx = 0, my = 0;
	for (int p = 0; p < 4; p++)
	{
		float x, y; int b, bk, bs;
		if (ShimPadRead(p, &x, &y, &b, &bk, &bs))
		{
			if ((x < 0 ? -x : x) > (mx < 0 ? -mx : mx)) mx = x;
			if ((y < 0 ? -y : y) > (my < 0 ? -my : my)) my = y;
		}
	}
	*u = my < -0.5f; *d = my > 0.5f; *l = mx < -0.5f; *r = mx > 0.5f;
	*conf = ShimAnyPadButton(SDL_GAMEPAD_BUTTON_SOUTH) || ShimAnyPadButton(SDL_GAMEPAD_BUTTON_START);
	*back = ShimAnyPadButton(SDL_GAMEPAD_BUTTON_EAST);
}

/* One player's card. Returns 1 = ready (advance), 0 = back (cancel), -1 = quit. */
static int runOnePlayerSetup (int seat, int labelNum, int hasSide, const char *ctrl)
{
	int nFields = hasSide ? 6 : 5;
	int focus = SF_CHAR;
	int armed = 0;                                  /* wait for the entry press to release */
	int pu = 0, pd = 0, pl = 0, pr = 0, pconf = 0, pback = 0;
	Uint8 pletter[27] = {0};
	int pbksp = 0;

	if (!portPlayerName[seat][0]) defaultNameFor(portPlayerChar[seat], portPlayerName[seat]);
	shimInput.tapFresh = 0; shimInput.backEdge = 0;

	if (shimHeadless) return 1;

	/* prime the edge state from whatever is held right now, so the key/button/tap
	 * that chose START (Enter/Space/South) doesn't leak a first action into the
	 * card — e.g. a held Space typing itself into the name. */
	{
		ShimPumpEvents();
		const bool *k = SDL_GetKeyboardState(NULL);
		int gu, gd, gl, gr, gc, gb;
		setupPadState(&gu, &gd, &gl, &gr, &gc, &gb);
		pu = k[SDL_SCANCODE_UP] || gu; pd = k[SDL_SCANCODE_DOWN] || gd;
		pl = k[SDL_SCANCODE_LEFT] || gl; pr = k[SDL_SCANCODE_RIGHT] || gr;
		pconf = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || gc;
		pback = k[SDL_SCANCODE_ESCAPE] || gb;
		for (int i = 0; i < 27; i++)
		{
			int sc = (i < 26) ? SDL_SCANCODE_A + i : SDL_SCANCODE_SPACE;
			pletter[i] = (Uint8)(k[sc] ? 1 : 0);
		}
		pbksp = k[SDL_SCANCODE_BACKSPACE] ? 1 : 0;
	}

	for (;;)
	{
		ShimPumpEvents();
		if (shimInput.quitRequested) return -1;

		const bool *ks = SDL_GetKeyboardState(NULL);
		int gu, gd, gl, gr, gconf, gback;
		setupPadState(&gu, &gd, &gl, &gr, &gconf, &gback);

		int cu = ks[SDL_SCANCODE_UP] || gu;
		int cd = ks[SDL_SCANCODE_DOWN] || gd;
		int cl = ks[SDL_SCANCODE_LEFT] || gl;
		int cr = ks[SDL_SCANCODE_RIGHT] || gr;
		int cconf = ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] || gconf;
		int cback = ks[SDL_SCANCODE_ESCAPE] || shimInput.backEdge || gback;

		if (!cconf && !cback) armed = 1;

		/* navigation */
		if (cl && !pl) focus = (focus + nFields - 1) % nFields;
		if (cr && !pr) focus = (focus + 1) % nFields;

		/* change the focused field */
		int dir = (cd && !pd) ? 1 : (cu && !pu) ? -1 : 0;
		if (dir)
		{
			if (focus == SF_CHAR) setupSetChar(seat, dir);
			else if (focus == SF_COLOR) portPlayerColorI[seat] = (portPlayerColorI[seat] + dir + COLOR_COUNT) % COLOR_COUNT;
			else if (focus >= SF_NAME0 && focus <= SF_NAME2)
				portPlayerName[seat][focus - SF_NAME0] = cycleChar(portPlayerName[seat][focus - SF_NAME0], dir);
			else if (focus == SF_SIDE) selSide ^= 1;
		}

		/* keyboard name typing: A-Z and space set the slot and advance */
		for (int i = 0; i < 27; i++)
		{
			int sc = (i < 26) ? SDL_SCANCODE_A + i : SDL_SCANCODE_SPACE;
			int now = ks[sc] ? 1 : 0;
			if (now && !pletter[i])
			{
				if (focus < SF_NAME0 || focus > SF_NAME2) focus = SF_NAME0;
				portPlayerName[seat][focus - SF_NAME0] = (i < 26) ? (char)('A' + i) : ' ';
				if (focus < SF_NAME2) focus++;
			}
			pletter[i] = (Uint8)now;
		}
		{
			int bk = ks[SDL_SCANCODE_BACKSPACE] ? 1 : 0;
			if (bk && !pbksp && focus >= SF_NAME0 && focus <= SF_NAME2)
			{
				if (focus > SF_NAME0) focus--;
				portPlayerName[seat][focus - SF_NAME0] = ' ';
			}
			pbksp = bk;
		}

		/* touch: tap a field to change it, or the READY / BACK buttons */
		if (shimInput.tapFresh)
		{
			int tx = (int)(shimInput.tapX * screenWide), ty = (int)(shimInput.tapY * screenHigh);
			int top = screenHigh / 2 - 120, vx = (screenWide / 2 - 190) + 150;
			Rect ready, back;
			shimInput.tapFresh = 0;
			setupButtonRects(&ready, &back);
			if (tx >= ready.left && tx <= ready.right && ty >= ready.top && ty <= ready.bottom) { if (armed) return 1; }
			else if (tx >= back.left && tx <= back.right && ty >= back.top && ty <= back.bottom) { if (armed) return 0; }
			else if (ty >= top + SC_CHAR_Y - 12 && ty <= top + SC_CHAR_Y + 4) setupSetChar(seat, 1);
			else if (ty >= top + SC_COLOR_Y - 12 && ty <= top + SC_COLOR_Y + 4) portPlayerColorI[seat] = (portPlayerColorI[seat] + 1) % COLOR_COUNT;
			else if (ty >= top + SC_NAME_Y - 14 && ty <= top + SC_NAME_Y + 4)
			{
				int slot = (tx - vx + 4) / 30;
				if (slot >= 0 && slot < 3)
				{
					focus = SF_NAME0 + slot;
					portPlayerName[seat][slot] = cycleChar(portPlayerName[seat][slot], 1);
				}
			}
			else if (hasSide && ty >= top + SC_SIDE_Y - 12 && ty <= top + SC_SIDE_Y + 4) selSide ^= 1;
		}

		if (armed && cconf && !pconf) return 1;
		if (armed && cback && !pback) return 0;

		pu = cu; pd = cd; pl = cl; pr = cr; pconf = cconf; pback = cback;

		drawSetupCard(seat, focus, nFields, labelNum, hasSide, ctrl);
		ShimForcePresent();
		SDL_Delay(10);
	}
}

/* Run setup for every human seat listed. Returns 1 to start the match, 0 if a
 * player backed out (return to the menu). seatLabel[i] is the P# shown. */
static int runPlayerSetup (const int *seats, const int *labels, const char *const *ctrls,
                           int nHumans, int hasSide)
{
	if (shimHeadless || portCpuDemo || portFourDemo)
		return 1;
	for (int i = 0; i < nHumans; i++)
	{
		int r = runOnePlayerSetup(seats[i], labels[i], hasSide && nHumans == 1, ctrls[i]);
		if (r == -1) { quitting = TRUE; return 0; }
		if (r == 0) { menuDirty = 1; return 0; }
	}
	menuDirty = 1;
	return 1;
}

/* --setup-preview: render the 1v1 card in each focus state to the dump dir and
 * quit (there is no way to drive the modal in headless). Testing only. */
static void PortSetupPreview (void)
{
	char path[512];
	if (!shimFrameDumpDir) return;
	strcpy(portPlayerName[0], "LKM");
	portPlayerChar[0] = 0;
	portPlayerColorI[0] = 0;
	for (int focus = SF_CHAR; focus <= SF_SIDE; focus++)
	{
		drawSetupCard(0, focus, 6, 1, 1, "KEYBOARD / MOUSE  OR  GAMEPAD");
		snprintf(path, sizeof path, "%s/setup_%d.ppm", shimFrameDumpDir, focus);
		ShimDumpScreenPPM(path);
	}
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
		int personas[4], seats[4], labels[4], nH = 0;
		const char *ctrls[4];
		personas[0] = kHumanPlayer;
		for (int i = 0; i < 3; i++)
			personas[1 + i] = selSeat[i] == 0 ? kHumanPlayer : selSeat[i];
		for (int i = 0; i < 4; i++)
			if (personas[i] == kHumanPlayer)
			{
				seats[nH] = i;
				labels[nH] = i + 1;
				ctrls[nH] = (i == 0) ? "KEYBOARD / MOUSE" : "GAMEPAD";
				nH++;
			}
		if (!runPlayerSetup(seats, labels, ctrls, nH, 0))
			return;                            /* a player backed out */
		whichHumanNumber = 1;
		PortFourRun(selMode, personas);
		menuDirty = 1;
		return;
	}

	/* 1v1: set up the human (P1); the card also picks the play side */
	if (!portCpuDemo)
	{
		int seats[1] = { 0 }, labels[1] = { 1 };
		const char *ctrls[1] = { "KEYBOARD / MOUSE  OR  GAMEPAD" };
		if (!runPlayerSetup(seats, labels, ctrls, 1, 1))
			return;                            /* backed out -> menu */
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

	if (shimSetupPreview)                     /* testing: dump the setup card and quit */
	{
		PortSetupPreview();
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

/* any input that should skip the announcer intro: Return/Space/Esc, a mouse
 * click / merged button, or a pad face/start button (read directly so it
 * works in split-input 4P games too) */
static int announcerSkipHeld (void)
{
	const bool *ks = SDL_GetKeyboardState(NULL);
	if (ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] ||
	    ks[SDL_SCANCODE_SPACE]  || ks[SDL_SCANCODE_ESCAPE]) return 1;
	if (shimInput.buttonDown || shimInput.brakeDown || shimInput.bashDown) return 1;
	if (ShimAnyPadButton(SDL_GAMEPAD_BUTTON_SOUTH) ||
	    ShimAnyPadButton(SDL_GAMEPAD_BUTTON_START)) return 1;
	return 0;
}

void DoOpeningAnnouncer (void)
{
	/* simplified from IdleRoutines.c: announcer art + the six voice clips.
	 * The full intro is ~10 s; any tap, click, button, or key skips the rest
	 * (unskippable, it reads as a hang — especially on a phone). */
	static const short clipIDs[6] = { 100, 101, 102, 103, 104, 105 };
	static const short clipTicks[6] = { 90, 90, 90, 100, 90, 120 };
	GrafPtr wasPort;
	PicHandle pict;
	int skipped = 0, armed = 0;

	GetPort(&wasPort);
	SetPort((GrafPtr)mainWndo);
	pict = GetPicture(isColor ? kAnnouncerPict4ID : kAnnouncerPict1ID);
	if (pict)
	{
		Rect dest = replayRect;
		DrawPicture(pict, &dest);
		ReleaseResource((Handle)pict);
	}
	shimInput.tapFresh = 0;               /* spend the input that started the game */
	shimInput.backEdge = 0;
	for (int i = 0; i < 6 && !skipped; i++)
	{
		if (!soundOn || !ShimFindAsset(SHIM_FOURCC('S','N','D',' '), clipIDs[i]))
			continue;
		{
			extern void SMSSTART (short);
			SMSSTART(clipIDs[i]);
		}
		long until = Ticks + clipTicks[i];
		while (Ticks < until && !skipped)
		{
			ShimPumpEvents();
			ShimTickSleep();
			if (shimInput.quitRequested)
				skipped = 1;
			else if (shimInput.tapFresh || shimInput.backEdge)
			{
				shimInput.tapFresh = 0;
				shimInput.backEdge = 0;
				skipped = 1;
			}
			else if (!announcerSkipHeld())
				armed = 1;                /* wait for a release, then a fresh press */
			else if (armed)
				skipped = 1;
		}
		if (skipped)
			ShimStopSoundID(clipIDs[i]); /* cut the clip; crowd loop is untouched */
	}
	/* restore what the announcer covered */
	CopyBits(&((GrafPtr)offCWorkPtr)->portBits, &(((GrafPtr)mainWndo)->portBits),
	         &replayRect, &replayRect, srcCopy, NULL);
	SetPort(wasPort);
}
