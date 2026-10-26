# Top-level Makefile for pgbench4perffarm
# Builds and installs both the contrib/pgbench server extension and
# the src/bin/pgbench client benchmark driver against PostgreSQL installations
# (PostgreSQL 10 through 20devel; client uses PG 14+ libpq/fe_utils when target < 14).

PG_CONFIG ?= pg_config
SYS_PG_CONFIG ?= $(shell for c in \
	"$(HOME)/pg20_install/bin/pg_config" \
	"$(HOME)/pg18/bin/pg_config" \
	"/var/tmp/pgbench4perffarm_work/client_pg_install/bin/pg_config" \
	"/var/tmp/pgbench4perffarm_work/metrics_pg_install/bin/pg_config" \
	"/usr/bin/pg_config" \
	"$$(command -v pg_config 2>/dev/null)"; do \
	if [ -n "$$c" ] && [ -x "$$c" ] && \
	   grep -q PQenterPipelineMode "$$($$c --includedir 2>/dev/null)/libpq-fe.h" 2>/dev/null && \
	   [ -f "$$($$c --includedir-server 2>/dev/null)/common/fe_memutils.h" ]; then \
		echo "$$c"; break; \
	fi; \
done)

HAS_SUBSCRIPTING := $(shell test -d contrib/log2_hist && test -f "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/nodes/subscripting.h" && grep -q construct_array_builtin "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/utils/array.h" 2>/dev/null && echo yes || echo no)
HAS_LIBPQ_PIPELINE := $(shell \
	grep -q PQenterPipelineMode "$$($(PG_CONFIG) --includedir 2>/dev/null)/libpq-fe.h" 2>/dev/null && \
	grep -q pg_malloc0_array "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/common/fe_memutils.h" 2>/dev/null && \
	grep -q psql_scan_get_location "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/fe_utils/psqlscan.h" 2>/dev/null && \
	grep -q setJsonLexContextOwnsTokens "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/common/jsonapi.h" 2>/dev/null && \
	grep -q pg_initialize_timing "$$($(PG_CONFIG) --includedir-server 2>/dev/null)/portability/instr_time.h" 2>/dev/null && \
	echo yes || echo no)

ifeq ($(HAS_LIBPQ_PIPELINE),yes)
CLIENT_PG_CONFIG := $(PG_CONFIG)
else
CLIENT_PG_CONFIG := $(if $(SYS_PG_CONFIG),$(SYS_PG_CONFIG),$(PG_CONFIG))
endif

.PHONY: all extension client install uninstall clean distclean check tapcheck installcheck

all: extension client

extension:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" all
ifeq ($(HAS_SUBSCRIPTING),yes)
	$(MAKE) -C contrib/log2_hist USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" all
endif

client:
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(CLIENT_PG_CONFIG)" all

install: all
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" install
ifeq ($(HAS_SUBSCRIPTING),yes)
	$(MAKE) -C contrib/log2_hist USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" install
endif
ifeq ($(HAS_LIBPQ_PIPELINE),yes)
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" install
else
	mkdir -p '$(DESTDIR)$(shell $(PG_CONFIG) --bindir)' '$(DESTDIR)$(shell $(PG_CONFIG) --sharedir)/pgbench/workloads' '$(DESTDIR)$(shell $(PG_CONFIG) --sharedir)/pgbench/viewer'
	install -m 755 src/bin/pgbench/pgbench '$(DESTDIR)$(shell $(PG_CONFIG) --bindir)/pgbench'
	cp -R src/bin/pgbench/workloads/* '$(DESTDIR)$(shell $(PG_CONFIG) --sharedir)/pgbench/workloads/'
	cp -R src/bin/pgbench/viewer/* '$(DESTDIR)$(shell $(PG_CONFIG) --sharedir)/pgbench/viewer/'
endif

uninstall:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" uninstall
ifeq ($(HAS_SUBSCRIPTING),yes)
	$(MAKE) -C contrib/log2_hist USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" uninstall
endif
ifeq ($(HAS_LIBPQ_PIPELINE),yes)
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" uninstall
else
	rm -f '$(DESTDIR)$(shell $(PG_CONFIG) --bindir)/pgbench'
	rm -rf '$(DESTDIR)$(shell $(PG_CONFIG) --sharedir)/pgbench'
endif

clean:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" clean || true
ifeq ($(HAS_SUBSCRIPTING),yes)
	$(MAKE) -C contrib/log2_hist USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" clean || true
endif
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(CLIENT_PG_CONFIG)" clean || true
	rm -rf contrib/pgbench/.deps contrib/log2_hist/.deps src/bin/pgbench/.deps

distclean: clean

installcheck:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" installcheck
ifeq ($(HAS_SUBSCRIPTING),yes)
	$(MAKE) -C contrib/log2_hist USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" installcheck
endif

tapcheck:
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(CLIENT_PG_CONFIG)" installcheck

check: tapcheck
