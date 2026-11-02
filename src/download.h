/*
download.h: one file from dl.ps3.world to disk, streamed, resumable, checked.

dest.part holds what's arrived and dest.part.sha the sha256 it's meant to
end with. A download that stops (network, quitting the app, pause) leaves
them, and the next try of the same file asks for the rest with Range. The
finished file is checked against the size and sha256 and only then renamed
to dest; one that doesn't match is deleted.
*/
#ifndef DOWNLOAD_H
#define DOWNLOAD_H

enum { DL_WAITING, DL_CHECKING, DL_RUNNING, DL_VERIFYING, DL_DONE, DL_FAILED, DL_CANCELLED, DL_PAUSED };
enum { ERR_NONE, ERR_NETWORK, ERR_TLS, ERR_NOT_FOUND, ERR_HTTP, ERR_CHECKSUM, ERR_DISK_FULL, ERR_WRITE };

typedef struct {
	// in
	char url[1024];
	char dest[512];
	long long size;
	unsigned char sha256[32];
	// set by the UI: cancel deletes what's arrived, pause keeps it for later
	volatile int cancel, pause;
	// out (written by the worker; the UI reads them)
	volatile int state;
	volatile long long got;			// bytes of the file on disk so far (resumed ones included)
	long long resumed_from;			// 0, or where Range started
	int err_kind;
	char err[200];
} download_t;

void download_run(download_t *d);	// blocking: on a worker thread

// the reason for a failed request, from http_ps3.c's message and the status
int download_error_kind(const char *http_err, int status);

#endif
