#!/usr/bin/env python3
"""A complete HTTP response must not wait for socket close or accept truncation."""
from pathlib import Path
import socket
import socketserver
import sys
import threading
import time

from cdp import Browser


class Handler(socketserver.BaseRequestHandler):
    counts = {}
    module_started = set()
    module_lock = threading.Lock()
    all_modules_started = threading.Event()
    module_starved = False
    def handle(self):
        # Keep-alive: the connection stays open after each response, as an
        # HTTP/1.1 server's does, so a client must go by the framing alone.
        while self.serve_one():
            pass

    def serve_one(self):
        data = b''
        while b'\r\n\r\n' not in data:
            part = self.request.recv(4096)
            if not part:
                return False
            data += part
        method, path, _ = data.split(b'\r\n', 1)[0].split(b' ', 2)
        self.counts[path] = self.counts.get(path, 0) + 1
        if path == b'/missing-resource-page':
            body = b'<head><link rel="stylesheet" href="/missing.css"></head><body><h1>missing resources</h1><script src="/missing.js"></script><script type="module" src="/missing.mjs"></script></body>'
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
            return True
        if path in (b'/missing.css', b'/missing.js', b'/missing.mjs'):
            self.request.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n')
            return True
        if path in (b'/cached.css', b'/uncached.css', b'/vary.css', b'/cookie.css', b'/expired.css'):
            policy = b'public, max-age=600' if path != b'/uncached.css' else b'public, max-age=600, no-store'
            extra = b'Vary: Accept-Language\r\n' if path == b'/vary.css' else b'Set-Cookie: test=yes\r\n' if path == b'/cookie.css' else b'Age: 700\r\n' if path == b'/expired.css' else b''
            body = b'h1{color:green}'
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nCache-Control: ' + policy + b'\r\n' + extra + b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
            return True
        if path == b'/cache-page':
            body = b'<head><link rel="stylesheet" href="/cached.css"><link rel="stylesheet" href="/uncached.css"></head><body><h1>cached page</h1></body>'
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
            return True
        if path == b'/module-page' or path == b'/module-root.js' or path.startswith(b'/mod/'):
            if path == b'/module-page':
                body = b'<body><h1>modules</h1><script type="module" src="/module-root.js"></script></body>'
                mime = b'text/html'
            elif path == b'/module-root.js':
                body = ('const decoy = `from \"./missing.js\"`; /* from \"./also-missing.js\" */\n' + '\n'.join(f'import {{value as v{i}}} from "./mod/{i}.js";' for i in range(36)) + '\nglobalThis.moduleTotal=' + '+'.join(f'v{i}' for i in range(36)) + ';').encode()
                mime = b'text/javascript'
            elif path == b'/mod/leaf.js':
                body, mime = b'export const leaf = 1;', b'text/javascript'
            else:
                with self.module_lock:
                    self.module_started.add(path)
                    if len(self.module_started) == 36:
                        self.all_modules_started.set()
                # One stalled request must not keep later imports in a batch
                # that cannot start. Other workers must keep taking work.
                if path == b'/mod/0.js' and not self.all_modules_started.wait(2):
                    Handler.module_starved = True
                body, mime = b'import {leaf} from "./leaf.js"; export const value = leaf;', b'text/javascript'
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: ' + mime + b'\r\nCache-Control: public, max-age=600\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
            return True
        if path == b'/slow-image':
            time.sleep(2)
            return False
        if path in (b'/page', b'/next'):
            body = b'<body><h1>fast page</h1><img src="/slow-image"></body>' if path == b'/page' else b'<body><h1>next page</h1></body>'
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
            return True
        if path == b'/truncated':
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nx')
            return False
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
        return True


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
        def navigate(url):
            browser.events.clear()
            browser.call('Page.navigate', {'url': url})
            browser.wait_event('Page.frameStoppedLoading')

        start = time.monotonic()
        navigate(origin + '/page')
        assert time.monotonic() - start < 1, 'image blocked document navigation'
        navigate(origin + '/next')
        assert browser.eval('document.querySelector("h1").textContent') == 'next page'
        navigate(origin + '/page')
        assert browser.eval('document.querySelector("h1").textContent') == 'fast page'
        for _ in range(3):
            navigate(origin + '/cache-page')
            assert browser.eval('document.querySelector("h1").textContent') == 'cached page'
        assert Handler.counts[b'/cached.css'] == 1, Handler.counts
        assert Handler.counts[b'/uncached.css'] == 3, Handler.counts
        assert Handler.counts[b'/cache-page'] == 3, 'documents must be fetched on navigation'
        for resource in ('vary.css', 'cookie.css', 'expired.css'):
            for _ in range(2):
                assert browser.eval(f"fetch('{origin}/{resource}').then(r=>r.text())") == 'h1{color:green}'
            assert Handler.counts[('/' + resource).encode()] == 2, Handler.counts
        for _ in range(2):
            navigate(origin + '/module-page')
            assert browser.eval('moduleTotal') == 36
        assert not Handler.module_starved, 'one slow module blocked later requests from starting'
        for resource in [b'/module-root.js', b'/mod/leaf.js'] + [f'/mod/{i}.js'.encode() for i in range(36)]:
            assert Handler.counts[resource] == 1, (resource, Handler.counts[resource])
        assert Handler.counts.get(b'/missing.js', 0) == 0
        assert Handler.counts.get(b'/also-missing.js', 0) == 0
        for _ in range(2):
            navigate(origin + '/missing-resource-page')
            assert browser.eval('document.querySelector("h1").textContent') == 'missing resources'
        assert Handler.counts[b'/missing.css'] == 2, 'failed stylesheet was retried while parsing'
        assert Handler.counts[b'/missing.js'] == 2, 'failed script was retried while parsing'
        assert Handler.counts[b'/missing.mjs'] == 2, 'failed module was retried while parsing'
        # localhost commonly resolves to ::1 first, while this listener is
        # IPv4 only. The other family must remain usable.
        assert browser.eval(f"fetch('http://localhost:{server.server_address[1]}/fixed').then(r=>r.text())") == 'hello'
        print('Cross-page cache, cache exclusions and 36-module graph: PASS')
        class Server6(Server):
            address_family = socket.AF_INET6
        try:
            server6 = Server6(('::1', 0), Handler)
        except OSError as error:
            print(f'IPv6 loopback unavailable: {error}')
        else:
            with server6:
                threading.Thread(target=server6.serve_forever, daemon=True).start()
                origin6 = f'http://[::1]:{server6.server_address[1]}'
                assert browser.eval(f"fetch('{origin6}/fixed').then(r => r.text())") == 'hello'
                navigate(origin6 + '/page')
                assert browser.eval('document.querySelector("h1").textContent') == 'fast page'
                server6.shutdown()
        print('HTTP framing, IPv6 and repeated navigation: PASS')
    finally:
        browser.close()
        server.shutdown()
