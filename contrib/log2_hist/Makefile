# contrib/log2_hist/Makefile

MODULE_big = log2_hist
OBJS = \
	$(WIN32RES) \
	log2_hist.o

EXTENSION = log2_hist
DATA = log2_hist--1.0.sql
PGFILEDESC = "log2_hist - 64-bucket RLE-compressed log2(V+1) histogram data type"

REGRESS = log2_hist

ifeq ($(wildcard ../../src/Makefile.global),)
USE_PGXS = 1
endif

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CLANG := $(shell awk -F'= *' '/^CLANG *=/{print $$2}' "$(dir $(PGXS))../../src/Makefile.global" 2>/dev/null)
ifneq ($(PG_CLANG),)
ifeq ($(wildcard $(PG_CLANG)),)
override with_llvm = no
endif
endif
include $(PGXS)
else
subdir = contrib/log2_hist
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lm, $(LIBS))
