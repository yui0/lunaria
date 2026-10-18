#!/usr/bin/env python3
"""Generate Java directionality ranges from Unicode 15.0 UnicodeData.txt."""
import sys
from pathlib import Path
names='L R AL EN ES ET AN CS NSM BN B S WS ON LRE LRO RLE RLO PDF LRI RLI FSI PDI'.split()
data=[-1]*0x110000
first=None
for line in Path(sys.argv[1]).read_text().splitlines():
 row=line.split(';');cp=int(row[0],16);value=names.index(row[4])
 if row[1].endswith(', First>'):first=cp;continue
 if row[1].endswith(', Last>'):
  data[first:cp+1]=[value]*(cp-first+1);first=None
 else:data[cp]=value
rows=[];start=0
for cp in range(1,len(data)+1):
 if cp==len(data) or data[cp]!=data[start]:
  if data[start]>=0:rows.append((start,cp-1,data[start]))
  start=cp
out=['/* Generated from Unicode 15.0.0 UnicodeData.txt. License: LICENSE.unicode. */',
 'static const struct uc_bidi_range { uint32_t first,last; int8_t direction; } uc_bidi_ranges[]={']
out+=['{0x%x,0x%x,%d},'%r for r in rows];out+=['};','']
(Path(__file__).resolve().parents[1]/'src/unicode/bidi_data.h').write_text('\n'.join(out))
