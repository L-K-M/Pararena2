/* shim_res.c — asset pack loading and the Resource/Memory Manager shims.
 *
 * Handles are simulated as a malloc'd pointer-to-pointer; the game only
 * ever locks/unlocks and dereferences them. GetResource serves big-endian
 * table bytes straight from the pack ('forc'/'vert'/'sPts') with byte
 * swapping applied (the originals are big-endian int16 arrays).
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"

static uint8_t   *packBlob;
static ShimAsset *packAssets;
static int        packCount;

static uint32_t rd32 (const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t  rd32s (const uint8_t *p) { return (int32_t)rd32(p); }

int ShimLoadPack (const char *path)
{
	/* SDL_IOFromFile, not fopen, so a relative name resolves inside the APK's
	 * bundled assets/ on Android (and like a normal file everywhere else). */
	SDL_IOStream *io = SDL_IOFromFile(path, "rb");
	if (!io)
		return 0;
	Sint64 sz = SDL_GetIOSize(io);
	if (sz <= 0) { SDL_CloseIO(io); return 0; }
	packBlob = (uint8_t *)malloc((size_t)sz);
	if (!packBlob) { SDL_CloseIO(io); return 0; }
	if (SDL_ReadIO(io, packBlob, (size_t)sz) != (size_t)sz) { SDL_CloseIO(io); free(packBlob); packBlob = NULL; return 0; }
	SDL_CloseIO(io);
	if (memcmp(packBlob, "PAR2", 4) != 0)
		return 0;
	packCount = (int)rd32(packBlob + 8);
	packAssets = (ShimAsset *)calloc((size_t)packCount, sizeof(ShimAsset));
	for (int i = 0; i < packCount; i++)
	{
		const uint8_t *e = packBlob + 12 + i * 28;
		packAssets[i].fourcc = SHIM_FOURCC(e[0], e[1], e[2], e[3]);
		packAssets[i].id = rd32s(e + 4);
		packAssets[i].data = packBlob + rd32(e + 8);
		packAssets[i].size = rd32(e + 12);
		packAssets[i].w = rd32(e + 16);
		packAssets[i].h = rd32(e + 20);
		packAssets[i].flags = rd32(e + 24);
	}
	/* palette */
	const ShimAsset *clut = ShimFindAsset(SHIM_FOURCC('C','L','U','T'), 128);
	if (clut)
		memcpy(shimPaletteRGBA, clut->data, 64);
	return 1;
}

const ShimAsset *ShimFindAsset (uint32_t fourcc, int id)
{
	for (int i = 0; i < packCount; i++)
		if (packAssets[i].fourcc == fourcc && packAssets[i].id == id)
			return &packAssets[i];
	return NULL;
}

/* ---------------- Memory Manager ---------------- */

Ptr NewPtr (Size byteCount)
{
	return (Ptr)calloc(1, (size_t)(byteCount > 0 ? byteCount : 1));
}
void DisposPtr (Ptr p) { free(p); }

Handle NewHandle (Size byteCount)
{
	Handle h = (Handle)malloc(sizeof(Ptr));
	*h = NewPtr(byteCount);
	return h;
}
void DisposHandle (Handle h) { if (h) { free(*h); free(h); } }
void HLock (Handle h) { (void)h; }
void HUnlock (Handle h) { (void)h; }
void MoveHHi (Handle h) { (void)h; }
void HNoPurge (Handle h) { (void)h; }
char HGetState (Handle h) { (void)h; return 0; }
void HSetState (Handle h, char state) { (void)h; (void)state; }
long GetHandleSize (Handle h) { (void)h; return 0; }
OSErr HandToHand (Handle *theHndl) { (void)theHndl; return memFullErr; }
void BlockMove (const void *src, void *dest, Size count) { memmove(dest, src, (size_t)count); }

/* ---------------- Resource Manager ---------------- */

/* GetResource returns a Handle to a byte-swapped copy of a table resource.
 * The game BlockMoves out of it and calls ReleaseResource. */
Handle GetResource (OSType theType, short theID)
{
	uint32_t fourcc;
	switch ((unsigned long)theType)
	{
		case SHIM_FOURCC('f','o','r','c'): fourcc = SHIM_FOURCC('F','O','R','C'); break;
		case SHIM_FOURCC('v','e','r','t'): fourcc = SHIM_FOURCC('V','E','R','T'); break;
		case SHIM_FOURCC('s','P','t','s'): fourcc = SHIM_FOURCC('S','P','T','S'); break;
		default:
			return NULL;
	}
	const ShimAsset *a = ShimFindAsset(fourcc, theID);
	if (!a)
		return NULL;
	Handle h = NewHandle((Size)a->size);
	uint8_t *dst = (uint8_t *)*h;
	/* big-endian int16 stream -> host int16 (Point arrays in sPts are pairs of
	 * shorts, forc/vert are shorts: swapping every 2 bytes covers all three) */
	for (uint32_t i = 0; i + 1 < a->size; i += 2)
	{
		int16_t v = (int16_t)((a->data[i] << 8) | a->data[i + 1]);
		memcpy(dst + i, &v, 2);
	}
	return h;
}

void ReleaseResource (Handle theResource)
{
	/* pictures are released via their own path; table handles are freed here */
	if (theResource)
		DisposHandle(theResource);
}

void CloseResFile (short refNum) { (void)refNum; }

/* STR# lists: count word then Pascal strings, raw Mac Roman */
void GetIndString (StringPtr theString, short strListID, short index)
{
	theString[0] = 0;
	const ShimAsset *a = ShimFindAsset(SHIM_FOURCC('S','T','R','#'), strListID);
	if (!a || index < 1)
		return;
	int count = (a->data[0] << 8) | a->data[1];
	if (index > count)
		return;
	const uint8_t *p = a->data + 2;
	for (int i = 1; i < index; i++)
		p += 1 + p[0];
	memcpy(theString, p, (size_t)p[0] + 1);
}

StringPtr *GetString (short stringID)
{
	(void)stringID;
	static unsigned char name[32] = "\006Player";
	static StringPtr ptr = name;
	return &ptr;
}
