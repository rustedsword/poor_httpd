#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Run poor_httpd on a temporary directory and check what it serves.
#
# Usage: test_httpd.py POOR_HTTPD TEST

import hashlib
import http.client
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

EXE = sys.argv[1]
BIG_SIZE = 5 << 20


class Server:
    """
    A server on a temporary directory. By default it gets its listening socket through socket activation, so the
    port is known up front and there is no race for it.
    """

    def __init__(self, *args, listen=None, big=False):
        self.dir = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.dir.name, 'root')
        make_tree(self.dir.name, self.root, big)
        self.log_path = os.path.join(self.dir.name, 'log')
        log = open(self.log_path, 'wb')
        if listen:
            self.port = listen
            self.proc = subprocess.Popen([EXE, *args, self.root], stdout=log, stderr=log)
            wait_for_port(listen)
        else:
            sock = socket.create_server(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
            fd = sock.fileno()
            move = f'exec 3<&{fd} {fd}<&-; ' if fd != 3 else ''
            script = move + 'export LISTEN_PID=$$ LISTEN_FDS=1; exec "$0" "$@"'
            self.proc = subprocess.Popen(['sh', '-c', script, EXE, *args, self.root], pass_fds=(fd,), stdout=log,
                                         stderr=log)
            sock.close()
        log.close()

    def connect(self, timeout=10):
        return socket.create_connection(('127.0.0.1', self.port), timeout=timeout)

    def request(self, path, headers=None, method='GET'):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        conn.request(method, path, headers=headers or {})
        response = conn.getresponse()
        body = response.read()
        conn.close()
        return response, body

    def raw(self, data):
        """Send 'data' and read until the server closes the connection"""
        with self.connect() as sock:
            sock.sendall(data)
            return read_all(sock)

    def log(self):
        with open(self.log_path) as f:
            return f.read()

    def stop(self, status=0):
        self.proc.send_signal(signal.SIGTERM)
        assert self.proc.wait(timeout=30) == status, self.log()
        return self.log()


def make_tree(base, root, big):
    files = {
        'hello.txt': b'hello\n',
        'index.html': b'<h1>poor_httpd</h1>\n',
        'sub/index.html': b'sub\n',
        'sub/page.html': b'page\n',
        'empty/.keep': b'',
        'nested/index.html/.keep': b'',
        'zero.bin': b'',
    }
    for path, data in files.items():
        os.makedirs(os.path.dirname(os.path.join(root, path)), exist_ok=True)
        with open(os.path.join(root, path), 'wb') as f:
            f.write(data)
    if big:
        with open(os.path.join(root, 'big.bin'), 'wb') as f:
            f.write(os.urandom(BIG_SIZE))
    with open(os.path.join(base, 'secret.txt'), 'wb') as f:
        f.write(b'secret\n')
    os.symlink('hello.txt', os.path.join(root, 'link.txt'))
    os.symlink(os.path.join(base, 'secret.txt'), os.path.join(root, 'absolute.txt'))
    os.symlink('..', os.path.join(root, 'up'))
    os.mkfifo(os.path.join(root, 'fifo'))


def wait_for_port(port):
    for _ in range(100):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.05)
    raise AssertionError(f'nothing listens on port {port}')


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def read_all(sock):
    data = b''
    while chunk := sock.recv(65536):
        data += chunk
    return data


def parse(data, heads=()):
    """Split a stream of responses; the ones at the indexes in 'heads' answer HEAD requests and have no body"""
    responses = []
    while data:
        head, _, data = data.partition(b'\r\n\r\n')
        lines = head.decode('latin-1').split('\r\n')
        headers = dict(line.split(': ', 1) for line in lines[1:])
        status = int(lines[0].split()[1])
        length = 0 if len(responses) in heads or status == 304 else int(headers.get('Content-Length', 0))
        responses.append((status, headers, data[:length]))
        data = data[length:]
    return responses


def read_response(sock):
    data = b''
    while b'\r\n\r\n' not in data:
        data += sock.recv(65536)
    length = int(re.search(rb'Content-Length: (\d+)', data).group(1))
    while len(data) - data.index(b'\r\n\r\n') - 4 < length:
        data += sock.recv(65536)
    return parse(data)[0]


def get(path, close=True, version='1.1'):
    return (f'GET {path} HTTP/{version}\r\nHost: test\r\n' + ('Connection: close\r\n' if close else '') + '\r\n').encode()


def test_get():
    s = Server()
    response, body = s.request('/hello.txt')
    assert response.status == 200 and body == b'hello\n'
    assert response.getheader('Content-Type') == 'text/plain; charset=utf-8'
    assert response.getheader('Content-Length') == '6'
    assert response.getheader('Accept-Ranges') == 'bytes'
    assert response.getheader('Server') == 'poor_httpd'
    assert response.getheader('Last-Modified').endswith(' GMT')
    response, body = s.request('/zero.bin')
    assert response.status == 200 and body == b'' and response.getheader('Content-Length') == '0'
    s.stop()


def test_head():
    s = Server()
    response, body = s.request('/hello.txt', method='HEAD')
    assert response.status == 200 and body == b'' and response.getheader('Content-Length') == '6'
    response, body = s.request('/missing', method='HEAD')
    assert response.status == 404 and body == b''
    s.stop()


def test_directories():
    s = Server()
    response, body = s.request('/')
    assert response.status == 200 and body == b'<h1>poor_httpd</h1>\n'
    assert response.getheader('Content-Type') == 'text/html; charset=utf-8'
    response, _ = s.request('/sub')
    assert response.status == 301 and response.getheader('Location') == '/sub/'
    response, _ = s.request('/sub?a=1')
    assert response.status == 301 and response.getheader('Location') == '/sub/?a=1'
    response, body = s.request('/sub/')
    assert response.status == 200 and body == b'sub\n'
    response, body = s.request('/sub/page.html?a=1')
    assert response.status == 200 and body == b'page\n'
    assert s.request('/empty/')[0].status == 403
    assert s.request('/nested/')[0].status == 403
    s.stop()


def test_not_found():
    s = Server()
    for path in ['/missing', '/hello.txt/more', '/sub/missing.html']:
        response, body = s.request(path)
        assert response.status == 404 and b'404 Not Found' in body, path
    s.stop()


def test_paths():
    s = Server()
    for path in ['/../hello.txt', '/sub/../hello.txt', '/./hello.txt', '//hello.txt', '/%68ello.txt', '/sub/%2e%2e/hello.txt']:
        status, _, body = parse(s.raw(get(path)))[0]
        assert status == 200 and body == b'hello\n', path
    for path in ['/../secret.txt', '/%2e%2e/secret.txt', '/..%2fsecret.txt', '/up/secret.txt', '/absolute.txt']:
        assert parse(s.raw(get(path)))[0][0] == 404, path
    assert s.request('/link.txt')[1] == b'hello\n'
    assert parse(s.raw(get('/a%00b')))[0][0] == 400
    assert parse(s.raw(get('/a%zz')))[0][0] == 400
    assert parse(s.raw(get('/' + 'a/' * 3000)))[0][0] == 414
    s.stop()


def test_special_files():
    s = Server()
    assert s.request('/fifo')[0].status == 403
    s.stop()


def test_big_file():
    s = Server(big=True)
    with open(os.path.join(s.root, 'big.bin'), 'rb') as f:
        data = f.read()
    response, body = s.request('/big.bin')
    assert response.status == 200 and hashlib.sha256(body).digest() == hashlib.sha256(data).digest()
    response, body = s.request('/big.bin', {'Range': 'bytes=1000000-2999999'})
    assert response.status == 206 and body == data[1000000:3000000]
    s.stop()


def test_ranges():
    s = Server()
    cases = {
        'bytes=0-1': (206, b'he', 'bytes 0-1/6'),
        'bytes=2-': (206, b'llo\n', 'bytes 2-5/6'),
        'bytes=-2': (206, b'o\n', 'bytes 4-5/6'),
        'bytes=1-100': (206, b'ello\n', 'bytes 1-5/6'),
        'bytes=-100': (206, b'hello\n', 'bytes 0-5/6'),
        'bytes=6-': (416, b'', 'bytes */6'),
        'bytes=-0': (416, b'', 'bytes */6'),
        'bytes=0-1,3-4': (200, b'hello\n', None),
        'bytes=3-1': (200, b'hello\n', None),
        'lines=0-1': (200, b'hello\n', None),
    }
    for value, (status, body, content_range) in cases.items():
        response, data = s.request('/hello.txt', {'Range': value})
        assert (response.status, data, response.getheader('Content-Range')) == (status, body, content_range), value
    modified = s.request('/hello.txt')[0].getheader('Last-Modified')
    response, body = s.request('/hello.txt', {'Range': 'bytes=0-1', 'If-Range': modified})
    assert response.status == 206 and body == b'he'
    response, body = s.request('/hello.txt', {'Range': 'bytes=0-1', 'If-Range': 'Sun, 06 Nov 1994 08:49:37 GMT'})
    assert response.status == 200 and body == b'hello\n'
    assert s.request('/zero.bin', {'Range': 'bytes=0-'})[0].status == 416
    s.stop()


def test_not_modified():
    s = Server()
    modified = s.request('/hello.txt')[0].getheader('Last-Modified')
    response, body = s.request('/hello.txt', {'If-Modified-Since': modified})
    assert response.status == 304 and body == b'' and response.getheader('Last-Modified') == modified
    response, body = s.request('/hello.txt', {'If-Modified-Since': 'Sun, 06 Nov 1994 08:49:37 GMT'})
    assert response.status == 200 and body == b'hello\n'
    response, body = s.request('/hello.txt', {'If-Modified-Since': 'not a date'})
    assert response.status == 200
    s.stop()


def test_keep_alive():
    s = Server()
    conn = http.client.HTTPConnection('127.0.0.1', s.port, timeout=10)
    conn.connect()
    sock = conn.sock
    for path in ['/hello.txt', '/missing', '/sub/', '/hello.txt']:
        conn.request('GET', path)
        conn.getresponse().read()
    assert conn.sock is sock
    conn.close()
    status, headers, body = parse(s.raw(get('/hello.txt', close=False, version='1.0')))[0]
    assert status == 200 and headers['Connection'] == 'close' and body == b'hello\n'
    with s.connect() as sock:
        sock.sendall(b'GET /hello.txt HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
        status, headers, body = read_response(sock)
        assert status == 200 and headers['Connection'] == 'keep-alive'
        sock.sendall(get('/sub/'))
        assert parse(read_all(sock))[0][2] == b'sub\n'
    s.stop()


def test_pipelining():
    s = Server()
    data = b'\r\n' + get('/hello.txt', close=False) + b'HEAD /hello.txt HTTP/1.1\r\nHost: test\r\n\r\n' + get('/sub/')
    responses = parse(s.raw(data), heads=(1,))
    assert [(status, body) for status, _, body in responses] == [(200, b'hello\n'), (200, b''), (200, b'sub\n')]
    s.stop()


def test_split_head():
    s = Server()
    for parts in [(b'GET /hello.txt HTTP/1.1\r\nHost: test\r\n\r', b'\n'), (b'GET /hello.txt HTTP/1.1\r\nHost: test\r\n', b'\r\n'),
                  (b'GET /hello.txt HTTP/1.1\nHost: test\n', b'\n'), (b'GET /hello.txt HTTP/1.1\r', b'\nHost: test\r\n\r\n')]:
        with s.connect() as sock:
            for part in parts:
                sock.sendall(part)
                time.sleep(0.05)
            assert read_response(sock)[2] == b'hello\n', parts
    s.stop()


def test_nodelay():
    # With Nagle's algorithm the second response waits for an ACK of the first, which the client delays by 40 ms
    s = Server()
    with s.connect() as sock:
        start = time.monotonic()
        for _ in range(20):
            sock.sendall(get('/hello.txt', close=False) * 2)
            data = b''
            while data.count(b'hello\n') < 2:
                data += sock.recv(65536)
        assert time.monotonic() - start < 0.3
    s.stop()


def test_bad_requests():
    s = Server()
    cases = [
        (b'GARBAGE\r\n\r\n', 400),
        (b'GET /\r\n\r\n', 400),
        (b'GET / HTTP/1.1 extra\r\nHost: test\r\n\r\n', 400),
        (b'GET hello.txt HTTP/1.1\r\nHost: test\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\nHost: test\r\nNo colon\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\nHost: test\r\n folded\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\nHost: test\r\nBad\x01: x\r\n\r\n', 400),
        (b'GET / HTTP/1.1\r\nHost: test\r\nContent-Length: x\r\n\r\n', 400),
        (b'GET / HTTP/2.0\r\nHost: test\r\n\r\n', 505),
        (b'GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 5\r\n\r\nhello', 413),
        (b'GET / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', 501),
        (b'GET / HTTP/1.1\r\nHost: test\r\nX: ' + b'a' * 9000 + b'\r\n\r\n', 431),
    ]
    for request, status in cases:
        response = parse(s.raw(request))[0]
        assert response[0] == status and response[1]['Connection'] == 'close', request[:40]
    status, headers, _ = parse(s.raw(b'POST / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n'))[0]
    assert status == 405 and headers['Allow'] == 'GET, HEAD'
    status, _, body = parse(s.raw(get('/hello.txt').replace(b'Host: test', b'Host: test\r\nContent-Length: 0')))[0]
    assert status == 200 and body == b'hello\n'
    status, _, body = parse(s.raw(b'GET http://test/hello.txt HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n'))[0]
    assert status == 200 and body == b'hello\n'
    s.stop()


def test_timeout():
    s = Server('-t', '1')
    with s.connect() as sock:
        start = time.monotonic()
        assert sock.recv(1) == b''
        assert 0.5 < time.monotonic() - start < 5
    with s.connect() as sock:
        sock.sendall(b'GET / HTTP/1.1\r\n')
        assert sock.recv(1) == b''
    assert ': timed out' in s.stop()


def test_concurrency():
    s = Server()
    errors = []

    def client():
        try:
            conn = http.client.HTTPConnection('127.0.0.1', s.port, timeout=30)
            for path, body in [('/hello.txt', b'hello\n'), ('/sub/', b'sub\n')] * 10:
                conn.request('GET', path)
                assert conn.getresponse().read() == body
            conn.close()
        except Exception as e:
            errors.append(e)

    threads = [threading.Thread(target=client) for _ in range(32)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not errors, errors
    assert 'served 640 requests' in s.stop()


def test_max_connections():
    s = Server('-c', '1')
    with s.connect() as first:
        first.sendall(get('/hello.txt', close=False))
        assert read_response(first)[0] == 200
        with s.connect() as second:
            second.sendall(get('/hello.txt'))
            second.settimeout(1)
            try:
                second.recv(1)
                raise AssertionError('a second connection was served')
            except TimeoutError:
                pass
            first.close()
            second.settimeout(10)
            assert parse(read_all(second))[0][2] == b'hello\n'
    s.stop()


def test_shutdown():
    s = Server(big=True)
    with s.connect() as idle, socket.socket() as slow:
        idle.sendall(get('/hello.txt', close=False))
        assert read_response(idle)[0] == 200
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.settimeout(10)
        slow.connect(('127.0.0.1', s.port))
        slow.sendall(get('/big.bin', close=False))
        data = slow.recv(4096)
        time.sleep(0.2)
        s.proc.send_signal(signal.SIGTERM)
        assert idle.recv(1) == b''
        data += read_all(slow)
        status, headers, body = parse(data)[0]
        assert status == 200 and len(body) == BIG_SIZE
    assert s.proc.wait(timeout=30) == 0
    log = s.log()
    assert 'Terminated: shutting down' in log and 'served 2 requests' in log


def test_forced_shutdown():
    s = Server(big=True)
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.settimeout(10)
        slow.connect(('127.0.0.1', s.port))
        slow.sendall(get('/big.bin'))
        slow.recv(4096)
        s.proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)
        s.proc.send_signal(signal.SIGTERM)
        assert s.proc.wait(timeout=30) == 0
    assert ': closing all connections' in s.log()


def test_grace():
    s = Server('-g', '1', big=True)
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.settimeout(10)
        slow.connect(('127.0.0.1', s.port))
        slow.sendall(get('/big.bin'))
        slow.recv(4096)
        start = time.monotonic()
        s.proc.send_signal(signal.SIGTERM)
        assert s.proc.wait(timeout=30) == 0
        assert 0.5 < time.monotonic() - start < 10
    assert 'closing 1 connections' in s.log()


def test_listen():
    port = free_port()
    s = Server('-l', f'127.0.0.1:{port}', listen=port)
    assert s.request('/hello.txt')[1] == b'hello\n'
    assert f'listening on 127.0.0.1:{port}' in s.stop()
    with socket.create_server(('127.0.0.1', 0)) as busy:
        port = busy.getsockname()[1]
        result = subprocess.run([EXE, '-l', f'127.0.0.1:{port}', '-l', f'127.0.0.1:{free_port()}'], capture_output=True,
                                text=True, timeout=30)
        assert result.returncode == 1 and f'127.0.0.1:{port}: bind: Address already in use' in result.stderr


def test_log():
    s = Server()
    s.request('/hello.txt')
    s.raw(b'GARBAGE\r\n\r\n')
    log = s.stop()
    date = r'\[\d\d/\w\w\w/\d{4}:\d\d:\d\d:\d\d \+0000\]'
    assert re.search(r'^127\.0\.0\.1 - - ' + date + r' "GET /hello\.txt HTTP/1\.1" 200 6$', log, re.M), log
    assert re.search(r'^127\.0\.0\.1 - - ' + date + r' "-" 400 \d+$', log, re.M), log
    s = Server('-q')
    s.request('/hello.txt')
    assert 'GET' not in s.stop()


def test_options():
    result = subprocess.run([EXE, '--help'], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and result.stdout.startswith('Usage: poor_httpd')
    for args, message in [
        (['--bogus'], 'unknown option --bogus'),
        (['-l'], '-l needs an argument'),
        (['-l', 'nowhere'], 'invalid argument for -l: nowhere'),
        (['-l', '127.0.0.1:0'], 'invalid argument for -l'),
        (['-l', '[::1]:99999'], 'invalid argument for -l'),
        (['-t', '0'], 'invalid argument for -t'),
        (['-c', 'x'], 'invalid argument for -c'),
        (['a', 'b'], 'only one directory'),
    ]:
        result = subprocess.run([EXE, *args], capture_output=True, text=True, timeout=30)
        assert result.returncode == 2 and message in result.stderr, (args, result.stderr)
    result = subprocess.run([EXE, '/nonexistent'], capture_output=True, text=True, timeout=30)
    assert result.returncode == 1 and '/nonexistent: No such file or directory' in result.stderr


if __name__ == '__main__':
    globals()['test_' + sys.argv[2]]()
