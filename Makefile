# Top-level Makefile for pgbench4perffarm
# Builds and installs both the contrib/pgbench server extension and
# the src/bin/pgbench client benchmark driver against any supported
# PostgreSQL installation (PostgreSQL 14 through 20devel).

PG_CONFIG ?= pg_config

.PHONY: all extension client install uninstall clean distclean check tapcheck installcheck

all: extension client

extension:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" all

client:
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" all

install: all
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" install
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" install

uninstall:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" uninstall
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" uninstall

clean:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" clean
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" clean

distclean: clean

installcheck:
	$(MAKE) -C contrib/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" installcheck

tapcheck:
	$(MAKE) -C src/bin/pgbench USE_PGXS=1 PG_CONFIG="$(PG_CONFIG)" installcheck

check: tapcheck
