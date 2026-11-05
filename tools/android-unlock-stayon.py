#!/usr/bin/env python3
"""
android-unlock-stayon.py — One-shot screen unlock and display stay-on configuration for Android.

Automates:
1. Wake display (keyevent 224)
2. Swipe up to reveal PIN/password entry
3. Input PIN from ANDROID_UNLOCK_PIN environment variable or --pin argument and submit (keyevent 66)
4. Configure permanent stay-on while plugged in
5. Verify wakefulness state
"""

import argparse
import os
import sys
import time
import subprocess

def run_adb(cmd, serial=None):
    base = ["adb"]
    if serial:
        base.extend(["-s", serial])
    full = base + cmd
    res = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout.strip(), res.stderr.strip()

def main():
    parser = argparse.ArgumentParser(description="Android one-shot screen unlock and stay-on configuration")
    parser.add_argument("--serial", "-s", default=os.environ.get("ANDROID_SERIAL"), help="Android device serial")
    parser.add_argument("--pin", "-p", default=os.environ.get("ANDROID_UNLOCK_PIN"), help="Screen unlock PIN/password (default: $ANDROID_UNLOCK_PIN)")
    args = parser.parse_args()

    serial = args.serial
    pin = args.pin

    if not pin:
        print("Error: Unlock PIN must be provided via ANDROID_UNLOCK_PIN environment variable or --pin argument", file=sys.stderr)
        return 1

    # Verify device presence
    rc, out, _ = run_adb(["get-state"], serial)
    if rc != 0 or out != "device":
        print(f"Error: Android device not ready (state={out})", file=sys.stderr)
        return 1

    print(f"Targeting device: {serial or 'default'}")

    # Step 1: Wake display
    run_adb(["shell", "input", "keyevent", "224"], serial)
    time.sleep(0.5)

    # Step 2: Swipe up to reveal password field
    run_adb(["shell", "input", "swipe", "500", "2000", "500", "500", "300"], serial)
    time.sleep(0.8)

    # Step 3: Input password and submit
    run_adb(["shell", "input", "text", pin], serial)
    time.sleep(0.3)
    run_adb(["shell", "input", "keyevent", "66"], serial)
    time.sleep(1.0)

    # Step 4: Configure stay on while plugged in
    run_adb(["shell", "svc", "power", "stayon", "true"], serial)
    run_adb(["shell", "settings", "put", "global", "stay_on_while_plugged_in", "3"], serial)

    # Step 5: Verify wakefulness
    rc, out, _ = run_adb(["shell", "dumpsys", "power"], serial)
    is_awake = False
    for line in out.splitlines():
        if "mWakefulness=Awake" in line:
            is_awake = True
            break

    if is_awake:
        print("Success: Device screen unlocked and configured for stay-on while plugged in.")
        return 0
    else:
        print("Warning: Device screen state could not be verified as Awake.")
        return 1

if __name__ == "__main__":
    sys.exit(main())
