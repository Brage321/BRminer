"""Exercise synthetic block responses, reconnect, and shutdown with no GPU work."""
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import threading
import time


def main():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)
    listener.settimeout(15)
    port = listener.getsockname()[1]
    output = []
    errors = []
    completed = threading.Event()

    def send(stream, message):
        stream.write((json.dumps(message) + "\n").encode())
        stream.flush()

    def job(stream, name, target="003fffff"):
        send(stream, {"id": None, "method": "mining.set_difficulty", "params": [1e12]})
        send(stream, {"id": None, "method": "mining.notify",
                      "params": [name, "00" * 32, "", "00000003", target, "00000001", True]})

    def server():
        try:
            for connection in range(2):
                client, _ = listener.accept()
                client.settimeout(10)
                with client, client.makefile("rwb", buffering=0) as stream:
                    while True:
                        line = stream.readline()
                        if not line:
                            break
                        request = json.loads(line)
                        method = request.get("method")
                        if method == "mining.subscribe":
                            send(stream, {"id": request["id"], "result": [[], "00", 4], "error": None})
                        elif method == "mining.authorize":
                            send(stream, {"id": request["id"], "result": True, "error": None})
                            job(stream, "first-job" if connection == 0 else "reconnected-job")
                        elif method == "mining.submit":
                            if connection == 0:
                                send(stream, {"id": request["id"], "result": True, "error": None})
                                break
                            send(stream, {"id": request["id"], "result": False,
                                          "error": [23, "synthetic rejection", None]})
                            job(stream, "steady-job", "00000000")
                            completed.set()
                        elif method == "mining.suggest_difficulty":
                            send(stream, {"id": request["id"], "result": False, "error": None})
        except Exception as error:
            errors.append(str(error))
        finally:
            listener.close()

    worker = threading.Thread(target=server, daemon=True)
    worker.start()
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("BRMINER_")}
    env["BRMINER_DISABLE_NVML"] = "1"
    command = [str(pathlib.Path(sys.argv[1]).resolve()), "-a", "warthog",
               "-p", f"127.0.0.1:{port}", "-w", "syntheticTestWallet",
               "--cpu_threads", "1", "--backend", "cpu"]
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace",
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
    reader = threading.Thread(target=lambda: output.extend(process.stdout), daemon=True)
    reader.start()
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            text = "".join(output)
            if (completed.is_set() and "BLOCK ACCEPTED" in text and
                    "BLOCK REJECTED" in text and "New job received: steady-job" in text):
                break
            if process.poll() is not None:
                raise AssertionError("Miner exited early:\n" + text)
            time.sleep(0.05)
        else:
            raise AssertionError("Missing lifecycle events:\n" + "".join(output) + "\n" + str(errors))
        process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
        process.wait(timeout=6)
        reader.join(timeout=1)
        if os.name != "nt":
            assert process.returncode == 0, "".join(output)
            assert "Mining stopped." in "".join(output)
        print("Synthetic block acceptance/rejection and reconnect passed; Linux SIGTERM shutdown verified."
              if os.name != "nt" else "Synthetic block acceptance/rejection and reconnect passed.")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        reader.join(timeout=1)


if __name__ == "__main__":
    main()
