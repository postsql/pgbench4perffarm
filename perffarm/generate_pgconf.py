#!/usr/bin/env python3
"""
generate_pgconf.py — Deterministic PostgreSQL server configuration generator
for pgbench4perffarm animals.

Sizes PostgreSQL memory, WAL, parallelism, I/O cost model, and observability
GUCs from either:
  1. Hardware discovery profile (hardware_profile.json / server_instance_info.json), or
  2. Explicit preset tier (small-dev, medium-vm, large-baremetal),
and filters version-specific GUCs across PostgreSQL 14 through 20devel.
"""

import argparse
import json
import os
import sys
from pathlib import Path


PRESETS = {
    "small-dev": {
        "ram_gb": 8,
        "cpus": 4,
        "media": "SSD",
        "shared_buffers": "1GB",
        "effective_cache_size": "4GB",
        "maintenance_work_mem": "256MB",
        "work_mem": "16MB",
        "wal_buffers": "16MB",
        "min_wal_size": "512MB",
        "max_wal_size": "2GB",
        "checkpoint_timeout": "15min",
    },
    "medium-vm": {
        "ram_gb": 32,
        "cpus": 8,
        "media": "NVMe",
        "shared_buffers": "8GB",
        "effective_cache_size": "24GB",
        "maintenance_work_mem": "1GB",
        "work_mem": "32MB",
        "wal_buffers": "64MB",
        "min_wal_size": "2GB",
        "max_wal_size": "8GB",
        "checkpoint_timeout": "15min",
    },
    "large-baremetal": {
        "ram_gb": 128,
        "cpus": 32,
        "media": "NVMe",
        "shared_buffers": "32GB",
        "effective_cache_size": "96GB",
        "maintenance_work_mem": "2GB",
        "work_mem": "64MB",
        "wal_buffers": "64MB",
        "min_wal_size": "4GB",
        "max_wal_size": "16GB",
        "checkpoint_timeout": "30min",
    },
}


def fmt_mb_or_gb(mb: int) -> str:
    if mb >= 1024 and mb % 1024 == 0:
        return f"{mb // 1024}GB"
    return f"{max(16, mb)}MB"


def derive_auto_sizing(hw_profile: dict, max_connections: int) -> dict:
    server_info = hw_profile.get("server_instance_info") or hw_profile
    cpu_info = server_info.get("cpu", {})
    mem_info = server_info.get("memory", {})
    disks_info = server_info.get("disks", {})

    logical_cpus = int(cpu_info.get("logical_cpus") or 4)
    ram_bytes = int(mem_info.get("ram_total_bytes") or (8 * 1024 ** 3))
    ram_mb = max(1024, ram_bytes // (1024 * 1024))
    ram_gb = round(ram_bytes / (1024 ** 3), 2)

    data_disk = disks_info.get("data_disk") or disks_info.get("system_disk") or {}
    media = data_disk.get("media_type", "SSD")
    data_avail_gb = float(data_disk.get("avail_gb") or 20.0)
    wal_disk = disks_info.get("wal_disk") or data_disk
    wal_avail_gb = float(wal_disk.get("avail_gb") or data_avail_gb)

    # Cap shared_buffers at 25% of RAM (up to 32GB for deterministic buffer pool behavior)
    sb_mb = min(32 * 1024, max(256, (ram_mb // 4 // 128) * 128))
    ecs_mb = min(128 * 1024, max(512, (ram_mb * 3 // 4 // 256) * 256))
    mwm_mb = min(2048, max(128, (ram_mb // 16 // 64) * 64))
    wm_mb = min(128, max(8, (ram_mb // max(32, max_connections * 2) // 4) * 4))

    # Size WAL safely within available disk space
    if wal_avail_gb >= 64 and ram_gb >= 32:
        min_wal = "2GB"
        max_wal = "8GB"
    elif wal_avail_gb >= 20:
        min_wal = "1GB"
        max_wal = "4GB"
    else:
        min_wal = "256MB"
        max_wal = "1GB"

    return {
        "ram_gb": ram_gb,
        "cpus": logical_cpus,
        "media": media,
        "shared_buffers": fmt_mb_or_gb(sb_mb),
        "effective_cache_size": fmt_mb_or_gb(ecs_mb),
        "maintenance_work_mem": fmt_mb_or_gb(mwm_mb),
        "work_mem": fmt_mb_or_gb(wm_mb),
        "wal_buffers": "64MB" if sb_mb >= 2048 else "16MB",
        "min_wal_size": min_wal,
        "max_wal_size": max_wal,
        "checkpoint_timeout": "15min",
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate deterministic PostgreSQL server configuration for a Perffarm animal."
    )
    parser.add_argument(
        "--template",
        default=str(Path(__file__).resolve().parent / "conf" / "postgresql.perffarm.conf.in"),
        help="Path to postgresql.perffarm.conf.in template",
    )
    parser.add_argument(
        "--hardware-profile",
        default="",
        help="Path to hardware_profile.json or server_instance_info.json",
    )
    parser.add_argument(
        "--animal-name",
        default=os.environ.get("ANIMAL_NAME", "perffarm-animal"),
        help="Animal name recorded in configuration header",
    )
    parser.add_argument(
        "--preset",
        choices=["auto", "small-dev", "medium-vm", "large-baremetal"],
        default=os.environ.get("PG_SIZING_PRESET", "auto"),
        help="Sizing preset tier (default: auto from hardware profile)",
    )
    parser.add_argument(
        "--pg-version",
        type=int,
        default=20,
        help="Target PostgreSQL major version (14..20, default: 20)",
    )
    parser.add_argument(
        "--max-connections",
        type=int,
        default=int(os.environ.get("PG_MAX_CONNECTIONS", "200")),
        help="max_connections setting (default: 200)",
    )
    parser.add_argument(
        "--listen-addresses",
        default="*",
        help="listen_addresses setting (default: *)",
    )
    parser.add_argument(
        "--enable-pg-stat-statements",
        action="store_true",
        default=True,
        help="Enable pg_stat_statements in shared_preload_libraries (default: true)",
    )
    parser.add_argument(
        "--no-pg-stat-statements",
        action="store_false",
        dest="enable_pg_stat_statements",
        help="Disable pg_stat_statements in shared_preload_libraries",
    )
    parser.add_argument(
        "--pg-config",
        default=os.environ.get("PREBUILT_PG_CONFIG", ""),
        help="Optional path to pg_config to check whether pg_stat_statements is installed",
    )
    parser.add_argument(
        "--configure-flags",
        default=os.environ.get("CONFIGURE_FLAGS", ""),
        help="Optional configure flags string (checked for --with-lz4)",
    )
    parser.add_argument(
        "--extra-gucs",
        default=os.environ.get("PG_EXTRA_GUCS", ""),
        help="Semicolon- or comma-separated extra GUC key=value overrides",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output path for generated postgresql.animal.conf",
    )
    args = parser.parse_args()

    hw_profile = {}
    if args.hardware_profile and os.path.exists(args.hardware_profile):
        with open(args.hardware_profile, "r", encoding="utf-8") as f:
            hw_profile = json.load(f)

    server_info = hw_profile.get("server_instance_info") or hw_profile
    hostname = server_info.get("hostname", "localhost")
    mem_info = server_info.get("memory", {})
    hugepages_total = int(mem_info.get("hugepages_total") or 0)

    if args.preset == "auto":
        sizing = derive_auto_sizing(hw_profile, args.max_connections)
    else:
        sizing = dict(PRESETS[args.preset])

    cpus = int(sizing["cpus"])
    media = sizing["media"]
    is_ssd = media.upper() in ("NVME", "SSD", "TMPFS/RAMDISK", "NETWORK/VIRTUAL")

    max_workers = max(8, min(64, cpus))
    max_parallel = max(4, min(32, cpus))
    max_parallel_gather = max(2, min(8, cpus // 2))
    max_parallel_maint = max(2, min(8, cpus // 2))
    autovac_workers = max(3, min(8, cpus // 4))

    random_page_cost = "1.1" if is_ssd else "4.0"
    eff_io_conc = "200" if is_ssd else "2"
    maint_io_conc_line = f"maintenance_io_concurrency = {eff_io_conc}" if args.pg_version >= 13 else ""
    track_wal_line = "track_wal_io_timing = on" if args.pg_version >= 15 else "# track_wal_io_timing not supported on PG < 15"
    huge_pages = "try" if hugepages_total > 0 else "try"

    cfg_flags_combined = args.configure_flags or ""
    if args.hardware_profile:
        eff_cfg_file = Path(args.hardware_profile).resolve().parent / "effective_configure_flags.txt"
        if eff_cfg_file.exists():
            try:
                cfg_flags_combined += " " + eff_cfg_file.read_text(encoding="utf-8")
            except Exception:
                pass

    use_pgss = args.enable_pg_stat_statements
    if args.pg_config:
        import shutil
        import subprocess
        pg_cfg_bin = shutil.which(args.pg_config)
        if pg_cfg_bin:
            try:
                cfg_flags_combined += " " + subprocess.check_output([pg_cfg_bin, "--configure"], text=True).strip()
            except Exception:
                pass
            if use_pgss:
                try:
                    pkglibdir = subprocess.check_output([pg_cfg_bin, "--pkglibdir"], text=True).strip()
                    has_lib = any(
                        os.path.exists(os.path.join(pkglibdir, name))
                        for name in ("pg_stat_statements.so", "pg_stat_statements.dylib", "pg_stat_statements.dll")
                    )
                    if not has_lib:
                        use_pgss = False
                except Exception:
                    pass

    wal_compression = "lz4" if (args.pg_version >= 15 and "--with-lz4" in cfg_flags_combined) else "on"

    if use_pgss:
        spl_line = "shared_preload_libraries = 'pg_stat_statements'"
        pgss_lines = (
            "pg_stat_statements.max = 10000\n"
            "pg_stat_statements.track = all\n"
            "pg_stat_statements.track_utility = on"
        )
    else:
        spl_line = "shared_preload_libraries = ''"
        pgss_lines = "# pg_stat_statements not installed in pkglibdir or disabled"

    extra_lines = []
    if args.extra_gucs:
        for item in args.extra_gucs.replace(";", ",").split(","):
            item = item.strip()
            if item and "=" in item:
                k, v = item.split("=", 1)
                extra_lines.append(f"{k.strip()} = {v.strip()}")
    extra_guc_block = "\n".join(extra_lines) if extra_lines else "# (none)"

    with open(args.template, "r", encoding="utf-8") as f:
        content = f.read()

    replacements = {
        "@ANIMAL_NAME@": args.animal_name,
        "@SIZING_PRESET@": args.preset,
        "@PG_VERSION@": str(args.pg_version),
        "@SERVER_HOSTNAME@": str(hostname),
        "@CPU_LOGICAL@": str(cpus),
        "@RAM_GB@": str(sizing["ram_gb"]),
        "@DATA_DISK_MEDIA@": str(media),
        "@LISTEN_ADDRESSES@": args.listen_addresses,
        "@MAX_CONNECTIONS@": str(args.max_connections),
        "@SHARED_BUFFERS@": sizing["shared_buffers"],
        "@EFFECTIVE_CACHE_SIZE@": sizing["effective_cache_size"],
        "@MAINTENANCE_WORK_MEM@": sizing["maintenance_work_mem"],
        "@WORK_MEM@": sizing["work_mem"],
        "@HUGE_PAGES@": huge_pages,
        "@WAL_BUFFERS@": sizing["wal_buffers"],
        "@MIN_WAL_SIZE@": sizing["min_wal_size"],
        "@MAX_WAL_SIZE@": sizing["max_wal_size"],
        "@CHECKPOINT_TIMEOUT@": sizing["checkpoint_timeout"],
        "@WAL_COMPRESSION@": wal_compression,
        "@MAX_WORKER_PROCESSES@": str(max_workers),
        "@MAX_PARALLEL_WORKERS@": str(max_parallel),
        "@MAX_PARALLEL_WORKERS_PER_GATHER@": str(max_parallel_gather),
        "@MAX_PARALLEL_MAINTENANCE_WORKERS@": str(max_parallel_maint),
        "@RANDOM_PAGE_COST@": random_page_cost,
        "@EFFECTIVE_IO_CONCURRENCY@": eff_io_conc,
        "@MAINTENANCE_IO_CONCURRENCY_LINE@": maint_io_conc_line,
        "@JIT_SETTING@": "on",
        "@AUTOVACUUM_MAX_WORKERS@": str(autovac_workers),
        "@TRACK_WAL_IO_TIMING_LINE@": track_wal_line,
        "@SHARED_PRELOAD_LIBRARIES_LINE@": spl_line,
        "@PG_STAT_STATEMENTS_LINES@": pgss_lines,
        "@EXTRA_GUC_LINES@": extra_guc_block,
    }

    for token, val in replacements.items():
        content = content.replace(token, val)

    # Filter out any built-in GUC not supported by the exact target postgres binary
    supported_gucs: set[str] = set()
    if args.pg_config:
        import shutil
        import subprocess
        pg_cfg_bin = shutil.which(args.pg_config)
        if pg_cfg_bin:
            try:
                bindir = subprocess.check_output([pg_cfg_bin, "--bindir"], text=True).strip()
                pg_bin = os.path.join(bindir, "postgres")
                if os.path.exists(pg_bin):
                    desc_out = subprocess.check_output(
                        [pg_bin, "--describe-config"],
                        stderr=subprocess.DEVNULL,
                        text=True,
                    )
                    for dline in desc_out.splitlines():
                        gname = dline.split("\t", 1)[0].strip()
                        if gname:
                            supported_gucs.add(gname)
            except Exception:
                pass

    if supported_gucs:
        filtered_lines = []
        for raw_line in content.splitlines():
            sline = raw_line.strip()
            if sline and not sline.startswith("#") and "=" in sline:
                gkey = sline.split("=", 1)[0].strip()
                if "." not in gkey and gkey not in supported_gucs:
                    filtered_lines.append(f"# {raw_line}  # unsupported on this server revision")
                    continue
            filtered_lines.append(raw_line)
        content = "\n".join(filtered_lines) + "\n"

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(content)

    return 0


if __name__ == "__main__":
    sys.exit(main())
