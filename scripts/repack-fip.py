#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, struct
from pathlib import Path
MAGIC=0xAA640001; HEADER=16; ENTRY=40
NT_FW_UUID=bytes.fromhex('d6d0eea7fcead54b97829934f234b6e4')
NT_END_MAX=0x77800; FIP_FILE_MAX=0x7B800

def parse(data: bytes):
    if len(data) < HEADER+ENTRY or struct.unpack_from('<I',data,0)[0] != MAGIC:
        raise SystemExit('donor is not an ARM FIP')
    out=[]; pos=HEADER
    while pos+ENTRY <= len(data):
        uuid=data[pos:pos+16]; off,size,flags=struct.unpack_from('<QQQ',data,pos+16)
        if uuid == b'\0'*16: break
        if off+size > len(data): raise SystemExit('FIP entry outside donor')
        out.append((pos,uuid,off,size,flags)); pos += ENTRY
    return out

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--donor',type=Path,required=True); ap.add_argument('--bl33',type=Path,required=True); ap.add_argument('--output',type=Path,required=True); a=ap.parse_args()
    donor=a.donor.read_bytes(); bl33=a.bl33.read_bytes(); entries=parse(donor)
    nts=[e for e in entries if e[1]==NT_FW_UUID]
    if len(nts)!=1: raise SystemExit(f'expected exactly one BL33/nt-fw entry, found {len(nts)}')
    pos,_,off,old_size,_=nts[0]
    if any(e[2]>off for e in entries): raise SystemExit('BL33 is not the last FIP payload')
    end=off+len(bl33)
    if end>NT_END_MAX: raise SystemExit(f'BL33 exceeds persistent FIP boundary: end=0x{end:x} max=0x{NT_END_MAX:x}')
    if len(donor)>FIP_FILE_MAX or end>len(donor): raise SystemExit('BL33/FIP does not fit proven donor footprint')
    out=bytearray(donor); struct.pack_into('<Q',out,pos+24,len(bl33)); out[off:end]=bl33
    a.output.parent.mkdir(parents=True,exist_ok=True); a.output.write_bytes(out)
    verify=a.output.read_bytes(); vnt=[e for e in parse(verify) if e[1]==NT_FW_UUID][0]
    if verify[vnt[2]:vnt[2]+vnt[3]] != bl33: raise SystemExit('repacked BL33 mismatch')
    for e0,e1 in zip(entries,parse(verify)):
        if e0[1] != NT_FW_UUID:
            if e0[1:] != e1[1:] or donor[e0[2]:e0[2]+e0[3]] != verify[e1[2]:e1[2]+e1[3]]: raise SystemExit('non-BL33 FIP data changed')
    print('FIP_REPACK=PASS',f'donor_sha256={hashlib.sha256(donor).hexdigest()}',f'bl33_sha256={hashlib.sha256(bl33).hexdigest()}',f'output_sha256={hashlib.sha256(verify).hexdigest()}',f'bl33_old={old_size}',f'bl33_new={len(bl33)}',f'bl33_end=0x{end:x}',f'margin={NT_END_MAX-end}')
if __name__=='__main__': main()
