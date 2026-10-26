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
	postgres_driver.o \
	quack_driver.o \
	script.o \
	spanner_driver.o \
	stats.o \
	variable.o \
	pgbench.o

ifdef USE_PGXS
PROGRAM = pgbench
TAP_TESTS = 1
OBJS += pgbench_funcs.o
PG_CONFIG ?= pg_config
HAS_PRNG_NORMAL := $(shell grep -q pg_prng_double_normal "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/common/pg_prng.h" 2>/dev/null && echo 1 || echo 0)
HAS_PG_INIT_TIMING := $(shell grep -q pg_initialize_timing "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/portability/instr_time.h" 2>/dev/null && echo 1 || echo 0)
HAS_PSQL_SCAN_LOC := $(shell grep -q psql_scan_get_location "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/fe_utils/psqlscan.h" 2>/dev/null && echo 1 || echo 0)
HAS_JSON_LEX_OWNS := $(shell grep -q setJsonLexContextOwnsTokens "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/common/jsonapi.h" 2>/dev/null && echo 1 || echo 0)
HAS_OPENSSL := $(shell printf '#include <openssl/ssl.h>\nint main(void){return 0;}\n' | $(if $(CC),$(CC),cc) -xc - -lssl -lcrypto -o /dev/null 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAS_OPENSSL),1)
OPENSSL_CPPFLAGS = -DUSE_OPENSSL
OPENSSL_LIBS = -lssl -lcrypto
endif
PG_CPPFLAGS = -DFRONTEND -I. -I$(srcdir) -I$(srcdir)/../../include -I$(libpq_srcdir) -DHAVE_PRNG_NORMAL=$(HAS_PRNG_NORMAL) -DHAVE_PG_INIT_TIMING=$(HAS_PG_INIT_TIMING) -DHAVE_PSQL_SCAN_LOC=$(HAS_PSQL_SCAN_LOC) -DHAVE_JSON_LEX_OWNS=$(HAS_JSON_LEX_OWNS) $(OPENSSL_CPPFLAGS)
PG_LIBS_INTERNAL = -L$(libdir) -lpgfeutils $(libpq_pgport)
PG_LIBS = $(PTHREAD_LIBS) $(OPENSSL_LIBS) -lm
EXTRA_CLEAN = exprparse.h exprparse.c exprscan.c pgbench_funcs.c tmp_check
override with_llvm = no

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifneq ($(enable_tap_tests),yes)
ifeq ($(PROVE),)
PROVE := $(shell command -v prove 2>/dev/null)
endif
ifneq ($(PROVE),)
ifneq ($(wildcard $(top_srcdir)/src/test/perl/PostgreSQL/Test/Cluster.pm),)
define prove_installcheck
echo "# +++ tap install-check in $(subdir) +++" && \
rm -rf '$(CURDIR)'/tmp_check && \
$(MKDIR_P) '$(CURDIR)'/tmp_check && \
cd $(srcdir) && \
   TESTLOGDIR='$(CURDIR)/tmp_check/log' \
   TESTDATADIR='$(CURDIR)/tmp_check' \
   PATH="$(CURDIR):$(bindir):$$PATH" \
   PGPORT='6$(DEF_PGPORT)' \
   PG_REGRESS='$(top_builddir)/src/test/regress/pg_regress' \
   $(PROVE) $(PG_PROVE_FLAGS) $(PROVE_FLAGS) $(if $(PROVE_TESTS),$(PROVE_TESTS),t/*.pl)
endef
endif
endif
endif

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
$(OBJS): pgbench.h db_driver.h variable.h config.h dataset.h script.h commands.h engine.h stats.h poller.h init.h logdb.h

install: install-workloads

install-workloads: installdirs
	$(MKDIR_P) '$(DESTDIR)$(datadir)/pgbench/workloads' '$(DESTDIR)$(datadir)/pgbench/viewer'
	cp -R $(srcdir)/workloads/* '$(DESTDIR)$(datadir)/pgbench/workloads/'
	cp -R $(srcdir)/viewer/* '$(DESTDIR)$(datadir)/pgbench/viewer/'

uninstall: uninstall-workloads

uninstall-workloads:
	rm -rf '$(DESTDIR)$(datadir)/pgbench'

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
ifneq ($(wildcard /usr/include/openssl/ssl.h),)
override CPPFLAGS += -DUSE_OPENSSL
LIBS += -lssl -lcrypto
endif


all: pgbench

pgbench: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# See notes in src/backend/parser/Makefile about the following two rules
exprparse.h: exprparse.c
	touch $@

exprparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
exprparse.o exprscan.o: exprparse.h
$(OBJS): pgbench.h db_driver.h variable.h config.h dataset.h script.h commands.h engine.h stats.h poller.h init.h logdb.h

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
