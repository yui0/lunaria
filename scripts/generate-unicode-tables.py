#!/usr/bin/env python3
"""Generate committed C data from Unicode 15.0 UCD.zip and Python's RFC3454 tables.
Usage: python3 scripts/generate-unicode-tables.py /path/to/UCD.zip
No Unicode library is required when building or running Lunaria.
"""
import sys, zipfile, stringprep
from pathlib import Path
z=zipfile.ZipFile(sys.argv[1])
N=0x110000
cats=('Cn','Lu','Ll','Lt','Lm','Lo','Mn','Me','Mc','Nd','Nl','No','Zs','Zl','Zp','Cc','Cf','Co','Cs','Pd','Ps','Pe','Pc','Po','Sm','Sc','Sk','So','Pi','Pf')
category=bytearray(N); flags=bytearray(N); combining=bytearray(N)
cases={}; decomps={}
def lines(name):
 for l in z.read(name).decode().splitlines():
  l=l.split('#')[0].strip()
  if l: yield [s.strip() for s in l.split(';')]
def points(s):
 r=s.split('..'); return range(int(r[0],16),int(r[-1],16)+1)
first=None
for row in lines('UnicodeData.txt'):
 cp=int(row[0],16); cat=cats.index(row[2]); ccc=int(row[3])
 if row[1].endswith(', First>'): first=cp; continue
 if row[1].endswith(', Last>'):
  for c in range(first,cp+1): category[c]=cat; combining[c]=ccc
  first=None; continue
 category[cp]=cat; combining[cp]=ccc
 upper=int(row[12],16) if row[12] else cp
 lower=int(row[13],16) if row[13] else cp
 if (upper,lower)!=(cp,cp): cases[cp]=(upper,lower)
 if row[5]:
  seq=row[5].split(); compat=seq[0].startswith('<')
  if compat: seq=seq[1:]
  decomps[cp]=(compat,[int(s,16) for s in seq])
for name in ('DerivedCoreProperties.txt','PropList.txt'):
 for row in lines(name):
  if row[1] in ('Alphabetic','Ideographic','Lowercase','Uppercase'):
   bit=('Alphabetic','Ideographic','Lowercase','Uppercase').index(row[1])
   for cp in points(row[0]): flags[cp]|=1<<bit
exclude=set()
for row in lines('DerivedNormalizationProps.txt'):
 if row[1]=='Full_Composition_Exclusion': exclude.update(points(row[0]))
compose=sorted((seq[0],seq[1],cp) for cp,(compat,seq) in decomps.items() if not compat and len(seq)==2 and cp not in exclude)
def ranges(values):
 result=[]; start=0; last=values(0)
 for cp in range(1,N):
  value=values(cp)
  if value!=last:
   if any(last):result.append((start,cp-1,*last))
   start=cp;last=value
 if any(last):result.append((start,N-1,*last))
 return result
out=['/* Generated from Unicode 15.0.0 UCD. License: LICENSE.unicode. */',
 'static const struct uc_range { uint32_t first,last; uint8_t category,flags,ccc; } uc_ranges[]={']
out+=['{%#x,%#x,%d,%d,%d},'%r for r in ranges(lambda cp:(category[cp],flags[cp],combining[cp]))];out+=['};','static const struct uc_range uc_ascii[128]={']
out+=['{%#x,%#x,%d,%d,%d},'%(cp,cp,category[cp],flags[cp],combining[cp]) for cp in range(128)];out+=['};']
out+=['static const struct uc_case { uint32_t cp,upper,lower; } uc_cases[]={']
out+=['{%#x,%#x,%#x},'%(cp,*v) for cp,v in sorted(cases.items())];out+=['};']
pool=[];rows=[]
for cp,(compat,seq) in sorted(decomps.items()):
 rows.append((cp,len(pool),len(seq),int(compat)));pool+=seq
out+=['static const uint32_t uc_decomp_data[]={'+','.join(hex(c) for c in pool)+'};',
 'static const struct uc_decomp { uint32_t cp,offset; uint8_t length,compat; } uc_decomps[]={']
out+=['{%#x,%d,%d,%d},'%r for r in rows];out+=['};',
 'static const struct uc_pair { uint32_t first,second,composed; } uc_pairs[]={']
out+=['{%#x,%#x,%#x},'%r for r in compose];out+=['};']
# RFC 3491 uses the Unicode 3.2 tables in Python's standard library.
prep=bytearray(N);mapping={}
for cp in range(N):
 c=chr(cp)
 prep[cp]=(int(stringprep.in_table_a1(c)) | (int(any(f(c) for f in (
  stringprep.in_table_c12,stringprep.in_table_c22,stringprep.in_table_c3,
  stringprep.in_table_c4,stringprep.in_table_c5,stringprep.in_table_c6,
  stringprep.in_table_c7,stringprep.in_table_c8,stringprep.in_table_c9)))<<1)
  | (int(stringprep.in_table_d1(c))<<2) | (int(stringprep.in_table_d2(c))<<3))
 if stringprep.in_table_b1(c): mapping[cp]=[]
 elif not prep[cp]&1:
  m=stringprep.map_table_b2(c)
  if m!=c:mapping[cp]=[ord(x) for x in m]
out+=['static const struct prep_range { uint32_t first,last; uint8_t flags; } prep_ranges[]={']
out+=['{%#x,%#x,%d},'%r for r in ranges(lambda cp:(prep[cp],))];out+=['};']
pool=[];rows=[]
for cp,seq in sorted(mapping.items()):rows.append((cp,len(pool),len(seq)));pool+=seq
out+=['static const uint32_t prep_data[]={'+','.join(hex(c) for c in pool)+'};',
 'static const struct prep_map { uint32_t cp,offset; uint8_t length; } prep_maps[]={']
out+=['{%#x,%d,%d},'%r for r in rows];out+=['};']
Path(__file__).resolve().parents[1].joinpath('src/unicode/character_data.h').write_text('\n'.join(out)+'\n')
