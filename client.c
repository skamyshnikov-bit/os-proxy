#include "client.h"
#include "network.h"
#include "download.h"

static int send_headers_to_client(int client_fd, cache_entry_t *entry) {
    char *headers_copy = NULL;
    size_t headers_size_copy = 0;
    
    if (entry->headers && entry->headers_size > 0) {
        headers_copy = malloc(entry->headers_size);
        if (headers_copy) {
            memcpy(headers_copy, entry->headers, entry->headers_size);
            headers_size_copy = entry->headers_size;
        }
    }
    
    if (headers_copy && headers_size_copy > 0) {
        if (send(client_fd, headers_copy, headers_size_copy, MSG_NOSIGNAL) < 0) {
            free(headers_copy);
            return -1;
        }
    }
    free(headers_copy);
    return 0;
}

static int send_chunk_to_client(int client_fd, cache_chunk_t *chunk) {
    ssize_t sent = send(client_fd, chunk->data, chunk->size, MSG_NOSIGNAL);
    if (sent < 0) {
        return -1;
    }
    return 0;
}

int send_cached_data(int client_fd, cache_entry_t *entry) {
    pthread_mutex_lock(&entry->mutex);
    
    while (!entry->headers && entry->status == CACHE_LOADING && running) {
        pthread_cond_wait(&entry->cond, &entry->mutex);
    }
    
    if (entry->status == CACHE_ERROR) {
        pthread_mutex_unlock(&entry->mutex);
        return -1;
    }
    
    cache_chunk_t *current = entry->chunks;
    int chunks_read = 0;
    
    pthread_mutex_unlock(&entry->mutex);
    
    if (send_headers_to_client(client_fd, entry) < 0) {
        return -1;
    }
    
    while (running && !force_shutdown) {
        pthread_mutex_lock(&entry->mutex);
        int available_chunks = entry->num_chunks;
        cache_status_t status = entry->status;
        
        while (chunks_read < available_chunks && current) {
            cache_chunk_t *to_send = current;
            current = current->next;
            chunks_read++;
            
            pthread_mutex_unlock(&entry->mutex);
            
            if (send_chunk_to_client(client_fd, to_send) < 0) {
                return -1;
            }
            
            pthread_mutex_lock(&entry->mutex);
        }
        
        if (status == CACHE_COMPLETE || status == CACHE_ERROR) {
            pthread_mutex_unlock(&entry->mutex);
            break;
        }
        
        if (chunks_read >= available_chunks) {
            pthread_cond_wait(&entry->cond, &entry->mutex);
        }
        
        pthread_mutex_unlock(&entry->mutex);
    }
    
    return 0;
}

static int parse_request(char *buffer, char *method, char *url, char *version) {
    if (sscanf(buffer, "%15s %4095s %15s", method, url, version) != 3) {
        return -1;
    }
    return 0;
}

static void start_download_if_needed(cache_entry_t *entry, const char *request, size_t request_len) {
    pthread_mutex_lock(&entry->mutex);
    int need_download = (entry->status == CACHE_LOADING && !entry->loading_thread_active);
    if (need_download) {
        entry->loading_thread_active = 1;
        if (!entry->original_request) {
            entry->original_request = strdup(request);
            entry->request_size = request_len;
        }
    }
    pthread_mutex_unlock(&entry->mutex);
    
    if (need_download) {
        pthread_t dl_thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&dl_thread, &attr, download_thread, entry);
        pthread_attr_destroy(&attr);
    }
}

void* client_thread(void *arg) {
    client_info_t *info = (client_info_t*)arg;
    int client_fd = info->client_fd;
    char *original_request = info->original_request;
    free(info);
    
    char buffer[BUFFER_SIZE];
    int eof = 0;
    ssize_t n = recv_fully(client_fd, buffer, sizeof(buffer) - 1, &eof);
    
    if (n <= 0) {
        close(client_fd);
        free(original_request);
        return NULL;
    }
    
    buffer[n] = '\0';
    
    char method[16], url[MAX_URL_LENGTH], version[16];
    if (parse_request(buffer, method, url, version) < 0) {
        close(client_fd);
        free(original_request);
        return NULL;
    }
    
    if (strcmp(method, "GET") != 0) {
        const char *response = "HTTP/1.0 501 Not Implemented\r\n\r\n";
        send(client_fd, response, strlen(response), MSG_NOSIGNAL);
        close(client_fd);
        free(original_request);
        return NULL;
    }
    
    cache_entry_t *entry = find_or_create_cache_entry(url);
    if (!entry) {
        const char *response = "HTTP/1.0 500 Internal Server Error\r\n\r\n";
        send(client_fd, response, strlen(response), MSG_NOSIGNAL);
        close(client_fd);
        free(original_request);
        return NULL;
    }
    
    start_download_if_needed(entry, buffer, n);
    
    free(original_request);
    
    send_cached_data(client_fd, entry);
    
    release_cache_entry(entry);
    close(client_fd);
    
    return NULL;
}
