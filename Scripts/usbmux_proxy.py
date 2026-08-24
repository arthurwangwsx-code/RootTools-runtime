#!/usr/bin/env python3
"""Small usbmux port-forward helper backed by pymobiledevice3.

This is the built-in fallback when libimobiledevice's iproxy/idevice_id are
not installed on the host. It only forwards TCP streams to a selected USB
device and does not add any privileged device capability.
"""

from __future__ import annotations

import asyncio
import contextlib
import socket
import threading

from pymobiledevice3.usbmux import select_device


async def _select(udid: str | None):
    device = await select_device(udid)
    if device is None:
        raise RuntimeError("No USB iPhone found")
    return device


def discover_udid() -> str:
    return asyncio.run(_select(None)).serial


def _device_socket(udid: str, remote_port: int) -> socket.socket:
    async def connect() -> socket.socket:
        device = await _select(udid)
        return await device.connect(remote_port)

    sock = asyncio.run(connect())
    # pymobiledevice3 returns the usbmux socket in non-blocking mode. The
    # forwarding pumps below are ordinary blocking threads, so normalize it.
    sock.setblocking(True)
    return sock


def _pump(source: socket.socket, destination: socket.socket) -> None:
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            destination.sendall(data)
    except OSError:
        pass
    finally:
        try:
            destination.shutdown(socket.SHUT_WR)
        except OSError:
            pass


@contextlib.contextmanager
def port_forward(udid: str, remote_port: int):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listener.settimeout(0.2)
    local_port = listener.getsockname()[1]
    stop = threading.Event()
    active: list[socket.socket] = []

    def accept_loop() -> None:
        while not stop.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                remote = _device_socket(udid, remote_port)
            except Exception:
                client.close()
                continue
            active.extend((client, remote))
            threading.Thread(target=_pump, args=(client, remote), daemon=True).start()
            threading.Thread(target=_pump, args=(remote, client), daemon=True).start()

    thread = threading.Thread(target=accept_loop, daemon=True)
    thread.start()
    try:
        yield local_port
    finally:
        stop.set()
        listener.close()
        for sock in active:
            try:
                sock.close()
            except OSError:
                pass
        thread.join(timeout=1)
