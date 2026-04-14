# Makefile for lib/llm/agents -- Side agent framework
#
# make          -- build libagents.a
# make test     -- build and run tests

CC = gcc
AR = ar
RANLIB = ranlib
RM = rm -f

CFLAGS = -g -O2 -Wall -Wextra -Wno-unused-parameter
INCLUDES = -I.

# Library
SOURCES = side_agent.c mem_agent.c cron_agent.c calendar_agent.c agents_setup.c
OBJECTS = side_agent.o mem_agent.o cron_agent.o calendar_agent.o agents_setup.o
LIBRARY = libagents.a

# Tests
TEST_SOURCES = tests/test_main.c tests/test_side_agent.c \
               tests/test_mem_agent.c tests/test_cron_agent.c \
               tests/test_calendar_agent.c tests/test_integration.c
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
TEST_BIN = tests/run_tests

.SUFFIXES:
.SUFFIXES: .c .o

.c.o:
	$(RM) $@
	$(CC) -c $(CFLAGS) $(INCLUDES) -o $@ $<

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	$(RM) $@
	$(AR) cr $@ $(OBJECTS)
	-$(RANLIB) $@

test: $(TEST_BIN)
	./$(TEST_BIN)

# For tests, recompile agents with AGENT_TESTING to stub production deps
tests/mem_agent_test.o: mem_agent.c mem_agent.h side_agent.h
	$(CC) -c $(CFLAGS) $(INCLUDES) -DAGENT_TESTING -o $@ $<

tests/cron_agent_test.o: cron_agent.c cron_agent.h side_agent.h
	$(CC) -c $(CFLAGS) $(INCLUDES) -DAGENT_TESTING -o $@ $<

tests/calendar_agent_test.o: calendar_agent.c calendar_agent.h side_agent.h
	$(CC) -c $(CFLAGS) $(INCLUDES) -DAGENT_TESTING -o $@ $<

$(TEST_BIN): $(TEST_OBJECTS) side_agent.o tests/mem_agent_test.o tests/cron_agent_test.o tests/calendar_agent_test.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) $(OBJECTS) $(LIBRARY)
	$(RM) $(TEST_OBJECTS) tests/mem_agent_test.o tests/cron_agent_test.o tests/calendar_agent_test.o $(TEST_BIN)

# Dependencies
side_agent.o: side_agent.c side_agent.h
mem_agent.o: mem_agent.c mem_agent.h side_agent.h
tests/test_main.o: tests/test_main.c tests/test_harness.h
tests/test_side_agent.o: tests/test_side_agent.c tests/test_harness.h side_agent.h
tests/test_mem_agent.o: tests/test_mem_agent.c tests/test_harness.h mem_agent.h
cron_agent.o: cron_agent.c cron_agent.h side_agent.h
agents_setup.o: agents_setup.c agents_setup.h side_agent.h mem_agent.h cron_agent.h
calendar_agent.o: calendar_agent.c calendar_agent.h side_agent.h
tests/test_cron_agent.o: tests/test_cron_agent.c tests/test_harness.h cron_agent.h
tests/test_calendar_agent.o: tests/test_calendar_agent.c tests/test_harness.h calendar_agent.h
tests/test_integration.o: tests/test_integration.c tests/test_harness.h side_agent.h mem_agent.h cron_agent.h
