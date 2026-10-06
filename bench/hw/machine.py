"""Machine description recorded with every result (shared by bench/hw and bench/e2e)."""
import os
import subprocess

POWER_OVERLAYS = {"ded574b5-45a0-4f42-8737-46345c09c238": "best_performance",
                  "961cc777-2547-4f9d-8174-7d86181b8a7a": "best_power_efficiency",
                  "00000000-0000-0000-0000-000000000000": "balanced"}


def windows_power():
    """Return AC/battery state and power mode as seen by Windows (best effort)."""
    ps = ("Add-Type -AssemblyName System.Windows.Forms;"
          "[System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus;"
          "(Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes')"
          ".ActiveOverlayAcPowerScheme")
    try:
        out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], cwd="/mnt/c", timeout=60,
                             check=True, text=True, capture_output=True).stdout.split()
        return {"power_line": out[0], "power_mode": POWER_OVERLAYS.get(out[1] if len(out) > 1 else "", "unknown")}
    except Exception as e:  # noqa: BLE001 - recorded, not fatal
        return {"power_line": "unknown", "power_mode": "unknown", "error": str(e)}


def machine_info(extra_versions=()):
    cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
    mem_kib = int(next(l.split()[1] for l in open("/proc/meminfo") if l.startswith("MemTotal")))
    info = {
        "cpu": cpu,
        "nproc": os.cpu_count(),
        "mem_total_gib": round(mem_kib / (1 << 20), 2),
        "kernel": os.uname().release,
        **windows_power(),
    }
    for name, cmd in extra_versions:
        try:
            info[name] = subprocess.run(cmd, check=True, text=True, capture_output=True).stdout.splitlines()[0].strip()
        except Exception as e:  # noqa: BLE001
            info[name] = f"unavailable: {e}"
    return info
