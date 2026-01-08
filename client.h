#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"
#include "cache.h"

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
    char *original_request;
} client_info_t;

void* client_thread(void *arg);
int send_cached_data(int client_fd, cache_entry_t *entry);

#endif
