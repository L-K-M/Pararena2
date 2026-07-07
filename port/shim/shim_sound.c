/* shim_sound.c — replacement for the binary-only SMS ("Sound Music System")
 * 68K driver, implementing the semantics recovered by disassembly:
 *
 *  - 4 voices of unsigned 8-bit mono PCM at 22,254.5 Hz (0x80 = silence)
 *  - SMSD header word 0 = priority, word 1 = repeat count (0x7FFF = loop)
 *  - SMSSTART: highest-numbered free channel wins; if none is free, steal
 *    the lowest-priority busy channel whose priority <= the new sound's
 *    (equal priority IS stealable); if every busy channel has strictly
 *    higher priority the sound is silently dropped
 *  - SMSSTARTCHAN(id, ch): no arbitration, always preempts that channel
 *    (the game pins the crowd loop to channel 3)
 *  - 4-channel mix = sum of (sample>>2 & 0x3F), i.e. each voice attenuated
 *    to quarter amplitude around the 0x80 bias
 *
 * Output goes through an SDL_AudioStream that resamples to the device rate.
 * System volume 0-7 (GetSoundVol/SetSoundVol) scales the mix.
 */

#include <SDL3/SDL.h>
#include "shim_internal.h"

#define NUM_CHANNELS 4
#define SMS_RATE 22254   /* 22254.54 Hz nominal */

typedef struct Clip {
	const uint8_t *pcm;
	uint32_t len;
	int priority;
	int loop;
} Clip;

typedef struct Voice {
	int active;
	int soundID;
	int priority;
	int loop;
	const uint8_t *pcm;
	uint32_t len, pos;
} Voice;

static Clip clips[128];             /* indexed by sound ID (1..105) */
static Voice voices[NUM_CHANNELS];
static SDL_AudioStream *stream;
static SDL_Mutex *lock;
static int sysVolume = 7;
static int soundLogging;

static Clip *clipFor (int id)
{
	if (id < 0 || id >= 128 || !clips[id].pcm)
		return NULL;
	return &clips[id];
}

static void audioCallback (void *userdata, SDL_AudioStream *astream,
                           int additional, int total)
{
	(void)userdata; (void)total;
	uint8_t buf[1024];
	while (additional > 0)
	{
		int n = additional > (int)sizeof buf ? (int)sizeof buf : additional;
		SDL_LockMutex(lock);
		for (int i = 0; i < n; i++)
		{
			int mix = 0;
			for (int c = 0; c < NUM_CHANNELS; c++)
			{
				Voice *v = &voices[c];
				int s = 0x80;
				if (v->active)
				{
					s = v->pcm[v->pos++];
					if (v->pos >= v->len)
					{
						if (v->loop)
							v->pos = 0;
						else
							v->active = 0;
					}
				}
				mix += (s >> 2) & 0x3F;
			}
			/* mix is 0..252 biased at 128; apply system volume about the bias */
			int out = 128 + ((mix - 128) * sysVolume) / 7;
			buf[i] = (uint8_t)(out < 0 ? 0 : out > 255 ? 255 : out);
		}
		SDL_UnlockMutex(lock);
		SDL_PutAudioStreamData(astream, buf, n);
		additional -= n;
	}
}

void ShimSoundInit (void)
{
	lock = SDL_CreateMutex();
	soundLogging = SDL_getenv("PARARENA_SOUND_LOG") != NULL;

	/* load every SND asset (id, priority, loop, pcm) */
	for (int id = 0; id < 128; id++)
	{
		const ShimAsset *a = ShimFindAsset(SHIM_FOURCC('S','N','D',' '), id);
		if (!a || a->size < 4)
			continue;
		clips[id].priority = a->data[0] | (a->data[1] << 8);   /* little-endian in pack */
		int rep = a->data[2] | (a->data[3] << 8);
		clips[id].loop = (rep == 0x7FFF);
		clips[id].pcm = a->data + 4;
		clips[id].len = a->size - 4;
	}

	SDL_AudioSpec spec;
	spec.format = SDL_AUDIO_U8;
	spec.channels = 1;
	spec.freq = SMS_RATE;
	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
	                                   &spec, audioCallback, NULL);
	if (stream)
		SDL_ResumeAudioStreamDevice(stream);
	else
		ShimLog("audio unavailable (%s) — sound events will still be tracked", SDL_GetError());
}

void ShimSoundQuit (void)
{
	if (stream)
	{
		SDL_DestroyAudioStream(stream);
		stream = NULL;
	}
}

/* ---------------- the SMS entry points the game uses ---------------- */

void SMSINIT (void) {}
void SMSEXIT (void)
{
	SDL_LockMutex(lock);
	for (int c = 0; c < NUM_CHANNELS; c++)
		voices[c].active = 0;
	SDL_UnlockMutex(lock);
}
void SMSSETMODE (short theMode) { (void)theMode; }   /* always 4-channel */
int  SMSGETMODE (void) { return NUM_CHANNELS; }

static void startOnChannel (int ch, int soundID, Clip *cl)
{
	Voice *v = &voices[ch];
	v->active = 1;
	v->soundID = soundID;
	v->priority = cl->priority;
	v->loop = cl->loop;
	v->pcm = cl->pcm;
	v->len = cl->len;
	v->pos = 0;
	if (soundLogging)
		ShimLog("snd: ch%d id=%d pri=0x%04X%s", ch, soundID, cl->priority, cl->loop ? " loop" : "");
}

void SMSSTART (short soundID)
{
	Clip *cl = clipFor(soundID);
	if (!cl)
		return;
	SDL_LockMutex(lock);
	int target = -1;
	for (int c = NUM_CHANNELS - 1; c >= 0; c--)     /* highest free channel */
		if (!voices[c].active) { target = c; break; }
	if (target < 0)
	{
		/* steal lowest-priority channel with priority <= the new sound's */
		int bestPri = 0x8000;
		for (int c = 0; c < NUM_CHANNELS; c++)
		{
			if (voices[c].priority <= cl->priority && voices[c].priority < bestPri)
			{
				bestPri = voices[c].priority;
				target = c;
			}
		}
	}
	if (target >= 0)
		startOnChannel(target, soundID, cl);
	else if (soundLogging)
		ShimLog("snd: id=%d pri=0x%04X dropped", soundID, cl->priority);
	SDL_UnlockMutex(lock);
}

void SMSSTARTCHAN (short soundID, short channel)
{
	Clip *cl = clipFor(soundID);
	if (!cl || channel < 0 || channel >= NUM_CHANNELS)
		return;
	SDL_LockMutex(lock);
	startOnChannel(channel, soundID, cl);
	SDL_UnlockMutex(lock);
}

void SMSSTOP (void)
{
	SDL_LockMutex(lock);
	for (int c = 0; c < NUM_CHANNELS; c++)
		voices[c].active = 0;
	SDL_UnlockMutex(lock);
}

void SMSSTOPCHAN (short channel)
{
	if (channel < 0 || channel >= NUM_CHANNELS)
		return;
	SDL_LockMutex(lock);
	voices[channel].active = 0;
	SDL_UnlockMutex(lock);
}

/* port helper: stop whichever voice is playing this sound ID, leaving the
 * other channels (e.g. the pinned crowd loop) alone — used by the announcer
 * skip, where the channel that won arbitration isn't known to the caller */
void ShimStopSoundID (short soundID)
{
	SDL_LockMutex(lock);
	for (int c = 0; c < NUM_CHANNELS; c++)
		if (voices[c].active && voices[c].soundID == soundID)
			voices[c].active = 0;
	SDL_UnlockMutex(lock);
}

/* declared in SMS.h but never called by the game — link stubs */
void SMSSTARTLO (short soundID) { SMSSTART(soundID); }
void SMSSTARTMID (short soundID) { SMSSTART(soundID); }
void SMSSTARTHI (short soundID) { SMSSTART(soundID); }
void SMSSTARTBIND (short soundID, Ptr completionProc) { (void)completionProc; SMSSTART(soundID); }
void SMSSTARTGEN (short soundID, short channel, short priority, short repetitions, Ptr completionProc)
{ (void)priority; (void)repetitions; (void)completionProc; SMSSTARTCHAN(soundID, channel); }
void SMSSTOPP (short priority) { (void)priority; SMSSTOP(); }
void SMSSTOPGEN (short channel, short priority) { (void)priority; SMSSTOPCHAN(channel); }
void SMSLOAD (short soundID) { (void)soundID; }
void SMSUNLOAD (short soundID) { (void)soundID; }
void SMSLOCK (short soundID) { (void)soundID; }
void SMSUNLOCK (short soundID) { (void)soundID; }
void SMSSOUNDON (void) {}
void SMSSOUNDOFF (void) {}
void SMSSETSTATE (Boolean state) { (void)state; }
char SMSGETSTATE (void) { return 1; }
char SMSCHANNELFREE (short channel)
{
	if (channel < 0 || channel >= NUM_CHANNELS) return 0;
	return (char)!voices[channel].active;
}
int  SMSDECOMPRESS (Handle theSound) { (void)theSound; return 0; }
void SMSSETIMING (short a, short b, short c) { (void)a; (void)b; (void)c; }
char SMSSOUNDMANAGER (void) { return 0; }
void SMSSWITCHER (EventRecord *theEvent) { (void)theEvent; }

/* classic Sound Driver system volume */
void GetSoundVol (short *level) { *level = (short)sysVolume; }
void SetSoundVol (short level)
{
	if (level < 0) level = 0;
	if (level > 7) level = 7;
	sysVolume = level;
}
