#!/usr/bin/env python3
import argparse
import base64
import contextlib
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

from usbmux_proxy import discover_udid, port_forward

FRIDA_PORT = 27042

def frida_python() -> str:
    candidates = [
        shutil.which("frida"),
        str(Path.home() / "Library/Python/3.9/bin/frida"),
        str(Path.home() / ".local/bin/frida"),
    ]
    executable = next((Path(item) for item in candidates if item and Path(item).is_file()), None)
    if executable is None: raise SystemExit("frida-tools not found")
    first = executable.read_text(errors="ignore").splitlines()[0]
    return first[2:].split()[0]

@contextlib.contextmanager
def bridge(udid: str):
    if shutil.which("iproxy") is None:
        with port_forward(udid, FRIDA_PORT) as port:
            yield port
        return
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    p = subprocess.Popen(["iproxy", "-u", udid, f"{port}:{FRIDA_PORT}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(.25); yield port
    finally:
        p.terminate()
        try: p.wait(timeout=1)
        except subprocess.TimeoutExpired: p.kill(); p.wait()

def run_root(udid: str, command: str, timeout: float = 15.0) -> tuple[int,str,str]:
    helper = r'''
import sys, threading, typing
if not hasattr(typing, "NotRequired"):
    from typing_extensions import NotRequired, Required, ParamSpec
    typing.NotRequired = NotRequired
    typing.Required = Required
    typing.ParamSpec = ParamSpec
import frida
endpoint, command, timeout_text = sys.argv[1:4]
device = frida.get_device_manager().add_remote_device(endpoint)
stdout=[]; stderr=[]
done=threading.Event()
def on_output(pid, fd, data):
    if not data:
        if fd == 1: done.set()
        return
    (stdout if fd == 1 else stderr).append(data)
    if fd == 1 and b'__RT_RC__' in b''.join(stdout): done.set()
device.on("output", on_output)
prefix="export PATH=/var/jb/bin:/var/jb/usr/bin:/var/jb/sbin:/var/jb/usr/sbin:/bin:/usr/bin:/usr/sbin; "
pid=device.spawn(["/var/jb/bin/sh", "-c", prefix + command + "; printf '\n__RT_RC__%s\n' $?"], stdio="pipe")
device.resume(pid)
if not done.wait(float(timeout_text)):
    try: device.kill(pid)
    except Exception: pass
    raise SystemExit(124)
out=b''.join(stdout).decode(errors='replace'); err=b''.join(stderr).decode(errors='replace')
marker='\n__RT_RC__'
rc=0
if marker in out:
    text, _, tail=out.rpartition(marker); out=text
    try: rc=int(tail.strip().splitlines()[0])
    except Exception: rc=1
print(out, end='')
print(err, end='', file=sys.stderr)
print('\n__PY_ROOT_RC__' + str(rc))
'''
    with bridge(udid) as port:
        result = subprocess.run([frida_python(), "-c", helper, f"127.0.0.1:{port}", command, str(timeout)], text=True, capture_output=True, timeout=timeout+4)
    marker = "\n__PY_ROOT_RC__"
    if marker in result.stdout:
        stdout, _, tail = result.stdout.rpartition(marker)
        try: remote_rc = int(tail.strip().splitlines()[0])
        except Exception: remote_rc = result.returncode
        return remote_rc, stdout, result.stderr
    return result.returncode, result.stdout, result.stderr

def push(udid: str, local: Path, remote: str):
    helper = r'''
import sys, threading, typing
if not hasattr(typing, "NotRequired"):
    from typing_extensions import NotRequired, Required, ParamSpec
    typing.NotRequired = NotRequired
    typing.Required = Required
    typing.ParamSpec = ParamSpec
import frida
endpoint, local, remote = sys.argv[1:4]
device = frida.get_device_manager().add_remote_device(endpoint)
data=open(local,'rb').read()
block=4096
blocks=(len(data)+block-1)//block
padded=data + b'\0' * (blocks*block-len(data))
done=threading.Event()
def on_output(pid,fd,chunk):
    if not chunk: done.set()
device.on('output', on_output)
pid=device.spawn(['/var/jb/usr/bin/dd', 'of='+remote, 'bs='+str(block), 'count='+str(blocks), 'iflag=fullblock'], stdio='pipe')
device.resume(pid)
for i in range(0, len(padded), 1024):
    device.input(pid, padded[i:i+1024])
if not done.wait(15):
    try: device.kill(pid)
    except Exception: pass
    raise SystemExit(124)
print('__PUSH_OK__' + str(len(data)))
'''
    parent = str(Path(remote).parent)
    rc, out, err = run_root(udid, f"mkdir -p '{parent}'")
    if rc: raise SystemExit(err or out)
    with bridge(udid) as port:
        result = subprocess.run([frida_python(), "-c", helper, f"127.0.0.1:{port}", str(local), remote], text=True, capture_output=True, timeout=30)
    if "__PUSH_OK__" not in result.stdout:
        raise SystemExit(result.stderr or result.stdout or f"push transport failed: {result.returncode}")
    rc, out, err = run_root(udid, f"truncate -s {local.stat().st_size} '{remote}'")
    if rc: raise SystemExit(err or out)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--udid", default=os.environ.get("ROOTTOOLS_UDID"))
    sub=ap.add_subparsers(dest="action", required=True)
    e=sub.add_parser("exec"); e.add_argument("command"); e.add_argument("--timeout", type=float, default=15.0)
    p=sub.add_parser("push"); p.add_argument("local", type=Path); p.add_argument("remote")
    args=ap.parse_args()
    if args.udid:
        udid=args.udid
    elif shutil.which("idevice_id"):
        udid=subprocess.check_output(["idevice_id","-l"], text=True).splitlines()[0]
    else:
        udid=discover_udid()
    if args.action=="exec":
        rc,out,err=run_root(udid,args.command, timeout=args.timeout); print(out,end=''); print(err,end='',file=sys.stderr); raise SystemExit(rc)
    push(udid,args.local,args.remote)

if __name__ == "__main__": main()

