#ifndef NETWORK_H
#define NETWORK_H

#include "common.h"
#include "cache.h"

int parse_url(const char *url, char *host, int *port, char *path);
int connect_to_server(const char *host, int port);
int extract_status_code(const char *headers);
ssize_t recv_fully(int fd, char *buffer, size_t max_len, int *eof);

#endif
