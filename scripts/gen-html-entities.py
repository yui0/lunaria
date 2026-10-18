#!/usr/bin/env python3
"""Regenerate the HTML4 named-entity table embedded in luna-ui.h."""
from html.entities import name2codepoint
from pathlib import Path
import re
entities = dict(name2codepoint, apos=39)
p = Path(__file__).resolve().parents[1] / 'src/luna-ui/luna-ui.h'
block = ('/* BEGIN generated html entities (scripts/gen-html-entities.py; HTML4 names) */\n'
         'static const struct {const char *name; unsigned value;} lh_entities[]={\n'
         + ''.join(' {"%s",0x%x},\n' % item for item in sorted(entities.items())) + '''};
static inline unsigned luna_html_named_entity(const char *name, size_t length)
{
 size_t lo=0,hi=sizeof lh_entities/sizeof *lh_entities;
 while(lo<hi){size_t mid=lo+(hi-lo)/2,n=strlen(lh_entities[mid].name);
  int c=memcmp(name,lh_entities[mid].name,length<n?length:n);
  if(!c)c=length<n?-1:length>n?1:0;
  if(!c) {return lh_entities[mid].value;}
  if(c<0)hi=mid;else lo=mid+1;
 }return 0;
}
/* END generated html entities */''')
s = p.read_text()
s, n = re.subn(r'/\* BEGIN generated html entities.*?/\* END generated html entities \*/',
               lambda m: block, s, flags=re.S)
assert n == 1
p.write_text(s)
