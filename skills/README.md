# Experimental Agentic Skills for `postsql/pgbench4perffarm`

This directory contains experimental Agentic skills designed for AI coding and benchmarking agents (Gemini CLI, Claude Code, Cursor, Codex, and custom MCP/agent harnesses) operating on `postsql/pgbench4perffarm`.

---

## Available Skills

| Skill Directory | Skill Name | Description |
| :--- | :--- | :--- |
| [`pgbench4perffarm/`](pgbench4perffarm/SKILL.md) | `pgbench4perffarm` | Unified progressive-disclosure skill for (A) building, initializing, executing, and validating `pgbench` + `contrib/pgbench` workloads across PostgreSQL 14–20devel, and (B) setting up, running, and analyzing continuous Perffarm animals (`perffarm/`), multi-duration benchmark sequences (`--durations`), and multi-commit historical regression sweeps (`--commits-file`). |

### Skill File Layout

```text
skills/
├── README.md                                              # This installation & usage guide
└── pgbench4perffarm/
    ├── SKILL.md                                           # Main entry point (loaded when skill triggers)
    └── references/
        ├── pgbench_main_usage_and_recipes.md              # Part A deep reference: 8 workloads, fast test plan, triage
        ├── perffarm_setup_and_runner.md                   # Part B deep reference: animal.conf, run_perffarm.sh, SQL views
        └── cli_and_functions_reference.md                 # Complete reference: 22 CLI flags, 39 meta-commands, 61 client + 51 SQL functions
```

---

## Installing the Skill Locally

### 1. Gemini CLI (`~/.gemini/skills/` or `~/.gemini/config/skills/`)
```bash
mkdir -p ~/.gemini/skills
cp -r /path/to/pgbench4perffarm/skills/pgbench4perffarm ~/.gemini/skills/
```

### 2. Claude Code (`.claude/skills/` or `~/.claude/skills/`)
```bash
mkdir -p ~/.claude/skills
cp -r /path/to/pgbench4perffarm/skills/pgbench4perffarm ~/.claude/skills/
```

### 3. Direct Subagent / Prompt Injection
If your agent framework does not automatically discover `SKILL.md` frontmatter, instruct the agent at the start of its task:
> *"Read [`AGENTS.md`](../AGENTS.md) and [`skills/pgbench4perffarm/SKILL.md`](pgbench4perffarm/SKILL.md) before running any `pgbench` or `perffarm/` commands."*

---

## Companion Agent-Oriented Documentation

In addition to the `pgbench4perffarm` skill, this repository provides comprehensive Agent-Oriented Guides in `doc/`:
- **[Root `AGENTS.md`](../AGENTS.md)**: Repository-wide agent rules, build/test commands, and invariants.
- **[`doc/agent-guide-pgbench.md`](../doc/agent-guide-pgbench.md)**: Agent-Oriented Guide for Main `pgbench` Usage (Intent-to-Workload Decision Matrix, Initialization State Machine, CLI Mutual Exclusions, Exit Code `0`/`1`/`2` Triage, JSONPath Assertions, and Subagent Prompt Templates).
- **[`doc/agent-guide-perffarm.md`](../doc/agent-guide-perffarm.md)**: Agent-Oriented Guide for Perffarm Setup & Running (Perffarm State Machine, Multi-Duration & Multi-Commit Sweep Recipes, 5-Point Verification Contract, Cross-Animal Replication, and Longitudinal SQL Regression Cookbook).
