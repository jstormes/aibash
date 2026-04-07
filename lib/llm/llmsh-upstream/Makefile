CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude -Ivendor
LDFLAGS = -lcurl -lreadline

SRCS    = src/main.c src/shell.c src/llm.c src/router.c src/builtin.c \
          src/exec.c src/history.c src/safety.c src/json_helpers.c \
          src/serverconf.c src/pathscan.c src/streams.c src/manscan.c \
          vendor/cJSON.c
OBJS    = $(SRCS:.c=.o)
TARGET  = llmsh

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(TEST_SRCS:.c=)

# Core sources (everything except main.c and shell.c)
CORE_SRCS = src/llm.c src/router.c src/builtin.c src/exec.c \
            src/history.c src/safety.c src/json_helpers.c src/serverconf.c \
            src/pathscan.c src/streams.c src/manscan.c vendor/cJSON.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Shell module (depends on globals from main.c or test harness)
SHELL_OBJS = src/shell.o

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t ==="; ./$$t || exit 1; done

# test_shell needs shell.o (and provides its own globals)
tests/test_shell: tests/test_shell.c $(SHELL_OBJS) $(CORE_OBJS)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(SHELL_OBJS) $(CORE_OBJS) $(LDFLAGS)

# Other tests link only core objects (no shell.o, no main globals needed)
tests/test_%: tests/test_%.c $(CORE_OBJS)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(CORE_OBJS) $(LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
