#!/usr/bin/env python3
"""Inspect a Parquet footer or extract the first N original row groups.

Standard-library development utility, not part of Rayforce. Uses curl HTTP ranges
for remote inputs; copying a sample does not decode/re-encode its data pages.
Usage: python3 scripts/parquet-inspect.py URL [--sample /tmp/sample.parquet --groups 1]
"""
import argparse
import collections
import json
import struct
import subprocess
from pathlib import Path

class Compact:
    def __init__(self, data): self.data, self.pos = data, 0
    def take(self, n):
        if n < 0 or n > len(self.data)-self.pos: raise ValueError('truncated compact value')
        p = self.pos; self.pos += n
        return self.data[p:self.pos]
    def var(self):
        n = 0
        for shift in range(0,70,7):
            b = self.take(1)[0]
            if shift == 63 and b > 1: raise ValueError('varint overflow')
            n |= (b & 127) << shift
            if not b & 128: return n
        raise ValueError('varint overflow')
    def integer(self):
        v = self.var(); return (v >> 1) ^ -(v & 1)
    def value(self, t, depth=0):
        if depth > 32: raise ValueError('compact nesting limit')
        if t in (1,2): return t == 1
        if t == 3: return self.take(1)[0]
        if t in (4,5,6): return self.integer()
        if t == 7: return self.take(8)
        if t == 8: return self.take(self.var())
        if t in (9,10):
            h = self.take(1)[0]; n = h >> 4
            if n == 15: n = self.var()
            if n > len(self.data)-self.pos: raise ValueError('invalid list length')
            et = h & 15
            return et, [self.take(1)[0] == 1 if et in (1,2) else self.value(et,depth+1) for _ in range(n)]
        if t == 12:
            out = []; fid = 0
            while True:
                h = self.take(1)[0]
                if not h: return out
                fid = fid + (h >> 4) if h >> 4 else self.integer()
                out.append((fid, h & 15, self.value(h & 15,depth+1)))
        raise ValueError(f'unsupported compact type {t}')

def var(n):
    out = bytearray()
    while n > 127: out.append((n & 127) | 128); n >>= 7
    return bytes(out + bytes([n]))

def encode(t, v):
    if t in (1,2): return b''
    if t == 3: return bytes([v])
    if t in (4,5,6): return var((v << 1) ^ (v >> 63))
    if t == 7: return v
    if t == 8: return var(len(v)) + v
    if t in (9,10):
        et, items = v; n = len(items)
        return bytes([(min(n,15)<<4)|et]) + (var(n) if n >= 15 else b'') + b''.join(bytes([1 if x else 2]) if et in (1,2) else encode(et,x) for x in items)
    if t == 12:
        out = bytearray(); last = 0
        for fid,ft,fv in v:
            delta = fid-last
            out += bytes([(delta<<4)|ft]) if 0 < delta < 16 else bytes([ft])+encode(4,fid)
            out += encode(ft,fv); last = fid
        return bytes(out + b'\0')
    raise ValueError(t)

def get(fields, fid, default=None):
    return next((v for i,t,v in fields if i == fid), default)

def put(fields, fid, val):
    return [(i,t,val if i == fid else v) for i,t,v in fields]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source'); parser.add_argument('--sample',type=Path)
    parser.add_argument('--groups',type=int,default=1)
    a = parser.parse_args()
    remote = a.source.startswith(('https://','http://'))
    if remote:
        headers = subprocess.run(['curl','--fail','--silent','--show-error','--head',a.source],check=True,capture_output=True).stdout.decode()
        sizes = [line.split(':',1)[1].strip() for line in headers.splitlines() if line.lower().startswith('content-length:')]
        if not sizes: raise ValueError('server did not provide Content-Length')
        size = int(sizes[-1])
    else: size = Path(a.source).stat().st_size
    def read(start, n):
        if start < 0 or n < 0 or start+n > size: raise ValueError('range outside file')
        if remote:
            data = subprocess.run(['curl','--fail','--silent','--show-error','--range',f'{start}-{start+n-1}',
                '--max-filesize',str(n),a.source],check=True,capture_output=True).stdout
        else:
            with open(a.source,'rb') as f: f.seek(start); data = f.read(n)
        if len(data) != n: raise ValueError('short/oversized range response')
        return data
    tail = read(size-8,8)
    if tail[4:] != b'PAR1': raise ValueError('not an unencrypted Parquet file')
    footer_len = struct.unpack('<I',tail[:4])[0]
    if footer_len > 64*1024*1024: raise ValueError('footer exceeds 64 MiB')
    reader = Compact(read(size-8-footer_len,footer_len)); meta = reader.value(12)
    schema = get(meta,2)[1]; groups = get(meta,4)[1]
    codecs = collections.Counter(); encodings = collections.Counter(); stats = pages = blooms = 0
    for group in groups:
        for chunk in get(group,1)[1]:
            md = get(chunk,3)
            codecs[get(md,4)] += 1; encodings[str(get(md,2)[1])] += 1
            stats += get(md,12) is not None
            pages += get(chunk,4) is not None and get(chunk,6) is not None
            blooms += get(md,14) is not None
    print(json.dumps({'source':a.source,'bytes':size,'rows':get(meta,3),'columns':len(schema)-1,
        'row_groups':len(groups),'created_by':get(meta,6,b'').decode(),'codec_counts':dict(codecs),
        'encoding_counts':dict(encodings),'chunks_with_statistics':stats,'chunks_with_page_indexes':pages,
        'chunks_with_bloom_filters':blooms},indent=2))
    if a.sample:
        if not 1 <= a.groups <= len(groups): raise ValueError('invalid group count')
        selected=[]; total=0
        # Exclusive output prevents accidental overwrite of a sample.
        with a.sample.open('xb') as out:
            out.write(b'PAR1')
            for group in groups[:a.groups]:
                chunks=[]
                for chunk in get(group,1)[1]:
                    md = get(chunk,3)
                    if get(chunk,1) is not None: raise ValueError('external column file')
                    offset=min(get(md,9),get(md,11,get(md,9))); length=get(md,7)
                    new_offset=out.tell(); out.write(read(offset,length))
                    delta=new_offset-offset
                    for fid in (9,10,11):
                        if get(md,fid) is not None: md=put(md,fid,get(md,fid)+delta)
                    # Drop source index/bloom references; sample has no copied index regions.
                    md=[x for x in md if x[0] not in (14,15)]
                    chunks.append([(2,6,new_offset),(3,12,md)])
                group=put(group,1,(12,chunks))
                group=[x for x in group if x[0] not in (5,6,7)]
                selected.append(group); total+=get(group,3)
            meta=put(put(meta,3,total),4,(12,selected)); footer=encode(12,meta)
            out.write(footer); out.write(struct.pack('<I',len(footer))); out.write(b'PAR1')
        print(f'Sample: {total} rows, {a.sample.stat().st_size} bytes, {a.sample}')

if __name__ == '__main__': main()
