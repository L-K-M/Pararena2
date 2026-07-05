/* shim_os.c — timing, randomness, and the pile of harmless Toolbox no-ops. */

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <time.h>
#include "shim_internal.h"

long randSeed = 1;

int  shimHeadless = 0;
long shimAutoQuitTicks = 0;
const char *shimFrameDumpDir = NULL;
int  shimFrameDumpEvery = 60;

static Uint64 startNS;
static long   lastDumpTick = -1;
static long   lastTickSeen = -1;
static int    spinReads;

void ShimLog (const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[pararena] ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

int shimTickMult = 1;      /* --fast N: time runs N x faster (testing) */

void ShimTimeInit (void)
{
	startNS = SDL_GetTicksNS();
}

long ShimTicksRead (void)
{
	long t = (long)((SDL_GetTicksNS() - startNS) * 60ull * (unsigned)shimTickMult / 1000000000ull);
	if (t != lastTickSeen)
	{
		lastTickSeen = t;
		spinReads = 0;
		ShimMaybeDumpFrame();
		if (shimAutoQuitTicks > 0 && t >= shimAutoQuitTicks)
		{
			ShimLog("auto-quit after %ld ticks", t);
			exit(0);
		}
	}
	else if (++spinReads > 200)
	{
		/* the game is spinning on the tick counter: yield and stay responsive */
		spinReads = 0;
		ShimPumpEvents();
		SDL_DelayNS(200000);
	}
	return t;
}

void ShimAdvanceTicks (void)
{
	(void)ShimTicksRead();
}

/* sleep politely while the game busy-waits on Ticks */
void ShimTickSleep (void)
{
	if (shimTickMult > 1)
		return;
	Uint64 now = SDL_GetTicksNS() - startNS;
	Uint64 nextTick = ((Uint64)lastTickSeen + 1) * 1000000000ull / 60ull;
	if (nextTick > now + 1500000ull)
		SDL_DelayNS(1000000);      /* 1 ms */
	else if (nextTick > now + 300000ull)
		SDL_DelayNS(100000);       /* 0.1 ms */
}

void Delay (long numTicks, long *finalTicks)
{
	long until = ShimTicksRead() + numTicks;
	while (ShimTicksRead() < until)
	{
		ShimPumpEvents();
		ShimTickSleep();
	}
	if (finalTicks)
		*finalTicks = ShimTicksRead();
}

void GetDateTime (unsigned long *secs)
{
	/* classic Mac epoch: 1904-01-01; unix epoch offset = 2082844800 */
	*secs = (unsigned long)time(NULL) + 2082844800ul;
}

/* QuickDraw Random(): Lehmer LCG, seed = seed * 16807 mod 2^31-1,
 * low word returned as signed, -32768 mapped to 0 (per Inside Macintosh) */
short Random (void)
{
	long long seed = randSeed & 0x7FFFFFFF;
	if (seed <= 0)
		seed += 0x7FFFFFFE;
	seed = (seed * 16807) % 0x7FFFFFFF;
	randSeed = (long)seed;
	short result = (short)(seed & 0xFFFF);
	if ((unsigned short)result == 0x8000)
		result = 0;
	return result;
}

/* ---------------- harmless stubs ---------------- */

void FlushEvents (short whichMask, short stopMask) { (void)whichMask; (void)stopMask; }
void UnloadSeg (void *routineAddr) { (void)routineAddr; }
void ErrorSound (ProcPtr soundProc) { (void)soundProc; }
void SysBeep (short duration) { (void)duration; }
void FlashMenuBar (short menuID) { (void)menuID; }
void DrawMenuBar (void) {}
void InitCursor (void) {}
void HideCursor (void) {}
void ShowCursor (void) {}
void ObscureCursor (void) {}
void ShieldCursor (const Rect *shieldRect, Point offsetPt) { (void)shieldRect; (void)offsetPt; }
void ExitToShell (void) { exit(0); }

static Str255 paramTexts[4];
void ParamText (ConstStr255Param a, ConstStr255Param b, ConstStr255Param c, ConstStr255Param d)
{
	if (a) memcpy(paramTexts[0], a, (size_t)a[0] + 1);
	if (b) memcpy(paramTexts[1], b, (size_t)b[0] + 1);
	if (c) memcpy(paramTexts[2], c, (size_t)c[0] + 1);
	if (d) memcpy(paramTexts[3], d, (size_t)d[0] + 1);
}

short Alert (short alertID, void *filterProc)
{
	(void)filterProc;
	ShimLog("Alert %d suppressed (returning default button 1)", alertID);
	return 1;
}

GDHandle GetMainDevice (void) { return NULL; }
OSErr SetDepth (GDHandle gd, short depth, short whichFlags, short flags)
{
	(void)gd; (void)depth; (void)whichFlags; (void)flags;
	return noErr;
}

Boolean GetOSEvent (short mask, EventRecord *theEvent)
{
	(void)mask;
	memset(theEvent, 0, sizeof(*theEvent));
	return FALSE;
}

/* file stubs — only reachable from RenderQD.c's developer PICT dumper */
OSErr GetVInfo (short drvNum, StringPtr volName, short *vRefNum, long *freeBytes)
{
	(void)drvNum; (void)volName; (void)vRefNum; (void)freeBytes;
	return -1;
}
OSErr Create (ConstStr255Param fileName, short vRefNum, OSType creator, OSType fileType)
{
	(void)fileName; (void)vRefNum; (void)creator; (void)fileType;
	return -1;
}
OSErr FSOpen (ConstStr255Param fileName, short vRefNum, short *refNum)
{
	(void)fileName; (void)vRefNum; (void)refNum;
	return -1;
}
OSErr FSWrite (short refNum, long *count, const void *buffPtr)
{
	(void)refNum; (void)count; (void)buffPtr;
	return -1;
}
OSErr FSClose (short refNum) { (void)refNum; return -1; }

/* ---------------- frame dumping (headless verification) ---------------- */

void ShimMaybeDumpFrame (void)
{
	if (!shimFrameDumpDir)
		return;
	long bucket = lastTickSeen / shimFrameDumpEvery;
	if (bucket == lastDumpTick)
		return;
	lastDumpTick = bucket;
	char path[512];
	snprintf(path, sizeof path, "%s/frame_%06ld.ppm", shimFrameDumpDir, lastTickSeen);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	int w = screenBits.bounds.right, h = screenBits.bounds.bottom;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	const uint8_t *pix = (const uint8_t *)screenBits.baseAddr;
	for (long i = 0; i < (long)w * h; i++)
		fwrite(shimPaletteRGBA[pix[i] & 15], 1, 3, f);
	fclose(f);
}
