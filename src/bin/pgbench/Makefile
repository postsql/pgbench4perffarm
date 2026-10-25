# src/bin/pgbench/Makefile

PGFILEDESC = "pgbench - a simple program for running benchmark tests"
PGAPPICON = win32

ifeq ($(wildcard ../../../src/Makefile.global),)
USE_PGXS = 1
endif

OBJS = \
	$(WIN32RES) \
	exprparse.o \
	exprscan.o \
	commands.o \
	config.o \
	dataset.o \
	engine.o \
	init.o \
	logdb.o \
	poller.o \
	script.o \
	stats.o \
	variable.o \
	pgbench.o

ifdef USE_PGXS
PROGRAM = pgbench
TAP_TESTS = 1
OBJS += pgbench_funcs.o
PG_CPPFLAGS = -DFRONTEND -I. -I$(srcdir) -I$(srcdir)/../../include -I$(libpq_srcdir)
PG_LIBS_INTERNAL = -L$(libdir) -lpgfeutils $(libpq_pgport)
PG_LIBS = $(PTHREAD_LIBS) -lm
EXTRA_CLEAN = exprparse.h exprparse.c exprscan.c pgbench_funcs.c tmp_check

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
else
override CPPFLAGS += -DFD_SETSIZE=1024
endif

pgbench_funcs.c: $(srcdir)/../../common/pgbench_funcs.c
	rm -f $@ && $(LN_S) $< .

pgbench_funcs.o: pgbench_funcs.c

exprparse.h: exprparse.c
	touch $@

exprparse.c: BISONFLAGS += -d

exprparse.o exprscan.o: exprparse.h
$(OBJS): pgbench.h variable.h config.h dataset.h script.h commands.h engine.h stats.h poller.h init.h logdb.h

install: install-workloads

install-workloads: installdirs
	$(MKDIR_P) '$(DESTDIR)$(datadir)/pgbench/workloads' '$(DESTDIR)$(datadir)/pgbench/viewer'
	cp -R $(srcdir)/workloads/* '$(DESTDIR)$(datadir)/pgbench/workloads/'
	cp -R $(srcdir)/viewer/* '$(DESTDIR)$(datadir)/pgbench/viewer/'

else
EXTRA_INSTALL = contrib/pgbench

subdir = src/bin/pgbench
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
else
override CPPFLAGS += -DFD_SETSIZE=1024
endif
LIBS += $(PTHREAD_LIBS)


all: pgbench

pgbench: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# See notes in src/backend/parser/Makefile about the following two rules
exprparse.h: exprparse.c
	touch $@

exprparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
exprparse.o exprscan.o: exprparse.h
$(OBJS): pgbench.h variable.h config.h dataset.h script.h commands.h engine.h stats.h poller.h init.h logdb.h

install: all installdirs
	$(INSTALL_PROGRAM) pgbench$(X) '$(DESTDIR)$(bindir)/pgbench$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pgbench$(X)'

clean distclean:
	rm -f pgbench$(X) $(OBJS)
	rm -rf tmp_check
	rm -f exprparse.h exprparse.c exprscan.c

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
endif
