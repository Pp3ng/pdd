CC = gcc
CFLAGS = -O2 -Wall -D_GNU_SOURCE
LDFLAGS =

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