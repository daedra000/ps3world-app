/*
plat.h: what the app needs from the machine it runs on (src/ps3, PSL1GHT).
*/
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>

#define SCREEN_W 1280
#define SCREEN_H 720

// ---- log (USRDIR/ps3world_log.txt on the PS3) ----
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- time ----
uint64_t plat_time_us(void);
void plat_sleep_us(unsigned us);

// ---- threads ----
typedef void (*plat_thread_fn)(void *arg);
// prio: PS3 style, 0 highest; the main thread is ~1000
int plat_thread_start(plat_thread_fn fn, void *arg, int prio, int stack_kb, const char *name);

// a blocking lock for worker threads (never taken on the audio thread)
typedef struct plat_mutex plat_mutex_t;
plat_mutex_t *plat_mutex_new(void);
void plat_mutex_lock(plat_mutex_t *m);
void plat_mutex_unlock(plat_mutex_t *m);

// a spin flag the audio thread can try without blocking
static inline int spin_trylock(volatile int *f)
{
	if (!__sync_bool_compare_and_swap(f, 0, 1))
		return 0;
	__sync_synchronize();
	return 1;
}
static inline void spin_lock(volatile int *f)
{
	while (!spin_trylock(f))
		plat_sleep_us(50);
}
static inline void spin_unlock(volatile int *f)
{
	__sync_synchronize();
	*f = 0;
}

// ---- files ----
const char *plat_data_dir(void);		// bundled fonts etc. (read-only)
const char *plat_save_dir(void);		// settings, log (writable)
// reads a whole file; NULL if it isn't there. *len gets the size. Caller frees.
unsigned char *plat_read_file(const char *path, size_t *len);
int plat_write_file(const char *path, const void *data, size_t len);
// downloads and install targets: big files are written with stdio (fopen/fwrite), these do the rest
long long plat_file_size(const char *path);		// -1 if it isn't there (or is a folder)
int plat_dir_exists(const char *path);
int plat_mkdirs(const char *path);				// every missing folder on the way; 1 if it's there after
long long plat_free_bytes(const char *path);	// on the drive path is on; -1 if unknown
// up to max names of the folders in path (each < 64 chars); returns how many
int plat_list_dirs(const char *path, char (*names)[64], int max);

// ---- screen ----
// the canvas to draw the next frame into (SCREEN_W x SCREEN_H, 0xAARRGGBB)
uint32_t *plat_canvas_next(void);

// ---- on-screen keyboard (Search) ----
int plat_osk_open(const char *title, const char *initial);	// 1 if it's up
int plat_osk_active(void);
int plat_osk_poll(char *out, int size);	// 1 text entered, -1 cancelled, 0 still open

// ---- system ----
int plat_circle_confirms(void);		// the console uses O to confirm and X to go back (Japan, Asia)
void plat_random(void *buf, int n);		// unpredictable bytes
// the picture: drawn by the CPU (720p) at the next start, rather than the graphics chip
int plat_renderer_cpu(void);			// chosen for the next start
void plat_renderer_set_cpu(int cpu);
int plat_renderer_gpu_now(void);		// the graphics chip draws this time

// ---- HTTP (http_ps3.c; http_sim.c in the simulator) ----
typedef struct {
	const char *method;			// "GET" (NULL) or "POST"
	const char *url;
	const char *headers;		// extra header lines, each ending "\r\n" (or NULL)
	const void *body;
	int body_len;
	volatile int *cancel;		// set to stop a download
	int timeout_s;				// per read; 0 = 30
	// streaming: called with each piece of the body; return 0 to stop
	int (*sink)(void *ctx, const void *data, int len);
	void *ctx;
	int no_redirect;			// a 3xx is the answer, not somewhere to go
	int total_s;				// the whole request, at most (0: no limit, for streams)
} http_req_t;

typedef struct {
	int status;					// HTTP status, 0 if no reply
	long long length;			// Content-Length, -1 if none
	char content_type[64];
	char err[160];				// why it failed, for the screen
} http_res_t;

// Any number of threads may make requests at once. Returns 1 when a reply
// with any status came back in full; sink gets the body of every status.
int http_request(const http_req_t *req, http_res_t *res);

// GET/POST into memory (NUL-terminated). Returns the body (caller frees) or NULL.
char *http_fetch(const char *method, const char *url, const char *headers, const char *body,
	int timeout_s, volatile int *cancel, size_t *len_out, http_res_t *res);
// the same, with HTTP_NO_REDIRECT (an identity check must ask the address itself)
#define HTTP_NO_REDIRECT 1
char *http_fetch_ex(const char *method, const char *url, const char *headers, const char *body,
	int timeout_s, volatile int *cancel, size_t *len_out, http_res_t *res, int flags);

#endif
