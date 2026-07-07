/* port_init.c — replaces Initialize.c + Environ.c: fixes the environment to
 * the 640x480 16-color "13-inch" mode, builds the four offscreen buffers,
 * loads sounds, and then walks the original InitializeAll() sequence using
 * the verbatim InitGameStructs.c / MainWindow.c code.
 */

#include "shim_internal.h"
#include "Globals.h"
#include "UnivUtilities.h"
#include "InitGameStructs.h"
#include "MainWindow.h"
#include "SoundUtils.h"
#include "Render.h"
#include "Initialize.h"
#include "Environ.h"

/* provided by port_shell.c (transcribed from TeamSetUp.c) */
void WhosOnFirst (void);
void PortInputSetPlayMode (int playing);
void PortShellSyncFromPrefs (void);

static uint8_t *portAlloc (BitMap *bm, int w, int h)
{
	uint8_t *pix = (uint8_t *)calloc((size_t)w * h, 1);
	bm->baseAddr = (Ptr)pix;
	bm->rowBytes = (short)w;
	bm->bounds.left = 0;
	bm->bounds.top = 0;
	bm->bounds.right = (short)w;
	bm->bounds.bottom = (short)h;
	return pix;
}

static void portSetup (GrafPtr p, BitMap *bm)
{
	memset(p, 0, sizeof(*p));
	p->portBits = *bm;
	p->portRect = bm->bounds;
	p->fgIndex = 15;               /* black */
	p->bkIndex = 0;                /* white */
	p->pnMode = 8;                 /* patCopy */
	p->txMode = srcOr;
	memset(p->pnPat, 0xFF, 8);
}

static void loadImageInto (int assetId, BitMap *bm)
{
	const ShimAsset *a = ShimFindAsset(SHIM_FOURCC('P','I','X',' '), assetId);
	if (!a)
	{
		ShimLog("missing image asset %d", assetId);
		DeathError(kErrNoPictRsrc);
	}
	int w = (int)a->w, h = (int)a->h;
	if (w > bm->bounds.right) w = bm->bounds.right;
	if (h > bm->bounds.bottom) h = bm->bounds.bottom;
	for (int y = 0; y < h; y++)
		memcpy((uint8_t *)bm->baseAddr + (long)y * bm->rowBytes, a->data + (long)y * a->w, (size_t)w);
}

static void buffersInit (void)
{
	const ShimAsset *parts = ShimFindAsset(SHIM_FOURCC('P','I','X',' '), rPartsPict4BitID);
	const ShimAsset *mask = ShimFindAsset(SHIM_FOURCC('P','I','X',' '), rMaskPict48ID);
	if (!parts || !mask)
		DeathError(kErrNoPictRsrc);

	/* parts (sprite sheet) */
	SetRect(&offPartsRect, 0, 0, (short)parts->w, (short)parts->h);
	partsRowBytes = (short)parts->w;
	offPartsPix = (Ptr)portAlloc(&offPartsBits, parts->w, parts->h);
	portSetup(&offCPartsPort, &offPartsBits);
	offCPartsPtr = &offCPartsPort;
	offPartsPtr = (GrafPtr)&offCPartsPort;
	loadImageInto(rPartsPict4BitID, &offPartsBits);

	/* work + back at screen size */
	SetRect(&offWorkRect, 0, 0, screenWide, screenHigh);
	workRowBytes = (short)screenWide;
	offWorkPix = (Ptr)portAlloc(&offWorkBits, screenWide, screenHigh);
	portSetup(&offCWorkPort, &offWorkBits);
	offCWorkPtr = &offCWorkPort;
	offWorkPtr = (GrafPtr)&offCWorkPort;

	SetRect(&offBackRect, 0, 0, screenWide, screenHigh);
	backRowBytes = (short)screenWide;
	offBackPix = (Ptr)portAlloc(&offBackBits, screenWide, screenHigh);
	portSetup(&offCBackPort, &offBackBits);
	offCBackPtr = &offCBackPort;
	offBackPtr = (GrafPtr)&offCBackPort;

	/* mask sheet: 0/1 coverage (color mask, PICT 1021) */
	SetRect(&offMaskRect, 0, 0, (short)mask->w, (short)mask->h);
	maskRowBytes = (short)mask->w;
	offMaskPix = (Ptr)portAlloc(&offMaskMap, mask->w, mask->h);
	portSetup(&offCMaskPort, &offMaskMap);
	offCMaskPtr = &offCMaskPort;
	offMaskPtr = (GrafPtr)&offCMaskPort;
	loadImageInto(rMaskPict48ID, &offMaskMap);
}

static void loadSoundsPort (void)
{
	GetSoundVol(&wasSoundVolume);
	SetSoundVol(soundVolume);

	soundPriorities[0] = 0;
	soundPriorities[kClashSound] = kClashPriority;
	soundPriorities[kRicochetSound] = kRicochetPriority;
	soundPriorities[kScoreSound] = kScorePriority;
	soundPriorities[kBallFiringSound] = kBallFiringPriority;
	soundPriorities[kBeamInSound] = kBeamInPriority;
	soundPriorities[kBeamOutSound] = kBeamOutPriority;
	soundPriorities[kFoulSound] = kFoulPriority;
	soundPriorities[kBallPickUpSound] = kBallPickUpPriority;
	soundPriorities[kCrowdSound] = kCrowdPriority;
	soundPriorities[kCrowdSwellSound] = kCrowdSwellPriority;
	soundPriorities[kApplauseSound] = kApplausePriority;
	soundPriorities[kCrowdFadeSound] = kCrowdFadePriority;
	soundPriorities[kBellSound] = kBellPriority;
	soundPriorities[kMobSwellSound] = kMobSwellPriority;
	soundPriorities[kMobSound] = kMobPriority;
	soundPriorities[kMobFadeSound] = kMobFadePriority;
	soundPriorities[kHoldingSound] = kHoldingPriority;
	soundPriorities[kGameSound] = kGamePriority;
	soundPriorities[kPointSound] = kPointPriority;
	soundPriorities[kBallDropSound] = kBallDropPriority;
	soundPriorities[kBrakeSound] = kBrakePriority;
	soundPriorities[kAllSound] = kAllPriority;
	soundPriorities[kTiedSound] = kTiedPriority;
	soundPriorities[kIdleSound] = kIdlePriority;
	soundPriorities[kOverSound] = kOverPriority;

	for (short i = 1; i < kMaxNumberOfSounds; i++)
		soundLoaded[i] = (ShimFindAsset(SHIM_FOURCC('S','N','D',' '), i) != NULL);
	for (short i = kMaxNumberOfSounds; i <= kLastIncidentalSounds; i++)
		incidentSoundLoaded[i - kMaxNumberOfSounds] =
			(ShimFindAsset(SHIM_FOURCC('S','N','D',' '), i) != NULL);
	soundFileRefNum = 1;   /* "external sound file present" */
}

void InitializeAll (void)
{
	/* fixed environment: 640x480, 16 colors, QuickDraw renderer */
	thisMac.wasDepth = 4;
	thisMac.isDepth = 4;
	thisMac.hasWNE = TRUE;
	thisMac.hasSystem7 = TRUE;
	thisMac.hasColor = TRUE;
	thisMac.hasGestalt = TRUE;
	thisMac.canSwitch = FALSE;
	thisMac.canColor = TRUE;
	isColor = TRUE;
	knowsColor = TRUE;
	isDepth = kDisplay4Bit;
	displayMode = kDisplay13Inch;
	useQD = TRUE;
	willUseQD = TRUE;
	autoSetDepth = FALSE;

	LoadThePreferences();          /* verbatim; validates + defaults */
	useQD = TRUE;                  /* the asm renderers do not exist here */
	willUseQD = TRUE;
	doSkipFrames = FALSE;
	netOnly = FALSE;
	encryptedNumber = 0;
	canNetwork = FALSE;
	if (theOpponent.persona == kNetHuman)
		theOpponent.persona = kMissTeak;
	PortShellSyncFromPrefs();      /* seed the menu from the loaded prefs */

	LoadLargeDataStructures();     /* verbatim: forc/vert/sPts via GetResource */
	VarInit();                     /* verbatim: all rects, mouseFrame, replay buffer */
	InitBallData();
	InitPlayerData();
	InitOpponentData();
	InitDigiDispData();
	OpenMainWindow();              /* verbatim (window == the shim screen port) */
	DoSplashScreen();              /* verbatim */
	buffersInit();
	WhosOnFirst();                 /* transcribed from TeamSetUp.c */
	UpdateGoalPicts(FALSE);        /* verbatim: arena into back+work, goal colors */
	loadSoundsPort();
	speedFlag = 1000000;           /* SpeedTest(): only used for net master election */

	PortInputSetPlayMode(0);
	PortLifecycleInit();           /* Android background/foreground watch; after
	                                * LoadThePreferences so a save never writes
	                                * uninitialized prefs */
	ShimForcePresent();
}
