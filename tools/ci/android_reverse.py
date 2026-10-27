"""Allocate uniquely named reverse rules without replacing the user's existing mappings."""
import random
import subprocess


def allocate_reverse(adb, host_port):
    # ADB 33.0.3 keys its host allow-list by the requested remote name. Several
    # tcp:0 allocations collide there even though adbd allocates different ports.
    for _ in range(32):
        device_port = random.SystemRandom().randrange(20000, 60000)
        rule = f"tcp:{device_port}"
        result = subprocess.run(adb + ["reverse", "--no-rebind", rule, f"tcp:{host_port}"],
            capture_output=True, text=True, timeout=20)
        if result.returncode == 0:
            return device_port, rule
        error = (result.stdout + result.stderr).lower()
        if not any(value in error for value in ("cannot rebind", "address already in use", "cannot bind")):
            raise RuntimeError("ADB reverse allocation failed: " + result.stdout + result.stderr)
    raise RuntimeError("Cannot allocate an owned reverse rule")
