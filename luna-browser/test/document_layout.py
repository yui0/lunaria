#!/usr/bin/env python3
"""Readable document flow: omitted end tags, floats, viewport scroll and links."""
from pathlib import Path
import sys
import tempfile

from cdp import Browser

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / 'luna-browser').resolve())
with tempfile.TemporaryDirectory(prefix='lunaria-document-') as directory:
    page = Path(directory) / 'index.html'
    destination = Path(directory) / 'next.html'
    destination.write_text('<body><h1 id="arrived">Link destination</h1></body>')
    page.write_text('''<!doctype html><html><head><style>
body{margin:0} #columns{width:500px;margin:0 auto}
#sidebar{float:right;width:150px;height:200px}
#article{float:left;width:300px;height:400px}
#footer{clear:both;height:40px} #fixed{position:fixed;top:0;right:0}
</style></head><body>
<div id="columns"><header style="height:60px">Header</header>
<aside id="sidebar">Sidebar</aside><main id="article">Article</main>
<footer id="footer">Footer</footer></div>
<ul id="list"><li id="one">One<li id="two">Two<li id="three">Three</ul>
<p id="para">Paragraph<div id="block">Block</div></p>
<table id="table"><tbody><tr><td id="a">A<td id="b">B<tr><td id="c">C</table>
<a id="link" href="next.html">Follow link</a>
<div style="height:1600px">Long document</div><p id="bottom">End of document</p>
<div id="fixed"><span id="fixed-child">Fixed</span></div>
</body></html>''')
    browser = Browser(binary)
    try:
        browser.open_page()
        browser.call('Page.navigate', {'url': page.as_uri()})
        tree = browser.eval('''({
          list: Array.from(document.getElementById('list').children).map(n=>n.id),
          block: document.getElementById('block').parentNode === document.body,
          cells: Array.from(document.getElementById('table').querySelectorAll('tr')).map(n=>n.children.length),
          end: document.getElementById('bottom').parentNode === document.body
        })''')
        assert tree == dict(list=['one', 'two', 'three'], block=True, cells=[2, 1], end=True), tree
        assert browser.eval('''(() => {
          const box=id=>document.getElementById(id).getBoundingClientRect();
          const p=box('columns'), s=box('sidebar'), a=box('article'), f=box('footer');
          return Math.abs(p.left-(innerWidth-500)/2)<1 && Math.abs(s.top-a.top)<1 &&
                 a.right<=s.left && Math.abs(s.right-p.right)<1 && f.top>=a.bottom &&
                 Math.abs(p.bottom-f.bottom)<1;
        })()''') is True
        assert browser.eval("document.body.offsetParent === null && document.getElementById('article').offsetParent === document.body && document.getElementById('fixed').offsetParent === null") is True
        assert browser.eval("(() => {let n=document.getElementById('article'), count=0; while(n && count<20) {n=n.offsetParent;count++;} return n===null && count===2;})()") is True
        initial = browser.eval("document.getElementById('bottom').getBoundingClientRect().top")
        browser.eval('window.scrollTo(0, 500)')
        assert browser.eval('''(() => {const fixed=document.getElementById('fixed-child').getBoundingClientRect();
          return scrollY===500 && pageYOffset===500 && document.scrollingElement.scrollTop===500 && Math.abs(fixed.top)<1;})()''') is True
        moved = browser.eval("document.getElementById('bottom').getBoundingClientRect().top")
        assert abs(initial - moved - 500) < 1, (initial, moved)
        browser.call('Input.dispatchMouseEvent', dict(type='mouseWheel', x=300, y=300, deltaX=0, deltaY=120))
        assert browser.eval('scrollY') == 620
        browser.eval('window.scrollTo(0, 1e9)')
        assert browser.eval("document.getElementById('bottom').getBoundingClientRect().bottom <= innerHeight") is True
        browser.eval("document.getElementById('link').scrollIntoView({block:'nearest'})")
        point = browser.eval("(() => {const r=document.getElementById('link').getBoundingClientRect();return {x:r.left+2,y:r.top+2}})()")
        for kind in ['mousePressed', 'mouseReleased']:
            browser.call('Input.dispatchMouseEvent', dict(type=kind, button='left', clickCount=1, **point))
        assert browser.eval("!!document.getElementById('arrived')") is True
        flex = Path(directory) / 'intrinsic-flex.html'
        flex.write_text("""<body style="margin:0">
<style>
#outer{display:flex;flex-direction:column;align-items:center;width:600px}
#auto{display:flex;flex-direction:column}
#percent{width:100%;max-width:none}
#heading{max-width:320px;margin:0;font-size:32px}
#half{width:50%}
</style><div id="outer"><section id="auto"><div id="percent"><h1 id="heading">Content must remain readable</h1></div></section></div>
<div style="display:flex;flex-direction:column;align-items:center;width:600px"><div id="auto-half"><div id="half">Percentage child with intrinsic text</div></div></div>
</body>""")
        browser.call('Page.navigate', {'url': flex.as_uri()})
        boxes = browser.eval("""['outer','auto','percent','heading','auto-half','half'].map(id=>{
          const r=document.getElementById(id).getBoundingClientRect();return [r.left,r.width,r.height];
        })""")
        assert 100 < boxes[1][1] <= 600, boxes
        assert abs(boxes[1][1]-boxes[2][1]) < 1 and boxes[3][1] > 100, boxes
        assert abs(boxes[1][0]-(600-boxes[1][1])/2) < 1, boxes
        assert boxes[4][1] > 50 and abs(boxes[5][1]*2-boxes[4][1]) < 1, boxes
        grid = Path(directory) / 'repeat-grid.html'
        grid.write_text("""<body style="margin:0"><style>
#grid{display:grid;width:600px;grid-template-columns:repeat(12,minmax(0,1fr))}
#grid>div{height:20px}#full{grid-column:auto / span 12}
#left{grid-column:span 6}#right{grid-column:auto / span 6}
#mixed{display:grid;width:600px;grid-template-columns:repeat(2,50px 1fr)}
#mixed>div{height:10px}
</style><div id="grid"><div id="full">Full</div><div id="left">Left</div><div id="right">Right</div></div>
<div id="mixed"><div>A</div><div>B</div><div>C</div><div>D</div></div></body>""")
        browser.call('Page.navigate', {'url': grid.as_uri()})
        boxes = browser.eval("['full','left','right'].map(id=>{const r=document.getElementById(id).getBoundingClientRect();return [r.left,r.top,r.width]})")
        assert boxes == [[0,0,600],[0,20,300],[300,20,300]], boxes
        widths = browser.eval("Array.from(document.getElementById('mixed').children).map(n=>n.offsetWidth)")
        assert widths == [50,250,50,250], widths
        # The same relative image path belongs to a different base after a
        # navigation. Compare painted frames, not just DOM attributes.
        shots = []
        for name, color in [('red', '#ff0000'), ('green', '#00ff00'), ('red', '#ff0000')]:
            folder = Path(directory) / name
            folder.mkdir(exist_ok=True)
            (folder / 'image.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80"><rect width="80" height="80" fill="{color}"/></svg>')
            image_page = folder / 'index.html'
            image_page.write_text('<head><title>Document title</title></head><body style="margin:0"><svg width="0" height="0"><title>Icon label</title></svg><div style="width:80px;height:80px;background-image:url(image.svg)"></div></body>')
            browser.call('Page.navigate', {'url': image_page.as_uri()})
            assert browser.eval('document.title') == 'Document title'
            shots.append(browser.call('Page.captureScreenshot')['result']['data'])
        assert shots[0] != shots[1], 'previous document texture was reused'
        assert shots[0] == shots[2], 'return navigation did not restore the image'
        print('document layout, percentage intrinsic sizing and 12-column grids: PASS')
    finally:
        browser.close()
