/* MacShim.h — classic Mac Toolbox shim for the Pararena 2 SDL port.
 *
 * Force-included (-include) into every original game source file so the
 * 1992 THINK C code compiles unmodified. Types mirror Inside Macintosh
 * closely enough for source compatibility; there is no binary compatibility
 * and none is needed. Pixel model: every BitMap/port is 8 bits per pixel
 * holding Apple 4-bit palette indices 0-15 (masks hold 0/1).
 */

#ifndef MACSHIM_H
#define MACSHIM_H

/* ---- keywords / basic macros ---- */
#define pascal
#ifndef nil
#define nil 0L
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0L
#endif

/* ---- scalar types (long stays the host's long; the game's arithmetic is
 *      sound with 64-bit intermediates and nothing binary is serialized) ---- */
typedef unsigned char   Boolean;
typedef unsigned char   Byte;
typedef signed char     SignedByte;
typedef char           *Ptr;
typedef Ptr            *Handle;
typedef short           OSErr;
typedef long            OSType;
typedef long            Fixed;
typedef long            Size;
typedef void          (*ProcPtr)(void);

#define noErr    0
#define dupFNErr (-48)
#define memFullErr (-108)

typedef unsigned char   Str255[256];
typedef unsigned char   Str63[64];
typedef unsigned char   Str32[33];
typedef unsigned char   Str31[32];
typedef unsigned char  *StringPtr;
typedef const unsigned char *ConstStr255Param;

/* ---- geometry ---- */
typedef struct Point { short v; short h; } Point;
typedef struct Rect  { short top; short left; short bottom; short right; } Rect;

/* ---- color / patterns ---- */
typedef struct RGBColor { unsigned short red, green, blue; } RGBColor;
typedef unsigned char Pattern[8];
extern Pattern black, white, gray, ltGray, dkGray;

/* ---- keyboard / events ---- */
typedef unsigned char KeyMap[16];
typedef struct EventRecord {
	short   what;
	long    message;
	long    when;
	Point   where;
	short   modifiers;
} EventRecord;

#define nullEvent    0
#define mouseDown    1
#define mouseUp      2
#define keyDown      3
#define keyUp        4
#define autoKey      5
#define updateEvt    6
#define diskEvt      7
#define activateEvt  8
#define app4Evt      15
#define everyEvent   0xFFFF
#define charCodeMask 0x000000FFL
#define cmdKey       0x0100
#define shiftKey     0x0200
#define optionKey    0x0800

/* ---- QuickDraw ---- */
typedef struct BitMap {
	Ptr     baseAddr;      /* 8bpp pixels (palette index, or 0/1 for masks) */
	short   rowBytes;      /* stride in bytes (== pixels) */
	Rect    bounds;
} BitMap;

typedef struct Cursor { short data[16]; short mask[16]; Point hotSpot; } Cursor;

/* Regions are shim objects: a bounding box plus an optional 1-byte-per-pixel
 * coverage mask anchored at the screen origin. Polygon recording via
 * OpenRgn/Line*/
typedef struct ShimRegion {
	short           rgnSize;
	Rect            rgnBBox;
	unsigned char  *mask;      /* screenWide*screenHigh coverage, may be NULL (=rect) */
} ShimRegion, *RgnPtr, **RgnHandle;

typedef struct PixMap {
	Ptr     baseAddr;
	short   rowBytes;
	Rect    bounds;
	short   pixelSize;
	Handle  pmTable;
} PixMap, *PixMapPtr, **PixMapHandle;

typedef struct GrafPort {
	short        device;
	BitMap       portBits;
	Rect         portRect;
	RgnHandle    visRgn;
	RgnHandle    clipRgn;
	Point        pnLoc;
	short        pnMode;
	short        pnVis;
	Pattern      pnPat;
	short        txFont, txFace, txMode, txSize;
	unsigned char fgIndex, bkIndex;   /* palette indices */
	PixMapHandle portPixMap;          /* shim: alias of portBits (DumpPict compiles) */
	/* shim internals */
	Boolean      rgnIsOpen;
	void        *polyRec;
} GrafPort, *GrafPtr;

typedef GrafPort  CGrafPort;
typedef GrafPtr   CGrafPtr;
typedef GrafPtr   WindowPtr;
typedef GrafPtr   DialogPtr;
typedef Handle    MenuHandle;
typedef Handle    CTabHandle;

typedef struct GDevice { PixMapHandle gdPMap; } GDevice, *GDPtr, **GDHandle;

/* classic QuickDraw color-plane constants (ForeColor/BackColor) */
#define whiteColor   30
#define blackColor   33
#define yellowColor  69
#define magentaColor 137
#define redColor     205
#define cyanColor    273
#define greenColor   341
#define blueColor    409

typedef struct Picture {
	short   picSize;
	Rect    picFrame;
	short   shimAssetId;      /* shim: pack image id */
} Picture, *PicPtr, **PicHandle;

/* transfer modes / fonts */
#define srcCopy    0
#define srcOr      1
#define srcXor     2
#define srcBic     3
#define notSrcCopy 4
#define patCopy    8
#define patXor     10
#define systemFont 0
#define applFont   1
#define geneva     3

/* ---- QuickDraw globals ---- */
extern BitMap        screenBits;
extern long          randSeed;

/* The game reads the low-memory tick counter as a plain global and spins on
 * it ("while (Ticks < waitTil) ;"). Here every read recomputes it from the
 * wall clock — and a read inside a tight spin loop yields the CPU and pumps
 * events, so those busy-waits pace correctly without special-casing. */
long ShimTicksRead (void);
#define Ticks ShimTicksRead()

/* ================= Toolbox routines implemented by the shim ================= */

/* memory */
Ptr    NewPtr (Size byteCount);
void   DisposPtr (Ptr p);
Handle NewHandle (Size byteCount);
void   DisposHandle (Handle h);
void   HLock (Handle h);
void   HUnlock (Handle h);
void   MoveHHi (Handle h);
void   HNoPurge (Handle h);
char   HGetState (Handle h);
void   HSetState (Handle h, char state);
long   GetHandleSize (Handle h);
OSErr  HandToHand (Handle *theHndl);
void   BlockMove (const void *src, void *dest, Size count);

/* resources (served from the asset pack) */
Handle GetResource (OSType theType, short theID);
PicHandle GetPicture (short picID);
void   ReleaseResource (Handle theResource);
void   CloseResFile (short refNum);
void   GetIndString (StringPtr theString, short strListID, short index);
StringPtr *GetString (short stringID);

/* ports / drawing */
void   GetPort (GrafPtr *port);
void   SetPort (GrafPtr port);
void   SetOrigin (short h, short v);
void   ClipRect (const Rect *r);
void   LocalToGlobal (Point *pt);
void   GlobalToLocal (Point *pt);
void   CopyBits (const BitMap *srcBits, const BitMap *dstBits,
                 const Rect *srcRect, const Rect *dstRect, short mode, RgnHandle maskRgn);
void   CopyMask (const BitMap *srcBits, const BitMap *maskBits, const BitMap *dstBits,
                 const Rect *srcRect, const Rect *maskRect, const Rect *dstRect);
void   MoveTo (short h, short v);
void   Move (short dh, short dv);
void   LineTo (short h, short v);
void   Line (short dh, short dv);
void   PenMode (short mode);
void   PenNormal (void);
void   PenPat (const unsigned char *pat);
void   PaintRect (const Rect *r);
void   FillRect (const Rect *r, const unsigned char *pat);
void   EraseRect (const Rect *r);
void   FrameRect (const Rect *r);
void   InvertRect (const Rect *r);
void   OffsetRect (Rect *r, short dh, short dv);
void   InsetRect (Rect *r, short dh, short dv);
Boolean SectRect (const Rect *src1, const Rect *src2, Rect *dstRect);
void   UnionRect (const Rect *src1, const Rect *src2, Rect *dstRect);
Boolean PtInRect (Point pt, const Rect *r);
Boolean EmptyRect (const Rect *r);
void   RGBForeColor (const RGBColor *color);
void   RGBBackColor (const RGBColor *color);
void   ForeColor (long color);
void   BackColor (long color);
void   Index2Color (long index, RGBColor *aColor);
void   PmForeColor (short colorIndex);
void   PmBackColor (short colorIndex);

/* regions */
RgnHandle NewRgn (void);
void   DisposeRgn (RgnHandle rgn);
void   CopyRgn (RgnHandle srcRgn, RgnHandle dstRgn);
void   RectRgn (RgnHandle rgn, const Rect *r);
void   SetRectRgn (RgnHandle rgn, short left, short top, short right, short bottom);
void   OpenRgn (void);
void   CloseRgn (RgnHandle dstRgn);
void   UnionRgn (RgnHandle srcA, RgnHandle srcB, RgnHandle dst);
void   SectRgn (RgnHandle srcA, RgnHandle srcB, RgnHandle dst);
void   DiffRgn (RgnHandle srcA, RgnHandle srcB, RgnHandle dst);
void   OffsetRgn (RgnHandle rgn, short dh, short dv);
void   PaintRgn (RgnHandle rgn);
void   FillRgn (RgnHandle rgn, const unsigned char *pat);

/* pictures */
void   DrawPicture (PicHandle myPicture, const Rect *dstRect);
PicHandle OpenPicture (const Rect *picFrame);
void   ClosePicture (void);
void   KillPicture (PicHandle myPicture);

/* text */
void   TextFont (short font);
void   TextFace (short face);
void   TextMode (short mode);
void   TextSize (short size);
void   DrawString (ConstStr255Param s);
short  StringWidth (ConstStr255Param s);
void   NumToString (long theNum, StringPtr theString);
void   UprString (StringPtr theString, Boolean diacSens);

/* windows (single fullscreen game window) */
WindowPtr GetNewWindow (short windowID, void *wStorage, WindowPtr behind);
WindowPtr GetNewCWindow (short windowID, void *wStorage, WindowPtr behind);
void   SizeWindow (WindowPtr w, short wd, short ht, Boolean fUpdate);
void   ShowWindow (WindowPtr w);
void   DisposeWindow (WindowPtr w);
void   ClosePort (GrafPtr port);
void   OpenCPort (CGrafPtr port);
void   CloseCPort (CGrafPtr port);
short  GetMBarHeight (void);

/* events / input */
void   GetKeys (KeyMap theKeys);
Boolean Button (void);
void   GetMouse (Point *mouseLoc);
Boolean BitTst (const void *bytePtr, long bitNum);
void   FlushEvents (short whichMask, short stopMask);
Boolean GetOSEvent (short mask, EventRecord *theEvent);

/* cursor */
void   InitCursor (void);
void   HideCursor (void);
void   ShowCursor (void);
void   ObscureCursor (void);
void   ShieldCursor (const Rect *shieldRect, Point offsetPt);

/* misc OS */
void   Delay (long numTicks, long *finalTicks);
void   GetDateTime (unsigned long *secs);
short  Random (void);
void   SysBeep (short duration);
void   FlashMenuBar (short menuID);
void   UnloadSeg (void *routineAddr);
void   ErrorSound (ProcPtr soundProc);
void   ExitToShell (void);
void   DrawMenuBar (void);
short  Alert (short alertID, void *filterProc);
void   ParamText (ConstStr255Param a, ConstStr255Param b, ConstStr255Param c, ConstStr255Param d);
void   GetSoundVol (short *level);
void   SetSoundVol (short level);
GDHandle GetMainDevice (void);
OSErr  SetDepth (GDHandle gd, short depth, short whichFlags, short flags);

/* file bits referenced by RenderQD.c's DumpPict (GetVInfo fails => early out) */
OSErr  GetVInfo (short drvNum, StringPtr volName, short *vRefNum, long *freeBytes);
OSErr  Create (ConstStr255Param fileName, short vRefNum, OSType creator, OSType fileType);
OSErr  FSOpen (ConstStr255Param fileName, short vRefNum, short *refNum);
OSErr  FSWrite (short refNum, long *count, const void *buffPtr);
OSErr  FSClose (short refNum);

#endif /* MACSHIM_H */
