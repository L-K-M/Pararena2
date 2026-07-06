/* port_prefs.c — replaces Prefs.c. The original raw-dumps the prefsInfo
 * struct into a resource-forkless file; here it is dumped natively with a
 * magic/size guard into SDL's per-user pref directory. The verbatim
 * LoadThePreferences() in InitGameStructs.c validates every field and falls
 * back to defaults, so a missing/short file is handled gracefully.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"
#include "Globals.h"
#include "Prefs.h"

#define PREFS_MAGIC "PAR2PRF1"

static char *prefsPath (void)
{
	static char path[1024];
	if (!path[0])
	{
		char *base = SDL_GetPrefPath("softdorothy", "Pararena2");
		if (!base)
			return NULL;
		snprintf(path, sizeof path, "%sprefs.bin", base);
		SDL_free(base);
	}
	return path;
}

Boolean SavePrefs (prefsInfo *thePrefs)
{
	char *p = prefsPath();
	if (!p)
		return FALSE;
	FILE *f = fopen(p, "wb");
	if (!f)
		return FALSE;
	uint32_t size = (uint32_t)sizeof(prefsInfo);
	fwrite(PREFS_MAGIC, 1, 8, f);
	fwrite(&size, 4, 1, f);
	fwrite(thePrefs, sizeof(prefsInfo), 1, f);
	fclose(f);
	return TRUE;
}

Boolean LoadPrefs (prefsInfo *thePrefs)
{
	char *p = prefsPath();
	if (!p)
		return FALSE;
	FILE *f = fopen(p, "rb");
	if (!f)
		return FALSE;
	char magic[8];
	uint32_t size = 0;
	Boolean ok = FALSE;
	if (fread(magic, 1, 8, f) == 8 && memcmp(magic, PREFS_MAGIC, 8) == 0 &&
	    fread(&size, 4, 1, f) == 1 && size == (uint32_t)sizeof(prefsInfo) &&
	    fread(thePrefs, sizeof(prefsInfo), 1, f) == 1)
		ok = TRUE;
	fclose(f);
	return ok;
}

/* ---- port-only settings (not part of the verbatim prefsInfo struct) ----
 * A tiny separate file holds preferences the original game never had, so the
 * verbatim prefs format stays byte-for-byte compatible. Currently just the
 * "classic mode" switch that hides the port's HUD enhancements. */

#define PORTSET_MAGIC "PAR2SET1"

static char *portSettingsPath (void)
{
	static char path[1024];
	if (!path[0])
	{
		char *base = SDL_GetPrefPath("softdorothy", "Pararena2");
		if (!base)
			return NULL;
		snprintf(path, sizeof path, "%ssettings.bin", base);
		SDL_free(base);
	}
	return path;
}

void PortSaveSettings (int classicMode)
{
	char *p = portSettingsPath();
	if (!p)
		return;
	FILE *f = fopen(p, "wb");
	if (!f)
		return;
	int32_t v = classicMode ? 1 : 0;
	fwrite(PORTSET_MAGIC, 1, 8, f);
	fwrite(&v, 4, 1, f);
	fclose(f);
}

/* leaves *classicMode untouched (caller keeps its default) on any failure */
void PortLoadSettings (int *classicMode)
{
	char *p = portSettingsPath();
	if (!p)
		return;
	FILE *f = fopen(p, "rb");
	if (!f)
		return;
	char magic[8];
	int32_t v = 0;
	if (fread(magic, 1, 8, f) == 8 && memcmp(magic, PORTSET_MAGIC, 8) == 0 &&
	    fread(&v, 4, 1, f) == 1)
		*classicMode = v ? 1 : 0;
	fclose(f);
}

/* the Sys6/Sys7 plumbing declared in Prefs.h — unused by the port */
Boolean CanUseFindFolder (void) { return FALSE; }
Boolean GetPrefsFPathSyst7 (long *a, short *b) { (void)a; (void)b; return FALSE; }
Boolean CreatePrefsFolder (short *a) { (void)a; return FALSE; }
Boolean GetPrefsFPathSyst6 (short *a) { (void)a; return FALSE; }
Boolean WritePrefsFileSyst7 (long *a, short *b, prefsInfo *c) { (void)a; (void)b; (void)c; return FALSE; }
Boolean WritePrefsFileSyst6 (short *a, prefsInfo *b) { (void)a; (void)b; return FALSE; }
Boolean ReadPrefsFileSyst7 (long *a, short *b, prefsInfo *c) { (void)a; (void)b; (void)c; return FALSE; }
Boolean ReadPrefsFileSyst6 (short *a, prefsInfo *b) { (void)a; (void)b; return FALSE; }
