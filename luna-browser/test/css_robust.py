#!/usr/bin/env python3
"""CSS/HTML robustness: nested at-rules must terminate, bare attribute values and
attribute selectors must work, and a stray token in a block must not stall it."""
from pathlib import Path
import sys
import tempfile
import signal

from cdp import Browser

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / 'luna-browser').resolve())
signal.alarm(60)                      # a stalled parser is a failure, not a hang
with tempfile.TemporaryDirectory(prefix='lunaria-css-') as directory:
    page = Path(directory) / 'index.html'
    page.write_text('''<!doctype html><html><head><style>
div{display:none}
@supports (display:grid){.x{color:red}@media (width>=1px){#nested{display:block}}}
@layer base{#layered{display:block}}
#bare{display:block}
#attr_eq[data-k=v]{display:block} [hidden]{display:none!important} #hid{display:block}
#stray{display:block;&:hover{color:red};.q{display:none}}
#after{display:block}
</style></head><body>
<div id=nested>n</div><div id=layered>l</div><div id=bare class=a>b</div>
<div id=attr_eq data-k=v>e</div><div id=hid hidden>h</div><div id=stray>s</div><div id=after>a</div>
</body></html>''')
    page2 = Path(directory) / 'modern.html'
    page2.write_text('''<!doctype html><html><head><style>
div{display:none} :is(#a1,#a2){display:block} #h:has(> b){display:block}
.card{display:block} .card{ .in{display:block} & + &{display:block} }
#nt:not(.x,.y){display:block}
table{border-spacing:0} td{padding:0}
</style></head><body>
<div id=a1>1</div><div id=a2>2</div><div id=a3>3</div><div id=h><b>b</b></div>
<div class=card><div class=in>in</div></div><div id=nt class=y>no</div>
<table id=tb width=300><tr><td id=c11 width=100>a<td id=c12>bb<tr><td id=c21>c<td id=c22>d</table>
</body></html>''')
    browser = Browser(binary)
    try:
        browser.open_page()
        browser.call('Page.navigate', {'url': page.as_uri()})
        shown = browser.eval('''Array.from(document.querySelectorAll("div"))
          .filter(d => d.getBoundingClientRect().height > 0).map(d => d.id)''')
        assert shown == ['nested', 'layered', 'bare', 'attr_eq', 'stray', 'after'], shown
        browser.call('Page.navigate', {'url': page2.as_uri()})
        shown = browser.eval('''Array.from(document.querySelectorAll("div"))
          .filter(d => d.getBoundingClientRect().height > 0).map(d => d.id || d.className)''')
        assert shown == ['a1', 'a2', 'h', 'card', 'in'], shown
        assert browser.eval("""(() => {
          const a=document.createElement('style');
          a.textContent='#a1{height:33px}'; document.head.appendChild(a);
          const b=document.createElement('style');
          b.textContent='#a2{height:47px}'; document.head.appendChild(b);
          return document.getElementById('a1').offsetHeight===33 && document.getElementById('a2').offsetHeight===47;
        })()""") is True, 'geometry must flush batched stylesheet updates'
        cells = browser.eval('''["c11","c12","c21","c22"].map(i => {const r=document.getElementById(i).getBoundingClientRect(); return [r.left, r.top, r.width]})''')
        assert cells[0][0] == cells[2][0] and cells[1][0] == cells[3][0], cells   # columns line up
        assert cells[0][1] == cells[1][1] and cells[2][1] > cells[0][1], cells   # rows
        assert abs(cells[0][2] - 100) < 1 and abs(cells[0][2] + cells[1][2] - 300) < 1, cells
        memo = Path(directory) / 'ancestor-sweep.html'
        memo.write_text("""<body class="scope leaf"><style>
.leaf{height:10px}.scope .leaf{height:31px}.scope .leaf+.leaf{height:35px}
.scope .branch>.leaf{height:43px}
</style><div id="first" class="leaf"></div><div id="second" class="leaf"></div>
<section class="branch"><div id="nested" class="leaf"></div></section></body>""")
        browser.call('Page.navigate', {'url': memo.as_uri()})
        def heights():
            return browser.eval("[document.body, ...['first','second','nested'].map(id=>document.getElementById(id))].map(n=>n.offsetHeight)")
        assert heights()[1:] == [31,35,43], heights()
        browser.eval("document.body.className='leaf'")
        assert heights()[1:] == [10,10,10], heights()
        browser.eval("document.body.className='scope leaf'")
        assert heights()[1:] == [31,35,43], heights()
        compound = Path(directory) / 'compound.html'
        compound.write_text('''<head><style>
div.card{--size:21px;height:var(--size)}span.card{display:block;height:7px}
section .card{--size:29px}#target.card{height:var(--size)}
div.card.active{--size:37px}div.card::before{content:'x';display:block;height:3px}
</style></head><body><section><div id="target" class="card"></div></section>
<span class="card" id="other">Other</span></body>''')
        browser.call('Page.navigate', {'url': compound.as_uri()})
        assert browser.eval('document.getElementById("target").offsetHeight') == 29
        assert browser.eval('document.getElementById("other").offsetHeight') == 7
        browser.eval('document.getElementById("target").classList.add("active")')
        assert browser.eval('document.getElementById("target").offsetHeight') == 37
        browser.eval('document.getElementById("target").className=""')
        assert browser.eval('document.getElementById("target").offsetHeight') != 37
        cascade = Path(directory) / 'many-sheets.html'
        cascade.write_text('<head><style>#pinned{height:13px!important}</style>' +
                           ''.join(f'<style>div.shared{{--height:{i}px;height:var(--height)}}</style>'
                                   for i in range(1, 41)) +
                           '</head><body><div id="ordered" class="shared"></div>'
                           '<div id="pinned" class="shared"></div></body>')
        browser.call('Page.navigate', {'url': cascade.as_uri()})
        assert browser.eval('document.getElementById("ordered").offsetHeight') == 40
        assert browser.eval('document.getElementById("pinned").offsetHeight') == 13
        browser.eval('''const sheet=document.createElement('style');
          sheet.textContent='#pinned{height:17px!important}div.shared{height:43px}';
          document.head.appendChild(sheet)''')
        assert browser.eval('document.getElementById("ordered").offsetHeight') == 43
        assert browser.eval('document.getElementById("pinned").offsetHeight') == 17
    finally:
        browser.close()
print('css robustness: PASS')
