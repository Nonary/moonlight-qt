#!/usr/bin/env python3
"""Run a command with Gamescope composition forced, then restore its prior value."""

import argparse
import datetime
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="inspect support without changing anything")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="command to run (default: ~/.local/bin/moonlight-dev)")
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        command = [str(Path.home() / ".local/bin/moonlight-dev")]

    # Keep this host-side: the Distrobox client does not own the compositor.
    if not os.environ.get("GAMESCOPE_WAYLAND_DISPLAY"):
        print("Run this test from the existing Gaming Mode session; no Gamescope socket was supplied.",
              file=sys.stderr)
        return 2

    log_dir = Path(os.environ.get("MOONLIGHT_DEV_LOG_DIR", Path.home() / "moonlight-logs"))
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = log_dir / f"composition-test-{stamp}-{os.getpid()}.log"
    with log_path.open("x") as log:
        def report(message):
            print(message, file=sys.stderr, flush=True)
            print(message, file=log, flush=True)

        def control(*words):
            result = subprocess.run(["gamescopectl", *words], capture_output=True,
                                    text=True, timeout=5, check=False)
            output = result.stdout + result.stderr
            report(f"gamescopectl {' '.join(words)} [exit {result.returncode}]:\n{output.strip()}")
            if result.returncode != 0 or "Command not found" in output:
                raise RuntimeError("Gamescope control failed; the test condition is unverified")
            return output

        def read_value():
            output = control("composite_force")
            value = re.search(r"\bcomposite_force:\s*(true|false|0|1)\b", output, re.IGNORECASE)
            if not value:
                raise RuntimeError("Gamescope did not report a readable composite_force value")
            return value[1].lower() in ("true", "1")

        changed = False
        result = 2
        child = None

        def interrupted(signum, _frame):
            if child is not None and child.poll() is None:
                child.send_signal(signum)
            raise KeyboardInterrupt

        signal.signal(signal.SIGTERM, interrupted)
        signal.signal(signal.SIGINT, interrupted)
        try:
            report(f"Composition test log: {log_path}")
            control("version")
            if "composite_force" not in control("help"):
                raise RuntimeError("This Gamescope build does not expose composite_force")
            original = read_value()
            if args.check:
                return 0
            if original:
                report("Composition was already forced; this run does not introduce a new condition.")
            else:
                # Restore even if setting succeeds but readback fails.
                changed = True
                control("composite_force", "1")
            if not read_value():
                raise RuntimeError("Forced composition did not remain enabled")
            report("Composition forced for this run. Compare the same scene with Steam's overlay off.")
            child = subprocess.Popen(command)
            next_check = time.monotonic() + 3
            condition_valid = True
            while child.poll() is None:
                try:
                    child.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    pass
                if child.poll() is None and time.monotonic() >= next_check:
                    if not read_value():
                        report("TEST CONDITION LOST: another component disabled forced composition.")
                        condition_valid = False
                    next_check = time.monotonic() + 10
            result = child.returncode if child.returncode >= 0 else 128 - child.returncode
            if not condition_valid and result == 0:
                result = 2
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            report(str(error))
            if child is not None and child.poll() is None:
                report("The stream is still running; this comparison is unverified.")
        except KeyboardInterrupt:
            result = 130
        finally:
            if changed:
                try:
                    control("composite_force", "0")
                    if read_value():
                        raise RuntimeError("Composition is still forced")
                    report("Restored composite_force to its original value: 0.")
                except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                    report(f"RESTORE FAILED: {error}. Run gamescopectl composite_force 0 in Gaming Mode.")
                    result = 2
        return result


if __name__ == "__main__":
    sys.exit(main())
