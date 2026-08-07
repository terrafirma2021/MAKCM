#!/usr/bin/env python3
import paramiko

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
cmds = [
    "timeout 5 curl -I https://pypi.org 2>&1 | head -5",
    "timeout 5 ping -c 1 8.8.8.8 2>&1 | head -5",
    "apt-cache search esptool 2>&1 | head -5",
    "which esptool esptool.py 2>/dev/null; dpkg -l | grep -i esptool || true",
    "ls /home/li/.local/share/makcm-ota/tools 2>/dev/null; find /home/li -name 'esptool*' 2>/dev/null | head -20",
]
for cmd in cmds:
    print("CMD:", cmd)
    _, o, e = c.exec_command(cmd, timeout=30)
    print(o.read().decode())
    err = e.read().decode()
    if err.strip():
        print("ERR", err)
    print("---")
c.close()
