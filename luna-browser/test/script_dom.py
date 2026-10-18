#!/usr/bin/env python3
"""Parser scripts retain DOM identity, raw source and execution semantics."""
from pathlib import Path
import sys
import tempfile

from cdp import Browser

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / "luna-browser").resolve())
source = 'x' * 1024 + ' &amp; <tag> </scriptx>'
with tempfile.TemporaryDirectory(prefix="lunaria-script-dom-") as directory:
    page = Path(directory) / "index.html"
    page.write_text('''<!doctype html><html><head>
<title>Script &amp; DOM</title><style id="rules">.unused{display:none}</style>
<script id="head-data" type="application/json">{}</script></head><body>
<section id="container"><div id="before"></div>
<script id="vconsole" type="application/json">''' + source + '''</script>
<div id="after"></div><script id="main">
window.sentinel = '</scriptx>';
window.executed = (window.executed || 0) + 1;
window.current = document.currentScript.id;
const node = document.getElementById('vconsole');
window.raw = node.textContent;
window.rawHTML = node.innerHTML;
window.order = Array.from(node.parentNode.children).map(n => n.id);
window.parent = node.parentNode.id;
node.parentNode.removeChild(node);
</script></section></body></html>''')
    browser = Browser(binary)
    try:
        browser.open_page()
        browser.call('Page.navigate', {'url': page.as_uri()})
        actual = browser.eval('''({executed, current, raw, rawHTML, order, parent,
          removed: document.getElementById('vconsole') === null,
          head: document.getElementById('head-data').parentNode === document.head,
          connected: document.documentElement.contains(document.head) &&
                     document.documentElement.contains(document.body),
          scripts: document.scripts.length, title: document.title, sentinel,
          rawBox: document.getElementById('head-data').getBoundingClientRect().width
        })''')
        assert actual == dict(executed=1, current='main', raw=source,
                             rawHTML=source, order=['before', 'vconsole', 'after', 'main'],
                             parent='container', removed=True, head=True,
                             connected=True, scripts=2, title='Script & DOM',
                             sentinel='</scriptx>', rawBox=0), actual
        assert browser.eval('''(() => {
          const holder = document.createElement('div');
          holder.innerHTML = '<script id="inert">window.executed += 100</' + 'script>';
          document.body.appendChild(holder);
          const s = document.createElement('script');
          s.textContent = 'window.executed += 1';
          document.head.appendChild(s);
          s.remove(); document.head.appendChild(s);
          const data = document.getElementById('head-data');
          data.textContent = 'z'.repeat(1200) + '&amp;<tag>';
          const textOK = data.textContent.length === 1210 && data.innerHTML === data.textContent;
          data.innerHTML = 'a'.repeat(1300) + '&amp;<tag>';
          return executed === 2 && textOK && data.textContent.length === 1310 &&
                 holder.firstChild.textContent === 'window.executed += 100';
        })()''') is True
        written = Path(directory) / 'written.html'
        written.write_text('''<body><div id="box"><script id="writer">document.write('<span id="first">One</span>');document.write('<span id="second">Two</span>');</script><p id="next">Next</p></div><iframe><script>window.fallbackRan=true;</script></iframe><noscript><script>window.fallbackRan=true;</script></noscript><input id="hidden" type="hidden" value="secret"></body>''')
        browser.call('Page.navigate', {'url': written.as_uri()})
        assert browser.eval("JSON.stringify(Array.from(document.getElementById('box').children).map(n=>n.id))") == '["writer","first","second","next"]'
        assert browser.eval("typeof fallbackRan === 'undefined' && document.getElementById('hidden').getBoundingClientRect().width === 0 && onload === null") is True
        before = browser.call('Page.captureScreenshot')['result']['data']
        browser.eval("window.detached = document.createElement('div'); detached.style='position:fixed;top:0;left:0;width:400px;height:400px;background:#ff00ff'; detached.textContent='Detached';")
        after = browser.call('Page.captureScreenshot')['result']['data']
        assert before == after, 'detached DOM nodes must not paint'
        written.write_text('<body><div id="box"><script id="writer">document.write("Leading text<br><span id=written>Inline</span>");</script><p id="next">Next</p></div></body>')
        browser.call('Page.navigate', {'url': written.as_uri()})
        assert browser.eval("document.getElementById('writer').nextSibling.textContent") == 'Leading text'
        assert browser.eval("document.getElementById('box').textContent.includes('Leading text')") is True
        assert browser.eval("""new Promise(resolve => {
          const order = [];
          setTimeout(() => { const end = performance.now() + 20; while (performance.now() < end) {} order.push('slow'); }, 0);
          setTimeout(() => order.push('next'), 0);
          requestAnimationFrame(() => order.push('paint'));
          setTimeout(() => resolve(order), 80);
        })""") == ['slow', 'paint', 'next'], 'timers must yield to rendering'
        assert browser.eval("""(() => {
          let built = 0, attached = 0;
          class NavigationFrame extends HTMLElement {
            constructor() { super(); built++; this.delegate = { ready: true }; }
            connectedCallback() { attached++; }
          }
          customElements.define('navigation-frame', NavigationFrame);
          const a = document.createElement('navigation-frame');
          const b = new NavigationFrame();
          if (built !== 2 || attached || !a.delegate.ready || !b.delegate.ready) return false;
          document.body.appendChild(a);
          return built === 2 && attached === 1 && a instanceof NavigationFrame;
        })()""") is True, 'custom element constructors must run before insertion'
        assert browser.eval("""(() => {
          const style = document.createElement('style');
          style.setAttribute('data-query-check', 'yes');
          style.textContent = 'body{color:rgb(10,20,30)}';
          document.head.appendChild(style);
          const found = document.querySelectorAll('style[data-query-check="yes"]');
          style.remove();
          return found.length === 1 && found[0] === style;
        })()""") is True, 'native head queries must retain virtual style nodes'
        assert browser.eval("""(() => {
          history.replaceState(null, '', '#n%65xt');
          const target = document.querySelector(':target');
          const ok = target && target.id === 'next' && document.querySelectorAll(':target').length === 1;
          history.replaceState(null, '', '#missing');
          return ok && document.querySelector(':target') === null;
        })()""") is True, 'fragment targets must accept percent-encoded IDs'
        print('script DOM: PASS')
    finally:
        browser.close()
