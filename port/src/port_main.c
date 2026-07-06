/* port_main.c — process entry: SDL bring-up, asset pack location, then hand
 * control to the original main() (renamed PararenaMain by the build).
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "shim_internal.h"

int PararenaMain (void);          /* Sources/Main.c, renamed via -Dmain= */
int PortVideoOpen (int w, int h, int startFullscreen);
void ShimTimeInit (void);
extern int portCpuDemo;
extern int portFourDemo;

static int loadPackSearching (const char *argv0)
{
	(void)argv0;
	const char *envPath = SDL_getenv("PARARENA_ASSETS");
	if (envPath && ShimLoadPack(envPath))
		return 1;
	if (ShimLoadPack("pararena2.dat"))
		return 1;
	if (ShimLoadPack("assets/pararena2.dat"))
		return 1;
	if (ShimLoadPack("port/assets/pararena2.dat"))
		return 1;
	const char *base = SDL_GetBasePath();
	if (base)
	{
		char path[1024];
		snprintf(path, sizeof path, "%spararena2.dat", base);
		if (ShimLoadPack(path))
			return 1;
		snprintf(path, sizeof path, "%s../assets/pararena2.dat", base);
		if (ShimLoadPack(path))
			return 1;
		snprintf(path, sizeof path, "%sassets/pararena2.dat", base);
		if (ShimLoadPack(path))
			return 1;
	}
	return 0;
}

int main (int argc, char **argv)
{
	int fullscreen = 0;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--headless") == 0)
			shimHeadless = 1;
		else if (strcmp(argv[i], "--fullscreen") == 0)
			fullscreen = 1;
		else if (strcmp(argv[i], "--cpu-demo") == 0)
			portCpuDemo = 1;
		else if (strcmp(argv[i], "--four-demo") == 0 && i + 1 < argc)
		{
			portFourDemo = atoi(argv[++i]);   /* 1=2v2 2=FFA-2 3=FFA-4, all AI */
			if (portFourDemo < 1 || portFourDemo > 3)
				portFourDemo = 1;
		}
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			shimAutoQuitTicks = atol(argv[++i]);
		else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
			shimFrameDumpDir = argv[++i];
		else if (strcmp(argv[i], "--dump-every") == 0 && i + 1 < argc)
			shimFrameDumpEvery = atoi(argv[++i]);
		else if (strcmp(argv[i], "--fast") == 0 && i + 1 < argc)
		{
			extern int shimTickMult;
			shimTickMult = atoi(argv[++i]);
			if (shimTickMult < 1) shimTickMult = 1;
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			printf("Pararena 2 (SDL port)\n"
			       "  --fullscreen     start in fullscreen (F11 / Alt+Enter toggles)\n"
			       "  --headless       no window (testing)\n"
			       "  --cpu-demo       auto-run one CPU vs CPU game and exit\n"
			       "  --frames N       quit after N ticks (testing)\n"
			       "  --dump DIR       write frame_XXXXXX.ppm screenshots to DIR\n"
			       "  --dump-every N   dump every N ticks (default 60)\n");
			return 0;
		}
	}

	if (shimHeadless)
	{
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
		SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
	}

	/* the shim handles touch directly (virtual joystick + on-screen buttons);
	 * stop SDL from also turning touches into mouse events, which would fight it */
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
	{
		/* try again without audio (containers, CI) */
		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
		{
			fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
			return 1;
		}
	}

	if (!loadPackSearching(argv[0]))
	{
		fprintf(stderr,
		        "pararena2.dat not found.\n"
		        "Generate it with:  python3 port/tools/build_assets.py\n"
		        "or point PARARENA_ASSETS at it.\n");
		return 1;
	}

	ShimTimeInit();
	ShimVideoInit(640, 480);
	if (!PortVideoOpen(640, 480, fullscreen))
		return 1;
	ShimSoundInit();

	PararenaMain();

	ShimSoundQuit();
	SDL_Quit();
	return 0;
}
