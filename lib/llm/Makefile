# Makefile for lib/llm -- LLM integration library for bash
#
# This builds libllm.a containing the LLM API client, tool system,
# agentic loop, and supporting modules.

srcdir = .

topdir = ../..
BUILD_DIR = /data/bash/bash-5.3

CC = gcc
RANLIB = ranlib
AR = ar
ARFLAGS = cr
RM = rm -f

SHELL = /bin/sh

CFLAGS = -g -O2
LOCAL_CFLAGS = 

CPPFLAGS = 
LDFLAGS =  -L./lib/termcap

DEFS = -DHAVE_CONFIG_H

INCLUDES = -I. -I$(srcdir) -I$(topdir) -I$(topdir)/include -I$(topdir)/lib

CCFLAGS = $(DEFS) $(LOCAL_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(INCLUDES)

SOURCES = llm_api.c llm_history.c llm_router.c llm_tools.c llm_exec.c \
          llm_streams.c llm_safety.c llm_serverconf.c llm_manscan.c \
          llm_shell.c llm_json_helpers.c cJSON.c

OBJECTS = llm_api.o llm_history.o llm_router.o llm_tools.o llm_exec.o \
          llm_streams.o llm_safety.o llm_serverconf.o llm_manscan.o \
          llm_shell.o llm_json_helpers.o cJSON.o

LIBRARY = libllm.a

.SUFFIXES:
.SUFFIXES: .c .o

.c.o:
	$(RM) $@
	$(CC) -c $(CCFLAGS) $<

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	$(RM) $@
	$(AR) $(ARFLAGS) $@ $(OBJECTS)
	-$(RANLIB) $@

clean:
	$(RM) $(OBJECTS) $(LIBRARY)

mostlyclean: clean

distclean maintainer-clean: clean
	$(RM) Makefile

# Dependencies
llm_api.o: llm_api.c llm_api.h llm_history.h llm_config.h llm_streams.h cJSON.h
llm_history.o: llm_history.c llm_history.h llm_api.h llm_config.h cJSON.h
llm_router.o: llm_router.c llm_router.h llm_api.h llm_tools.h llm_exec.h llm_safety.h llm_streams.h llm_manscan.h llm_config.h llm_serverconf.h cJSON.h
llm_tools.o: llm_tools.c llm_tools.h llm_manscan.h llm_streams.h llm_config.h cJSON.h
llm_exec.o: llm_exec.c llm_exec.h llm_config.h
llm_streams.o: llm_streams.c llm_streams.h
llm_safety.o: llm_safety.c llm_safety.h
llm_serverconf.o: llm_serverconf.c llm_serverconf.h llm_config.h llm_streams.h
llm_manscan.o: llm_manscan.c llm_manscan.h llm_streams.h
llm_shell.o: llm_shell.c llm_shell.h llm_api.h llm_router.h llm_history.h llm_streams.h llm_serverconf.h cJSON.h
llm_json_helpers.o: llm_json_helpers.c llm_json_helpers.h cJSON.h
cJSON.o: cJSON.c cJSON.h
