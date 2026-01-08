#include "network.h"

int parse_url(const char *url, char *host, int *port, char *path) {
    const char *start = url;
    
    if (strncmp(start, "http://", 7) == 0) {
        start += 7;
    } else if (strncmp(start, "https://", 8) == 0) {
        return -1;
    }
    
    const char *slash = strchr(start, '/');
    const char *colon = strchr(start, ':');
    
    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - start;
        if (host_len >= 256) return -1;
        strncpy(host, start, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t host_len = slash ? (size_t)(slash - start) : strlen(start);
        if (host_len >= 256) return -1;
        strncpy(host, start, host_len);
        host[host_len] = '\0';
        *port = 80;
    }
    
    if (slash) {
        strncpy(path, slash, 1023);
        path[1023] = '\0';
    } else {
        strcpy(path, "/");
    }
    
    return 0;
}

int connect_to_server(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Failed to resolve host: %s\n", host);
        return -1;
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    return sock;
}

int extract_status_code(const char *headers) {
    char version[16];
    int status_code;
    if (sscanf(headers, "%s %d", version, &status_code) == 2) {
        return status_code;
    }
    return 0;
}

ssize_t recv_fully(int fd, char *buffer, size_t max_len, int *eof) {
    size_t total = 0;
    *eof = 0;
    
    while (total < max_len) {
        ssize_t n = recv(fd, buffer + total, max_len - total, 0);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total > 0) break;
                continue;
            }
            return -1;
        }
        
        if (n == 0) {
            *eof = 1;
            break;
        }
        
        total += n;
        
        if (strstr(buffer, "\r\n\r\n")) {
            break;
        }
    }
    
    return total;
}
