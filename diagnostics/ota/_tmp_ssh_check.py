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
    "echo HI; whoami; ls -l /dev/ttyACM0",
    "python3 -c 'print(123)'",
    "python3 -c 'import esptool; print(getattr(esptool,\"__version__\", \"ok\"))' 2>&1 || echo NO_ESPTOOL",
    "python3 -m pip show esptool 2>&1 | head -5",
]
for cmd in cmds:
    print("CMD:", cmd)
    _, o, e = c.exec_command(cmd, timeout=60)
    print("OUT:", o.read().decode())
    err = e.read().decode()
    if err.strip():
        print("ERR:", err)
    print("code:", o.channel.recv_exit_status())
    print("---")
c.close()
