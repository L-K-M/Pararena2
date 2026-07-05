/* port_video.c — the SDL3 window/renderer. The game renders into an 8bpp
 * indexed buffer (screenBits); presenting expands it through the Apple
 * 16-color palette into a streaming texture, letterboxed to the window.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static uint32_t     *rgba;
static int           texW, texH;
static int           isFullscreen;

int PortVideoOpen (int w, int h, int startFullscreen)
{
	if (shimHeadless)
		return 1;
	win = SDL_CreateWindow("Pararena 2", w * 2, h * 2, SDL_WINDOW_RESIZABLE);
	if (!win)
	{
		ShimLog("SDL_CreateWindow failed: %s", SDL_GetError());
		return 0;
	}
	ren = SDL_CreateRenderer(win, NULL);
	if (!ren)
	{
		ShimLog("SDL_CreateRenderer failed: %s", SDL_GetError());
		return 0;
	}
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

void PortVideoPresent (const uint8_t *pix, int w, int h)
{
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
	SDL_RenderPresent(ren);
}

void PortVideoSetFullscreen (int on)
{
	if (!win)
		return;
	SDL_SetWindowFullscreen(win, on ? true : false);
	isFullscreen = on;
}

int PortVideoIsFullscreen (void) { return isFullscreen; }
