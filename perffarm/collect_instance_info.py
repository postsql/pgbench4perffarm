#!/usr/bin/env python3
"""
collect_instance_info.py — Hardware, CPU, RAM, and per-disk (system /, data, WAL)
inventory collector for pgbench4perffarm instances (local or remote over SSH).

Can be run directly on a local machine or remotely against an external/Cloud VM
via --ssh-target="user@host" (streams this script over SSH to python3 on the target).
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import time


def run_cmd(cmd: list[str], timeout: int = 10) -> str:
    try:
        res = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
        return res.stdout.strip()
    except Exception:
        return ""


def read_file_first_line(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.readline().strip()
    except Exception:
        return ""


def collect_cpu_info() -> dict:
    logical_cpus = os.cpu_count() or 1
    model_name = "Unknown CPU"
    arch = platform.machine() or "unknown"
    sockets = 1
    cores_per_socket = logical_cpus
    threads_per_core = 1
    numa_nodes = 1
    l3_cache = ""
    freq_cur = None
    freq_max = None

    lscpu_out = run_cmd(["lscpu"])
    if lscpu_out:
        kv = {}
        for line in lscpu_out.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                kv[k.strip()] = v.strip()
        model_name = kv.get("Model name", model_name)
        arch = kv.get("Architecture", arch)
        l3_cache = kv.get("L3 cache", "")
        try:
            sockets = max(1, int(kv.get("Socket(s)", "1").split()[0]))
        except Exception:
            sockets = 1
        try:
            cores_per_socket = max(1, int(kv.get("Core(s) per socket", str(logical_cpus)).split()[0]))
        except Exception:
            cores_per_socket = max(1, logical_cpus // sockets)
        try:
            threads_per_core = max(1, int(kv.get("Thread(s) per core", "1").split()[0]))
        except Exception:
            threads_per_core = 1
        try:
            numa_nodes = max(1, int(kv.get("NUMA node(s)", "1").split()[0]))
        except Exception:
            numa_nodes = 1
        for fk in ("CPU MHz", "CPU max MHz"):
            if fk in kv:
                try:
                    val = round(float(kv[fk].split()[0]), 2)
                    if fk == "CPU MHz":
                        freq_cur = val
                    else:
                        freq_max = val
                except Exception:
                    pass
    else:
        try:
            with open("/proc/cpuinfo", "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.lower().startswith("model name") and ":" in line:
                        model_name = line.split(":", 1)[1].strip()
                        break
        except Exception:
            pass

    physical_cores = sockets * cores_per_socket
    if physical_cores > logical_cpus:
        physical_cores = logical_cpus

    governor = read_file_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") or "unknown"

    return {
        "model_name": model_name,
        "architecture": arch,
        "cpu_count": logical_cpus,
        "logical_cpus": logical_cpus,
        "physical_cores": physical_cores,
        "sockets": sockets,
        "threads_per_core": threads_per_core,
        "numa_nodes": numa_nodes,
        "freq_mhz_current": freq_cur,
        "freq_mhz_max": freq_max,
        "scaling_governor": governor,
        "l3_cache": l3_cache,
    }


def collect_memory_info() -> dict:
    meminfo = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if ":" in line:
                    k, v = line.split(":", 1)
                    parts = v.strip().split()
                    if parts and parts[0].isdigit():
                        meminfo[k.strip()] = int(parts[0])
    except Exception:
        pass

    ram_total_kb = meminfo.get("MemTotal", 0)
    ram_avail_kb = meminfo.get("MemAvailable", meminfo.get("MemFree", 0))
    swap_total_kb = meminfo.get("SwapTotal", 0)
    swap_free_kb = meminfo.get("SwapFree", 0)
    hp_total = meminfo.get("HugePages_Total", 0)
    hp_free = meminfo.get("HugePages_Free", 0)
    hp_size_kb = meminfo.get("Hugepagesize", 2048)

    thp_raw = read_file_first_line("/sys/kernel/mm/transparent_hugepage/enabled")
    thp_mode = "unknown"
    if thp_raw:
        m = re.search(r"\[([^\]]+)\]", thp_raw)
        thp_mode = m.group(1) if m else thp_raw

    return {
        "ram_total_bytes": ram_total_kb * 1024,
        "ram_total_mb": round(ram_total_kb / 1024.0, 1),
        "ram_total_gb": round(ram_total_kb / (1024.0 * 1024.0), 2),
        "ram_available_bytes": ram_avail_kb * 1024,
        "ram_available_gb": round(ram_avail_kb / (1024.0 * 1024.0), 2),
        "swap_total_bytes": swap_total_kb * 1024,
        "swap_free_bytes": swap_free_kb * 1024,
        "hugepages_total": hp_total,
        "hugepages_free": hp_free,
        "hugepage_size_kb": hp_size_kb,
        "transparent_hugepage": thp_mode,
    }


def find_existing_parent(path_str: str) -> Path:
    p = Path(path_str).expanduser()
    while not p.exists() and p.parent != p:
        p = p.parent
    try:
        return p.resolve()
    except Exception:
        return p


def parse_mounts() -> list[dict]:
    mounts = []
    try:
        with open("/proc/mounts", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 4:
                    dev, mnt, fstype, opts = parts[0], parts[1], parts[2], parts[3]
                    mounts.append(
                        {
                            "device": dev,
                            "mount_point": mnt.replace("\\040", " "),
                            "filesystem_type": fstype,
                            "mount_options": opts,
                        }
                    )
    except Exception:
        pass
    # Sort longest mount_point first for prefix matching
    mounts.sort(key=lambda m: len(m["mount_point"]), reverse=True)
    return mounts


def resolve_parent_block_name(dev_path: str, existing_path_str: str = "") -> tuple[str, str]:
    resolved_dev = dev_path
    # If /proc/mounts reports /dev/root (common on Ubuntu cloud kernels), resolve via st_dev in /sys/dev/block
    if existing_path_str and (not dev_path.startswith("/dev/") or dev_path == "/dev/root" or not os.path.exists(dev_path)):
        try:
            st_dev = os.stat(existing_path_str).st_dev
            sys_dev_block = Path(f"/sys/dev/block/{os.major(st_dev)}:{os.minor(st_dev)}")
            if sys_dev_block.exists():
                real_sys = sys_dev_block.resolve()
                part_name = real_sys.name
                if os.path.exists(f"/dev/{part_name}"):
                    resolved_dev = f"/dev/{part_name}"
                if os.path.exists(f"/sys/block/{part_name}"):
                    return resolved_dev, part_name
                if os.path.exists(f"/sys/block/{real_sys.parent.name}"):
                    return resolved_dev, real_sys.parent.name
        except Exception:
            pass

    if not resolved_dev.startswith("/dev/"):
        return resolved_dev, ""
    try:
        real_dev = os.path.realpath(resolved_dev)
    except Exception:
        real_dev = resolved_dev
    name = os.path.basename(real_dev)
    # Direct match in /sys/block
    if os.path.exists(f"/sys/block/{name}"):
        return resolved_dev, name
    # Partition match in /sys/class/block/<part>
    sys_part = Path(f"/sys/class/block/{name}")
    if sys_part.exists():
        try:
            resolved_sys = sys_part.resolve()
            parent = resolved_sys.parent.name
            if os.path.exists(f"/sys/block/{parent}"):
                return resolved_dev, parent
        except Exception:
            pass
    # Regex fallback for nvme0n1p1 -> nvme0n1 or sda1 -> sda
    m_nvme = re.match(r"^(nvme\d+n\d+)p\d+$", name)
    if m_nvme:
        return resolved_dev, m_nvme.group(1)
    m_sd = re.match(r"^([a-z]+)\d+$", name)
    if m_sd:
        return resolved_dev, m_sd.group(1)
    return resolved_dev, name


def inspect_disk_for_path(role: str, target_path: str, mounts: list[dict], run_io_probe: bool = False) -> dict:
    existing_path = find_existing_parent(target_path)
    existing_str = str(existing_path)

    matched_mount = {
        "device": "unknown",
        "mount_point": "/",
        "filesystem_type": "unknown",
        "mount_options": "",
    }
    for m in mounts:
        mp = m["mount_point"]
        if existing_str == mp or existing_str.startswith(mp.rstrip("/") + "/") or mp == "/":
            matched_mount = m
            break

    total_bytes = 0
    avail_bytes = 0
    used_pct = 0.0
    try:
        st = os.statvfs(existing_str)
        total_bytes = st.f_frsize * st.f_blocks
        avail_bytes = st.f_frsize * st.f_bavail
        if total_bytes > 0:
            used_pct = round(100.0 * (total_bytes - avail_bytes) / total_bytes, 2)
    except Exception:
        pass

    raw_dev = matched_mount["device"]
    fstype = matched_mount["filesystem_type"]
    dev, parent_blk = resolve_parent_block_name(raw_dev, existing_str)

    rotational = None
    scheduler = ""
    model = ""
    if parent_blk and os.path.exists(f"/sys/block/{parent_blk}"):
        rot_str = read_file_first_line(f"/sys/block/{parent_blk}/queue/rotational")
        if rot_str in ("0", "1"):
            rotational = rot_str == "1"
        sched_raw = read_file_first_line(f"/sys/block/{parent_blk}/queue/scheduler")
        if sched_raw:
            m = re.search(r"\[([^\]]+)\]", sched_raw)
            scheduler = m.group(1) if m else sched_raw
        model = read_file_first_line(f"/sys/block/{parent_blk}/device/model")

    if fstype in ("tmpfs", "ramfs"):
        media_type = "tmpfs/ramdisk"
    elif parent_blk.startswith("nvme"):
        media_type = "NVMe"
    elif rotational is False:
        media_type = "SSD"
    elif rotational is True:
        media_type = "HDD"
    else:
        media_type = "network/virtual"

    info = {
        "role": role,
        "target_path": target_path,
        "resolved_path": existing_str,
        "mount_point": matched_mount["mount_point"],
        "filesystem_type": fstype,
        "mount_options": matched_mount["mount_options"],
        "block_device": dev,
        "parent_disk": parent_blk or dev,
        "total_bytes": total_bytes,
        "total_gb": round(total_bytes / (1024.0 ** 3), 2),
        "avail_bytes": avail_bytes,
        "avail_gb": round(avail_bytes / (1024.0 ** 3), 2),
        "used_pct": used_pct,
        "rotational": rotational,
        "media_type": media_type,
        "device_model": model,
        "io_scheduler": scheduler,
    }

    if run_io_probe:
        probe_dir = existing_path if os.access(existing_str, os.W_OK) else Path("/tmp")
        probe_file = probe_dir / f".perffarm_io_probe_{os.getpid()}_{role}.tmp"
        try:
            # 16 MB sequential write + fsync micro-probe
            chunk = b"0123456789abcdef" * 65536  # 1 MiB
            blocks = 16
            t0 = time.perf_counter()
            with open(probe_file, "wb") as fp:
                for _ in range(blocks):
                    fp.write(chunk)
                fp.flush()
                os.fsync(fp.fileno())
            dt = max(1e-6, time.perf_counter() - t0)
            info["seq_write_mbps_probe"] = round((blocks * 1.048576) / dt, 2)
        except Exception:
            info["seq_write_mbps_probe"] = None
        finally:
            try:
                if probe_file.exists():
                    probe_file.unlink()
            except Exception:
                pass

    return info


def collect_os_and_toolchain() -> tuple[dict, dict]:
    distro_name = platform.system()
    distro_version = platform.release()
    try:
        with open("/etc/os-release", "r", encoding="utf-8", errors="replace") as f:
            os_rel = {}
            for line in f:
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    os_rel[k] = v.strip('"')
            distro_name = os_rel.get("NAME", distro_name)
            distro_version = os_rel.get("VERSION_ID", os_rel.get("VERSION", distro_version))
    except Exception:
        pass

    sysctl_keys = [
        "vm.swappiness",
        "vm.dirty_background_ratio",
        "vm.dirty_ratio",
        "vm.overcommit_memory",
        "kernel.sched_autogroup_enabled",
    ]
    sysctls = {}
    for sk in sysctl_keys:
        proc_path = "/proc/sys/" + sk.replace(".", "/")
        val = read_file_first_line(proc_path)
        if val != "":
            sysctls[sk] = val

    virt = run_cmd(["systemd-detect-virt"]) or "none"

    os_info = {
        "kernel_release": platform.release(),
        "kernel_version": platform.version(),
        "distro_name": distro_name,
        "distro_version": distro_version,
        "libc_version": " ".join(platform.libc_ver()).strip(),
        "virtualization": virt,
        "is_virtual_machine": virt not in ("none", ""),
        "sysctl": sysctls,
    }

    cc_bin = os.environ.get("CC", "gcc")
    cc_ver = (run_cmd([cc_bin, "--version"]) or "").splitlines()
    make_ver = (run_cmd(["make", "--version"]) or "").splitlines()
    git_ver = (run_cmd(["git", "--version"]) or "").splitlines()

    toolchain = {
        "cc": cc_bin,
        "cc_version": cc_ver[0] if cc_ver else "unknown",
        "make_version": make_ver[0] if make_ver else "unknown",
        "python_version": sys.version.split()[0],
        "git_version": git_ver[0] if git_ver else "unknown",
    }

    return os_info, toolchain


def collect_local_instance(role: str, system_dir: str, data_dir: str, wal_dir: str, io_probe: bool) -> dict:
    mounts = parse_mounts()
    sys_disk = inspect_disk_for_path("system", system_dir, mounts, run_io_probe=io_probe)
    data_disk = inspect_disk_for_path("data", data_dir, mounts, run_io_probe=io_probe)
    wal_disk = inspect_disk_for_path("wal", wal_dir, mounts, run_io_probe=io_probe)

    dedicated_data = (
        data_disk["mount_point"] != sys_disk["mount_point"]
        or data_disk["parent_disk"] != sys_disk["parent_disk"]
    )
    dedicated_wal = (
        wal_disk["mount_point"] != data_disk["mount_point"]
        or wal_disk["parent_disk"] != data_disk["parent_disk"]
    )
    wal_disk["is_dedicated_wal_device"] = dedicated_wal
    data_disk["is_dedicated_data_device"] = dedicated_data

    cpu_info = collect_cpu_info()
    mem_info = collect_memory_info()
    os_info, toolchain = collect_os_and_toolchain()

    return {
        "role": role,
        "hostname": platform.node(),
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "cpu": cpu_info,
        "memory": mem_info,
        "disks": {
            "system_disk": sys_disk,
            "data_disk": data_disk,
            "wal_disk": wal_disk,
            "dedicated_data_disk": dedicated_data,
            "dedicated_wal_disk": dedicated_wal,
        },
        "os": os_info,
        "toolchain": toolchain,
    }


def collect_remote_instance(
    ssh_target: str,
    ssh_opts: str,
    role: str,
    system_dir: str,
    data_dir: str,
    wal_dir: str,
    io_probe: bool,
) -> dict:
    script_path = Path(__file__).resolve()
    with open(script_path, "r", encoding="utf-8") as f:
        script_source = f.read()

    remote_args = [
        "python3",
        "-",
        "--role",
        role,
        "--system-dir",
        system_dir,
        "--data-dir",
        data_dir,
        "--wal-dir",
        wal_dir,
    ]
    if io_probe:
        remote_args.append("--io-probe")

    ssh_cmd = ["ssh"]
    if ssh_opts:
        ssh_cmd.extend(shlex.split(ssh_opts))
    ssh_cmd.append(ssh_target)
    ssh_cmd.append(" ".join(shlex.quote(a) for a in remote_args))

    res = subprocess.run(
        ssh_cmd,
        input=script_source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=45,
        check=False,
    )
    if res.returncode != 0:
        raise RuntimeError(
            f"Remote instance collection on {ssh_target} failed (exit {res.returncode}): {res.stderr.strip()}"
        )
    data = json.loads(res.stdout)
    data["ssh_target"] = ssh_target
    return data


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Collect CPU, RAM, and separate system/data/WAL disk telemetry for a Perffarm instance."
    )
    parser.add_argument(
        "--role",
        choices=["server", "client", "combined", "controller"],
        default="combined",
        help="Instance role (server, client, combined, controller)",
    )
    parser.add_argument(
        "--system-dir",
        default="/",
        help="System disk mount path (default: /)",
    )
    parser.add_argument(
        "--data-dir",
        default=os.environ.get("PGDATA_DIR", "/var/tmp/pgbench4perffarm_work/pgdata"),
        help="PostgreSQL data directory path to inspect for data_disk",
    )
    parser.add_argument(
        "--wal-dir",
        default=os.environ.get("PGWAL_DIR", "/var/tmp/pgbench4perffarm_work/pgwal"),
        help="PostgreSQL WAL directory path to inspect for wal_disk",
    )
    parser.add_argument(
        "--ssh-target",
        default="",
        help="Optional user@host SSH target when inspecting a remote/Cloud VM instance",
    )
    parser.add_argument(
        "--ssh-opts",
        default="-o BatchMode=yes -o StrictHostKeyChecking=accept-new",
        help="SSH options when --ssh-target is used",
    )
    parser.add_argument(
        "--io-probe",
        action="store_true",
        help="Run a brief 16 MB sequential write + fsync probe on each disk",
    )
    parser.add_argument(
        "--output",
        default="",
        help="Optional output file path for the JSON payload",
    )
    args = parser.parse_args()

    if args.ssh_target:
        payload = collect_remote_instance(
            ssh_target=args.ssh_target,
            ssh_opts=args.ssh_opts,
            role=args.role,
            system_dir=args.system_dir,
            data_dir=args.data_dir,
            wal_dir=args.wal_dir,
            io_probe=args.io_probe,
        )
    else:
        payload = collect_local_instance(
            role=args.role,
            system_dir=args.system_dir,
            data_dir=args.data_dir,
            wal_dir=args.wal_dir,
            io_probe=args.io_probe,
        )

    formatted = json.dumps(payload, indent=2)
    if args.output:
        out_path = Path(args.output)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(formatted + "\n")
    else:
        print(formatted)

    return 0


if __name__ == "__main__":
    sys.exit(main())
