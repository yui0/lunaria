#!/usr/bin/env python3
"""Text fields: selection (keys, mouse, double click), clipboard, context menu."""
from pathlib import Path
import base64, sys, tempfile, signal, time

from cdp import Browser

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / 'luna-browser').resolve())
signal.alarm(90)
shots = sys.argv[2] if len(sys.argv) > 2 else None

with tempfile.TemporaryDirectory(prefix='lunaria-edit-') as directory:
    page = Path(directory) / 'index.html'
    page.write_text('''<!doctype html><body style="margin:20px;font:16px sans-serif">
<input id=a value="hello brave new world" style="width:300px;display:block;margin-bottom:10px">
<textarea id=t style="width:300px;height:80px;display:block">line one
line two</textarea></body>''')
    b = Browser(binary)
    try:
        b.open_page()
        b.call('Page.navigate', {'url': page.as_uri()})
        val = lambda i: b.eval(f"document.getElementById('{i}').value")
        def mouse(t, x, y, button='left', mods=0, clicks=1):
            b.call('Input.dispatchMouseEvent', dict(type=t, x=x, y=y, button=button, modifiers=mods, clickCount=clicks))
        def key(k, mods=0):
            b.call('Input.dispatchKeyEvent', dict(type='rawKeyDown', key=k, modifiers=mods))
            b.call('Input.dispatchKeyEvent', dict(type='keyUp', key=k, modifiers=mods))
        def shot(name):
            r = b.call('Page.captureScreenshot', {'format': 'png'})     # also draws a frame
            if shots:
                Path(shots, name).write_bytes(base64.b64decode(r['result']['data']))
        def click(x, y, **kw):
            mouse('mousePressed', x, y, **kw); mouse('mouseReleased', x, y, **kw)
        r = b.eval("JSON.stringify(document.getElementById('a').getBoundingClientRect())")
        import json; ra = json.loads(r)
        y = ra['top'] + ra['height'] / 2
        # click then Ctrl+A, then type replaces the whole selection
        click(ra['left'] + 20, y)
        key('a', 2); shot('select_all.png')
        b.call('Input.insertText', {'text': 'X'})
        assert val('a') == 'X', val('a')
        # shift+arrows extend; typing replaces; Home/End
        b.call('Input.insertText', {'text': 'yz'})
        key('Home'); key('ArrowRight', 8); key('ArrowRight', 8)
        b.call('Input.insertText', {'text': 'Q'})
        assert val('a') == 'Qz', val('a')
        # word selection with a double click, delete it
        b.eval("document.getElementById('a').value='hello brave new world'")
        click(ra['left'] + 60, y); mouse('mousePressed', ra['left'] + 60, y, clicks=2); mouse('mouseReleased', ra['left'] + 60, y, clicks=2)
        shot('word.png')
        key('Backspace')
        assert val('a') == 'hello  new world', val('a')
        # drag selection, cut, paste elsewhere
        b.eval("document.getElementById('a').value='abcdef'")
        time.sleep(0.6)
        mouse('mousePressed', ra['left'] + 2, y); mouse('mouseMoved', ra['left'] + 30, y); mouse('mouseReleased', ra['left'] + 30, y)
        key('x', 2)
        left = val('a'); assert 0 < len(left) < 6, left
        key('End'); key('v', 2)   # paste goes through the window in the real browser; here the page's own clipboard
        # context menu: right click, screenshot, Escape closes
        b.eval("document.getElementById('a').value='right click me'")
        mouse('mousePressed', ra['left'] + 20, y, button='right'); mouse('mouseReleased', ra['left'] + 20, y, button='right')
        shot('menu.png')
        key('Escape')
        # textarea: selection across lines
        rt = json.loads(b.eval("JSON.stringify(document.getElementById('t').getBoundingClientRect())"))
        click(rt['left'] + 10, rt['top'] + 10); key('a', 2); shot('textarea.png')
        key('Delete')
        assert val('t') == '', val('t')
        # undo / redo
        b.eval("document.getElementById('a').value='start'")
        click(ra['left'] + 100, y)
        b.call('Input.insertText', {'text': 'A'}); b.call('Input.insertText', {'text': 'B'})
        key('Backspace')
        assert val('a') == 'startA', val('a')
        key('z', 2); assert val('a') == 'startAB', val('a')
        key('z', 2); assert val('a') == 'start', val('a')
        key('z', 10); assert val('a') == 'startAB', val('a')      # Ctrl+Shift+Z redoes
        # page text: drag-select across paragraphs, copy
        b.eval("document.body.insertAdjacentHTML('beforeend', '<p id=p1 style=\"margin:0\">alpha beta gamma</p><p id=p2 style=\"margin:0\">delta epsilon</p>')")
        shot('page.png')
        rp = json.loads(b.eval("JSON.stringify(document.getElementById('p1').getBoundingClientRect())"))
        r2 = json.loads(b.eval("JSON.stringify(document.getElementById('p2').getBoundingClientRect())"))
        time.sleep(0.6)
        mouse('mousePressed', rp['left'] + 40, rp['top'] + 8)
        mouse('mouseMoved', r2['left'] + 40, r2['top'] + 8)
        mouse('mouseReleased', r2['left'] + 40, r2['top'] + 8)
        shot('page_sel.png')
        key('c', 2)
        got = b.eval("''")  # copied text is reported to the embedder as Lunaria.clipboard
        ev = [e for e in b.events if e.get('method') == 'Lunaria.clipboard']
        assert ev, 'no clipboard event'
        txt = ev[-1]['params']['text']
        assert 'beta' in txt and 'delta' in txt and '\n' in txt, repr(txt)
        # find in page and the link under the pointer
        b.call('Lunaria.find', dict(text='epsilon', forward=1, restart=1))
        assert b.wait_event('Lunaria.findResult')['params']['found'] == 1
        b.call('Lunaria.find', dict(text='no such text', forward=1, restart=1))
        assert b.wait_event('Lunaria.findResult')['params']['found'] == 0
    finally:
        b.close()
print('text editing: PASS')
