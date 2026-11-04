/*
audio_ps3.c: the UI's sound effects, through cellAudio.

Three short sounds, made here (no files): a soft tick for moving, a rising
two-note blip for Cross, a falling one for Circle. A thread keeps the audio
port's ring of blocks filled, mixing whatever's playing; with nothing playing
it fills silence. Quiet, so the XMB's own music isn't drowned.
*/
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <ppu-types.h>
#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sysmodule/sysmodule.h>
#include "../plat.h"

#define RATE		48000
#define VOICES		4

static float *pcm[SND_COUNT];		// mono, RATE a second
static int pcm_len[SND_COUNT];
static volatile int asked[SND_COUNT];	// counted up by plat_sound; the thread plays one per step
static volatile int on = 1, running;
static u32 port;
static sys_event_queue_t queue;
static sys_ipc_key_t key;

// a short tone gliding from f0 to f1 Hz, with a soft attack and an exponential fade
static void Make(int id, float ms, float f0, float f1, float level)
{
	int n = (int)(RATE * ms / 1000);
	float *p = malloc(n * sizeof(float)), ph = 0;
	if (!p) return;
	for (int i = 0; i < n; i++) {
		float t = (float)i / n, f = f0 + (f1 - f0) * t;
		float env = (i < RATE / 1000 ? i / (RATE / 1000.0f) : 1.0f) * expf(-5.0f * t);
		ph += 2 * (float)M_PI * f / RATE;
		p[i] = sinf(ph) * env * level;
	}
	pcm[id] = p;
	pcm_len[id] = n;
}

typedef struct { int id, pos; } voice_t;

static void Thread(void *arg)
{
	(void)arg;
	audioPortConfig cfg;
	audioGetPortConfig(port, &cfg);
	volatile u64 *read_index = (volatile u64 *)(u64)cfg.readIndex;
	float *ring = (float *)(u64)cfg.audioDataStart;
	int blocks = (int)cfg.numBlocks, next = 1, seen[SND_COUNT] = {0};
	voice_t v[VOICES];
	memset(v, 0, sizeof(v));
	for (int i = 0; i < VOICES; i++) v[i].id = -1;
	while (running) {
		sys_event_t ev;
		sysEventQueueReceive(queue, &ev, 20 * 1000);
		// new sounds asked for
		for (int s = 0; s < SND_COUNT; s++)
			while (seen[s] != asked[s]) {
				seen[s]++;
				if (!on || !pcm[s]) continue;
				int k = 0;
				for (int i = 1; i < VOICES; i++)		// a free voice, or the one furthest along
					if (v[i].id < 0 || (v[k].id >= 0 && v[i].pos > v[k].pos)) k = i;
				v[k].id = s;
				v[k].pos = 0;
			}
		// every block the port isn't playing yet, up to the one it is
		while (next != (int)(*read_index % blocks)) {
			float *out = ring + (size_t)next * AUDIO_BLOCK_SAMPLES * 2;
			for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				float m = 0;
				for (int k = 0; k < VOICES; k++) {
					if (v[k].id < 0) continue;
					m += pcm[v[k].id][v[k].pos++];
					if (v[k].pos >= pcm_len[v[k].id]) v[k].id = -1;
				}
				out[i * 2] = out[i * 2 + 1] = m;
			}
			next = (next + 1) % blocks;
		}
	}
}

int plat_sound_init(void)
{
	Make(SND_MOVE, 28, 1500, 1350, 0.05f);
	Make(SND_OK, 70, 660, 990, 0.08f);
	Make(SND_BACK, 70, 880, 560, 0.07f);
	audioPortParam param;
	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_2CH;
	param.numBlocks = AUDIO_BLOCK_8;
	param.level = 1.0f;
	s32 r;
	if ((r = sysModuleLoad(SYSMODULE_AUDIO)) != 0 && r != 0x80012003)		// (already loaded is fine)
		plat_log("sound: sysModuleLoad = 0x%08x", (unsigned)r);
	if ((r = audioInit()) != 0 && r != (s32)0x80310701) {		// (already initialised)
		plat_log("sound: audioInit = 0x%08x: no sound effects", (unsigned)r);
		return 0;
	}
	if ((r = audioPortOpen(&param, &port)) != 0) {
		plat_log("sound: audioPortOpen = 0x%08x: no sound effects", (unsigned)r);
		return 0;
	}
	if ((r = audioCreateNotifyEventQueue(&queue, &key)) != 0 || (r = audioSetNotifyEventQueue(key)) != 0) {
		plat_log("sound: event queue = 0x%08x: no sound effects", (unsigned)r);
		audioPortClose(port);
		return 0;
	}
	audioPortStart(port);
	running = 1;
	if (!plat_thread_start(Thread, NULL, 900, 64, "sound")) {		// (above the UI: a late block is a click)
		running = 0;
		audioPortStop(port);
		audioPortClose(port);
		return 0;
	}
	plat_log("sound: on");
	return 1;
}

void plat_sound(int id)
{
	if (id >= 0 && id < SND_COUNT && running && on) asked[id]++;
}

void plat_sound_enable(int enable) { on = enable; }

void plat_sound_quit(void)
{
	if (!running) return;
	running = 0;
	usleep(60 * 1000);		// (the thread's last wait)
	audioPortStop(port);
	audioRemoveNotifyEventQueue(key);
	sysEventQueueDestroy(queue, 0);
	audioPortClose(port);
	audioQuit();
}
