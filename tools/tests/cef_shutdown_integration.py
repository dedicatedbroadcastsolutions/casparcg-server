#!/usr/bin/env python3
"""Exercise an isolated Linux CasparCG/CEF server; requires X11 and Pillow."""

import argparse
import ctypes
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import signal
import socket
import subprocess
import termios
import time

from PIL import Image


FAILURES = re.compile(
    r"FATAL:|Failed to send GetTerminationStatus|GPU state invalid|"
    r"Network service crashed|GPU process launch failed|GPU process isn't usable"
)


def belongs_to_test(pid, server_pid):
    seen = set()
    while pid > 1 and pid not in seen:
        if pid in (server_pid, os.getpid()):
            return True
        seen.add(pid)
        try:
            status = Path(f"/proc/{pid}/status").read_text()
        except (FileNotFoundError, ProcessLookupError):
            return False
        pid = int(re.search(r"^PPid:\s+(\d+)", status, re.MULTILINE).group(1))
    return False


def server_processes(binary, server_pid=None):
    found = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if (proc / "exe").resolve() == binary and (
                server_pid is None or belongs_to_test(int(proc.name), server_pid)
            ):
                found.append(int(proc.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return found


def reap_children():
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def wait_for(predicate, seconds, description):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for {description}")


def command(client, text):
    client.sendall((text + "\r\n").encode())
    response = b""
    while b"\r\n" not in response:
        data = client.recv(4096)
        if not data:
            raise RuntimeError(f"AMCP disconnected during {text}: {response!r}")
        response += data
    if not response.startswith(b"202 "):
        raise RuntimeError(f"{text}: {response!r}")


def run_case(binary, root, count, mode, gpu, replace):
    root.mkdir()
    for folder in ("media", "templates", "log", "data"):
        (root / folder).mkdir()
    (root / "templates" / "test.html").write_text(
        '<html><body style="margin:0;background:rgb(24,192,48)">'
        '<script>requestAnimationFrame(()=>console.log("CEF_RENDER_READY"));</script></body></html>'
    )
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    config = root / "config.xml"
    config.write_text(
        f"""<configuration>
<paths><media-path>{root}/media/</media-path><template-path>{root}/templates/</template-path>
<data-path>{root}/data/</data-path><log-path>{root}/log/</log-path></paths>
<log-level>info</log-level>
<html><enable-gpu>{str(gpu).lower()}</enable-gpu><cache-path>{root}/cache</cache-path></html>
<channels><channel><video-mode>PAL</video-mode><consumers/></channel></channels>
<controllers><tcp><port>{port}</port><protocol>AMCP</protocol></tcp></controllers>
</configuration>"""
    )
    stdout_path, stderr_path = root / "stdout.log", root / "stderr.log"
    env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1", LP_NUM_THREADS="2")
    tty_master = tty_slave = None
    client = None
    process = None
    start_shutdown = None
    if mode == "ctrl-c":
        tty_master, tty_slave = pty.openpty()

    def terminal_setup():
        fcntl.ioctl(tty_slave, termios.TIOCSCTTY, 0)

    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(
                [str(binary), str(config)],
                cwd=root,
                env=env,
                stdin=tty_slave if tty_slave is not None else subprocess.DEVNULL,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
                preexec_fn=terminal_setup if tty_slave is not None else None,
            )

            def connect():
                nonlocal client
                if process.poll() is not None:
                    raise RuntimeError(f"Server exited during startup: {process.returncode}")
                try:
                    client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    client.settimeout(15)
                    return True
                except (ConnectionRefusedError, socket.timeout):
                    return False

            wait_for(connect, 40, "AMCP startup")
            for layer in range(1, count + 1):
                command(client, f"PLAY 1-{layer} test")
            if count and mode != "pending":
                wait_for(
                    lambda: stdout_path.read_text(errors="replace").count("CEF_RENDER_READY") >= count,
                    20,
                    "HTML render callbacks",
                )
                if replace:
                    command(client, "PLAY 1-1 test")
                    wait_for(
                        lambda: stdout_path.read_text(errors="replace").count("CEF_RENDER_READY") > count,
                        20,
                        "replacement browser render callback",
                    )

                def rendered():
                    command(client, "ADD 1 IMAGE screenshot")
                    image_path = root / "media" / "screenshot.png"
                    wait_for(lambda: image_path.exists() and image_path.stat().st_size > 0, 5, "screenshot")
                    with Image.open(image_path) as image:
                        pixel = image.convert("RGBA").getpixel((image.width // 2, image.height // 2))
                    return pixel == (24, 192, 48, 255)

                wait_for(rendered, 15, "correct template pixel output")
            stderr_before_shutdown = stderr_path.stat().st_size
            processes = []
            for pid in server_processes(binary, process.pid):
                try:
                    processes.append({
                        "pid": pid, "group": os.getpgid(pid),
                        "command": Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace"),
                    })
                except (FileNotFoundError, ProcessLookupError):
                    continue
            (root / "processes.json").write_text(json.dumps(processes, indent=2))
            start_shutdown = time.monotonic()
            if mode in ("amcp", "pending"):
                # The server may close this connection before flushing its reply.
                client.sendall(b"KILL\r\n")
            elif mode == "sigint":
                process.send_signal(signal.SIGINT)
            elif mode == "sigterm":
                process.send_signal(signal.SIGTERM)
            elif mode == "group-sigint":
                os.killpg(process.pid, signal.SIGINT)
            elif mode == "ctrl-c":
                os.write(tty_master, b"\x03")
            else:
                raise ValueError(mode)
            process.wait(timeout=20)
            elapsed = time.monotonic() - start_shutdown
        text = stdout_path.read_text(errors="replace")
        errors = stderr_path.read_text(errors="replace")
        if process.returncode != 0:
            raise RuntimeError(f"Server exited with {process.returncode}")
        if "Successfully shutdown CasparCG Server." not in text:
            raise RuntimeError("Missing successful server shutdown")
        if FAILURES.search(errors):
            raise RuntimeError("CEF crash/restart diagnostics found in stderr.log")
        wait_for(lambda: not server_processes(binary, process.pid), 5, "CEF subprocess exit")
        reap_children()
        return {
            "case": root.name,
            "html": count,
            "gpu": gpu,
            "mode": mode,
            "shutdown_seconds": round(elapsed, 3),
            "native_stderr_lines": len(errors.splitlines()),
            "shutdown_stderr_bytes": stderr_path.stat().st_size - stderr_before_shutdown,
            "status": "passed",
        }
    finally:
        if client:
            client.close()
        if process and process.poll() is None:
            process.kill()
            process.wait()
        if process:
            for pid in server_processes(binary, process.pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    continue
        reap_children()
        for fd in (tty_master, tty_slave):
            if fd is not None:
                os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path, help="New directory for captured logs and JSON results")
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--gpu", choices=("false", "true", "both"), default="both")
    modes = ["amcp", "sigint", "sigterm", "group-sigint", "ctrl-c", "pending"]
    parser.add_argument("--modes", nargs="+", choices=modes, default=modes)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    binary = args.binary.resolve(strict=True)
    if server_processes(binary):
        raise RuntimeError("Test binary is already running; refusing to interfere")
    args.output = args.output.resolve()
    args.output.mkdir()
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "Cannot become test subprocess reaper")
    results = []
    gpu_values = [False, True] if args.gpu == "both" else [args.gpu == "true"]
    for gpu in gpu_values:
        for repeat in range(args.repeat):
            for mode in args.modes:
                for count in (0, 1, 3):
                    if mode == "pending" and count == 0:
                        continue
                    name = f"gpu-{gpu}-{repeat}-{mode}-html-{count}"
                    try:
                        result = run_case(binary, args.output / name, count, mode, gpu, replace=repeat > 0)
                    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
                        result = {"case": name, "status": "failed", "error": str(error)}
                    results.append(result)
                    print(json.dumps(result), flush=True)
                    (args.output / "results.json").write_text(json.dumps(results, indent=2))
    return int(any(result["status"] != "passed" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
