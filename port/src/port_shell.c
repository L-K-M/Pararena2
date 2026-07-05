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

extern short soundVolume;     /* declared in ConfigureSound.h (not included here) */

int portCpuDemo = 0;          /* --cpu-demo: auto-start George vs Mara */

/* menu model: a main page and an options page */
enum { PAGE_MAIN, PAGE_OPTIONS };
enum { MI_START, MI_GAME, MI_OPPONENT, MI_LEAGUE, MI_SIDE, MI_OPTIONS, MI_QUIT, MI_COUNT };
enum { OI_VOLUME, OI_SOUND, OI_CURSOR, OI_ANNOUNCER, OI_RGOALS, OI_RFOULS, OI_RKEY,
       OI_FULLSCREEN, OI_BACK, OI_COUNT };
static int menuPage = PAGE_MAIN;
static int menuSel = MI_START;
static int optSel = OI_VOLUME;
static int selGame = 0;        /* 0 standard, 1 tournament, 2 boardin', 3 scorin' */
static int selOpponent = 0;    /* persona-1: default Simple George (the game's own default) */
static int selLeague = 4;      /* professional */
static int selSide = 0;        /* 0 = defend left goal */
static int wasPlaying = 0;
static int menuDirty = 1;

static const char *gameNames[4] = { "STANDARD GAME", "TOURNAMENT", "PRACTICE BOARDIN'", "PRACTICE SCORIN'" };
static const short gameConsts[4] = { kStandardGame, kTournament, kPracticeBoardin, kPracticeScoring };
static const char *oppNames[6] = { "SIMPLE GEORGE", "MAD MARA", "HEAVY OTTO", "CLEVER CLAIRE", "MR. EAZE", "MS. TEAK" };
static const char *leagueNames[5] = { "LITTLE LEAGUE", "JUNIOR VARSITY", "VARSITY", "MINOR LEAGUE", "PROFESSIONAL" };

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
		case MI_GAME: snprintf(buf, bufsz, "%s", gameNames[selGame]); break;
		case MI_OPPONENT: snprintf(buf, bufsz, "%s", oppNames[selOpponent]); break;
		case MI_LEAGUE: snprintf(buf, bufsz, "%s", leagueNames[selLeague]); break;
		case MI_SIDE: snprintf(buf, bufsz, "%s", selSide == 0 ? "LEFT GOAL" : "RIGHT GOAL"); break;
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
		default: buf[0] = 0; break;
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

	static const char *mainLabels[MI_COUNT] = {
		"START GAME", "GAME", "OPPONENT", "LEAGUE", "PLAY SIDE", "OPTIONS...", "QUIT"
	};
	static const char *optLabels[OI_COUNT] = {
		"VOLUME", "SOUND", "BOARD CURSOR", "ANNOUNCER", "REPLAY GOALS",
		"REPLAY FOULS", "REPLAY KEY R", "FULLSCREEN", "BACK"
	};
	char val[48];
	if (menuPage == PAGE_MAIN)
	{
		for (int i = 0; i < MI_COUNT; i++)
		{
			short y = (short)(panel.top + 56 + i * 20);
			unsigned char col = (i == menuSel) ? IDX_YELLOW : IDX_WHITE;
			drawText((short)(panel.left + 28), y, i == menuSel ? ">" : " ", col);
			drawText((short)(panel.left + 44), y, mainLabels[i], col);
			menuValueText(i, val, sizeof val);
			if (val[0])
				drawText((short)(panel.left + 170), y, val, col);
		}
	}
	else
	{
		for (int i = 0; i < OI_COUNT; i++)
		{
			short y = (short)(panel.top + 46 + i * 18);
			unsigned char col = (i == optSel) ? IDX_YELLOW : IDX_WHITE;
			drawText((short)(panel.left + 28), y, i == optSel ? ">" : " ", col);
			drawText((short)(panel.left + 44), y, optLabels[i], col);
			optionValueText(i, val, sizeof val);
			if (val[0])
				drawText((short)(panel.left + 190), y, val, col);
		}
	}
	drawText((short)(panel.left + 16), (short)(panel.bottom - 14),
	         "ARROWS: MOVE  RETURN: SELECT  F11: FULLSCREEN", IDX_GRAY);

	Index2Color(IDX_BLACK, &c);
	RGBForeColor(&c);
	SetPort(wasPort);
}

static void startGame (void)
{
	whichGame = gameConsts[selGame];
	if (whichGame == kTournament)
		selLeague = 4;                     /* tournaments are Professional-league */
	isLeague = (short)selLeague;
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
		menuSel = (menuSel + MI_COUNT + dir) % MI_COUNT;
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
		switch (menuSel)
		{
			case MI_GAME: selGame = (selGame + dir + 4) % 4; break;
			case MI_OPPONENT: selOpponent = (selOpponent + dir + 6) % 6; break;
			case MI_LEAGUE: selLeague = (selLeague + dir + 5) % 5; break;
			case MI_SIDE: selSide ^= 1; break;
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
		switch (menuSel)
		{
			case MI_START: startGame(); break;
			case MI_OPTIONS: menuPage = PAGE_OPTIONS; optSel = OI_VOLUME; menuDirty = 1; break;
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

	if (portCpuDemo)
	{
		startGame();
		wasPlaying = 1;
		return;
	}

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
			menuSel = MI_QUIT;
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
