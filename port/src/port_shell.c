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

void PortInputSetPlayMode (int playing);
void PortFourRun (int mode, const int personas[4]);   /* port_four.c */

int portCpuDemo = 0;          /* --cpu-demo: auto-start George vs Mara */
int portFourDemo = 0;         /* --four-demo N: auto-run one 4P AI game */

/* menu model; items are shown/hidden depending on the selected mode */
enum { MI_START, MI_MODE, MI_GAME, MI_OPPONENT, MI_LEAGUE, MI_SIDE,
       MI_P2, MI_P3, MI_P4, MI_SOUND, MI_FULLSCREEN, MI_QUIT, MI_ITEMS };
static int menuSel = 0;        /* index into the visible-item list */
static int selMode = 0;        /* 0 = classic 1v1, 1 = 2v2, 2 = FFA-2, 3 = FFA-4 */
static int selGame = 0;        /* 0 standard, 1 tournament, 2 boardin', 3 scorin' */
static int selOpponent = 5;    /* persona-1: default Ms. Teak (index 5 = persona 6) */
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
	visItems[n++] = MI_SOUND;
	visItems[n++] = MI_FULLSCREEN;
	visItems[n++] = MI_QUIT;
	visCount = n;
	if (menuSel >= visCount)
		menuSel = visCount - 1;
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
		case MI_SOUND: snprintf(buf, bufsz, "%s", soundOn ? "ON" : "OFF"); break;
		case MI_FULLSCREEN: snprintf(buf, bufsz, "%s", PortVideoIsFullscreen() ? "ON" : "OFF"); break;
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
		case MI_SOUND: return "SOUND";
		case MI_FULLSCREEN: return "FULLSCREEN";
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

	buildMenuItems();
	char val[48];
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
	drawText((short)(panel.left + 16), (short)(panel.bottom - 14),
	         "ARROWS: MOVE  RETURN: SELECT  F11: FULLSCREEN", IDX_GRAY);

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
		case MI_SOUND: soundOn = !soundOn; break;
		case MI_FULLSCREEN: PortVideoSetFullscreen(!PortVideoIsFullscreen()); break;
		default: break;
	}
	menuDirty = 1;
}

static void menuActivate (void)
{
	buildMenuItems();
	switch (visItems[menuSel])
	{
		case MI_START: startGame(); break;
		case MI_QUIT: quitting = TRUE; break;
		default: menuAdjust(1); break;
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

	/* paused mid-game: draw a notice, wait for Tab to resume */
	if (primaryMode == kPlayMode && pausing)
	{
		drawText((short)(screenWide / 2 - 100), 20, "PAUSED - PRESS TAB TO RESUME", IDX_YELLOW);
		if (keyPressedOnce(SDL_SCANCODE_TAB))
			pausing = FALSE;
		SDL_Delay(10);
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
	if (keyPressedOnce(SDL_SCANCODE_UP)) { menuSel = (menuSel + visCount - 1) % visCount; menuDirty = 1; }
	if (keyPressedOnce(SDL_SCANCODE_DOWN)) { menuSel = (menuSel + 1) % visCount; menuDirty = 1; }
	if (keyPressedOnce(SDL_SCANCODE_LEFT)) menuAdjust(-1);
	if (keyPressedOnce(SDL_SCANCODE_RIGHT)) menuAdjust(1);
	if (keyPressedOnce(SDL_SCANCODE_RETURN) || keyPressedOnce(SDL_SCANCODE_KP_ENTER)
	    || keyPressedOnce(SDL_SCANCODE_SPACE))
		menuActivate();
	if (keyPressedOnce(SDL_SCANCODE_ESCAPE) && !splashIsUp)
	{
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
		if (u && !prevU) { menuSel = (menuSel + visCount - 1) % visCount; menuDirty = 1; }
		if (d && !prevD) { menuSel = (menuSel + 1) % visCount; menuDirty = 1; }
		if (l && !prevL) menuAdjust(-1);
		if (r && !prevR) menuAdjust(1);
		if (s && !prevS) menuActivate();
		prevU = u; prevD = d; prevL = l; prevR = r; prevS = s;
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
