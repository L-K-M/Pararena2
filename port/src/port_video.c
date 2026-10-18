/* port_video.c — the SDL3 window/renderer. The game renders into an 8bpp
 * indexed buffer (screenBits); presenting expands it through the Apple
 * 16-color palette into a streaming texture, letterboxed to the window.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "app_icon.h"

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static uint32_t     *rgba;
static int           texW, texH;
static int           isFullscreen;

/* side-panel textures (control cards in the letterbox bars, port_panels.c) */
static SDL_Texture  *panelTex[2];
static int           panelTexW[2], panelTexH[2];
static uint32_t      panelTexVer[2];
static uint32_t     *panelRGBA;
static size_t        panelRGBACap;

int PortVideoOpen (int w, int h, int startFullscreen)
{
	int winW = shimWinWOverride > 0 ? shimWinWOverride : w * 2;
	int winH = shimWinHOverride > 0 ? shimWinHOverride : h * 2;
	if (shimHeadless)
		return 1;
	win = SDL_CreateWindow("Pararena 2", winW, winH, SDL_WINDOW_RESIZABLE);
	if (!win)
	{
		ShimLog("SDL_CreateWindow failed: %s", SDL_GetError());
		return 0;
	}
	/* the classic Pararena arena+ball icon: shown in the title bar / taskbar
	 * (Linux, Windows) and the dock while running (macOS uses the .app icon).
	 * SDL_SetWindowIcon copies the surface, so we free it right after. */
	{
		SDL_Surface *icon = SDL_CreateSurfaceFrom(APP_ICON_W, APP_ICON_H,
		                    SDL_PIXELFORMAT_RGBA32, (void *)appIconRGBA, APP_ICON_W * 4);
		if (icon)
		{
			SDL_SetWindowIcon(win, icon);
			SDL_DestroySurface(icon);
		}
	}
	ren = SDL_CreateRenderer(win, NULL);
	if (!ren)
	{
		ShimLog("SDL_CreateRenderer failed: %s", SDL_GetError());
		return 0;
	}
	/* vsync stops tearing and cannot slow the game down: Ticks are derived
	 * from the wall clock, so a blocking present just consumes idle budget */
	if (!SDL_SetRenderVSync(ren, SDL_RENDERER_VSYNC_ADAPTIVE))
		SDL_SetRenderVSync(ren, 1);
	SDL_SetRenderLogicalPresentation(ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
	                        SDL_TEXTUREACCESS_STREAMING, w, h);
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
	texW = w;
	texH = h;
	rgba = (uint32_t *)malloc((size_t)w * h * 4);
	if (startFullscreen)
		PortVideoSetFullscreen(1);
	return 1;
}

/* draw the control panels into the letterbox side bars. Runs with logical
 * presentation DISABLED, so all coordinates are render-output pixels; `pr`
 * is where the letterboxed game image sits in that same space. The panel
 * bitmaps come from port_panels.c and are re-uploaded only when their
 * version changes. */
static void drawSidePanels (const SDL_FRect *pr, int ow)
{
	float scale = pr->h / 480.0f;
	int wLog;
	if (scale < 1.0f)        /* downscaled 8x8 text drops glyph rows; hide */
		return;
	wLog = (int)(pr->x / scale) - 8;      /* usable logical width of a bar */
	if (wLog < 96)                        /* too narrow to be readable */
		return;
	if (wLog > 176)
		wLog = 176;
	for (int side = 0; side < 2; side++)
	{
		int w = 0, h = 0;
		uint32_t ver = 0;
		const uint8_t *pix = PortPanelImage(side, wLog, &w, &h, &ver);
		if (!pix || w <= 0 || h <= 0)
			continue;
		if (panelTex[side] && (panelTexW[side] != w || panelTexH[side] != h))
		{
			SDL_DestroyTexture(panelTex[side]);
			panelTex[side] = NULL;
		}
		if (!panelTex[side])
		{
			panelTex[side] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
			                                   SDL_TEXTUREACCESS_STATIC, w, h);
			if (!panelTex[side])
				continue;
			SDL_SetTextureScaleMode(panelTex[side], SDL_SCALEMODE_NEAREST);
			panelTexW[side] = w;
			panelTexH[side] = h;
			panelTexVer[side] = ver - 1;   /* force the first upload */
		}
		if (panelTexVer[side] != ver)
		{
			size_t need = (size_t)w * h * 4;
			if (need > panelRGBACap)
			{
				free(panelRGBA);
				panelRGBA = (uint32_t *)malloc(need);
				panelRGBACap = panelRGBA ? need : 0;
			}
			if (!panelRGBA)
				continue;
			for (long i = 0; i < (long)w * h; i++)
			{
				const uint8_t *c = shimPaletteRGBA[pix[i] & 15];
				panelRGBA[i] = (uint32_t)c[0] | (uint32_t)c[1] << 8 |
				               (uint32_t)c[2] << 16 | 0xFF000000u;
			}
			SDL_UpdateTexture(panelTex[side], NULL, panelRGBA, w * 4);
			panelTexVer[side] = ver;
		}
		{
			float pw = w * scale, ph = h * scale;
			float bx = side ? pr->x + pr->w : 0.0f;
			float bw = side ? (float)ow - pr->x - pr->w : pr->x;
			SDL_FRect dst = { bx + (bw - pw) / 2, pr->y + (pr->h - ph) / 2, pw, ph };
			SDL_RenderTexture(ren, panelTex[side], NULL, &dst);
		}
	}
}

void PortVideoPresent (const uint8_t *pix, int w, int h)
{
	SDL_FRect pr;
	int ow = 0, oh = 0;
	if (!ren || !tex)
		return;
	if (w > texW) w = texW;
	if (h > texH) h = texH;
	for (long i = 0; i < (long)w * h; i++)
	{
		const uint8_t *c = shimPaletteRGBA[pix[i] & 15];
		rgba[i] = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | 0xFF000000u;
	}
	SDL_UpdateTexture(tex, NULL, rgba, texW * 4);
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	SDL_RenderTexture(ren, tex, NULL, NULL);
	/* letterbox side bars? fill them with the control panels. Toggling
	 * logical presentation mid-frame is supported: it applies at
	 * command-queue time, so the game texture above keeps its letterbox
	 * mapping while the panels draw in raw output pixels. */
	/* SDL_GetRenderOutputSize, NOT ...Current...: the latter reports the
	 * logical-presentation-adjusted size, but the bars live outside it */
	if (SDL_GetRenderLogicalPresentationRect(ren, &pr) && pr.w > 0 && pr.h > 0 &&
	    SDL_GetRenderOutputSize(ren, &ow, &oh) &&
	    (pr.x >= 1.0f || shimWindowShotPath))
	{
		SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
		if (pr.x >= 1.0f)
			drawSidePanels(&pr, ow);
		if (shimWindowShotPath)   /* testing: full-window screenshot per present */
		{
			SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
			if (shot)
			{
				SDL_SaveBMP(shot, shimWindowShotPath);
				SDL_DestroySurface(shot);
			}
		}
		SDL_SetRenderLogicalPresentation(ren, texW, texH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	}
	SDL_RenderPresent(ren);
}

void PortVideoSetFullscreen (int on)
{
	if (!win)
		return;
	SDL_SetWindowFullscreen(win, on ? true : false);
	isFullscreen = on;
}

/* Convert a window-normalized touch point (nx,ny in 0..1, the way SDL reports
 * finger coords) to logical render coords: lx in 0..640, ly in 0..480. SDL's
 * touch coordinates are relative to the whole window, so on a phone — where the
 * 4:3 image is letterboxed/pillarboxed inside a wider window — a raw nx*640 is
 * offset and scaled wrong. SDL_RenderCoordinatesFromWindow undoes the logical
 * presentation (letterbox + scale). Returns 0 only with no renderer (headless);
 * otherwise lx/ly are filled and may fall outside 0..640/0..480 when the touch
 * lands in a letterbox bar (callers reject those via their hit-test bounds). */
int PortVideoTouchToLogical (float nx, float ny, float *lx, float *ly)
{
	int ww = 0, wh = 0;
	if (!ren || !win)
		return 0;
	SDL_GetWindowSize(win, &ww, &wh);         /* window points; RenderCoordinatesFromWindow expects points */
	if (ww <= 0 || wh <= 0)
		return 0;
	return SDL_RenderCoordinatesFromWindow(ren, nx * (float)ww, ny * (float)wh, lx, ly) ? 1 : 0;
}

int PortVideoIsFullscreen (void) { return isFullscreen; }
