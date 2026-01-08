#ifndef CACHE_H
#define CACHE_H

#include "common.h"

typedef enum {
    CACHE_LOADING,
    CACHE_COMPLETE,
    CACHE_ERROR
} cache_status_t;

typedef struct cache_chunk {
    char *data;
    size_t size;
    struct cache_chunk *next;
} cache_chunk_t;

typedef struct cache_entry {
    char *url;
    char *headers;
    size_t headers_size;
    int status_code;
    cache_chunk_t *chunks;
    cache_chunk_t *last_chunk;
    size_t total_size;
    cache_status_t status;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ref_count;
    time_t last_accessed;
    int loading_thread_active;
    struct cache_entry *next;
    struct cache_entry *prev;
    char *cache_file_path;
    int cache_fd;
    int num_chunks;
    char *original_request;
    size_t request_size;
} cache_entry_t;

typedef struct {
    cache_entry_t *head;
    cache_entry_t *tail;
    pthread_mutex_t mutex;
    size_t total_size;
    size_t max_size;
} cache_t;

void init_cache(size_t max_size_mb);
void cleanup_cache(void);
cache_entry_t* find_or_create_cache_entry(const char *url);
void release_cache_entry(cache_entry_t *entry);
int add_chunk_to_cache(cache_entry_t *entry, const char *data, size_t size);
void set_cache_error(cache_entry_t *entry);
void* gc_thread_func(void *arg);

cache_t* get_cache(void);

#endif
