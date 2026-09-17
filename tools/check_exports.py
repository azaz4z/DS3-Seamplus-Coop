import os
import subprocess

vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
vs = subprocess.check_output([vswhere, "-latest", "-property", "installationPath"], text=True).strip()
vcvars = os.path.join(vs, r"VC\Auxiliary\Build\vcvars64.bat")
cmd = f'"{vcvars}" && dumpbin.exe /exports build/SeamlessCoop/ds3sc_companion.dll'
res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
for line in res.stdout.splitlines():
    if "ds3sc_" in line:
        print(line)
