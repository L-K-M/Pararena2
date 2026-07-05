/* port_stubs.c — link stubs for original modules that are not compiled in
 * the port: networking (AppleTalk), the asm renderers, the dissolve effect,
 * menus, dialogs, cursors. Everything here is either unreachable (guarded by
 * canNetwork/netGameInSession/useQD) or cosmetic.
 */

#include "shim_internal.h"
#include "Globals.h"

/* ---- globals owned by modules we don't compile ---- */
Boolean imTheMaster = TRUE;          /* AppleTalkDDP.c */
short   wasMenuBarHeight = 0;        /* Menu.c / MainWindow.c consumers */

/* ---- AppleTalkDDP.c / NetOpponent.c (netGameInSession is always FALSE) ---- */
short  InitializeAppleTalk (void) { return 1; /* != kNetErrNoErr */ }
short  CloseDownAppleTalk (void) { return 0; }
Boolean SendModalMessage (short messageType) { (void)messageType; return TRUE; }
void   SendMessage (void) {}
void   ReceiveMessage (void) {}
void   PrepareStandardMessage (void) {}
void   InitializeMessages (void) {}
Boolean WaitForSynch (long tickOut) { (void)tickOut; return FALSE; }
Boolean WhatsTheGamesOutcome (void) { return TRUE; }
Boolean ConfirmEnvironmentMatch (void) { return FALSE; }
Boolean SpotPoints (void) { return FALSE; }
OSErr  RegisterNameOnNet (void) { return -1; }
OSErr  DeRegisterName (void) { return -1; }
Boolean SelectNetOpponentAsynch (void) { return FALSE; }
Str255 opponentsName;

/* ---- RenderAsm1.c / RenderAsm4.c (useQD is always TRUE) ---- */
void RenderSceneAsm1 (void) {}
void RenderSceneAsm4 (void) {}
void ReplayWorkToMainAsm1 (void) {}
void ReplayWorkToMainAsm4 (void) {}

/* ---- DissBits.c: splash-to-arena dissolve; simple block reveal instead ---- */
extern GrafPtr shimMainWndoPort (void);
void DissolveWorkToMain (void)
{
	/* reveal the work buffer in coarse random-ish column strips for a nod to
	 * the original LFSR dissolve, then present */
	extern CGrafPtr offCWorkPtr;
	extern WindowPtr mainWndo;
	extern Rect offWorkRect;
	Rect strip;
	long dummy;
	int w = offWorkRect.right, cols = 32, cw = (w + cols - 1) / cols;
	for (int pass = 0; pass < 2; pass++)
	{
		for (int c = pass; c < cols; c += 2)
		{
			strip.left = (short)(c * cw);
			strip.right = (short)((c + 1) * cw < w ? (c + 1) * cw : w);
			strip.top = 0;
			strip.bottom = offWorkRect.bottom;
			CopyBits(&((GrafPtr)offCWorkPtr)->portBits, &(((GrafPtr)mainWndo)->portBits),
			         &strip, &strip, srcCopy, NULL);
			if ((c & 7) == 7)
				Delay(1, &dummy);
		}
	}
}

/* ---- Menu.c ---- */
void SetMBarToPlaying (void) {}
void SetMBarToIdle (void) {}
void ValidateMenuBar (void) {}
void CheckGameMenu (void) {}

/* ---- PlayerStats.c: transcribed verbatim (the rest of that file is the
 * stats dialog, not yet ported) ---- */
extern string31 mostTitlesName, mostPointsName, mostFoulsName, mostCritsName;
extern long mostTitlesDate, mostPointsDate, mostFoulsDate, mostCritsDate;
extern short mostTitles, mostPoints, mostFouls, mostCrits;
void PasStringCopy (StringPtr, StringPtr);

void ResetWorldRecords (void)
{
	PasStringCopy((StringPtr)"\006No one", (StringPtr)mostTitlesName);
	GetDateTime((unsigned long *)&mostTitlesDate);
	mostTitles = 0;
	PasStringCopy((StringPtr)"\006No one", (StringPtr)mostPointsName);
	GetDateTime((unsigned long *)&mostPointsDate);
	mostPoints = 0;
	PasStringCopy((StringPtr)"\006No one", (StringPtr)mostFoulsName);
	GetDateTime((unsigned long *)&mostFoulsDate);
	mostFouls = 0;
	PasStringCopy((StringPtr)"\006No one", (StringPtr)mostCritsName);
	GetDateTime((unsigned long *)&mostCritsDate);
	mostCrits = 0;
}

/* ---- AnimCursor.c ---- */
void DisposCursors (void) {}
void SpinCursor (short increment) { (void)increment; }
void StopSpinning (void) {}

/* ---- PlayerStats.c: called when a game/tournament finishes ---- */
void UpdateStats (void);         /* real one lives in PlayUtils.c */

/* ---- IdleRoutines.c pieces referenced from compiled code ---- */
void DoInstantReplayOptions (void) {}
