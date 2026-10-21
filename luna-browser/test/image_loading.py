#!/usr/bin/env python3
"""Late image dimensions must update without changing the CSS cascade."""
import base64
import os
from pathlib import Path
import socketserver
import sys
import threading
import time

from cdp import Browser

WEBP = base64.b64decode('UklGRhwAAABXRUJQVlA4TA8AAAAvE0ACAAcQ/Y/+ByKi/wEA')  # red, 20 x 10


class Handler(socketserver.BaseRequestHandler):
    release = threading.Event()

    def handle(self):
        request = b''
        while b'\r\n\r\n' not in request:
            part = self.request.recv(4096)
            if not part:
                return
            request += part
        path = request.split(b' ')[1]
        if path == b'/image.webp':
            self.release.wait(5)
            body, mime = WEBP, b'image/webp'
        else:
            body = b'''<style>body{margin:0}img{display:block}#percent{width:50%}
#limited{width:40px;max-width:10px}</style><body><div style="width:200px">
<img id="natural" src="/image.webp" alt="waiting">
<img id="wide" src="/image.webp" width="40">
<img id="tall" src="/image.webp" height="30">
<img id="percent" src="/image.webp"><img id="limited" src="/image.webp">
<img id="fixed" src="/image.webp" width="7" height="9"></div></body>'''
            mime = b'text/html'
        self.request.sendall(b'HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: ' + mime +
                             b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / 'luna-browser').resolve())
with Server(('127.0.0.1', 0), Handler) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    shots = []
    previous = os.environ.get('LUNA_BROWSER_SOFTWARE_WEBP')
    try:
        for software in (False, True):
            if software:
                os.environ['LUNA_BROWSER_SOFTWARE_WEBP'] = '1'
            else:
                os.environ.pop('LUNA_BROWSER_SOFTWARE_WEBP', None)
            Handler.release.clear()
            browser = Browser(binary)
            try:
                browser.open_page()
                browser.events.clear()
                browser.call('Page.navigate', {'url': f'http://127.0.0.1:{server.server_address[1]}/page'})
                browser.wait_event('Page.frameStoppedLoading')
                browser.call('Page.captureScreenshot')  # paint the still pending image
                Handler.release.set()
                expected = [[20, 10], [40, 20], [60, 30], [100, 50], [10, 5], [7, 9]]
                deadline = time.monotonic() + 5
                while True:
                    boxes = browser.eval('''["natural","wide","tall","percent","limited","fixed"].map(id=>{
                      const r=document.getElementById(id).getBoundingClientRect();return [r.width,r.height]})''')
                    if boxes == expected or time.monotonic() >= deadline:
                        break
                    time.sleep(.02)
                assert boxes == expected, (software, boxes)
                shots.append(browser.call('Page.captureScreenshot')['result']['data'])
            finally:
                Handler.release.set()
                browser.close()
        assert shots[0] == shots[1], 'native and bundled lossless WebP output differ'
    finally:
        if previous is None:
            os.environ.pop('LUNA_BROWSER_SOFTWARE_WEBP', None)
        else:
            os.environ['LUNA_BROWSER_SOFTWARE_WEBP'] = previous
        server.shutdown()
print('late image sizing and native/bundled WebP: PASS')
