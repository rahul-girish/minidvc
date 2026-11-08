CC := gcc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude $(shell pkg-config --cflags libcrypto libzstd)
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -O2
LDLIBS := $(shell pkg-config --libs libcrypto libzstd)

TARGET := minidvc
SOURCES := src/main.c src/hash.c src/storage.c src/manifest.c src/project.c src/remote.c
OBJECTS := $(SOURCES:.c=.o)


PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TARGET)
	python3 tests/run_tests.py

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJECTS)
