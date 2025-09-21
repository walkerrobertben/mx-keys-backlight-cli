CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

# Try pkg-config first; fall back to -lhidapi
HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi 2>/dev/null | sed 's|/hidapi$$||')
HIDAPI_LIBS := $(shell pkg-config --libs hidapi 2>/dev/null)

ifeq ($(HIDAPI_LIBS),)
  HIDAPI_LIBS := -lhidapi
endif

TARGET := mx-keys-backlight
SRCS := mx-keys-backlight.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(HIDAPI_CFLAGS) -o $@ $(SRCS) $(HIDAPI_LIBS)

.PHONY: clean
clean:
	rm -f $(TARGET)
