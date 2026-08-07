import paramiko, sys
c=paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect("192.168.137.2", username="li", password="1234567", look_for_keys=False, allow_agent=False, timeout=10)
cmd = r"""
set +e
ls -l /dev/ttyACM* 2>&1
fuser -k /dev/ttyACM0 2>/dev/null
sleep 0.5
cd /home/li/.local/share/makcm-ota/go1-left-gen16
PYTHONUNBUFFERED=1 python3 -u - <<'PY'
import json, sys
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, decode_status_v2
print("open", flush=True)
t=SerialTransport("/dev/ttyACM0", baudrate=4_000_000, timeout=0.05)
try:
    c=OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=1)
    print("hello", flush=True)
    st=decode_status_payload(c.hello().payload)
    print(json.dumps({"ok":True,"state":st.state,"active":st.active_generation,"target":st.target_generation,"flags":st.status_flags,"local":bool(st.status_flags&0x80)}), flush=True)
    v2=decode_status_v2(c.status(v2=True).payload)
    print(json.dumps({"v2":True,"active":v2.active_generation,"image_state":v2.image_state,"running":v2.running_slot,"boot":v2.boot_slot,"version":v2.app_version,"local_validated":v2.local_validated,"elf":v2.app_elf_sha256.hex()}), flush=True)
except Exception as e:
    print(json.dumps({"ok":False,"error":type(e).__name__+": "+str(e)}), flush=True)
finally:
    t.close()
PY
"""
chan=c.get_transport().open_session(); chan.get_pty(); chan.settimeout(40); chan.exec_command(cmd)
import time
deadline=time.time()+35
buf=""
while time.time()<deadline:
    if chan.recv_ready():
        chunk=chan.recv(4096).decode("utf-8","replace"); buf+=chunk; sys.stdout.write(chunk); sys.stdout.flush()
    if chan.recv_stderr_ready():
        chunk=chan.recv_stderr(4096).decode("utf-8","replace"); buf+=chunk; sys.stderr.write(chunk)
    if chan.exit_status_ready():
        break
    time.sleep(0.1)
while chan.recv_ready():
    chunk=chan.recv(4096).decode("utf-8","replace"); sys.stdout.write(chunk)
print("remote_exit", chan.recv_exit_status() if chan.exit_status_ready() else "timeout")
c.close()
