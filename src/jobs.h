/*
jobs.h: work for background threads (network calls, image decoding).

The main loop makes a job, posts it and checks job->done each frame. When it
no longer wants the result it calls job_release(); the job is freed once both
sides are finished with it.
*/
#ifndef JOBS_H
#define JOBS_H

enum { LANE_API, LANE_ART, LANE_DL, NUM_LANES };	// DL: downloads, one at a time

typedef struct job job_t;
struct job {
	void (*run)(job_t *j);		// on a worker thread
	void (*cleanup)(job_t *j);	// frees what run made (results), on whichever side frees it
	volatile int done;
	volatile int cancel;		// a hint for run
	volatile int refs;
	job_t *next;
	// what the job is about and what it found: for run and its poster
	void *arg;
	void *result;
	int ok;
	int ival;
	long long lval;
	char sval[512];
	char err[160];
};

// the poster's check: has run finished? An acquire load, so on the PPU (weakly
// ordered) everything run wrote is visible once this says yes
static inline int job_done(job_t *j) { return j && __atomic_load_n(&j->done, __ATOMIC_ACQUIRE); }

void jobs_init(void);
job_t *job_new(void (*run)(job_t *), void (*cleanup)(job_t *));	// main thread only
// LANE_ART runs newest first (the icons on screen now matter most)
void job_post(job_t *j, int lane);
void job_detach(job_t *j);		// the poster is done and the job should still run
void job_release(job_t *j);		// the poster is done with it
void jobs_cancel_lane(int lane);	// drops what hasn't started
void jobs_wait_lane(int lane, unsigned max_us);	// until it's empty, or max_us

#endif
