#!/usr/bin/env python3
import paramiko
import sys
import time

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(
    "192.168.137.2",
    username="li",
    password="1234567",
    look_for_keys=False,
    allow_agent=False,
    timeout=10,
)
cmd = r"""
set -x
python3 -m pip install --user 'esptool==4.8.1' 2>&1 | tail -20
python3 -c 'import esptool; print("esptool", getattr(esptool,"__version__","?"))'
"""
chan = c.get_transport().open_session()
chan.settimeout(120)
chan.exec_command(cmd)
deadline = time.time() + 110
while time.time() < deadline:
    if chan.recv_ready():
        sys.stdout.write(chan.recv(4096).decode("utf-8", "replace"))
        sys.stdout.flush()
    if chan.recv_stderr_ready():
        sys.stderr.write(chan.recv_stderr(4096).decode("utf-8", "replace"))
        sys.stderr.flush()
    if chan.exit_status_ready():
        break
    time.sleep(0.2)
while chan.recv_ready():
    sys.stdout.write(chan.recv(4096).decode("utf-8", "replace"))
print("exit", chan.recv_exit_status() if chan.exit_status_ready() else "pending")
c.close()
