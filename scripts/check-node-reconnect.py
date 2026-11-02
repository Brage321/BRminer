#!/usr/bin/env python3
"""Verify reconnect/shutdown against a real node, dropping only our proxy connection."""
import argparse
import json
import os
from pathlib import Path
import queue
import select
import signal
import socket
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("--host", required=True)
parser.add_argument("--port", type=int, default=3456)
parser.add_argument("--wallet", required=True)
args = parser.parse_args()
stop = threading.Event()
drop = threading.Event()
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(2)
listener.settimeout(.2)
local_port = listener.getsockname()[1]
proxy_errors = []
connections = []

def proxy():
    try:
        while not stop.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            with client, socket.create_connection((args.host, args.port), timeout=5) as remote:
                connections.append(time.monotonic())
                first = len(connections) == 1
                while not stop.is_set():
                    if first and drop.is_set():
                        break
                    readable, _, _ = select.select([client, remote], [], [], .1)
                    disconnected = False
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            disconnected = True
                            break
                        (remote if source is client else client).sendall(data)
                    if disconnected:
                        break
    except Exception as error:
        if not stop.is_set():
            proxy_errors.append(str(error))

worker = threading.Thread(target=proxy, daemon=True)
worker.start()
env = {k: v for k, v in os.environ.items() if not k.upper().startswith("BRMINER_")}
env["BRMINER_DISABLE_NVML"] = "1"
options = {}
kernel = None
if os.name == "nt":
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.FreeConsole()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    options = dict(creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
miner = subprocess.Popen([str(args.binary.resolve()), "-a", "warthog", "-p", f"127.0.0.1:{local_port}",
                          "-w", args.wallet, "--cpu_threads", "1", "--backend", "cpu"],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1, **options)
lines = queue.Queue()

def reader():
    for line in miner.stdout:
        lines.put(line)
    lines.put(None)

reader_worker = threading.Thread(target=reader, daemon=True)
reader_worker.start()
connected = 0
authorized = 0
jobs_after_reconnect = 0
all_lines = []
success = False
try:
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=.2)
        except queue.Empty:
            if proxy_errors:
                raise RuntimeError(proxy_errors)
            continue
        if line is None:
            raise RuntimeError("Miner exited before reconnect verification")
        print(line, end="", flush=True)
        all_lines.append(line)
        if "Connected to " in line:
            connected += 1
        if "Payout address authorized" in line:
            authorized += 1
        if "New job received:" in line:
            if connected == 1:
                drop.set()
            elif connected >= 2:
                jobs_after_reconnect += 1
                success = True
                break
    if not success:
        raise RuntimeError("Timed out waiting for a fresh job after the forced disconnect")
finally:
    try:
        if miner.poll() is None:
            if kernel:
                if not kernel.AttachConsole(miner.pid):
                    raise RuntimeError("Cannot attach hidden miner console for graceful shutdown")
                kernel.SetConsoleCtrlHandler(None, True)
                if not kernel.GenerateConsoleCtrlEvent(0, 0):
                    raise RuntimeError("Cannot send Ctrl+C to miner")
            else:
                miner.send_signal(signal.SIGTERM)
            miner.wait(timeout=15)
    except BaseException:
        if miner.poll() is None:
            miner.kill()
            miner.wait(timeout=5)
        raise
    finally:
        if kernel:
            kernel.FreeConsole()
        stop.set()
        listener.close()
        worker.join(timeout=2)
        reader_worker.join(timeout=2)
        while not lines.empty():
            line = lines.get()
            if line is not None:
                all_lines.append(line)
                print(line, end="", flush=True)

text = "".join(all_lines)
if (miner.returncode != 0 or "Mining stopped." not in text or connected < 2 or authorized < 2
        or "Warthog GPU backend: CPU" not in text):
    raise RuntimeError("Real-node reconnect/shutdown verification failed")
print("BRMINER_RECONNECT_RESULT " + json.dumps({
    "schema": "brminer.node-reconnect.v1", "node": f"{args.host}:{args.port}",
    "backend": "CPU", "forced_disconnect": True, "proxy_connections": len(connections),
    "miner_connections": connected, "authorizations": authorized,
    "jobs_after_reconnect": jobs_after_reconnect, "graceful_shutdown": True}), flush=True)
