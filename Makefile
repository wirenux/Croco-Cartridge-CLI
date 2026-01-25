CFLAGS = $(shell pkg-config --cflags libusb-1.0)
LIBS = $(shell pkg-config --libs libusb-1.0)

HOMEBREW_PREFIX = /opt/homebrew
INCLUDES = -I$(HOMEBREW_PREFIX)/include/libusb-1.0

all:
	@mkdir -p build
	gcc $(CFLAGS) $(INCLUDES) src/main.c -o build/croco_cli $(LIBS)
	@echo "\n\x1b[1;32mBuild successful!\x1b[0m\n"

run:
	@./build/croco_cli -l