#!/usr/bin/env python3
"""Rebuild small Parquet fixtures using Python's standard library only.

Independent format writer for reader tests: no Arrow/Thrift/Snappy libraries.
Not installed, linked, or invoked by Rayforce or the normal test suite.
"""
import struct
from pathlib import Path

STOP, TRUE, FALSE, BYTE, I16, I32, I64, DOUBLE, BINARY, LIST, SET, MAP, STRUCT = range(13)

def var(n):
    out = bytearray()
    while n > 127:
        out.append((n & 127) | 128)
        n >>= 7
    return bytes(out + bytes([n]))

def value(t, v):
    if t in (TRUE, FALSE): return b''
    if t == BYTE: return bytes([v & 255])
    if t in (I16, I32, I64): return var((v << 1) ^ (v >> 63))
    if t == BINARY:
        v = v.encode() if isinstance(v, str) else v
        return var(len(v)) + v
    if t == LIST:
        et, items = v
        h = bytes([(min(len(items), 15) << 4) | et])
        return h + (var(len(items)) if len(items) >= 15 else b'') + b''.join(bytes([TRUE if x else FALSE]) if et == TRUE else value(et, x) for x in items)
    if t == STRUCT:
        out, last = b'', 0
        for fid, ft, fv in v:
            d = fid-last
            out += bytes([(d << 4) | ft]) if 0 < d < 16 else bytes([ft]) + value(I16, fid)
            out += value(ft, fv)
            last = fid
        return out + b'\0'
    raise ValueError(t)

def fields(items): return value(STRUCT, items)

def snappy(data):
    """Literal-only valid block; back references are covered by C codec tests."""
    if not data: return b'\0'
    n = len(data)-1
    if n < 60: tag = bytes([n << 2])
    else:
        nb = max(1, (n.bit_length()+7)//8)
        tag = bytes([(59+nb) << 2]) + n.to_bytes(nb, 'little')
    return var(len(data)) + tag + data

def rle(values, width):
    return b''.join(var(2) + x.to_bytes((width+7)//8, 'little') for x in values)

def packed(values, width):
    if not values: return b''
    padded = values + [0] * (-len(values) % 8)
    bits = sum(v << (i*width) for i,v in enumerate(padded))
    return var((len(padded)//8)*2+1) + bits.to_bytes(len(padded)*width//8, 'little')

def plain(pt, values):
    if pt == 0: return bytes(sum(bool(x) << j for j,x in enumerate(values[i:i+8])) for i in range(0,len(values),8))
    if pt == 6:
        encoded = [x if isinstance(x,bytes) else x.encode() for x in values]
        return b''.join(struct.pack('<I',len(x))+x for x in encoded)
    return struct.pack('<'+{1:'i',2:'q',4:'f',5:'d'}[pt]*len(values), *values)

def integer_hash(x, width):
    mask=(1<<64)-1
    def rot(x,n): return ((x<<n)|(x>>(64-n))) & mask
    p1,p2,p3,p4=11400714785074694791,14029467366897019727,1609587929392839161,9650029242287828579
    h=2870177450012600261+width
    if width==8:
        h ^= (rot(((x&mask)*p2)&mask,31)*p1)&mask
        h=(rot(h,27)*p1+p4)&mask
    else:
        h ^= ((x&0xffffffff)*p1)&mask
        h=(rot(h,23)*p2+p3)&mask
    h ^= h>>33; h=(h*p2)&mask; h ^= h>>29; h=(h*p3)&mask
    return h ^ (h>>32)

class Page(bytes):
    def __new__(cls, data, uncompressed):
        obj = super().__new__(cls, data)
        obj.uncompressed = uncompressed
        return obj

def page(kind, header, body, codec=1, prefix=b'', compressed=True, crc=False):
    import zlib  # stdlib, fixture checksums only
    payload = prefix + (snappy(body) if codec and compressed else body)
    f = [(1,I32,kind),(2,I32,len(prefix)+len(body)),(3,I32,len(payload))]
    if crc:
        chk = zlib.crc32(payload)
        f.append((4,I32,chk if chk < 2**31 else chk-2**32))
    f.append(({0:5,2:7,3:8}[kind],STRUCT,header))
    header_bytes = fields(f)
    return Page(header_bytes+payload,len(header_bytes)+len(prefix)+len(body))

def write(path, specs, groups, codec=1, v2=False, dictionary=True, crc=False, legacy=False, statistics=True, compress_v2=True, indexes=False, bloom=False, bad_index=False, bad_bloom=False, mismatched=False, corrupt_column=None, rle_ids=False, rle_bools=False):
    data = bytearray(b'PAR1')
    schema = [[(3,I32,0),(4,BINARY,'schema'),(5,I32,len(specs))]]
    for name,pt,optional,converted in specs:
        s = [(1,I32,pt),(3,I32,int(optional)),(4,BINARY,name)]
        if converted is not None: s.append((6,I32,converted))
        schema.append(s)
    rowgroups=[]
    for group in groups:
        cols=[]; rgsize=0
        for ci, ((name,pt,optional,converted), values) in enumerate(zip(specs,group)):
            pages=[]; start=len(data); dict_off=None
            if not values:
                # A zero-row group as common writers emit it: an empty
                # dictionary page, no data page, data_page_offset 0.
                pages.append(page(2,[(1,I32,0),(2,I32,0)],b'',codec,crc=crc))
                chunk=pages[0]; data+=chunk
                md=[(1,I32,pt),(2,LIST,(I32,[0,3])),(3,LIST,(BINARY,[name])),(4,I32,codec),(5,I64,0),(6,I64,pages[0].uncompressed),(7,I64,len(chunk)),(9,I64,0),(11,I64,start)]
                cols.append([(2,I64,start),(3,STRUCT,md)]); rgsize+=pages[0].uncompressed
                continue
            # Three pages force dictionary->plain fallback and independent
            # page boundaries between columns; last page is always PLAIN.
            cuts=[0,max(1,len(values)//3),max(2,2*len(values)//3),len(values)]
            if mismatched and ci: cuts=[0,2,7,len(values)]
            cuts=sorted(set(min(x,len(values)) for x in cuts))
            vocab=list(dict.fromkeys(x for x in values if x is not None))
            use_dict=dictionary and pt!=0 and bool(vocab)
            if use_dict:
                dict_off=start
                pages.append(page(2,[(1,I32,len(vocab)),(2,I32,2 if legacy else 0)],plain(pt,vocab),codec,crc=crc))
            data_off=start+sum(map(len,pages))
            for k,(lo,hi) in enumerate(zip(cuts,cuts[1:])):
                vals=values[lo:hi]; valid=[x for x in vals if x is not None]
                enc=(2 if legacy else 8) if use_dict and k<len(cuts)-2 else 0
                width=max(0,(len(vocab)-1).bit_length())
                if enc and rle_ids:
                    from itertools import groupby
                    ids=[vocab.index(x) for x in valid]
                    body=bytes([width])+b''.join(var(len(list(run))*2)+v.to_bytes((width+7)//8,'little') for v,run in groupby(ids))
                elif pt==0 and rle_bools:
                    # RLE value encoding: 4-byte length, then a width-1
                    # hybrid stream (runs, then a bit-packed tail).
                    from itertools import groupby
                    bits=[int(x) for x in valid]
                    half=len(bits)//2
                    hybrid=b''.join(var(len(list(run))*2)+bytes([v]) for v,run in groupby(bits[:half]))+packed(bits[half:],1)
                    body=struct.pack('<I',len(hybrid))+hybrid; enc=3
                else:
                    body=bytes([width])+packed([vocab.index(x) for x in valid],width) if enc else plain(pt,valid)
                levels=packed([int(x is not None) for x in vals],1) if optional else b''
                if v2:
                    h=[(1,I32,len(vals)),(2,I32,len(vals)-len(valid)),(3,I32,len(vals)),(4,I32,enc),(5,I32,len(levels)),(6,I32,0),(7,TRUE if compress_v2 else FALSE,compress_v2)]
                    pages.append(page(3,h,body,codec,prefix=levels,crc=crc,compressed=compress_v2))
                else:
                    body=(struct.pack('<I',len(levels))+levels if optional else b'')+body
                    h=[(1,I32,len(vals)),(2,I32,enc),(3,I32,3),(4,I32,3)]
                    pages.append(page(0,h,body,codec,crc=crc))
            chunk=b''.join(pages); data+=chunk
            if name == corrupt_column: data[start+len(pages[0])-1] ^= 0x80
            raw_size=sum(p.uncompressed for p in pages); rgsize+=raw_size
            md=[(1,I32,pt),(2,LIST,(I32,[0,3,2 if legacy else 8] if use_dict else [3] if pt==0 and rle_bools else [0,3])),(3,LIST,(BINARY,[name])),(4,I32,codec),(5,I64,len(values)),(6,I64,raw_size),(7,I64,len(chunk)),(9,I64,data_off)]
            if dict_off is not None: md.append((11,I64,dict_off))
            valid=[x for x in values if x is not None]
            stats=[(3,I64,len(values)-len(valid))]
            if valid and pt in (1,2):
                stats += [(5,BINARY,plain(pt,[max(valid)])),(6,BINARY,plain(pt,[min(valid)])),(7,TRUE,True),(8,TRUE,True)]
            if statistics: md.append((12,STRUCT,stats))
            cc=[(2,I64,start),(3,STRUCT,md)]
            if indexes and pt in (1,2):
                locs=[]; nulls=[]; lows=[]; highs=[]; offset=data_off
                for pg,(lo,hi) in zip(pages[int(use_dict):],zip(cuts,cuts[1:])):
                    v=[x for x in values[lo:hi] if x is not None]
                    locs.append([(1,I64,offset),(2,I32,len(pg)),(3,I64,lo)])
                    nulls.append(not v); lows.append(plain(pt,[min(v)]) if v else b''); highs.append(plain(pt,[max(v)]) if v else b'')
                    offset+=len(pg)
                if bad_index: locs[1][2]=(3,I64,0)  # invalid row ordering => fallback
                oi=fields([(1,LIST,(STRUCT,locs))])
                ix=fields([(1,LIST,(TRUE,nulls)),(2,LIST,(BINARY,lows)),(3,LIST,(BINARY,highs)),(4,I32,0)])
                cc += [(4,I64,len(data)),(5,I32,len(oi)),(6,I64,len(data)+len(oi)),(7,I32,len(ix))]
                data += oi+ix
            if bloom and pt in (1,2):
                words=[0]*64  # Eight blocks exercise the high-hash block choice.
                salt=[0x47b6137b,0x44974d91,0x8824ad5b,0xa2b7289d,0x705495c7,0x2df1424b,0x9efc4947,0x5c6bfb31]
                for v in valid:
                    h=integer_hash(v,4 if pt==1 else 8); block=((h>>32)*8)>>32
                    for j,k in enumerate(salt): words[block*8+j] |= 1 << (((h*k)&0xffffffff)>>27)
                union=[(2 if bad_bloom else 1,STRUCT,[])]
                bf=fields([(1,I32,256),(2,STRUCT,union),(3,STRUCT,union),(4,STRUCT,union)])+struct.pack('<64I',*words)
                md += [(14,I64,len(data)),(15,I32,len(bf))]; data+=bf
            cols.append(cc)
        rowgroups.append([(1,LIST,(STRUCT,cols)),(2,I64,rgsize),(3,I64,len(group[0]))])
    footer=fields([(1,I32,1),(2,LIST,(STRUCT,schema)),(3,I64,sum(len(g[0]) for g in groups)),(4,LIST,(STRUCT,rowgroups)),(6,BINARY,'rayforce fixture writer')])
    path.write_bytes(data+footer+struct.pack('<I',len(footer))+b'PAR1')

def long_strings(path, rows=8000, size=9000):
    """Decodes to ~72 MB of strings: past one 64 MiB batch pool, so the
    reader must split the row group into smaller batches.  Too large to
    commit; the suite generates it on demand."""
    write(Path(path),[('id',2,False,None),('doc',6,False,0)],
          [[list(range(rows)),[str(i).encode()+b' '+b'x'*(size-len(str(i))-1) for i in range(rows)]]],
          codec=0,dictionary=False)

if __name__ == '__main__':
    import sys
    if sys.argv[1:2] == ['--long-strings']:
        long_strings(sys.argv[2]); sys.exit(0)
    root=Path(__file__).parent
    specs=[('x',1,True,None),('name',6,True,0),('flag',0,True,None),('wide',2,False,None),('day',1,False,6),('ts',2,False,10),('f',5,True,None)]
    rows=[[1,None,3,4,5,None,7,8,9],['alpha','a long string over twelve bytes',None,'','alpha','beta','gamma','delta','z'],[True,None,False,True,False,True,None,False,True],list(range(100,109)),list(range(10957,10966)),[946684800000000+i for i in range(9)],[1.25,None,-2.,3.,4.,5.,6.,7.,8.]]
    for codec in (0,1):
        for v2 in (False,True):
            write(root/f'flat-{codec}-v{2 if v2 else 1}.parquet',specs,[rows,rows],codec,v2,crc=True)
    write(root/'legacy.parquet',specs,[rows,rows],legacy=True)
    write(root/'v2-uncompressed.parquet',specs,[rows,rows],v2=True,compress_v2=False)
    write(root/'no-statistics.parquet',specs,[rows,rows],statistics=False)
    write(root/'empty.parquet',specs,[])
    write(root/'all-null.parquet',[('x',1,True,None),('s',6,True,0)],[[[None]*9,[None]*9]],v2=True)
    write(root/'bool.parquet',[('b',0,False,None)],[[[True,False]*5]],v2=True)
    bools=[True,True,True,None,False,True,False,False,True,None,True,False,True,True,False,False,True,False,True,True]
    for v2 in (False,True):
        write(root/f'bool-rle-v{2 if v2 else 1}.parquet',[('b',0,True,None)],[[bools],[bools[::-1]]],v2=v2,rle_bools=True)
    # TIMESTAMP_MILLIS (converted 9) outside the nanosecond range:
    # 9999-12-31 and 1000-01-01.
    write(root/'timestamp-range.parquet',[('ts',2,False,9)],
          [[[946684800000,253402214400000,946684801000,-30610224000000]]],dictionary=False)
    write(root/'empty-group.parquet',[('x',2,True,None),('s',6,False,0)],
          [[[1,None,3],['one','two','three']],[[],[]],[[4,5],['four','five']]])
    write(root/'sentinels.parquet',[('i',1,False,None),('l',2,False,None),('f',5,False,None),('s',6,False,0)],[[[-2**31,2],[-2**63,3],[float('nan'),1.],['','x']]],dictionary=False)

    for v2 in (False,True):
        for bad in (False,True):
            write(root/f'indexed-v{2 if v2 else 1}{"-bad" if bad else ""}.parquet',specs,[rows,rows],v2=v2,indexes=True,bad_index=bad,mismatched=True)
    for bad in (False,True):
        write(root/f'bloom{"-bad" if bad else ""}.parquet',specs,[rows,rows],bloom=True,bad_bloom=bad)
    write(root/'indexed-null.parquet',[('x',1,True,None),('row',2,False,None)],[[[None]*3+[4,5,6]+[None]*3,list(range(9))]],indexes=True,mismatched=True)
    # Large required PLAIN values exercise SIMD, multiple batches and worker migration.
    n=70003
    write(root/'parallel.parquet',[('x',1,False,None),('y',2,False,None),('s',6,False,0)],
          [[list(range(n)),[i*10000000001 for i in range(n)],['s'+str(i%11) for i in range(n)]]],dictionary=False)

    write(root/'projection.parquet',specs,[rows],crc=True,corrupt_column='name')
    write(root/'simd-sentinels.parquet',[('x',1,False,None),('y',2,False,None)],
          [[[(-2**31 if i%7==0 else i) for i in range(39)],[(-2**63 if i%5==0 else i) for i in range(39)]]],dictionary=False)
    # Distinct row groups, pooled strings and nulls expose ordering/merge bugs.
    starts=[0,5003,15004,22005,27006,33007,39008]
    groups=[]
    for g,lo in enumerate(starts):
        hi=starts[g+1] if g+1<len(starts) else 44009
        groups.append([list(range(lo,hi)),[None if i%13==0 else i*3 for i in range(lo,hi)],
                       ['long pooled string row '+str(i) for i in range(lo,hi)]])
    write(root/'row-groups.parquet',[('x',1,False,None),('y',2,True,None),('s',6,False,0)],groups,dictionary=False,indexes=True)
    write(root/'unix.parquet',[('day',1,False,None),('ts',2,False,None),('s',6,False,0)],
          [[[10957,10958],[946684800,946684801],['pooled string number one','pooled string number two']]],dictionary=False)
    write(root/'rle-runs.parquet',[('x',1,False,None),('y',2,False,None),('s',6,False,0)],
          [[[7]*4099+[9]*4100,[11]*4099+[13]*4100,['a repeated pooled string']*4099+['']*4100]]*3,rle_ids=True)
    for dictionary in (False,True):
        write(root/f'strict-collisions-{int(dictionary)}.parquet',
              [('i',1,False,None),('j',2,False,None),('f',5,False,None),('s',6,True,0)],
              [[[1,-2147483648,2],[1,-9223372036854775808,2],[1.,float('nan'),2.],['ok','',None]]],dictionary=dictionary)
    write(root/'strict-good.parquet',[('i',1,True,None),('s',6,True,0)],
          [[[1,None,2],['hello','\u00a2\u20ac\U0001f30d',None]]])
    for i,bad in enumerate([b'\x80',b'\xc0\x80',b'\xe0\x80\x80',b'\xed\xa0\x80',b'\xf4\x90\x80\x80',b'\xf5\x80\x80\x80',b'\xe2\x82',b'\xe2x\x80']):
        write(root/f'strict-utf8-{i}.parquet',[('s',6,False,0)],[[[bad]]],dictionary=i%2==0)
