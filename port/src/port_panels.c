/* port_panels.c — "which button does what" cards for the pillarbox bars.
 *
 * The game is a fixed 4:3 canvas; on a widescreen display SDL letterboxes it,
 * leaving black bars either side. When those bars are wide enough, the port
 * fills them with control-reference panels rendered in the game's own 8bpp
 * look: keyboard (or touch) on the left, gamepad (or pause help) on the
 * right, plus per-player device assignments during four-player games. The
 * bitmaps are rebuilt only when the input-state signature changes;
 * port_video.c uploads them as textures scaled to match the game pixels.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"

extern int mobileOnScreen;            /* Options toggle (port_shell.c) */

#define IDX_WHITE  0
#define IDX_YELLOW 1
#define IDX_GRAY   13
#define IDX_DGRAY  14
#define IDX_BLACK  15

#define PANEL_H   480
#define LINE_H    10
#define MARGIN    4
#define MAX_LINES 48

typedef struct {
	char text[24];
	unsigned char color;
	unsigned char rule;      /* separator line under this row (titles) */
	short indent;
	short gap;               /* extra blank pixels above the row */
} PLine;

static int addLine (PLine *ls, int n, int gap, int indent, unsigned char color, const char *t)
{
	if (n >= MAX_LINES)
		return n;
	snprintf(ls[n].text, sizeof ls[n].text, "%s", t);
	ls[n].color = color;
	ls[n].rule = 0;
	ls[n].indent = (short)indent;
	ls[n].gap = (short)gap;
	return n + 1;
}

static int addTitle (PLine *ls, int n, int gap, const char *t)
{
	n = addLine(ls, n, gap, 0, IDX_YELLOW, t);
	ls[n - 1].rule = 1;
	return n;
}

/* an input (white) and the action it performs (gray); dim = grayed out */
static int addPair (PLine *ls, int n, const char *key, const char *action, int dim)
{
	n = addLine(ls, n, 6, 0, dim ? IDX_GRAY : IDX_WHITE, key);
	n = addLine(ls, n, 0, 8, dim ? IDX_DGRAY : IDX_GRAY, action);
	return n;
}

/* left bar: the primary device (keyboard / touch), then MENU keys while in
 * the menu or the seat assignments while a multi-seat game runs */
static int buildLeft (PLine *ls, int wChars, int inPlay, const int slots[4], int nHumans)
{
	int n = 0;
	if (shimMobile)
	{
		n = addTitle(ls, n, 0, "TOUCH");
		if (mobileOnScreen)
		{
			n = addPair(ls, n, "STICK", "SKATE", 0);
			n = addPair(ls, n, "BALL BTN", "CATCH", 0);
			n = addPair(ls, n, "BAR BTN", "BRAKE", 0);
			n = addPair(ls, n, "X BTN", "BASH", 0);
		}
		else
		{
			n = addPair(ls, n, "DRAG LEFT", "SKATE", 0);
			n = addPair(ls, n, "RIGHT TOP", "CATCH", 0);
			n = addPair(ls, n, "RIGHT LOW", "BRAKE", 0);
		}
		n = addPair(ls, n, "(||), BACK", "PAUSE", 0);
	}
	else
	{
		n = addTitle(ls, n, 0, "KEYBOARD");
		n = addLine(ls, n, 6, 0, IDX_WHITE, "MOUSE");
		n = addLine(ls, n, 0, 0, IDX_WHITE, "OR ARROWS");
		n = addLine(ls, n, 0, 8, IDX_GRAY, "SKATE");
		n = addPair(ls, n, "X OR CLICK", "CATCH", 0);
		n = addPair(ls, n, "SPACE", "BRAKE", 0);
		n = addPair(ls, n, "B, N OR M", "BASH", 0);
		n = addPair(ls, n, "ESC", "PAUSE", 0);
	}

	if (inPlay && nHumans >= 2)
	{
		/* mirrors the head plates: seat colors from port_four.c */
		static const unsigned char seatCol[4] = { 8, 7, 2, 4 };
		char buf[24];
		n = addTitle(ls, n, 14, "PLAYERS");
		for (int i = 0; i < 4; i++)
		{
			if (slots[i] < 0)
				continue;
			if (slots[i] == 0)
				snprintf(buf, sizeof buf, "P1  %s",
				         shimMobile ? "TOUCH" : (wChars >= 14 ? "KEYS+PAD 1" : "KEYS"));
			else
				snprintf(buf, sizeof buf, "P%d  PAD %d", i + 1, slots[i]);
			n = addLine(ls, n, 4, 0, seatCol[i], buf);
		}
	}
	else if (!inPlay)
	{
		n = addTitle(ls, n, 14, "MENU");
		if (shimMobile)
		{
			n = addPair(ls, n, "TAP", "CHOOSE", 0);
			n = addPair(ls, n, "TAP AGAIN", "OK", 0);
		}
		else
		{
			n = addPair(ls, n, "ARROWS", "CHOOSE", 0);
			n = addPair(ls, n, "ENTER", "OK", 0);
			n = addPair(ls, n, "ESC", "BACK", 0);
			n = addPair(ls, n, "F11", "FULLSCREEN", 0);
		}
	}
	return n;
}

/* right bar: gamepad reference (grayed out until one is connected); phones
 * additionally get the pause-screen gestures, which are otherwise invisible */
static int buildRight (PLine *ls, int inPlay)
{
	int n = 0;
	int pad = shimInput.padConnected;

	if (pad || !shimMobile)
	{
		n = addTitle(ls, n, 0, "GAMEPAD");
		if (!pad)
			n = addLine(ls, n, 4, 0, IDX_DGRAY, "NOT FOUND");
		n = addPair(ls, n, "STICK/D-PAD", "SKATE", !pad);
		n = addPair(ls, n, "(A)", "CATCH", !pad);
		n = addPair(ls, n, "(B) OR RT", "BRAKE", !pad);
		n = addPair(ls, n, "(X)", "BASH", !pad);
		n = addPair(ls, n, "START", "PAUSE", !pad);
		if (!inPlay && pad)
		{
			n = addTitle(ls, n, 14, "MENU");
			n = addPair(ls, n, "D-PAD", "CHOOSE", 0);
			n = addPair(ls, n, "(A)", "OK", 0);
		}
	}
	if (shimMobile)
	{
		n = addTitle(ls, n, n ? 14 : 0, "WHEN PAUSED");
		n = addPair(ls, n, "TAP", "RESUME", 0);
		n = addPair(ls, n, "BACK", "END GAME", 0);
	}
	return n;
}

/* ------------------------------------------------------------------ */

typedef struct {
	int side, wLog, inPlay, mobile, onScreen, pad;
	int slots[4];
} PanelSig;

static uint8_t  *panelPix[2];
static int       panelW[2], panelH[2];
static uint32_t  panelVersion[2];
static PanelSig  lastSig[2];
static int       haveSig[2];

static void renderPanel (int side, const PanelSig *sig, int nHumans)
{
	PLine lines[MAX_LINES];
	int wChars = (sig->wLog - 2 * MARGIN) / 8;
	int n = side ? buildRight(lines, sig->inPlay)
	             : buildLeft(lines, wChars, sig->inPlay, sig->slots, nHumans);

	if (!panelPix[side] || panelW[side] != sig->wLog)
	{
		uint8_t *np = (uint8_t *)malloc((size_t)sig->wLog * PANEL_H);
		if (!np)
			return;                      /* keep the old buffer/width */
		free(panelPix[side]);
		panelPix[side] = np;
		panelW[side] = sig->wLog;
		panelH[side] = PANEL_H;
	}
	memset(panelPix[side], IDX_BLACK, (size_t)sig->wLog * PANEL_H);

	int totalH = 0;
	for (int i = 0; i < n; i++)
		totalH += lines[i].gap + LINE_H + (lines[i].rule ? 4 : 0);
	int y = (PANEL_H - totalH) / 3;       /* slightly above center reads better */
	if (y < 12)
		y = 12;

	GrafPort port;
	GrafPtr wasPort;
	memset(&port, 0, sizeof port);
	port.portBits.baseAddr = (Ptr)panelPix[side];
	port.portBits.rowBytes = (short)sig->wLog;
	port.portBits.bounds.right = (short)sig->wLog;
	port.portBits.bounds.bottom = PANEL_H;
	port.portRect = port.portBits.bounds;
	port.pnMode = patCopy;
	port.txMode = srcOr;
	memcpy(port.pnPat, black, 8);
	GetPort(&wasPort);
	SetPort(&port);

	for (int i = 0; i < n; i++)
	{
		Str255 ps;
		size_t len = strlen(lines[i].text);
		if (len > 255) len = 255;
		ps[0] = (unsigned char)len;
		memcpy(ps + 1, lines[i].text, len);

		y += lines[i].gap;
		PmForeColor(lines[i].color);
		MoveTo((short)(MARGIN + lines[i].indent), (short)(y + 7));
		DrawString(ps);
		y += LINE_H;
		if (lines[i].rule)
		{
			Rect r;
			r.left = MARGIN;
			r.top = (short)(y - 1);
			r.right = (short)(sig->wLog - MARGIN);
			r.bottom = (short)y;
			PmForeColor(IDX_DGRAY);
			PaintRect(&r);
			y += 4;
		}
	}
	SetPort(wasPort);
}

/* the panel bitmap for one side (0 = left, 1 = right) at the given logical
 * width, rebuilt only when the state it reflects changes. version bumps on
 * every rebuild so the caller knows when to re-upload its texture. */
const uint8_t *PortPanelImage (int side, int wLog, int *w, int *h, uint32_t *version)
{
	PanelSig sig;
	int nHumans = 0;

	if (side < 0 || side > 1 || wLog < 8)
		return NULL;
	memset(&sig, 0, sizeof sig);
	sig.side = side;
	sig.wLog = wLog;
	sig.inPlay = ShimInPlayMode();
	sig.mobile = shimMobile;
	sig.onScreen = mobileOnScreen;
	sig.pad = shimInput.padConnected;
	for (int i = 0; i < 4; i++)
	{
		sig.slots[i] = PortFourSeatSlot(i);
		if (sig.slots[i] >= 0)
			nHumans++;
	}

	if (!haveSig[side] || memcmp(&sig, &lastSig[side], sizeof sig) != 0)
	{
		renderPanel(side, &sig, nHumans);
		if (panelPix[side] && panelW[side] == wLog)
		{
			lastSig[side] = sig;         /* built: cache the state it reflects */
			haveSig[side] = 1;
			panelVersion[side]++;
		}
		else
			haveSig[side] = 0;           /* allocation failed: retry next present */
	}
	if (!panelPix[side])
		return NULL;
	*w = panelW[side];
	*h = panelH[side];
	*version = panelVersion[side];
	return panelPix[side];
}
