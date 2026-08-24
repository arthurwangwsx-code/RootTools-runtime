#!/usr/bin/env python3
"""Diagnose the bootstrap state of the reference iPhone without root access.

This script deliberately uses Apple lockdown/developer services plus passive
USB port probes. It never asks for, stores, or attempts to bypass a device
passcode and it never executes a privileged command on the phone.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import socket
import shutil
import subprocess
import sys
import time

from usbmux_proxy import discover_udid, port_forward

from pymobiledevice3.lockdown import create_using_usbmux


PORTS = {
    "ssh": 22,
    "frida": 27042,
    "zxtouch": 6000,
    "roottools": 45821,
}


def run_json(command: list[str]) -> object | None:
    if command and command[0] == "pymobiledevice3" and shutil.which("pymobiledevice3") is None:
        command = [sys.executable, "-m", "pymobiledevice3", *command[1:]]
    try:
        result = subprocess.run(command, text=True, capture_output=True, timeout=8)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return None


def lockdown_value(udid: str, key: str) -> str | None:
    if shutil.which("ideviceinfo") is None:
        async def read_value():
            client = await create_using_usbmux(serial=udid)
            try:
                return await client.get_value(key=key)
            finally:
                await client.close()

        try:
            value = asyncio.run(read_value())
        except Exception:
            return None
        return str(value) if value is not None else None
    try:
        value = subprocess.check_output(["ideviceinfo", "-u", udid, "-k", key], text=True, timeout=5).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    return value or None


def passive_port_probe(udid: str, remote_port: int) -> bool:
    if shutil.which("iproxy") is None:
        try:
            with port_forward(udid, remote_port) as local_port:
                with socket.create_connection(("127.0.0.1", local_port), timeout=1) as sock:
                    sock.settimeout(0.35)
                    try:
                        data = sock.recv(1)
                        return data != b""
                    except socket.timeout:
                        return True
                    except OSError:
                        return False
        except Exception:
            return False
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as holder:
        holder.bind(("127.0.0.1", 0))
        local_port = holder.getsockname()[1]

    process = subprocess.Popen(
        ["iproxy", "-u", udid, f"{local_port}:{remote_port}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.25)
        if process.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", local_port), timeout=1) as sock:
                sock.settimeout(0.35)
                try:
                    data = sock.recv(1)
                    return data != b""
                except socket.timeout:
                    # A quiet service is still reachable; an unavailable device
                    # port is closed/reset by usbmuxd instead of remaining idle.
                    return True
                except (ConnectionResetError, ConnectionAbortedError, OSError):
                    return False
        except OSError:
            return False
    finally:
        process.terminate()
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def device_locked(udid: str) -> bool | None:
    payload = run_json(["pymobiledevice3", "developer", "accessibility", "list-items", "--udid", udid])
    if not isinstance(payload, list):
        return None
    captions = "\n".join(str(item.get("caption", "")) for item in payload if isinstance(item, dict)).lower()
    lock_markers = ("已锁定", "locked", "锁定")
    return any(marker in captions for marker in lock_markers)


def dopamine_state(udid: str) -> tuple[bool | None, bool | None]:
    apps = run_json(["pymobiledevice3", "apps", "list", "--udid", udid])
    installed: bool | None = None
    if isinstance(apps, dict):
        installed = "com.opa334.Dopamine" in apps

    processes = run_json(["pymobiledevice3", "developer", "dvt", "proclist", "--udid", udid])
    running: bool | None = None
    if isinstance(processes, list):
        running = any(isinstance(item, dict) and item.get("name") == "Dopamine" for item in processes)
    elif isinstance(processes, dict):
        # Some pymobiledevice3 releases wrap the process list in a keyed payload.
        rows = processes.get("processes")
        if isinstance(rows, list):
            running = any(isinstance(item, dict) and item.get("name") == "Dopamine" for item in rows)
    return installed, running


def main() -> int:
    parser = argparse.ArgumentParser(description="Diagnose RootTools/jailbreak bootstrap state")
    parser.add_argument("--udid", default=os.environ.get("ROOTTOOLS_UDID"))
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    udid = args.udid
    if not udid:
        try:
            if shutil.which("idevice_id"):
                udid = subprocess.check_output(["idevice_id", "-l"], text=True, timeout=5).splitlines()[0]
            else:
                udid = discover_udid()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, IndexError):
            print("No USB iPhone found", file=sys.stderr)
            return 2

    installed, running = dopamine_state(udid)
    ports = {name: passive_port_probe(udid, port) for name, port in PORTS.items()}
    locked = device_locked(udid)
    payload = {
        "udid": udid,
        "productType": lockdown_value(udid, "ProductType"),
        "productVersion": lockdown_value(udid, "ProductVersion"),
        "buildVersion": lockdown_value(udid, "BuildVersion"),
        "deviceLocked": locked,
        "dopamineInstalled": installed,
        "dopamineProcessRunning": running,
        "ports": ports,
        "jailbreakExecutionReady": bool(ports["frida"] or ports["ssh"] or ports["roottools"]),
    }

    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(f"Device: {payload['productType']} iOS {payload['productVersion']} ({payload['buildVersion']})")
        print(f"Locked: {payload['deviceLocked']}")
        print(f"Dopamine: installed={installed} processRunning={running}")
        for name, ready in ports.items():
            print(f"{name:9s}: {'ready' if ready else 'offline'}")
        print(f"Jailbreak execution ready: {payload['jailbreakExecutionReady']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
