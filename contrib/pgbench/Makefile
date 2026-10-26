# contrib/pgbench/Makefile

MODULE_big = pgbench
OBJS = \
	$(WIN32RES) \
	pgbench.o \
	pgbench_funcs.o

EXTENSION = pgbench
DATA = pgbench--1.0.sql
PGFILEDESC = "pgbench - random distributions and benchmarking utilities"

REGRESS = pgbench

ifeq ($(wildcard ../../src/Makefile.global),)
USE_PGXS = 1
endif

ifdef USE_PGXS
PG_CONFIG ?= pg_config
HAS_PRNG_NORMAL := $(shell grep -q pg_prng_double_normal "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/common/pg_prng.h" 2>/dev/null && echo 1 || echo 0)
HAS_CONSTRUCT_ARRAY_BUILTIN := $(shell grep -q construct_array_builtin "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/utils/array.h" 2>/dev/null && echo 1 || echo 0)
PG_CPPFLAGS = -I$(srcdir)/../../src/include -DHAVE_PRNG_NORMAL=$(HAS_PRNG_NORMAL) -DHAVE_CONSTRUCT_ARRAY_BUILTIN=$(HAS_CONSTRUCT_ARRAY_BUILTIN)
EXTRA_CLEAN = pgbench_funcs.c
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CLANG := $(shell awk -F'= *' '/^CLANG *=/{print $$2}' "$(dir $(PGXS))../../src/Makefile.global" 2>/dev/null)
ifneq ($(PG_CLANG),)
ifeq ($(wildcard $(PG_CLANG)),)
override with_llvm = no
endif
endif
include $(PGXS)

pgbench_funcs.c: $(srcdir)/../../src/common/pgbench_funcs.c
	rm -f $@ && $(LN_S) $< .

pgbench_funcs.o: pgbench_funcs.c
else
EXTRA_CLEAN = pgbench_funcs.c
subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

# pgbench_funcs.c is shared from src/common
pgbench_funcs.c: % : $(top_srcdir)/src/common/%
	rm -f $@ && $(LN_S) $< .
endif
