/*
jobs.c: worker threads.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "jobs.h"

static const int lane_threads[NUM_LANES] = {1, 2, 1};

static struct {
	plat_mutex_t *lock;
	job_t *head, *tail;
	volatile int count;
	volatile int busy;		// jobs running now
} lanes[NUM_LANES];

static void Job_Unref(job_t *j)
{
	if (__sync_sub_and_fetch(&j->refs, 1) == 0) {
		if (j->cleanup)
			j->cleanup(j);
		free(j);
	}
}

static job_t *Take(int lane)
{
	job_t *j;
	plat_mutex_lock(lanes[lane].lock);
	j = lanes[lane].head;
	if (j) {
		lanes[lane].head = j->next;
		if (!lanes[lane].head)
			lanes[lane].tail = NULL;
		lanes[lane].count--;
	}
	plat_mutex_unlock(lanes[lane].lock);
	return j;
}

static void Worker(void *arg)
{
	int lane = (int)(intptr_t)arg;
	for (;;) {
		__sync_fetch_and_add(&lanes[lane].busy, 1);
		job_t *j = Take(lane);
		if (!j) {
			__sync_fetch_and_sub(&lanes[lane].busy, 1);
			plat_sleep_us(5000);
			continue;
		}
		if (!j->cancel)
			j->run(j);
		__sync_fetch_and_sub(&lanes[lane].busy, 1);
		__atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
		Job_Unref(j);
	}
}

void jobs_init(void)
{
	static const char *names[NUM_LANES] = {"jobs_api", "jobs_art", "jobs_dl"};
	for (int l = 0; l < NUM_LANES; l++) {
		lanes[l].lock = plat_mutex_new();
		for (int t = 0; t < lane_threads[l]; t++)
			plat_thread_start(Worker, (void *)(intptr_t)l, 1700, 256, names[l]);	// art decoding etc: after the frames
	}
}

job_t *job_new(void (*run)(job_t *), void (*cleanup)(job_t *))
{
	job_t *j = calloc(1, sizeof(*j));
	if (!j) return NULL;
	j->run = run;
	j->cleanup = cleanup;
	j->refs = 2;	// the poster and the worker
	return j;
}

void job_post(job_t *j, int lane)
{
	plat_mutex_lock(lanes[lane].lock);
	if (lane == LANE_ART) {
		j->next = lanes[lane].head;
		lanes[lane].head = j;
		if (!lanes[lane].tail)
			lanes[lane].tail = j;
	} else {
		j->next = NULL;
		if (lanes[lane].tail)
			lanes[lane].tail->next = j;
		else
			lanes[lane].head = j;
		lanes[lane].tail = j;
	}
	lanes[lane].count++;
	plat_mutex_unlock(lanes[lane].lock);
}

void job_release(job_t *j)
{
	if (!j) return;
	j->cancel = 1;
	Job_Unref(j);
}

// fire and forget: the poster lets go but the job still runs (job_release would cancel it)
void job_detach(job_t *j)
{
	if (j) Job_Unref(j);
}

void jobs_cancel_lane(int lane)
{
	job_t *list;
	plat_mutex_lock(lanes[lane].lock);
	list = lanes[lane].head;
	lanes[lane].head = lanes[lane].tail = NULL;
	lanes[lane].count = 0;
	plat_mutex_unlock(lanes[lane].lock);
	// never ran: the worker's reference goes now
	while (list) {
		job_t *next = list->next;
		list->cancel = 1;
		__atomic_store_n(&list->done, 1, __ATOMIC_RELEASE);
		Job_Unref(list);
		list = next;
	}
}

// at exit: give a lane's last jobs a moment to finish
void jobs_wait_lane(int lane, unsigned max_us)
{
	uint64_t until = plat_time_us() + max_us;
	while ((lanes[lane].count > 0 || lanes[lane].busy > 0) && plat_time_us() < until)
		plat_sleep_us(20000);
}
