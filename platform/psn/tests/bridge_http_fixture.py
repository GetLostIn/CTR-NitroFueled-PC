# SPDX-License-Identifier: GPL-2.0-or-later
"""Private loopback HTTP contract fixture. No game, Steam client or publisher."""
import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.parse

TOKEN = 'a' * 64
OWNER, SENDER = '76561198000000001', '76561198000000002'


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, code, value):
        raw = json.dumps(value).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def authorized(self):
        if self.headers.get('Authorization') != 'Bearer ' + TOKEN:
            self.reply(401, {})
            return False
        return True

    def do_GET(self):
        if not self.authorized():
            return
        if self.path == '/psn/v1/info':
            if self.server.mode == 'redirect':
                self.send_response(302)
                self.send_header('Location', 'http://192.0.2.1/unreachable')
                self.end_headers()
                return
            self.reply(200, dict(version=1, accountId=OWNER if self.server.mode != 'bad-info' else 76561198000000001,
                                 onlineId='Receiver', signedIn=True,
                                 capabilities=['identity', 'id-token', 'webapi', 'push', 'invitation-accepted']))
            return
        assert self.path.startswith('/psn/v1/events?after=')
        after = int(urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['after'][0])
        with self.server.lock:
            if self.server.failure == 'lost-cursor':
                self.reply(409, {})
                return
            if self.server.failure == 'malformed':
                self.reply(200, dict(version=1, events=[dict(sequence=after+1, event={})]))
                return
            # Deliberately replay delivered entries once. The client must deduplicate by cursor.
            result = self.server.events if after == 0 or not self.server.replayed else []
            if after:
                self.server.replayed = True
        self.reply(200, dict(version=1, events=result))

    def do_POST(self):
        if not self.authorized():
            return
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/psn/v1/token':
            assert data == dict(kind=1, client_id='client', scope='scope')
            self.reply(200, dict(status=0, issuerId=256, token='fixture-local-capability'))
            return
        assert self.path == '/psn/v1/webapi'
        path = data['path']
        if path == '/slow':
            time.sleep(3)
        if path == '/slow-complete':
            time.sleep(6)
        if path not in ('/slow',):
            assert data['bodyHex'] == '0001ff'
        if path in ('/trigger', '/overflow'):
            peer = lambda account, name: dict(accountId=account, onlineId=name)
            push = dict(version=1, kind='push', service=None, serviceLabel=0,
                        dataType='np:service:presence:onlineStatus', data='{"onlineStatus":"online"}',
                        extData=dict(fromAccountId=SENDER))
            push['from'], push['to'] = peer(SENDER, 'Sender'), peer(OWNER, 'Receiver')
            invite = dict(version=1, kind='invitationAccepted', sessionId='76561198099999999',
                          invitationId='76561198099999999-76561198000000002-1')
            invite['from'], invite['to'] = push['from'], push['to']
            with self.server.lock:
                events = [push, push, invite] if path == '/trigger' else [push] * 5
                self.server.events = [dict(sequence=n+1, event=e) for n, e in enumerate(events)]
        elif path == '/account-change':
            with self.server.lock:
                self.server.events = [dict(sequence=1, event=dict(version=1, kind='identity',
                    accountId=SENDER, onlineId='Other', signedIn=True))]
        elif path in ('/lost-cursor', '/malformed'):
            with self.server.lock:
                self.server.failure = path[1:]
        self.reply(200, dict(status=0, httpStatus=207, contentType='application/octet-stream', bodyHex='0001ff'))


def main():
    for mode in ('good', 'arbitrary-user', 'zero-user', 'overflow', 'account-change', 'lost-cursor', 'malformed', 'bad-info', 'bad-token', 'redirect', 'nonloopback'):
        server = Server(('127.0.0.1', 0), Handler)
        server.mode, server.events, server.replayed, server.failure = mode, [], False, ''
        server.lock = threading.Lock()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            host = '192.0.2.1' if mode == 'nonloopback' else '127.0.0.1'
            token = 'b' * 64 if mode == 'bad-token' else TOKEN
            subprocess.run([sys.argv[1], sys.argv[2], f'http://{host}:{server.server_port}', token, mode], check=True, timeout=15)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        print('PASS bridge HTTP', mode, flush=True)


if __name__ == '__main__':
    main()
