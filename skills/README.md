<!--
  INSPIRATION & TRADEMARK DISCLAIMER:
  The benchmark workloads in this repository are clean-room, non-comparable engineering workloads
  historically inspired by industry benchmarks (such as TPC-B, TPC-C, TPC-E, TPC-H, TPC-DS,
  CH-benCHmark, LDBC, and VecBench). Those names are mentioned ONLY in this opening header comment
  to acknowledge architectural inspiration. Throughout all file names, titles, documentation,
  configurations, and scripts, always use the clean-room `pgbench-*` workload names.
-->
# Experimental Agentic Skills for `pgbench4perffarm` (`//third_party/pgext/pgbench4perffarm`)

This directory contains experimental Agentic skills designed for **Jetski** (`~/.gemini/config/skills/`), internal Google3 subagents (`DeepCoder`, `invoke_subagent`, `swarm`), Gemini CLI, Claude Code, Cursor, Codex, and custom MCP/agent harnesses operating on `//third_party/pgext/pgbench4perffarm` and standalone `postsql/pgbench4perffarm`.

---

## Available Skills

| Skill Directory | Skill Name | Description |
| :--- | :--- | :--- |
| [`pgbench4perffarm/`](pgbench4perffarm/SKILL.md) | `pgbench4perffarm` | Unified progressive-disclosure skill for (A) building, testing, initializing, executing, and validating `pgbench` + `contrib/pgbench` workloads in Google3 (via `blaze-for-agents`) and standalone `PGXS` (PostgreSQL 14–20devel), and (B) setting up, running, and analyzing continuous Perffarm animals (`perffarm/`), multi-duration benchmark sequences (`--durations`), and multi-commit historical regression sweeps (`--commits-file`). |

### Skill File Layout

```text
skills/
├── README.md                                              # This installation & usage guide
└── pgbench4perffarm/
    ├── SKILL.md                                           # Main entry point (loaded when skill triggers in Jetski / Gemini)
    └── references/
        ├── pgbench_main_usage_and_recipes.md              # Part A deep reference: Google3 Blaze & PGXS, 8 workloads, fast test plan, triage
        ├── perffarm_setup_and_runner.md                   # Part B deep reference: animal.conf, run_perffarm.sh, SQL views
        ├── pgbench_order_workflow_and_porting_spec.md     # Wholesale order-entry (pgbench-order / pgbench-c) spec & SQL/non-SQL porting blueprint
        └── cli_and_functions_reference.md                 # Complete reference: 22 CLI flags, 39 meta-commands, 61 client + 51 SQL functions
```

---

## Installing the Skill Locally

### 1. Jetski (`~/.gemini/config/skills/`)
```bash
mkdir -p ~/.gemini/config/skills
cp -r /google/src/cloud/$USER/design_pgbench_lib_extraction/google3/third_party/pgext/pgbench4perffarm/skills/pgbench4perffarm \
  ~/.gemini/config/skills/
```

### 2. Gemini CLI (`~/.gemini/skills/`)
```bash
mkdir -p ~/.gemini/skills
cp -r /google/src/cloud/$USER/design_pgbench_lib_extraction/google3/third_party/pgext/pgbench4perffarm/skills/pgbench4perffarm \
  ~/.gemini/skills/
```

### 3. Claude Code (`.claude/skills/` or `~/.claude/skills/`)
```bash
mkdir -p ~/.claude/skills
cp -r /google/src/cloud/$USER/design_pgbench_lib_extraction/google3/third_party/pgext/pgbench4perffarm/skills/pgbench4perffarm \
  ~/.claude/skills/
```

### 4. Delegating to Internal Subagents (`DeepCoder` / `invoke_subagent` / `swarm`)
When delegating coding or benchmarking tasks in a CitC workspace to `DeepCoder` or another subagent, include in the subagent prompt:
> *"Read [`//third_party/pgext/pgbench4perffarm/AGENTS.md`](../AGENTS.md) and [`//third_party/pgext/pgbench4perffarm/skills/pgbench4perffarm/SKILL.md`](pgbench4perffarm/SKILL.md) before running any `pgbench`, `perffarm/`, or Blaze commands. Never run `find`, `grep`, `rg`, or `ag` in `google3/` (use `code_search`), always use `/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents` for Blaze builds/tests, and write all Perffarm `$WORKDIR` and `--json-report` outputs to `/tmp` or `/var/tmp` outside CitC."*

---

## Companion Agent-Oriented Documentation

In addition to the `pgbench4perffarm` skill, this package provides comprehensive Agent-Oriented Guides in `doc/`:
- **[Root `AGENTS.md`](../AGENTS.md)**: Package-wide agent rules, Google3 Blaze (`blaze-for-agents`) and `PGXS` build/test commands, and CitC invariants.
- **[`doc/agent-guide-pgbench.md`](../doc/agent-guide-pgbench.md)**: Agent-Oriented Guide for Main `pgbench` Usage (Intent-to-Workload Decision Matrix, Initialization State Machine, CLI Mutual Exclusions, Exit Code `0`/`1`/`2` Triage, JSONPath Assertions, and Subagent Prompt Templates).
- **[`doc/agent-guide-perffarm.md`](../doc/agent-guide-perffarm.md)**: Agent-Oriented Guide for Perffarm Setup & Running (Perffarm State Machine, Multi-Duration & Multi-Commit Sweep Recipes, 5-Point Verification Contract, Cross-Animal Replication, and Longitudinal SQL Regression Cookbook).
- **[`doc/agent-guide-pgbench-order.md`](../doc/agent-guide-pgbench-order.md)**: Agent-Oriented Specification & Re-Creation Guide for the Wholesale Order-Entry OLTP Workload (`pgbench-order` / `pgbench-c`) across SQL dialects and non-SQL query languages.
