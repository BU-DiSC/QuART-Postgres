#-------------------------------------------------------------------------
#
# Makefile for quart_bench/replay
#
# Builds the offline replay tool against this tree's lib/radixtree.h and
# lib/quart.h (not installed headers), linking the backend memory-context
# objects of an already-built PostgreSQL tree.
#
#   make                         # in-tree build (./configure && make at the root)
#   make PG_BUILD=/path/to/build # VPATH build directory
#
#-------------------------------------------------------------------------

PG_SRC   ?= $(abspath ..)
PG_BUILD ?= $(PG_SRC)

CC       ?= gcc
CFLAGS   ?= -O2 -march=native
CPPFLAGS := -I$(PG_BUILD)/src/include -I$(PG_SRC)/src/include

MMGR := $(addprefix $(PG_BUILD)/src/backend/utils/mmgr/, \
	mcxt.o aset.o slab.o generation.o bump.o alignedalloc.o memdebug.o)
LIBS := $(PG_BUILD)/src/common/libpgcommon_srv.a \
	$(PG_BUILD)/src/port/libpgport_srv.a -lm

HEADERS := $(PG_SRC)/src/include/lib/radixtree.h $(PG_SRC)/src/include/lib/quart.h

all: replay

replay: replay.o elog_shim.o $(MMGR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

replay.o: replay.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

elog_shim.o: elog_shim.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(MMGR):
	@echo "missing $@: build PostgreSQL first (or set PG_BUILD)" >&2; exit 1

clean:
	rm -f replay replay.o elog_shim.o

.PHONY: all clean
