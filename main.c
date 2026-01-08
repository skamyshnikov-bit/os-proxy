#include "common.h"
#include "cache.h"
#include "client.h"

volatile sig_atomic_t running = 1;
volatile sig_atomic_t force_shutdown = 0;
static int listen_fd = -1;
static pthread_t gc_thread;

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        if (!running) {
            force_shutdown = 1;
        }
        running = 0;
        if (listen_fd >= 0) {
            shutdown(listen_fd, SHUT_RDWR);
        }
    } else if (sig == SIGALRM) {
        fprintf(stderr, "\nTimeout waiting for threads to terminate, forcing exit\n");
        exit(1);
    }
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nOptions:\n");
    printf("  -p, --port PORT          Listen port (default: %d)\n", DEFAULT_PORT);
    printf("  -c, --cache SIZE         Cache size in MB (default: %d)\n", DEFAULT_CACHE_SIZE_MB);
    printf("  -h, --help               Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s -p 8080 -c 200\n", prog_name);
    printf("  %s --port 3128 --cache 500\n", prog_name);
}

static int parse_arguments(int argc, char *argv[], int *port, int *cache_size_mb) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            *port = atoi(argv[++i]);
            if (*port <= 0 || *port > 65535) {
                fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
                return -1;
            }
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cache") == 0) && i + 1 < argc) {
            *cache_size_mb = atoi(argv[++i]);
            if (*cache_size_mb <= 0) {
                fprintf(stderr, "Error: Invalid cache size: %s\n", argv[i]);
                return -1;
            }
        } else if (i == 1 && argc == 2) {
            *port = atoi(argv[i]);
            if (*port <= 0 || *port > 65535) {
                fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
                print_usage(argv[0]);
                return -1;
            }
        } else if (i == 2 && argc == 3) {
            *cache_size_mb = atoi(argv[i]);
            if (*cache_size_mb <= 0) {
                fprintf(stderr, "Error: Invalid cache size: %s\n", argv[i]);
                return -1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static void setup_signals(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, signal_handler);
}

static int create_listen_socket(int port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    
    if (listen(listen_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    
    return 0;
}

static void handle_client_connection(int client_fd, struct sockaddr_in client_addr) {
    client_info_t *info = malloc(sizeof(client_info_t));
    if (!info) {
        close(client_fd);
        return;
    }
    info->client_fd = client_fd;
    info->client_addr = client_addr;
    info->original_request = NULL;
    
    pthread_t thread;
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    
    if (pthread_create(&thread, &thread_attr, client_thread, info) != 0) {
        perror("pthread_create");
        close(client_fd);
        free(info);
    }
    
    pthread_attr_destroy(&thread_attr);
}

static void accept_loop(void) {
    while (running && !force_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF) {
                continue;
            }
            if (running) {
                perror("accept");
            }
            continue;
        }
        
        handle_client_connection(client_fd, client_addr);
    }
}

static void shutdown_gracefully(void) {
    printf("Shutting down gracefully...\n");
    
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    
    alarm(5);
    
    int join_result = pthread_join(gc_thread, NULL);
    
    alarm(0);
    
    if (join_result != 0) {
        fprintf(stderr, "Warning: pthread_join failed, forcing exit\n");
        exit(1);
    }
    
    sleep(1);
    
    cleanup_cache();
    
    printf("Server stopped successfully.\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int cache_size_mb = DEFAULT_CACHE_SIZE_MB;
    
    if (parse_arguments(argc, argv, &port, &cache_size_mb) < 0) {
        return (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) ? 0 : 1;
    }
    
    printf("Configuration:\n");
    printf("  Port:       %d\n", port);
    printf("  Cache size: %d MB\n", cache_size_mb);
    
    setup_signals();
    
    init_cache(cache_size_mb);
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&gc_thread, &attr, gc_thread_func, NULL);
    pthread_attr_destroy(&attr);
    
    if (create_listen_socket(port) < 0) {
        return 1;
    }
    
    printf("Proxy server is listening on port %d\n", port);
    
    accept_loop();
    
    shutdown_gracefully();
    
    return 0;
}
