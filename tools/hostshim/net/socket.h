/* host shim for tools/https_test.c: Linux has no SO_NBIO, so the option fails
   and http_ps3.c connects blocking (its fallback) */
#include <sys/socket.h>
#include <unistd.h>
#define SO_NBIO 0x7ffe
