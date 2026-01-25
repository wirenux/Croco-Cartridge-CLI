# Detect libusb flags using pkg-config
CFLAGS = $(shell pkg-config --cflags libusb-1.0)
LIBS = $(shell pkg-config --libs libusb-1.0)

# If pkg-config fails on Apple Silicon, it's often because /opt/homebrew/bin isn't in the PATH
# Let's add common Homebrew paths just in case
HOMEBREW_PREFIX = /opt/homebrew
INCLUDES = -I$(HOMEBREW_PREFIX)/include/libusb-1.0

all:
	@mkdir -p build
	gcc $(CFLAGS) $(INCLUDES) src/main.c -o build/croco_cli $(LIBS)
	@echo "Build successful!"

run:
	@./build/croco_cli -l