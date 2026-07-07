/* shim_qd.c — the QuickDraw subset Pararena 2 draws with.
 *
 * Pixel model: 8bpp palette indices. All rect coordinates are local to a
 * bitmap's bounds (which are 0-origin everywhere in this port). Transfer
 * mode is effectively srcCopy; the pen supports patCopy/patXor which is all
 * the game uses (star twinkles, region fills, frames, text).
 */

#include "shim_internal.h"

/* QD pattern globals */
Pattern black  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
Pattern white  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
Pattern gray   = {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55};
Pattern ltGray = {0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22};
Pattern dkGray = {0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD};

BitMap   screenBits;
GrafPort shimScreenPort;
GrafPtr  shimCurPort = &shimScreenPort;
int      shimScreenDirty = 0;
uint8_t  shimPaletteRGBA[16][4];

#define IDX_WHITE 0
#define IDX_BLACK 15

/* ------------------------------------------------ helpers */

static uint8_t *bmAddr (const BitMap *bm, int x, int y)
{
	return (uint8_t *)bm->baseAddr
		+ (long)(y - bm->bounds.top) * bm->rowBytes + (x - bm->bounds.left);
}

static void markDirty (const BitMap *bm)
{
	if (bm->baseAddr == screenBits.baseAddr)
		shimScreenDirty = 1;
}

static int clip2 (const BitMap *sb, const BitMap *db,
                  int *sx, int *sy, int *dx, int *dy, int *w, int *h)
{
	int d;
	/* clip against source bounds */
	if (*sx < sb->bounds.left)   { d = sb->bounds.left - *sx; *sx += d; *dx += d; *w -= d; }
	if (*sy < sb->bounds.top)    { d = sb->bounds.top - *sy;  *sy += d; *dy += d; *h -= d; }
	if (*sx + *w > sb->bounds.right)  *w = sb->bounds.right - *sx;
	if (*sy + *h > sb->bounds.bottom) *h = sb->bounds.bottom - *sy;
	/* clip against dest bounds */
	if (*dx < db->bounds.left)   { d = db->bounds.left - *dx; *dx += d; *sx += d; *w -= d; }
	if (*dy < db->bounds.top)    { d = db->bounds.top - *dy;  *dy += d; *sy += d; *h -= d; }
	if (*dx + *w > db->bounds.right)  *w = db->bounds.right - *dx;
	if (*dy + *h > db->bounds.bottom) *h = db->bounds.bottom - *dy;
	return (*w > 0 && *h > 0);
}

void CopyBits (const BitMap *srcBits, const BitMap *dstBits,
               const Rect *srcRect, const Rect *dstRect, short mode, RgnHandle maskRgn)
{
	(void)mode; (void)maskRgn;
	int w = srcRect->right - srcRect->left;
	int h = srcRect->bottom - srcRect->top;
	int dw = dstRect->right - dstRect->left;
	int dh = dstRect->bottom - dstRect->top;
	int sx = srcRect->left, sy = srcRect->top;
	int dx = dstRect->left, dy = dstRect->top;

	if (w != dw || h != dh)		/* the game never scales in hot paths */
	{
		static int warned = 0;
		if (!warned++) ShimLog("CopyBits size mismatch %dx%d -> %dx%d (using src size)", w, h, dw, dh);
	}
	if (!clip2(srcBits, dstBits, &sx, &sy, &dx, &dy, &w, &h))
		return;
	if (srcBits->baseAddr == dstBits->baseAddr && dy > sy)
	{
		for (int y = h - 1; y >= 0; y--)
			memmove(bmAddr(dstBits, dx, dy + y), bmAddr(srcBits, sx, sy + y), (size_t)w);
	}
	else
	{
		for (int y = 0; y < h; y++)
			memmove(bmAddr(dstBits, dx, dy + y), bmAddr(srcBits, sx, sy + y), (size_t)w);
	}
	markDirty(dstBits);
}

void CopyMask (const BitMap *srcBits, const BitMap *maskBits, const BitMap *dstBits,
               const Rect *srcRect, const Rect *maskRect, const Rect *dstRect)
{
	int w = srcRect->right - srcRect->left;
	int h = srcRect->bottom - srcRect->top;
	int sx = srcRect->left, sy = srcRect->top;
	int mx = maskRect->left, my = maskRect->top;
	int dx = dstRect->left, dy = dstRect->top;
	int sx0 = sx, sy0 = sy;

	if (!clip2(srcBits, dstBits, &sx, &sy, &dx, &dy, &w, &h))
		return;
	mx += sx - sx0;  my += sy - sy0;
	/* clip against the mask bitmap too — it can be smaller than the source
	 * sheet (the color mask sheet is 320 px wide vs. the 400 px parts sheet),
	 * and a mask rect past its edge would read out of bounds */
	if (mx < maskBits->bounds.left)
	{
		int d = maskBits->bounds.left - mx;
		mx += d; sx += d; dx += d; w -= d;
	}
	if (my < maskBits->bounds.top)
	{
		int d = maskBits->bounds.top - my;
		my += d; sy += d; dy += d; h -= d;
	}
	if (mx + w > maskBits->bounds.right)
		w = maskBits->bounds.right - mx;
	if (my + h > maskBits->bounds.bottom)
		h = maskBits->bounds.bottom - my;
	if (w <= 0 || h <= 0)
		return;
	for (int y = 0; y < h; y++)
	{
		const uint8_t *s = bmAddr(srcBits, sx, sy + y);
		const uint8_t *m = bmAddr(maskBits, mx, my + y);
		uint8_t *d = bmAddr(dstBits, dx, dy + y);
		for (int x = 0; x < w; x++)
			if (m[x]) d[x] = s[x];
	}
	markDirty(dstBits);
}

/* ------------------------------------------------ rect utilities */

void OffsetRect (Rect *r, short dh, short dv) { r->left += dh; r->right += dh; r->top += dv; r->bottom += dv; }
void InsetRect (Rect *r, short dh, short dv)  { r->left += dh; r->right -= dh; r->top += dv; r->bottom -= dv; }

Boolean SectRect (const Rect *a, const Rect *b, Rect *dst)
{
	dst->left = a->left > b->left ? a->left : b->left;
	dst->top = a->top > b->top ? a->top : b->top;
	dst->right = a->right < b->right ? a->right : b->right;
	dst->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
	if (dst->left >= dst->right || dst->top >= dst->bottom)
	{
		dst->left = dst->right = dst->top = dst->bottom = 0;
		return FALSE;
	}
	return TRUE;
}

void UnionRect (const Rect *a, const Rect *b, Rect *dst)
{
	Rect r;
	r.left = a->left < b->left ? a->left : b->left;
	r.top = a->top < b->top ? a->top : b->top;
	r.right = a->right > b->right ? a->right : b->right;
	r.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
	*dst = r;
}

Boolean PtInRect (Point pt, const Rect *r)
{
	return (pt.h >= r->left && pt.h < r->right && pt.v >= r->top && pt.v < r->bottom);
}

Boolean EmptyRect (const Rect *r) { return (r->left >= r->right || r->top >= r->bottom); }

/* ------------------------------------------------ port / pen state */

void GetPort (GrafPtr *port) { *port = shimCurPort; }
void SetPort (GrafPtr port)  { if (port) shimCurPort = port; }
void SetOrigin (short h, short v) { (void)h; (void)v; }   /* play area == window */
void ClipRect (const Rect *r) { (void)r; }
void LocalToGlobal (Point *pt) { (void)pt; }
void GlobalToLocal (Point *pt) { (void)pt; }

void MoveTo (short h, short v) { shimCurPort->pnLoc.h = h; shimCurPort->pnLoc.v = v; }
void Move (short dh, short dv) { shimCurPort->pnLoc.h += dh; shimCurPort->pnLoc.v += dv; }
void PenMode (short mode) { shimCurPort->pnMode = mode; }
void PenNormal (void) { shimCurPort->pnMode = patCopy; memcpy(shimCurPort->pnPat, black, 8); }
void PenPat (const unsigned char *pat) { memcpy(shimCurPort->pnPat, pat, 8); }

static void plotPixel (GrafPtr p, int x, int y)
{
	const BitMap *bm = &p->portBits;
	if (x < bm->bounds.left || x >= bm->bounds.right || y < bm->bounds.top || y >= bm->bounds.bottom)
		return;
	uint8_t *px = bmAddr(bm, x, y);
	if (p->pnMode == patXor)
		*px ^= 0x0F;
	else
	{
		int bit = (p->pnPat[y & 7] >> (7 - (x & 7))) & 1;
		*px = bit ? p->fgIndex : p->bkIndex;
	}
	markDirty(bm);
}

/* polygon recording (OpenRgn) */
#define MAX_POLY_PTS 64
typedef struct PolyRec { int n; Point pts[MAX_POLY_PTS]; } PolyRec;

void LineTo (short h, short v)
{
	GrafPtr p = shimCurPort;
	if (p->rgnIsOpen && p->polyRec)
	{
		PolyRec *pr = (PolyRec *)p->polyRec;
		if (pr->n < MAX_POLY_PTS)
		{
			pr->pts[pr->n].h = h;
			pr->pts[pr->n].v = v;
			pr->n++;
		}
		p->pnLoc.h = h; p->pnLoc.v = v;
		return;
	}
	/* Bresenham */
	int x0 = p->pnLoc.h, y0 = p->pnLoc.v, x1 = h, y1 = v;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		plotPixel(p, x0, y0);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
	p->pnLoc.h = h; p->pnLoc.v = v;
}

void Line (short dh, short dv) { LineTo(shimCurPort->pnLoc.h + dh, shimCurPort->pnLoc.v + dv); }

/* ------------------------------------------------ fills */

static void fillRectPat (GrafPtr p, const Rect *r, const unsigned char *pat, int useForeOnly)
{
	Rect c;
	Rect bnds = p->portBits.bounds;
	if (!SectRect(r, &bnds, &c))
		return;
	for (int y = c.top; y < c.bottom; y++)
	{
		uint8_t *row = bmAddr(&p->portBits, c.left, y);
		for (int x = c.left; x < c.right; x++)
		{
			int bit = (pat[y & 7] >> (7 - (x & 7))) & 1;
			row[x - c.left] = bit ? (useForeOnly ? p->fgIndex : IDX_BLACK)
			                      : (useForeOnly ? p->bkIndex : IDX_WHITE);
		}
	}
	markDirty(&p->portBits);
}

void PaintRect (const Rect *r)
{
	GrafPtr p = shimCurPort;
	Rect c, bnds = p->portBits.bounds;
	if (!SectRect(r, &bnds, &c))
		return;
	for (int y = c.top; y < c.bottom; y++)
		memset(bmAddr(&p->portBits, c.left, y), p->fgIndex, (size_t)(c.right - c.left));
	markDirty(&p->portBits);
}

void FillRect (const Rect *r, const unsigned char *pat) { fillRectPat(shimCurPort, r, pat, 0); }

void EraseRect (const Rect *r)
{
	GrafPtr p = shimCurPort;
	Rect c, bnds = p->portBits.bounds;
	if (!SectRect(r, &bnds, &c))
		return;
	for (int y = c.top; y < c.bottom; y++)
		memset(bmAddr(&p->portBits, c.left, y), p->bkIndex, (size_t)(c.right - c.left));
	markDirty(&p->portBits);
}

void InvertRect (const Rect *r)
{
	GrafPtr p = shimCurPort;
	Rect c, bnds = p->portBits.bounds;
	if (!SectRect(r, &bnds, &c))
		return;
	for (int y = c.top; y < c.bottom; y++)
	{
		uint8_t *row = bmAddr(&p->portBits, c.left, y);
		for (int x = 0; x < c.right - c.left; x++)
			row[x] ^= 0x0F;
	}
	markDirty(&p->portBits);
}

void FrameRect (const Rect *r)
{
	GrafPtr p = shimCurPort;
	for (int x = r->left; x < r->right; x++)
	{
		plotPixel(p, x, r->top);
		plotPixel(p, x, r->bottom - 1);
	}
	for (int y = r->top; y < r->bottom; y++)
	{
		plotPixel(p, r->left, y);
		plotPixel(p, r->right - 1, y);
	}
}

/* ------------------------------------------------ color */

static unsigned char nearestIndex (const RGBColor *c)
{
	long best = 0x7FFFFFFF;
	unsigned char bi = 0;
	for (int i = 0; i < 16; i++)
	{
		long dr = (long)(c->red >> 8) - shimPaletteRGBA[i][0];
		long dg = (long)(c->green >> 8) - shimPaletteRGBA[i][1];
		long db = (long)(c->blue >> 8) - shimPaletteRGBA[i][2];
		long d = dr * dr + dg * dg + db * db;
		if (d < best) { best = d; bi = (unsigned char)i; }
	}
	return bi;
}

void RGBForeColor (const RGBColor *color) { shimCurPort->fgIndex = nearestIndex(color); }
void RGBBackColor (const RGBColor *color) { shimCurPort->bkIndex = nearestIndex(color); }
static unsigned char classicColorIndex (long color)
{
	switch (color)
	{
		case whiteColor:   return 0;
		case yellowColor:  return 1;
		case redColor:     return 3;
		case magentaColor: return 4;
		case blueColor:    return 6;
		case cyanColor:    return 7;
		case greenColor:   return 8;
		default:           return IDX_BLACK;
	}
}
void ForeColor (long color) { shimCurPort->fgIndex = classicColorIndex(color); }
void BackColor (long color) { shimCurPort->bkIndex = classicColorIndex(color); }
void PmForeColor (short colorIndex) { shimCurPort->fgIndex = (unsigned char)colorIndex; }
void PmBackColor (short colorIndex) { shimCurPort->bkIndex = (unsigned char)colorIndex; }

void Index2Color (long index, RGBColor *aColor)
{
	if (index < 0 || index > 15) index = 15;
	aColor->red   = (unsigned short)(shimPaletteRGBA[index][0] * 0x0101);
	aColor->green = (unsigned short)(shimPaletteRGBA[index][1] * 0x0101);
	aColor->blue  = (unsigned short)(shimPaletteRGBA[index][2] * 0x0101);
}

/* ------------------------------------------------ regions */

static int rgnW, rgnH;   /* region coordinate space = screen */

static ShimRegion *deref (RgnHandle h) { return *h; }

static void rgnEnsureMask (ShimRegion *rg)
{
	if (rg->mask) return;
	rg->mask = (unsigned char *)calloc((size_t)rgnW * rgnH, 1);
	Rect c = rg->rgnBBox;
	if (c.left < 0) c.left = 0;
	if (c.top < 0) c.top = 0;
	if (c.right > rgnW) c.right = (short)rgnW;
	if (c.bottom > rgnH) c.bottom = (short)rgnH;
	for (int y = c.top; y < c.bottom; y++)
		memset(rg->mask + (size_t)y * rgnW + c.left, 1, (size_t)(c.right - c.left));
}

static void rgnRecomputeBBox (ShimRegion *rg)
{
	int minx = rgnW, miny = rgnH, maxx = -1, maxy = -1;
	for (int y = 0; y < rgnH; y++)
	{
		const unsigned char *row = rg->mask + (size_t)y * rgnW;
		for (int x = 0; x < rgnW; x++)
			if (row[x])
			{
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
	}
	if (maxx < 0)
		rg->rgnBBox.left = rg->rgnBBox.top = rg->rgnBBox.right = rg->rgnBBox.bottom = 0;
	else
	{
		rg->rgnBBox.left = (short)minx;  rg->rgnBBox.top = (short)miny;
		rg->rgnBBox.right = (short)(maxx + 1); rg->rgnBBox.bottom = (short)(maxy + 1);
	}
}

RgnHandle NewRgn (void)
{
	if (!rgnW) { rgnW = screenBits.bounds.right; rgnH = screenBits.bounds.bottom; }
	ShimRegion *rg = (ShimRegion *)calloc(1, sizeof(ShimRegion));
	rg->rgnSize = (short)sizeof(ShimRegion);
	return (RgnHandle)ShimNewHandleFor((Ptr)rg, (Size)sizeof(ShimRegion));
}

void DisposeRgn (RgnHandle rgn)
{
	if (!rgn) return;
	free(deref(rgn)->mask);
	free(deref(rgn));
	free(rgn);
}

void RectRgn (RgnHandle rgn, const Rect *r)
{
	ShimRegion *rg = deref(rgn);
	free(rg->mask);
	rg->mask = NULL;
	rg->rgnBBox = *r;
}

void SetRectRgn (RgnHandle rgn, short left, short top, short right, short bottom)
{
	Rect r;
	r.left = left; r.top = top; r.right = right; r.bottom = bottom;
	RectRgn(rgn, &r);
}

void CopyRgn (RgnHandle srcRgn, RgnHandle dstRgn)
{
	ShimRegion *s = deref(srcRgn), *d = deref(dstRgn);
	free(d->mask);
	d->mask = NULL;
	d->rgnBBox = s->rgnBBox;
	if (s->mask)
	{
		d->mask = (unsigned char *)malloc((size_t)rgnW * rgnH);
		memcpy(d->mask, s->mask, (size_t)rgnW * rgnH);
	}
}

void OpenRgn (void)
{
	GrafPtr p = shimCurPort;
	p->rgnIsOpen = TRUE;
	if (!p->polyRec) p->polyRec = calloc(1, sizeof(PolyRec));
	((PolyRec *)p->polyRec)->n = 0;
}

/* even-odd scanline polygon fill of the recorded path */
void CloseRgn (RgnHandle dstRgn)
{
	GrafPtr p = shimCurPort;
	PolyRec *pr = (PolyRec *)p->polyRec;
	ShimRegion *rg = deref(dstRgn);
	p->rgnIsOpen = FALSE;
	free(rg->mask);
	rg->mask = (unsigned char *)calloc((size_t)rgnW * rgnH, 1);
	if (pr && pr->n >= 3)
	{
		int n = pr->n;
		for (int y = 0; y < rgnH; y++)
		{
			double xs[MAX_POLY_PTS];
			int nx = 0;
			double fy = y + 0.5;
			for (int i = 0; i < n; i++)
			{
				Point a = pr->pts[i], b = pr->pts[(i + 1) % n];
				if ((a.v <= fy && b.v > fy) || (b.v <= fy && a.v > fy))
					xs[nx++] = a.h + (fy - a.v) * (double)(b.h - a.h) / (double)(b.v - a.v);
			}
			/* insertion sort */
			for (int i = 1; i < nx; i++)
			{
				double v = xs[i];
				int j = i - 1;
				while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
				xs[j + 1] = v;
			}
			for (int i = 0; i + 1 < nx; i += 2)
			{
				int x0 = (int)(xs[i] + 0.5), x1 = (int)(xs[i + 1] + 0.5);
				if (x0 < 0) x0 = 0;
				if (x1 > rgnW) x1 = rgnW;
				if (x1 > x0)
					memset(rg->mask + (size_t)y * rgnW + x0, 1, (size_t)(x1 - x0));
			}
		}
	}
	rgnRecomputeBBox(rg);
}

static void rgnOp (RgnHandle a, RgnHandle b, RgnHandle dst, int op)  /* 0=union 1=sect 2=diff */
{
	ShimRegion *ra = deref(a), *rb = deref(b), *rd = deref(dst);
	rgnEnsureMask(ra);
	rgnEnsureMask(rb);
	unsigned char *out = (unsigned char *)malloc((size_t)rgnW * rgnH);
	for (long i = 0; i < (long)rgnW * rgnH; i++)
	{
		switch (op)
		{
			case 0: out[i] = ra->mask[i] | rb->mask[i]; break;
			case 1: out[i] = ra->mask[i] & rb->mask[i]; break;
			default: out[i] = (unsigned char)(ra->mask[i] & !rb->mask[i]); break;
		}
	}
	free(rd->mask);
	rd->mask = out;
	rgnRecomputeBBox(rd);
}

void UnionRgn (RgnHandle a, RgnHandle b, RgnHandle dst) { rgnOp(a, b, dst, 0); }
void SectRgn (RgnHandle a, RgnHandle b, RgnHandle dst)  { rgnOp(a, b, dst, 1); }
void DiffRgn (RgnHandle a, RgnHandle b, RgnHandle dst)  { rgnOp(a, b, dst, 2); }

void OffsetRgn (RgnHandle rgn, short dh, short dv)
{
	ShimRegion *rg = deref(rgn);
	OffsetRect(&rg->rgnBBox, dh, dv);
	if (!rg->mask)
		return;
	unsigned char *nm = (unsigned char *)calloc((size_t)rgnW * rgnH, 1);
	for (int y = 0; y < rgnH; y++)
	{
		int sy = y - dv;
		if (sy < 0 || sy >= rgnH) continue;
		for (int x = 0; x < rgnW; x++)
		{
			int sx = x - dh;
			if (sx < 0 || sx >= rgnW) continue;
			nm[(size_t)y * rgnW + x] = rg->mask[(size_t)sy * rgnW + sx];
		}
	}
	free(rg->mask);
	rg->mask = nm;
}

static void paintRgnPat (RgnHandle rgn, const unsigned char *pat, int usePat)
{
	GrafPtr p = shimCurPort;
	ShimRegion *rg = deref(rgn);
	rgnEnsureMask(rg);
	Rect c = rg->rgnBBox, bnds = p->portBits.bounds, cl;
	if (!SectRect(&c, &bnds, &cl))
		return;
	for (int y = cl.top; y < cl.bottom; y++)
	{
		uint8_t *row = bmAddr(&p->portBits, cl.left, y);
		const unsigned char *m = rg->mask + (size_t)y * rgnW;
		for (int x = cl.left; x < cl.right; x++)
		{
			if (!m[x]) continue;
			if (usePat)
			{
				int bit = (pat[y & 7] >> (7 - (x & 7))) & 1;
				row[x - cl.left] = bit ? IDX_BLACK : IDX_WHITE;
			}
			else
				row[x - cl.left] = p->fgIndex;
		}
	}
	markDirty(&p->portBits);
}

void PaintRgn (RgnHandle rgn) { paintRgnPat(rgn, NULL, 0); }
void FillRgn (RgnHandle rgn, const unsigned char *pat) { paintRgnPat(rgn, pat, 1); }

/* ------------------------------------------------ pictures */

PicHandle GetPicture (short picID)
{
	const ShimAsset *a = ShimFindAsset(SHIM_FOURCC('P','I','X',' '), picID);
	if (!a)
		return NULL;
	Picture *pic = (Picture *)calloc(1, sizeof(Picture));
	pic->picSize = (short)sizeof(Picture);
	pic->picFrame.left = 0;
	pic->picFrame.top = 0;
	pic->picFrame.right = (short)a->w;
	pic->picFrame.bottom = (short)a->h;
	pic->shimAssetId = picID;
	return (PicHandle)ShimNewHandleFor((Ptr)pic, (Size)sizeof(Picture));
}

void DrawPicture (PicHandle myPicture, const Rect *dstRect)
{
	if (!myPicture) return;
	const ShimAsset *a = ShimFindAsset(SHIM_FOURCC('P','I','X',' '), (*myPicture)->shimAssetId);
	if (!a) return;
	BitMap src;
	src.baseAddr = (Ptr)a->data;
	src.rowBytes = (short)a->w;
	src.bounds.left = 0; src.bounds.top = 0;
	src.bounds.right = (short)a->w; src.bounds.bottom = (short)a->h;
	Rect sr = src.bounds;
	CopyBits(&src, &shimCurPort->portBits, &sr, dstRect, srcCopy, NULL);
}

void ReleaseResourcePicture (PicHandle h) { if (h) { free(*h); free(h); } }
PicHandle OpenPicture (const Rect *picFrame) { (void)picFrame; return NULL; }
void ClosePicture (void) {}
void KillPicture (PicHandle myPicture) { ReleaseResourcePicture(myPicture); }

/* ------------------------------------------------ windows */

static ShimRegion **makeScreenRgn (void)
{
	RgnHandle h = NewRgn();
	RectRgn(h, &screenBits.bounds);
	return h;
}

void ShimVideoInit (int w, int h)
{
	screenBits.baseAddr = (Ptr)calloc((size_t)w * h, 1);
	memset(screenBits.baseAddr, IDX_BLACK, (size_t)w * h);
	screenBits.rowBytes = (short)w;
	screenBits.bounds.left = 0;  screenBits.bounds.top = 0;
	screenBits.bounds.right = (short)w;  screenBits.bounds.bottom = (short)h;
	rgnW = w; rgnH = h;

	memset(&shimScreenPort, 0, sizeof(shimScreenPort));
	shimScreenPort.portBits = screenBits;
	shimScreenPort.portRect = screenBits.bounds;
	shimScreenPort.fgIndex = IDX_BLACK;
	shimScreenPort.bkIndex = IDX_WHITE;
	shimScreenPort.pnMode = patCopy;
	shimScreenPort.txMode = srcOr;         /* QuickDraw's default text mode */
	memcpy(shimScreenPort.pnPat, black, 8);
	shimScreenPort.visRgn = makeScreenRgn();
	shimScreenPort.clipRgn = makeScreenRgn();
	shimCurPort = &shimScreenPort;
	shimScreenDirty = 1;
}

WindowPtr GetNewWindow (short windowID, void *wStorage, WindowPtr behind)
{
	(void)windowID; (void)wStorage; (void)behind;
	return &shimScreenPort;
}
WindowPtr GetNewCWindow (short windowID, void *wStorage, WindowPtr behind)
{
	return GetNewWindow(windowID, wStorage, behind);
}
void SizeWindow (WindowPtr w, short wd, short ht, Boolean fUpdate) { (void)w; (void)wd; (void)ht; (void)fUpdate; }
void ShowWindow (WindowPtr w) { (void)w; }
void DisposeWindow (WindowPtr w) { (void)w; }
void ClosePort (GrafPtr port) { (void)port; }
void OpenCPort (CGrafPtr port) { if (port) memset(port, 0, sizeof(*port)); }
void CloseCPort (CGrafPtr port) { (void)port; }
short GetMBarHeight (void) { return 0; }
