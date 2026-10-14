#!/usr/bin/env python3
"""A complete HTTP response must not wait for socket close or accept truncation."""
from pathlib import Path
import socketserver
import sys
import threading
import time

from cdp import Browser


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        data = b''
        while b'\r\n\r\n' not in data:
            part = self.request.recv(4096)
            if not part:
                return
            data += part
        method, path, _ = data.split(b'\r\n', 1)[0].split(b' ', 2)
        if path == b'/truncated':
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nx')
            return
        if path == b'/chunked':
            pieces = [b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',
                      b'2;ext=yes\r\nhe\r\n', b'3\r\nllo\r\n',
                      b'0\r\nX-Trailer: yes\r\n', b'\r\n']
        elif path == b'/no-content':
            pieces = [b'HTTP/1.1 204 No Content\r\n\r\n']
        else:
            pieces = [b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n']
            if path == b'/early':
                pieces.insert(0, b'HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n')
            if method != b'HEAD':
                pieces.append(b'hello')
        for piece in pieces:
            self.request.sendall(piece)
            time.sleep(.005)
        time.sleep(2)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path(__file__).parent.parent / 'luna-browser').resolve())
with Server(('127.0.0.1', 0), Handler) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    browser = Browser(binary)
    try:
        browser.open_page()
        origin = f'http://127.0.0.1:{server.server_address[1]}'
        for path, method, expected in [('/fixed', 'GET', 'hello'),
                                       ('/fixed', 'HEAD', ''),
                                       ('/early', 'GET', 'hello'),
                                       ('/chunked', 'GET', 'hello'),
                                       ('/no-content', 'GET', '')]:
            start = time.monotonic()
            result = browser.eval(f"fetch('{origin}{path}', {{method:'{method}'}}).then(r => r.text())")
            elapsed = time.monotonic() - start
            print(f'{method} {path}: {elapsed:.3f}s')
            assert result == expected, (path, method, result)
            assert elapsed < 1, (path, method, elapsed)
        assert browser.eval(f"fetch('{origin}/truncated').then(() => false, () => true)") is True
        print('HTTP framing: PASS')
    finally:
        browser.close()
        server.shutdown()
