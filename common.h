#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>

#define MAX_CLIENTS 1000
#define BUFFER_SIZE 16384
#define MAX_URL_LENGTH 4096
#define CHUNK_SIZE 8192
#define DEFAULT_PORT 8080
#define DEFAULT_CACHE_SIZE_MB 100
#define GC_CHECK_INTERVAL 5
#define CACHE_DIR "cache"

extern volatile sig_atomic_t running;
extern volatile sig_atomic_t force_shutdown;

#endif
