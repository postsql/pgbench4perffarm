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
PG_CPPFLAGS = -I$(srcdir)/../../src/include
EXTRA_CLEAN = pgbench_funcs.c
PGXS := $(shell $(PG_CONFIG) --pgxs)
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
