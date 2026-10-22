CC := gcc
CPPFLAGS := -Iinclude $(shell pkg-config --cflags libcrypto)
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -O2
LDLIBS := $(shell pkg-config --libs libcrypto)

TARGET := minidvc
SOURCES := src/main.c src/hash.c
OBJECTS := $(SOURCES:.c=.o)


.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS)
