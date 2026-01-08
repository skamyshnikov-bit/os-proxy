CC = gcc
CFLAGS = -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L
TARGET = proxy
SRCS = main.c cache.c network.c client.c download.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf cache/

# Dependencies
main.o: main.c common.h cache.h client.h
cache.o: cache.c cache.h common.h
network.o: network.c network.h common.h
client.o: client.c client.h common.h cache.h network.h download.h
download.o: download.c download.h cache.h network.h common.h
