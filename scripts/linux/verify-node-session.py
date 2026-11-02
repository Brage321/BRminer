#!/usr/bin/env python3
"""Run authorized node/pool mining until acceptance and a minimum observation time."""
import argparse
import json
import os
import queue
import signal
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--node", required=True)
parser.add_argument("--wallet", required=True)
parser.add_argument("--threads", type=int)
parser.add_argument("--profile-dir")
parser.add_argument("--backend", default="auto")
parser.add_argument("--acceptance", choices=("block", "share"), default="block")
parser.add_argument("--minimum-accepted", type=int, default=1)
parser.add_argument("--duration", type=float, default=7200)
parser.add_argument("--minimum-seconds", type=float, default=300)
args = parser.parse_args()
command = [args.binary, "-a", "warthog", "-p", args.node, "-w", args.wallet, "--backend", args.backend]
if args.threads is not None:
    command += ["--cpu_threads", str(args.threads)]
if args.profile_dir:
    command += ["--profile-dir", args.profile_dir]
platform_options = {}
kernel = None
if os.name == "nt":
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.FreeConsole()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    platform_options = dict(creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
miner = subprocess.Popen(command, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1, **platform_options)
lines = queue.Queue()

def read_output():
    for line in miner.stdout:
        lines.put(line)
    lines.put(None)

thread = threading.Thread(target=read_output, daemon=True)
thread.start()
interrupted = False

def stop_requested(signum, frame):
    global interrupted
    interrupted = True

signal.signal(signal.SIGINT, stop_requested)
signal.signal(signal.SIGTERM, stop_requested)
start = time.monotonic()
accepted = False
accepted_shares = 0
stopped = False
stop_at = None
while True:
    elapsed = time.monotonic() - start
    meets_acceptance = accepted if args.acceptance == "block" else accepted_shares >= args.minimum_accepted
    if stop_at is None and (interrupted or elapsed >= args.duration or
                           (meets_acceptance and elapsed >= args.minimum_seconds)):
        if miner.poll() is None:
            if kernel:
                try:
                    if not kernel.AttachConsole(miner.pid):
                        raise RuntimeError("Cannot attach hidden miner console for Ctrl+C")
                    kernel.SetConsoleCtrlHandler(None, True)
                    if not kernel.GenerateConsoleCtrlEvent(0, 0):
                        raise RuntimeError("Cannot send Ctrl+C to the miner")
                except BaseException:
                    miner.kill()
                    miner.wait(timeout=5)
                    kernel.FreeConsole()
                    raise
            else:
                miner.send_signal(signal.SIGINT)
        stop_at = time.monotonic()
    if stop_at is not None and time.monotonic() - stop_at > 15 and miner.poll() is None:
        miner.kill()
    try:
        line = lines.get(timeout=1)
    except queue.Empty:
        continue
    if line is None:
        break
    print(line, end="", flush=True)
    # This string is emitted only on a positive server response for a block candidate.
    accepted |= "BLOCK ACCEPTED" in line
    if "BLOCK ACCEPTED" in line or "Share accepted by the Stratum server." in line:
        accepted_shares += 1
    stopped |= "Mining stopped." in line
miner.wait(timeout=20)
if kernel:
    kernel.FreeConsole()
elapsed = time.monotonic() - start
meets_acceptance = accepted if args.acceptance == "block" else accepted_shares >= args.minimum_accepted
summary = {"schema": "brminer.node-session.v1", "elapsed_seconds": elapsed,
           "accepted_block": accepted, "graceful_shutdown": stopped and miner.returncode == 0,
           "accepted_submissions": accepted_shares, "acceptance_target": args.acceptance,
           "miner_exit_code": miner.returncode, "interrupted": interrupted}
print("BRMINER_SESSION_RESULT " + json.dumps(summary), flush=True)
raise SystemExit(0 if meets_acceptance and elapsed >= args.minimum_seconds and stopped and miner.returncode == 0 else 2)
