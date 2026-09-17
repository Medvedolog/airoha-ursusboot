#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, struct, zlib
from pathlib import Path

FIP_MAGIC=0xAA640001
NT_UUID=bytes.fromhex('d6d0eea7fcead54b97829934f234b6e4')
CHECKSUM_UUID=bytes.fromhex('a2cceab7f8254b279704633a6fd69ad8')
CERT_UUIDS=[
 bytes.fromhex('827ee890f860e411a1b4777a21b4f94c'),
 bytes.fromhex('8ab8beccf960e4119ad0eb4822d8dcf8'),
 bytes.fromhex('8ad5832afb60e4118aafdf30bbc49859'),
 bytes.fromhex('d6e269ea5d63e4118d8c9fbabe9956a5'),
 bytes.fromhex('e2b20c205e63e4119ce8abccf92bb666'),
 bytes.fromhex('8ec4c1f35d63e411a7a987ee40b23fa7'),
]
CERT_REL=[0x0000,0x0c00,0x1400,0x1c00,0x2400,0x2c00]
CHECKSUM_REL=0x3400
END_REL=0x3800
ALIGN=0x800

def sha(b): return hashlib.sha256(b).hexdigest()
def align_up(x,a=ALIGN): return (x+a-1)//a*a

def parse(data: bytes):
    magic, serial, flags=struct.unpack_from('<IIQ',data,0)
    if magic != FIP_MAGIC: raise ValueError('bad FIP magic')
    out=[]; pos=16
    while pos+40 <= len(data):
        uid=data[pos:pos+16]; off,size,ef=struct.unpack_from('<QQQ',data,pos+16)
        if uid == b'\0'*16:
            return serial,flags,out,pos,off
        if off+size>len(data): raise ValueError('entry OOB')
        out.append({'uuid':uid,'off':off,'size':size,'flags':ef,'toc_pos':pos,'payload':data[off:off+size]})
        pos += 40
    raise ValueError('missing terminator')

def rebuild(template: bytes, nt_payload: bytes, force_old_layout=False):
    serial,flags,entries,term_pos,old_end=parse(template)
    by={e['uuid']:e for e in entries}
    if NT_UUID not in by or CHECKSUM_UUID not in by: raise ValueError('required entries missing')
    for u in CERT_UUIDS:
        if u not in by: raise ValueError('certificate entry missing')
    nt=by[NT_UUID]
    nt_off=nt['off']
    nt_end=nt_off+len(nt_payload)
    old_first=by[CERT_UUIDS[0]]['off']
    first=old_first if (force_old_layout or nt_end <= old_first) else align_up(nt_end)
    if first < nt_end: raise ValueError('NT overlaps certificate block')
    cert_offsets={u:first+r for u,r in zip(CERT_UUIDS,CERT_REL)}
    checksum_off=first+CHECKSUM_REL
    final_end=first+END_REL
    # hard safety: persistent FIP begins at physical 0x800 and must leave the protected tail before env
    if 0x800 + final_end > 0x7bffc:
        raise ValueError(f'FIP too large: physical end=0x{0x800+final_end:x}')
    out=bytearray(final_end)
    # preserve exact header/table/padding prefix through first payload boundary
    out[:0x400]=template[:0x400]
    # copy fixed payloads before NT exactly
    for e in entries:
        if e['uuid'] in (NT_UUID,CHECKSUM_UUID) or e['uuid'] in CERT_UUIDS: continue
        out[e['off']:e['off']+e['size']]=e['payload']
    out[nt_off:nt_off+len(nt_payload)]=nt_payload
    for u in CERT_UUIDS:
        e=by[u]; off=cert_offsets[u]
        out[off:off+e['size']]=e['payload']
    # Update TOC entries. Keep UUID, flags and all other metadata exactly.
    for e in entries:
        off=e['off']; size=e['size']
        if e['uuid']==NT_UUID:
            size=len(nt_payload)
        elif e['uuid'] in cert_offsets:
            off=cert_offsets[e['uuid']]
        elif e['uuid']==CHECKSUM_UUID:
            off=checksum_off; size=40
        struct.pack_into('<QQQ',out,e['toc_pos']+16,off,size,e['flags'])
    # Terminator end pointer
    struct.pack_into('<QQQ',out,term_pos+16,final_end,0,0)
    # Airoha checksum metadata covers bytes before checksum entry payload.
    crc=zlib.crc32(out[:checksum_off]) & 0xffffffff
    digest=hashlib.sha256(out[:checksum_off]).digest()
    meta=struct.pack('<II',checksum_off,crc)+digest
    assert len(meta)==40
    out[checksum_off:checksum_off+40]=meta
    final=bytes(out)
    # structural verification
    _s,_f,e2,_tp,end2=parse(final)
    if end2 != len(final): raise ValueError('terminator mismatch')
    b2={e['uuid']:e for e in e2}
    if b2[NT_UUID]['payload'] != nt_payload: raise ValueError('NT payload mismatch')
    if struct.unpack_from('<I',b2[CHECKSUM_UUID]['payload'],0)[0] != checksum_off: raise ValueError('checksum offset mismatch')
    if struct.unpack_from('<I',b2[CHECKSUM_UUID]['payload'],4)[0] != (zlib.crc32(final[:checksum_off])&0xffffffff): raise ValueError('checksum crc mismatch')
    if b2[CHECKSUM_UUID]['payload'][8:] != hashlib.sha256(final[:checksum_off]).digest(): raise ValueError('checksum sha mismatch')
    return final, {'nt_off':nt_off,'nt_size':len(nt_payload),'nt_end':nt_end,'cert_first':first,'checksum_off':checksum_off,'fip_end':final_end,'physical_end':0x800+final_end,'sha256':sha(final)}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('template',type=Path)
    ap.add_argument('nt',type=Path)
    ap.add_argument('output',type=Path)
    ap.add_argument('--selftest',action='store_true')
    args=ap.parse_args()
    t=args.template.read_bytes(); nt=args.nt.read_bytes()
    out,rep=rebuild(t,nt,force_old_layout=args.selftest)
    args.output.write_bytes(out)
    if args.selftest and out != t:
        # first difference for deterministic diagnosis
        i=next((i for i,(a,b) in enumerate(zip(out,t)) if a!=b), min(len(out),len(t)))
        raise SystemExit(f'SELFTEST FAIL: output differs at 0x{i:x}; out={len(out)} template={len(t)}')
    print('SELFTEST=PASS' if args.selftest else 'BUILD=PASS')
    for k,v in rep.items(): print(f'{k}={hex(v) if isinstance(v,int) else v}')
    return 0
if __name__=='__main__': raise SystemExit(main())
