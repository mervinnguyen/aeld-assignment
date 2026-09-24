CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Werror -g
LDFLAGS ?=

TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)

default: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
