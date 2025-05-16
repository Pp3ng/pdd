CC = gcc
CFLAGS = -O3 -Wall -pedantic
LDFLAGS =

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DHAVE_LINUX_FEATURES
endif
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S),FreeBSD)
    CFLAGS += -D_BSD_SOURCE
endif

TARGET = pdd
SRC = pdd.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
	rm -rf test_dir

test: $(TARGET)
	./test_dd.sh 