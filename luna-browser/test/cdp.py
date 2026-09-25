#!/usr/bin/env python3
"""Minimal CDP pipe client for luna-browser (or Chrome) tests.

    cdp.py BROWSER 'Method {json params}' ...   prints each reply
"""
import json, os, subprocess, sys

class Browser:
    def __init__(self, binary, extra=()):
        import fcntl
        hi = lambda fd: (fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 10), os.close(fd))[0]
        to_r, to_w = map(hi, os.pipe())
        from_r, from_w = map(hi, os.pipe())
        def child():
            # above 10 first, so dup2 onto 3/4 cannot clobber a pipe end
            os.dup2(to_r, 3); os.dup2(from_w, 4)
        self.p = subprocess.Popen([binary, '--headless', '--remote-debugging-pipe', *extra],
                                  preexec_fn=child, close_fds=False)
        os.close(to_r); os.close(from_w)
        self.w, self.r = to_w, from_r
        self.buf = b''
        self.id = 0
        self.session = None
        self.events = []

    def recv(self):
        while b'\0' not in self.buf:
            chunk = os.read(self.r, 1 << 20)
            if not chunk:
                raise EOFError('browser closed the pipe')
            self.buf += chunk
        msg, self.buf = self.buf.split(b'\0', 1)
        return json.loads(msg)

    def call(self, method, params=None, session=True):
        self.id += 1
        m = {'id': self.id, 'method': method, 'params': params or {}}
        if session and self.session:
            m['sessionId'] = self.session
        os.write(self.w, json.dumps(m).encode() + b'\0')
        while True:
            r = self.recv()
            if r.get('id') == self.id:
                return r
            self.events.append(r)

    def wait_event(self, name):
        for i, e in enumerate(self.events):
            if e.get('method') == name:
                return self.events.pop(i)
        while True:
            r = self.recv()
            if r.get('method') == name:
                return r
            self.events.append(r)

    def open_page(self):
        t = self.call('Target.createTarget', {'url': 'about:blank'}, False)['result']['targetId']
        self.session = self.call('Target.attachToTarget', {'targetId': t, 'flatten': True}, False)['result']['sessionId']
        self.call('Page.enable')

    def eval(self, expr):
        r = self.call('Runtime.evaluate', {'expression': expr, 'returnByValue': True, 'awaitPromise': True})
        return r['result'].get('result', {}).get('value')

    def close(self):
        os.close(self.w)
        try: self.p.wait(timeout=10)
        except subprocess.TimeoutExpired: self.p.kill()

    def __del__(self):
        if self.p.poll() is None: self.p.kill()

if __name__ == '__main__':
    b = Browser(sys.argv[1])
    b.open_page()
    for arg in sys.argv[2:]:
        method, _, params = arg.partition(' ')
        print(json.dumps(b.call(method, json.loads(params) if params else {}))[:2000])
    b.close()
