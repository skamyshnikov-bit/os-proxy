#include "download.h"
#include "network.h"

static int send_request_to_server(int server_fd, cache_entry_t *entry) {
    pthread_mutex_lock(&entry->mutex);
    char *request = entry->original_request;
    size_t req_len = entry->request_size;
    pthread_mutex_unlock(&entry->mutex);
    
    if (!request || req_len == 0) {
        return -1;
    }
    
    if (send(server_fd, request, req_len, 0) < 0) {
        perror("send");
        return -1;
    }
    
    return 0;
}

static int process_response_headers(cache_entry_t *entry, char *headers_buf, 
                                     size_t headers_len, int *headers_done) {
    char *header_end = strstr(headers_buf, "\r\n\r\n");
    if (!header_end) {
        return 0;
    }
    
    *headers_done = 1;
    size_t header_size = header_end - headers_buf + 4;
    int status_code = extract_status_code(headers_buf);
    
    pthread_mutex_lock(&entry->mutex);
    entry->headers = malloc(header_size);
    if (entry->headers) {
        memcpy(entry->headers, headers_buf, header_size);
        entry->headers_size = header_size;
    }
    entry->status_code = status_code;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
    
    if (status_code == 200) {
        size_t body_start = header_size;
        if (body_start < headers_len) {
            add_chunk_to_cache(entry, headers_buf + body_start, headers_len - body_start);
        }
    } else {
        if (header_size < headers_len) {
            add_chunk_to_cache(entry, headers_buf + header_size, headers_len - header_size);
        }
    }
    
    return 0;
}

static void download_complete(cache_entry_t *entry) {
    pthread_mutex_lock(&entry->mutex);
    if (entry->cache_fd >= 0) {
        close(entry->cache_fd);
        entry->cache_fd = -1;
    }
    entry->status = CACHE_COMPLETE;
    entry->loading_thread_active = 0;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
}

void* download_thread(void *arg) {
    cache_entry_t *entry = (cache_entry_t*)arg;
    
    char host[256], path[1024];
    int port;
    
    if (parse_url(entry->url, host, &port, path) < 0) {
        fprintf(stderr, "Failed to parse URL: %s\n", entry->url);
        set_cache_error(entry);
        return NULL;
    }
    
    int server_fd = connect_to_server(host, port);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
        set_cache_error(entry);
        return NULL;
    }
    
    if (send_request_to_server(server_fd, entry) < 0) {
        close(server_fd);
        set_cache_error(entry);
        return NULL;
    }
    
    char *headers_buf = malloc(BUFFER_SIZE);
    if (!headers_buf) {
        close(server_fd);
        set_cache_error(entry);
        return NULL;
    }
    
    size_t headers_capacity = BUFFER_SIZE;
    size_t headers_len = 0;
    int headers_done = 0;
    char buffer[CHUNK_SIZE];
    
    while (running && !force_shutdown) {
        ssize_t n = recv(server_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recv");
            break;
        }
        if (n == 0) break;
        
        if (!headers_done) {
            if (headers_len + n >= headers_capacity) {
                headers_capacity *= 2;
                char *new_buf = realloc(headers_buf, headers_capacity);
                if (!new_buf) break;
                headers_buf = new_buf;
            }
            memcpy(headers_buf + headers_len, buffer, n);
            headers_len += n;
            
            process_response_headers(entry, headers_buf, headers_len, &headers_done);
        } else {
            add_chunk_to_cache(entry, buffer, n);
        }
    }
    
    free(headers_buf);
    close(server_fd);
    download_complete(entry);
    
    return NULL;
}
