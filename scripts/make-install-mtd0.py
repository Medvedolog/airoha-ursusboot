#!/usr/bin/env python3
import argparse, hashlib
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--template',required=True); p.add_argument('--fip',required=True); p.add_argument('--output',required=True); p.add_argument('--fip-offset',type=lambda x:int(x,0),default=0x800); p.add_argument('--env-offset',type=lambda x:int(x,0),default=0x7c000)
a=p.parse_args(); t=bytearray(Path(a.template).read_bytes()); f=Path(a.fip).read_bytes()
if len(t)!=0x80000: raise SystemExit('template must be exactly 512 KiB')
if a.fip_offset+len(f)>a.env_offset: raise SystemExit('FIP overlaps preserved stock env area')
if f[:4]!=b'\x01\x00\x64\xaa': raise SystemExit('FIP magic mismatch')
t[a.fip_offset:a.fip_offset+len(f)]=f; Path(a.output).parent.mkdir(parents=True,exist_ok=True); Path(a.output).write_bytes(t)
print('output:',a.output); print('sha256:',hashlib.sha256(t).hexdigest())
