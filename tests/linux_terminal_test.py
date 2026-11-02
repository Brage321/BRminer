"""Verify the live renderer through a real PTY and an independent VT emulator."""
import errno
import fcntl
import os
import pathlib
import pty
import select
import struct
import subprocess
import sys
import termios
import time

import pyte


def main():
    master, slave = pty.openpty()
    screen = pyte.Screen(120, 35)
    stream = pyte.Stream(screen)

    def resize(columns, lines):
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", lines, columns, 0, 0))
        screen.resize(lines=lines, columns=columns)

    def consume(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if select.select([master], [], [], .05)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        return
                    raise
                stream.feed(data.decode("utf8", errors="replace"))

    def check():
        text = "\n".join(screen.display)
        assert screen.display[0].startswith("Warthog | CUDA"), text
        assert text.count("JANUS (10s)") == 1, text
        return text

    resize(120, 35)
    env = dict(os.environ, TERM="xterm")
    process = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), "--live-demo"],
                               stdin=slave, stdout=slave, stderr=slave, env=env)
    os.close(slave)
    try:
        consume(.7)
        check()
        resize(80, 25)
        consume(.7)
        compact = check()
        assert "GPU sensors" in compact and "N/A" in compact, compact
        def anchor(text, prefix):
            return next(i for i, line in enumerate(text.splitlines()) if line.startswith(prefix))
        headings = ["GPU 0 clock limits:", "Events (newest below)"]
        positions = [anchor(compact, prefix) for prefix in headings]
        consume(4.5)
        text = check()
        assert "SIMULATED BLOCK ACCEPTED." in text, text
        assert "2 / 0 (0.0%)" in text, text
        assert "SIMULATED new job" in text, text
        assert [anchor(text, prefix) for prefix in headings] == positions, text
        consume(10)
        process.wait(timeout=2)
        assert process.returncode == 0
        print("Pinned Linux dashboard, event scrolling, resize, and immediate counter updates passed.")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        os.close(master)


if __name__ == "__main__":
    main()
