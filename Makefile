CC := gcc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude $(shell pkg-config --cflags libcrypto libzstd)
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -O2
LDLIBS := $(shell pkg-config --libs libcrypto libzstd)

TARGET := minidvc
SOURCES := src/main.c src/hash.c src/storage.c
OBJECTS := $(SOURCES:.c=.o)


.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS)
