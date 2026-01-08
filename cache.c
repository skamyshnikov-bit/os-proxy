#include "cache.h"

static cache_t cache;

void init_cache(size_t max_size_mb) {
    cache.head = NULL;
    cache.tail = NULL;
    pthread_mutex_init(&cache.mutex, NULL);
    cache.total_size = 0;
    cache.max_size = max_size_mb * 1024 * 1024;
    
    mkdir(CACHE_DIR, 0755);
}

cache_t* get_cache(void) {
    return &cache;
}

static unsigned long hash_url(const char *url) {
    unsigned long hash = 5381;
    int c;
    while ((c = *url++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static char* get_cache_file_path(const char *url) {
    unsigned long hash = hash_url(url);
    char *path = malloc(512);
    if (path) {
        snprintf(path, 512, "%s/%lu.cache", CACHE_DIR, hash);
    }
    return path;
}

static cache_entry_t* create_cache_entry(const char *url) {
    cache_entry_t *entry = calloc(1, sizeof(cache_entry_t));
    if (!entry) return NULL;
    
    entry->url = strdup(url);
    if (!entry->url) {
        free(entry);
        return NULL;
    }
    
    entry->cache_file_path = get_cache_file_path(url);
    if (!entry->cache_file_path) {
        free(entry->url);
        free(entry);
        return NULL;
    }
    
    entry->status = CACHE_LOADING;
    entry->status_code = 0;
    entry->cache_fd = -1;
    pthread_mutex_init(&entry->mutex, NULL);
    pthread_cond_init(&entry->cond, NULL);
    entry->ref_count = 1;
    entry->last_accessed = time(NULL);
    entry->loading_thread_active = 0;
    entry->num_chunks = 0;
    
    return entry;
}

static cache_entry_t* find_cache_entry_locked(const char *url) {
    cache_entry_t *entry = cache.head;
    while (entry) {
        if (strcmp(entry->url, url) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

cache_entry_t* find_or_create_cache_entry(const char *url) {
    pthread_mutex_lock(&cache.mutex);
    
    cache_entry_t *entry = find_cache_entry_locked(url);
    if (entry) {
        pthread_mutex_lock(&entry->mutex);
        entry->ref_count++;
        entry->last_accessed = time(NULL);
        pthread_mutex_unlock(&entry->mutex);
        pthread_mutex_unlock(&cache.mutex);
        return entry;
    }
    
    entry = create_cache_entry(url);
    if (entry) {
        entry->next = cache.head;
        entry->prev = NULL;
        if (cache.head) {
            cache.head->prev = entry;
        }
        cache.head = entry;
        if (!cache.tail) {
            cache.tail = entry;
        }
    }
    
    pthread_mutex_unlock(&cache.mutex);
    return entry;
}

void release_cache_entry(cache_entry_t *entry) {
    if (!entry) return;
    pthread_mutex_lock(&entry->mutex);
    entry->ref_count--;
    pthread_mutex_unlock(&entry->mutex);
}

static void remove_entry_from_list(cache_entry_t *entry) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache.head = entry->next;
    }
    
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache.tail = entry->prev;
    }
}

static void free_cache_entry(cache_entry_t *entry) {
    if (!entry) return;
    
    if (entry->cache_fd >= 0) {
        close(entry->cache_fd);
    }
    
    if (entry->cache_file_path) {
        unlink(entry->cache_file_path);
        free(entry->cache_file_path);
    }
    
    free(entry->url);
    free(entry->headers);
    free(entry->original_request);
    
    cache_chunk_t *chunk = entry->chunks;
    while (chunk) {
        cache_chunk_t *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    
    pthread_mutex_destroy(&entry->mutex);
    pthread_cond_destroy(&entry->cond);
    free(entry);
}

int add_chunk_to_cache(cache_entry_t *entry, const char *data, size_t size) {
    cache_chunk_t *chunk = malloc(sizeof(cache_chunk_t));
    if (!chunk) return -1;
    
    chunk->data = malloc(size);
    if (!chunk->data) {
        free(chunk);
        return -1;
    }
    
    memcpy(chunk->data, data, size);
    chunk->size = size;
    chunk->next = NULL;
    
    pthread_mutex_lock(&cache.mutex);
    pthread_mutex_lock(&entry->mutex);
    
    int fd = entry->cache_fd;
    if (fd < 0 && entry->cache_file_path) {
        entry->cache_fd = open(entry->cache_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fd = entry->cache_fd;
    }
    
    if (!entry->chunks) {
        entry->chunks = chunk;
        entry->last_chunk = chunk;
    } else {
        entry->last_chunk->next = chunk;
        entry->last_chunk = chunk;
    }
    
    entry->total_size += size;
    entry->num_chunks++;
    cache.total_size += size;
    
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
    pthread_mutex_unlock(&cache.mutex);
    
    if (fd >= 0) {
        write(fd, data, size);
    }
    
    return 0;
}

void set_cache_error(cache_entry_t *entry) {
    pthread_mutex_lock(&entry->mutex);
    entry->status = CACHE_ERROR;
    entry->loading_thread_active = 0;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
}

void* gc_thread_func(void *arg) {
    (void)arg;
    
    while (running && !force_shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += GC_CHECK_INTERVAL;
        
        pthread_mutex_lock(&cache.mutex);
        
        while (running && !force_shutdown && 
               cache.total_size < cache.max_size * 0.9) {
            pthread_mutex_unlock(&cache.mutex);
            sleep(GC_CHECK_INTERVAL);
            pthread_mutex_lock(&cache.mutex);
        }
        
        if (!running || force_shutdown) {
            pthread_mutex_unlock(&cache.mutex);
            break;
        }
        
        cache_entry_t *oldest = NULL;
        time_t oldest_time = time(NULL);
        
        cache_entry_t *entry = cache.tail;
        while (entry) {
            pthread_mutex_lock(&entry->mutex);
            if (entry->ref_count == 0 && 
                entry->status == CACHE_COMPLETE &&
                entry->last_accessed < oldest_time) {
                oldest = entry;
                oldest_time = entry->last_accessed;
            }
            pthread_mutex_unlock(&entry->mutex);
            entry = entry->prev;
        }
        
        if (oldest) {
            pthread_mutex_lock(&oldest->mutex);
            if (oldest->ref_count == 0) {
                cache.total_size -= oldest->total_size;
                remove_entry_from_list(oldest);
                pthread_mutex_unlock(&cache.mutex);
                pthread_mutex_unlock(&oldest->mutex);
                free_cache_entry(oldest);
            } else {
                pthread_mutex_unlock(&oldest->mutex);
                pthread_mutex_unlock(&cache.mutex);
            }
        } else {
            pthread_mutex_unlock(&cache.mutex);
        }
    }
    
    return NULL;
}

void cleanup_cache(void) {
    pthread_mutex_lock(&cache.mutex);
    cache_entry_t *entry = cache.head;
    while (entry) {
        cache_entry_t *next = entry->next;
        free_cache_entry(entry);
        entry = next;
    }
    pthread_mutex_unlock(&cache.mutex);
    pthread_mutex_destroy(&cache.mutex);
}
