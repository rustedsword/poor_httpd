#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Load poor_httpd with wrk and measure what a request costs it. Every scenario stresses one part of the server, which
# runs unchanged: its thread on a CPU of its own, its io-wq workers next to it and wrk on other cores. Next to
# throughput and latency the results show the CPU time of a request in user space, in the kernel, in interrupts and in
# io-wq workers, its io_uring submissions and context switches and, with perf, its user-space instructions.
#
# Usage: bench.py POOR_HTTPD [SCENARIO...] [OPTIONS], see --help

import argparse
import dataclasses
import datetime
import email.utils
import json
import os
import re
import resource
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

HZ = os.sysconf('SC_CLK_TCK')
MTIME = 1767225600
FILES = {
    'index.html': 1 << 10,
    'small.html': 1 << 10,
    'medium.jpg': 256 << 10,
    'big.bin': 64 << 20,
    'site/index.html': 6 << 10,
    'site/style.css': 14 << 10,
    'site/app.js': 40 << 10,
    'site/logo.svg': 3 << 10,
    'site/photo.jpg': 90 << 10,
    'site/icon.png': 5 << 10,
    'site/font.woff2': 22 << 10,
    'site/data.json': 2 << 10,
    'site/favicon.ico': 1 << 10,
}
BROWSER = {
    'Connection': 'keep-alive',
    'sec-ch-ua': '"Chromium";v="140", "Not=A?Brand";v="24", "Google Chrome";v="140"',
    'sec-ch-ua-mobile': '?0',
    'sec-ch-ua-platform': '"Linux"',
    'Upgrade-Insecure-Requests': '1',
    'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36',
    'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,'
              'application/signed-exchange;v=b3;q=0.7',
    'Sec-Fetch-Site': 'none',
    'Sec-Fetch-Mode': 'navigate',
    'Sec-Fetch-User': '?1',
    'Sec-Fetch-Dest': 'document',
    'Accept-Encoding': 'gzip, deflate, br, zstd',
    'Accept-Language': 'en-US,en;q=0.9',
    'Cookie': 'session=0123456789abcdef0123456789abcdef; theme=dark',
}
SITE = ['/site/', '/site/style.css', '/site/app.js', '/site/logo.svg', '/site/photo.jpg', '/site/icon.png',
        '/site/font.woff2', '/site/data.json', '/site/favicon.ico']


@dataclasses.dataclass
class Scenario:
    name: str
    about: str
    paths: list
    conns: int = 0
    depth: int = 1
    headers: dict = dataclasses.field(default_factory=dict)
    close: bool = False
    idle: bool = False
    log: bool = False
    status: int = 200
    saturates: bool = True

    def requests(self, depth=None):
        fields = ''.join(f'{name}: {value}\r\n' for name, value in self.headers.items())
        fields += 'Connection: close\r\n' if self.close else ''
        return [f'GET {path} HTTP/1.1\r\nHost: bench\r\n{fields}\r\n' * (depth or self.depth) for path in self.paths]


SCENARIOS = [
    Scenario('latency', 'a small file over one connection: the round trip of a request', ['/small.html'], conns=1,
             saturates=False),
    Scenario('small', 'a small file over keep-alive connections', ['/small.html']),
    Scenario('browser', 'like small, with the headers of a browser', ['/small.html'], headers=BROWSER),
    Scenario('pipeline', 'a small file, 16 pipelined requests at a time', ['/small.html'], depth=16),
    Scenario('close', 'a small file, a new connection for every request', ['/small.html'], close=True),
    Scenario('idle', 'like close, while many idle connections stay open', ['/small.html'], close=True, idle=True),
    Scenario('index', 'a directory, served by its index.html', ['/']),
    Scenario('missing', 'a missing file, 404', ['/missing.html'], status=404),
    Scenario('notmod', 'a small file, 304 for If-Modified-Since', ['/small.html'],
             headers={'If-Modified-Since': email.utils.formatdate(MTIME, usegmt=True)}, status=304),
    Scenario('range', 'half of a small file, 206 for a Range', ['/small.html'], headers={'Range': 'bytes=0-511'},
             status=206),
    Scenario('medium', 'a 256 KiB file', ['/medium.jpg']),
    Scenario('big', 'a 64 MiB file over 8 connections', ['/big.bin'], conns=8),
    Scenario('site', 'the files of a web page, one after another', SITE),
    Scenario('log', 'like small, with the access log written to a file', ['/small.html'], log=True),
]

LUA = '''
local i = 0

request = function()
  i = i % #reqs + 1
  return reqs[i]
end

done = function(summary, latency)
  local e = summary.errors
  io.write(string.format('BENCH {"duration": %d, "requests": %d, "bytes": %d, "connect": %d, "read": %d, ' ..
    '"write": %d, "timeout": %d, "status": %d, "p50": %d, "p90": %d, "p99": %d, "max": %d}\\n',
    summary.duration, summary.requests, summary.bytes, e.connect, e.read, e.write, e.timeout, e.status,
    latency:percentile(50), latency:percentile(90), latency:percentile(99), latency.max))
end
'''

LOG_EXPECTED = re.compile(r'serving |listening on |Terminated: shutting down$|served \d+ requests|\S+ - - \[')


class BenchError(Exception):
    pass


def read(path):
    with open(path) as f:
        return f.read()


def sysfs(cpu, name):
    try:
        return read(f'/sys/devices/system/cpu/cpu{cpu}/{name}').strip()
    except OSError:
        return None


def parse_cpus(text):
    if text == 'all':
        return []
    cpus = set()
    for part in text.split(','):
        first, _, last = part.partition('-')
        cpus.update(range(int(first), int(last or first) + 1))
    return sorted(cpus)


def default_cpus():
    """
    Everything stays in the L3 cache of the fastest core, so loopback traffic doesn't cross caches: the server thread
    gets that core to itself, its io-wq workers the next core and wrk up to four CPUs of the other cores. With fewer
    than three cores in the cache the workers go to the SMT sibling of the server thread.
    """
    allowed = sorted(os.sched_getaffinity(0))
    if len(allowed) < 3:
        return [], [], []
    cores = {}
    for cpu in allowed:
        cores.setdefault(sysfs(cpu, 'topology/thread_siblings_list') or str(cpu), []).append(cpu)

    def rank(cpu):
        for name in ('acpi_cppc/highest_perf', 'cpufreq/amd_pstate_prefcore_ranking', 'cpufreq/cpuinfo_max_freq'):
            if (value := sysfs(cpu, name)) and value.isdigit():
                return int(value)
        return 0

    order = sorted(cores.values(), key=lambda core: (-rank(core[0]), core[0]))
    cache = set(parse_cpus(sysfs(order[0][0], 'cache/index3/shared_cpu_list') or ','.join(map(str, allowed))))
    order.sort(key=lambda core: core[0] not in cache)
    if sum(core[0] in cache for core in order) < 3 and len(order[0]) > 1:
        workers, rest = order[0][1:2], order[1:]
    else:
        workers, rest = order[1][:1], order[2:]
    near = [cpu for core in rest if core[0] in cache for cpu in core]
    far = [core[0] for core in rest if core[0] not in cache]
    return order[0][:1], workers, (near if len(near) > 1 else near + far)[:4]


def cpu_ticks(cpus):
    """Ticks of 'cpus' from /proc/stat: running tasks, in interrupts, idle and all of them"""
    ticks = {'task': 0, 'irq': 0, 'idle': 0, 'total': 0}
    for line in read('/proc/stat').splitlines():
        name, *fields = line.split()
        if name.startswith('cpu') and name[3:].isdigit() and int(name[3:]) in cpus:
            user, nice, system, idle, iowait, irq, softirq, steal = map(int, fields[:8])
            ticks['task'] += user + nice + system
            ticks['irq'] += irq + softirq
            ticks['idle'] += idle + iowait
            ticks['total'] += user + nice + system + idle + iowait + irq + softirq + steal
    return ticks


def pin(cpus):
    return (lambda: os.sched_setaffinity(0, cpus)) if cpus else None


def make_tree(root):
    for path, size in FILES.items():
        path = os.path.join(root, path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(os.urandom(size))
        os.utime(path, (MTIME, MTIME))


def lua_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\r', '\\r').replace('\n', '\\n') + '"'


class Server:
    """
    The server on 'root', with its listening socket from socket activation like in the tests. io-wq workers start on
    the CPUs of the thread that creates them, so they are moved to their own CPUs once seen: on the CPU of the server
    thread they would take turns with it instead of running next to it.
    """

    def __init__(self, exe, root, args, opts, log_path):
        sock = socket.create_server(('127.0.0.1', 0), backlog=socket.SOMAXCONN)
        self.port = sock.getsockname()[1]
        self.opts = opts
        self.moved = set()
        self.log_path = log_path
        fd = sock.fileno()
        move = f'exec 3<&{fd} {fd}<&-; ' if fd != 3 else ''
        script = move + 'export LISTEN_PID=$$ LISTEN_FDS=1; exec "$0" "$@"'
        with open(log_path, 'wb') as log:
            self.proc = subprocess.Popen(['sh', '-c', script, exe, *args, root], pass_fds=(fd,), stdout=log, stderr=log,
                                         preexec_fn=pin(opts.server_cpus))
        sock.close()
        self.pid = self.proc.pid
        self.ring = None

    def find_ring(self):
        for fd in os.listdir(f'/proc/{self.pid}/fd'):
            if os.readlink(f'/proc/{self.pid}/fd/{fd}') == 'anon_inode:[io_uring]':
                self.ring = fd
                return
        raise BenchError('the server has no io_uring')

    def ring_counters(self):
        """fdinfo only shows them when it gets the ring lock, which the server holds while it submits"""
        for _ in range(1000):
            if match := re.search(r'^SqHead:\s*(\d+)$.*^CqTail:\s*(\d+)$', read(f'/proc/{self.pid}/fdinfo/{self.ring}'),
                                  re.M | re.S):
                return int(match[1]), int(match[2])
            time.sleep(0.001)
        raise BenchError('fdinfo of the io_uring shows no counters')

    def snapshot(self, clients):
        task = f'/proc/{self.pid}/task/{self.pid}'
        sq, cq = self.ring_counters()
        process = read(f'/proc/{self.pid}/stat').rsplit(')', 1)[1].split()
        main = read(f'{task}/stat').rsplit(')', 1)[1].split()
        status = dict(line.split(':', 1) for line in read(f'{task}/status').splitlines())
        run, wait, _ = map(int, read(f'{task}/schedstat').split())
        return {
            'cpu': int(process[11]) + int(process[12]),
            'utime': int(main[11]),
            'stime': int(main[12]),
            'run': run,
            'wait': wait,
            'cs': int(status['voluntary_ctxt_switches']) + int(status['nonvoluntary_ctxt_switches']),
            'rss': int(status['VmRSS'].split()[0]),
            'sq': sq,
            'cq': cq,
            'server': cpu_ticks(self.opts.server_cpus),
            'workers': cpu_ticks(self.opts.worker_cpus),
            'client': cpu_ticks(clients),
        }

    def workers(self):
        count = 0
        for tid in os.listdir(f'/proc/{self.pid}/task'):
            try:
                if not read(f'/proc/{self.pid}/task/{tid}/comm').startswith('iou-wrk'):
                    continue
                count += 1
                if self.opts.worker_cpus and tid not in self.moved:
                    os.sched_setaffinity(int(tid), self.opts.worker_cpus)
                    self.moved.add(tid)
            except OSError:
                pass
        return count

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            return self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise BenchError('the server did not stop in 30 seconds')

    def warnings(self):
        counts = {}
        with open(self.log_path, errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                if line and not LOG_EXPECTED.match(line):
                    counts[line] = counts.get(line, 0) + 1
        return [f'server: {line}' + (f' (x{n})' if n > 1 else '') for line, n in counts.items()]

    def log_tail(self):
        with open(self.log_path, errors='replace') as f:
            return ''.join(f.readlines()[-10:])


class Perf:
    """
    perf on the server, writing into 'path'. Its events on the io-wq workers would slow the server down: the workers
    switch contexts for nearly every request, and every switch reprograms the counters. So it counts on the server
    thread only, which runs all the user-space code, and profiles whole CPUs where it may.
    """

    def __init__(self, cmd, path, delay):
        self.path = path
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(delay)

    @classmethod
    def count(cls, pid, path, events):
        return cls(['perf', 'stat', '-x', ',', '-o', path, '-e', ','.join(events), '--no-inherit', '-t', str(pid)],
                   path, 0.25)

    @classmethod
    def record(cls, pid, path, mode, cpus):
        target = ['-C', ','.join(map(str, cpus))] if mode == 'cpu' and cpus else ['-p', str(pid)]
        return cls(['perf', 'record', '-q', '-F', '4999', '-g', '-e', 'cycles:u' if mode == 'user' else 'cycles', '-o',
                    path, *target], path, 1)

    def stop(self):
        self.proc.send_signal(signal.SIGINT)
        self.proc.wait(timeout=60)

    def counters(self):
        counts = {}
        for line in read(self.path).splitlines():
            fields = line.split(',')
            if len(fields) > 2 and re.fullmatch(r'[\d.]+', fields[0]):
                counts[fields[2]] = float(fields[0])
        return counts


def perf_mode():
    """Whether perf sees user space only, the kernel too, or whole CPUs"""
    if not shutil.which('perf'):
        return None
    try:
        result = subprocess.run(['perf', 'stat', '-x', ',', '-e', 'instructions:u', '--', 'true'], capture_output=True,
                                text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode or not re.search(r'^[\d.]+,[^,]*,instructions:u', result.stderr, re.M):
        return None
    paranoid = int(read('/proc/sys/kernel/perf_event_paranoid'))
    return 'cpu' if os.geteuid() == 0 or paranoid <= 0 else 'kernel' if paranoid <= 1 else 'user'


def perf_report(path, key, *args):
    return subprocess.run(['perf', 'report', '-i', path, '--stdio', '--no-children', '--sort', key, '-g', 'none',
                           '--percent-limit', '0.5', *args], capture_output=True, text=True).stdout


def profile_summary(path, count=15):
    """The share of every task, and the top symbols of the time that wasn't idle"""
    comms = re.findall(r'^\s+([\d.]+)%\s+(\S.*?)\s*$', perf_report(path, 'comm'), re.M)
    tasks = {}
    for percent, comm in comms:
        name = 'idle' if comm == 'swapper' else re.sub(r'^(iou-wrk)-\d+$', r'\1', comm)
        tasks[name] = tasks.get(name, 0) + float(percent)
    busy = ','.join(comm for _, comm in comms if comm != 'swapper')
    report = perf_report(path, 'sym', *(['--comms', busy, '--percentage', 'relative'] if busy else []))
    return sorted(tasks.items(), key=lambda task: -task[1]), re.findall(r'^\s+([\d.]+)%\s+(\[[.k]\] .+?)\s*$', report,
                                                                     re.M)[:count]


def check(server, request, status):
    """Make the request once, so a scenario can't silently measure something else"""
    try:
        with socket.create_connection(('127.0.0.1', server.port), timeout=10) as sock:
            sock.sendall(request.encode())
            data = b''
            while b'\r\n' not in data and (chunk := sock.recv(4096)):
                data += chunk
    except OSError as e:
        raise BenchError(f'the server does not answer: {e}\n{server.log_tail()}')
    line = data.split(b'\r\n', 1)[0].decode('latin-1')
    if line.split()[1:2] != [str(status)]:
        raise BenchError(f'{request.split()[1]}: expected {status}, got {line or "nothing"!r}')


def open_idle(server, count):
    """Connections are accepted in order, so a request on a newer one returns once the older ones are accepted"""
    socks = []
    try:
        while len(socks) < count:
            for _ in range(min(1000, count - len(socks))):
                socks.append(socket.create_connection(('127.0.0.1', server.port)))
            check(server, 'GET /small.html HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n', 200)
    except BaseException as e:
        for sock in socks:
            sock.close()
        if isinstance(e, OSError):
            raise BenchError(f'opening idle connection {len(socks) + 1} failed: {e}, see ulimit -n and --idle')
        raise
    return socks


def run_wrk(port, script, threads, conns, seconds, cpus, poll):
    with tempfile.TemporaryFile() as out:
        proc = subprocess.Popen(['wrk', '-t', str(threads), '-c', str(conns), '-d', f'{seconds}s', '-T', '10s', '-s', script,
                                 f'http://127.0.0.1:{port}/'], stdout=out, stderr=subprocess.STDOUT, preexec_fn=pin(cpus))
        try:
            while not (waited := os.wait4(proc.pid, os.WNOHANG))[0]:
                poll()
                time.sleep(0.01)
        except BaseException:
            proc.kill()
            os.waitpid(proc.pid, 0)
            raise
        proc.returncode = os.waitstatus_to_exitcode(waited[1])
        out.seek(0)
        text = out.read().decode(errors='replace')
    match = re.search(r'^BENCH (.*)$', text, re.M)
    if proc.returncode or not match:
        raise BenchError(f'wrk failed:\n{text}')
    result = json.loads(match[1])
    result['cpu'] = waited[2].ru_utime + waited[2].ru_stime
    return result


def run(sc, opts, root, tmp, profile=None):
    conns = sc.conns or opts.conns
    threads = min(len(opts.client_cpus) or 4, conns)
    clients = opts.client_cpus[:threads]
    idle = opts.idle if sc.idle else 0
    args = [] if sc.log else ['-q']
    if idle:
        args += ['-c', str(idle + conns + 100), '-t', str(opts.warmup + opts.duration + 60)]
    script = os.path.join(tmp, 'wrk.lua')
    with open(script, 'w') as f:
        f.write('reqs = {' + ', '.join(map(lua_string, sc.requests())) + '}\n' + LUA)
    server = Server(opts.exe, root, args, opts, os.path.join(tmp, 'server.log'))
    socks = []
    peak = 0

    def poll():
        nonlocal peak
        peak = max(peak, server.workers())

    try:
        for request in sc.requests(depth=1):
            check(server, request, sc.status)
        server.find_ring()
        socks = open_idle(server, idle)
        if opts.warmup:
            run_wrk(server.port, script, threads, conns, opts.warmup, clients, server.workers)
        server.workers()
        if profile:
            perf = Perf.record(server.pid, profile, opts.perf, opts.server_cpus + opts.worker_cpus)
        elif opts.perf:
            events = ['instructions:u', 'cycles:u'] + (['instructions:k', 'cycles:k'] if opts.perf != 'user' else [])
            perf = Perf.count(server.pid, os.path.join(tmp, 'perf.stat'), events)
        else:
            perf = None
        try:
            before = server.snapshot(clients)
            wrk = run_wrk(server.port, script, threads, conns, opts.duration, clients, poll)
            after = server.snapshot(clients)
        finally:
            if perf:
                perf.stop()
    finally:
        for sock in socks:
            sock.close()
        status = server.stop()
    if status:
        raise BenchError(f'the server exited with {status}:\n{server.log_tail()}')

    seconds = wrk['duration'] / 1e6
    requests = wrk['requests'] or 1
    if sc.depth > 1:
        wrk.update(p50=None, p90=None, p99=None, max=None)
    main = (after['run'] - before['run']) / 1e9
    ticks = after['utime'] - before['utime'] + after['stime'] - before['stime']
    user = main * (after['utime'] - before['utime']) / ticks if ticks else 0
    wq = max((after['cpu'] - before['cpu']) / HZ - main, 0)
    shared = not opts.worker_cpus or set(opts.worker_cpus) & set(opts.server_cpus)
    cpus = {name: {key: after[name][key] - before[name][key] for key in after[name]}
            for name in ('server', 'workers', 'client')}

    def busy(name):
        return (1 - cpus[name]['idle'] / cpus[name]['total']) * 100 if cpus[name]['total'] else None

    m = {
        'requests': wrk['requests'],
        'rps': wrk['requests'] / seconds,
        'mbps': wrk['bytes'] / seconds / 1e6,
        'p50': wrk['p50'],
        'p90': wrk['p90'],
        'p99': wrk['p99'],
        'max': wrk['max'],
        'usr': user / requests * 1e6,
        'sys': (main - user) / requests * 1e6,
        'irq': cpus['server']['irq'] / HZ / requests * 1e6 if opts.server_cpus else None,
        'wq': wq / requests * 1e6,
        'workers': peak,
        'sqe': (after['sq'] - before['sq']) % (1 << 32) / requests,
        'cqe': (after['cq'] - before['cq']) % (1 << 32) / requests,
        'cs': (after['cs'] - before['cs']) / requests,
        'thread': main / seconds * 100,
        'waiting': (after['wait'] - before['wait']) / 1e9 / seconds * 100,
        'cpu': busy('server'),
        'other': (max(cpus['server']['task'] / HZ - main - (wq if shared else 0), 0) / (cpus['server']['total'] / HZ)
                  * 100 if opts.server_cpus else None),
        'wcpu': busy('workers'),
        'client': busy('client') if clients else wrk['cpu'] / seconds / threads * 100,
        'rss': after['rss'] / 1024,
        'errors': {key: wrk[key] for key in ('connect', 'read', 'write', 'timeout', 'status') if wrk[key]},
        'warnings': server.warnings(),
    }
    if perf and not profile:
        counts = perf.counters()
        for key, event in (('ins', 'instructions:u'), ('cyc', 'cycles:u'), ('kins', 'instructions:k'),
                           ('kcyc', 'cycles:k')):
            if event in counts:
                m[key] = counts[event] / requests
    return m


def median(runs):
    m = {key: statistics.median(run[key] for run in runs) if isinstance(value, (int, float)) else value
         for key, value in runs[0].items()}
    m['workers'] = max(run['workers'] for run in runs)
    m['errors'] = {}
    for run in runs:
        for key, value in run['errors'].items():
            m['errors'][key] = m['errors'].get(key, 0) + value
    m['warnings'] = sorted({warning for run in runs for warning in run['warnings']})
    if len(runs) > 1:
        m['spread'] = (max(run['rps'] for run in runs) - min(run['rps'] for run in runs)) / m['rps'] * 100
    return m


def notes(sc, m):
    found = []
    errors = {key: value for key, value in m['errors'].items() if key != 'status' or sc.status < 400}
    if errors:
        found.append('wrk: ' + ', '.join(f'{value} {key} errors' for key, value in errors.items()))
    if sc.saturates and m['cpu'] is not None and m['cpu'] < 90:
        found.append(f'the server CPU was {100 - m["cpu"]:.0f}% idle')
    if m['wcpu'] is not None and m['wcpu'] > 90:
        found.append(f'the io-wq CPUs were {m["wcpu"]:.0f}% busy, they may limit this result')
    if m['client'] > 80:
        found.append(f'the wrk CPUs were {m["client"]:.0f}% busy, they may limit this result')
    if m['other'] is not None and m['other'] > 5:
        found.append(f'other tasks took {m["other"]:.0f}% of the server CPU')
    return found + m['warnings']


def num(value):
    for limit, suffix in ((1e9, 'G'), (1e6, 'M'), (1e3, 'k')):
        if value >= limit:
            return f'{value / limit:.1f}{suffix}'
    return f'{value:.0f}'


def us(value):
    scale, unit = (1e6, 's') if value >= 1e6 else (1e3, 'ms') if value >= 1e3 else (1, 'us')
    value /= scale
    return f'{value:.2f}{unit}' if value < 10 else f'{value:.1f}{unit}' if value < 100 else f'{value:.0f}{unit}'


def given(fmt, value):
    return '-' if value is None else fmt(value)


COLUMNS = [
    ('req/s', 8, lambda m: num(m['rps'])),
    ('MB/s', 7, lambda m: f'{m["mbps"]:.1f}' if m['mbps'] < 100 else f'{m["mbps"]:.0f}'),
    ('p50', 8, lambda m: given(us, m['p50'])),
    ('p99', 8, lambda m: given(us, m['p99'])),
    ('usr/req', 8, lambda m: us(m['usr'])),
    ('sys/req', 8, lambda m: us(m['sys'])),
    ('irq/req', 8, lambda m: given(us, m['irq'])),
    ('wq/req', 8, lambda m: us(m['wq'])),
    ('wq', 3, lambda m: str(m['workers'])),
    ('sqe/req', 8, lambda m: f'{m["sqe"]:.2f}'),
    ('cs/req', 7, lambda m: f'{m["cs"]:.2f}'),
    ('ins/req', 8, lambda m: given(num, m.get('ins'))),
    ('cpu', 5, lambda m: given(lambda v: f'{v:.0f}%', m['cpu'])),
    ('wrk', 5, lambda m: f'{m["client"]:.0f}%'),
    ('rss', 6, lambda m: f'{m["rss"]:.0f}M'),
]


def row(name, cells, widths):
    return f'{name:<10}' + ''.join(f'{cell:>{width}}' for cell, width in zip(cells, widths))


def print_header(opts, env):
    build = env['build']
    flags = f'{build.get("buildtype")} -O{build.get("optimization")}' if build else 'unknown build'
    print(f'poor_httpd {env["git"]}, poor_loop {env["poor_loop"]}, {flags}, Linux {env["kernel"]}')
    cpus = [','.join(map(str, cpus)) or 'any' for cpus in (opts.server_cpus, opts.worker_cpus, opts.client_cpus)]
    print(f'{env["cpu"]}: server thread on CPU {cpus[0]}, io-wq on {cpus[1]}, wrk on {cpus[2]}')
    print(f'cpufreq {env["governor"]}/{env["epp"]} up to {env["max_mhz"]} MHz, platform profile '
          f'{env["platform_profile"]}, load {env["load"]}')
    perf = {'cpu': 'perf with the kernel and whole CPUs', 'kernel': 'perf with the kernel',
            'user': 'perf in user space only'}.get(opts.perf, 'no perf')
    print(f'{opts.duration} s runs after {opts.warmup} s of warm-up, {opts.repeat} per scenario, {perf}')
    sanitize = build.get('b_sanitize')
    if build.get('optimization') in ('0', 'g') or (sanitize and sanitize != 'none'):
        print('warning: this build is not optimized or has sanitizers, configure it with -Dbuildtype=release')
    print()
    widths = [width for _, width, _ in COLUMNS] + [7]
    print(row('scenario', [label for label, _, _ in COLUMNS] + (['spread'] if opts.repeat > 1 else []), widths))
    return widths


def compare(path, base, results):
    keys = [('usr/req', 'usr'), ('sys/req', 'sys'), ('irq/req', 'irq'), ('wq/req', 'wq'), ('ins/req', 'ins'),
            ('sqe/req', 'sqe'), ('p99', 'p99')]
    print(f'\nchange since {path} ({base["env"]["git"]}, {base["date"]}):')
    differ = [key for key in ('duration', 'warmup', 'conns', 'idle', 'server_cpus', 'worker_cpus', 'client_cpus')
              if base['options'].get(key) != results['options'][key]]
    differ += [key for key in ('build', 'kernel', 'governor', 'epp', 'max_mhz', 'platform_profile', 'perf')
               if base['env'].get(key) != results['env'][key]]
    if differ:
        print(f'note: different {", ".join(differ)}')
    widths = [18, 9] + [9] * len(keys)
    print(row('scenario', ['req/s', ''] + [label for label, _ in keys], widths))
    for name, entry in results['scenarios'].items():
        if name not in base['scenarios']:
            continue
        old, new = base['scenarios'][name]['median'], entry['median']
        cells = [f'{num(old["rps"])} -> {num(new["rps"])}', f'{(new["rps"] - old["rps"]) / old["rps"] * 100:+.1f}%']
        for _, key in keys:
            a, b = old.get(key), new.get(key)
            cells.append(f'{(b - a) / a * 100:+.1f}%' if a and b is not None else '-')
        print(row(name, cells, widths))


def environment(opts):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def git(path):
        try:
            return subprocess.run(['git', '-C', path, 'describe', '--always', '--dirty'], capture_output=True,
                                  text=True, timeout=10).stdout.strip() or None
        except OSError:
            return None

    try:
        options = json.loads(read(os.path.join(os.path.dirname(opts.exe), 'meson-info', 'intro-buildoptions.json')))
        build = {o['name']: o['value'] for o in options
                 if o['name'] in ('buildtype', 'optimization', 'debug', 'b_ndebug', 'b_sanitize', 'b_lto', 'c_args')}
    except (OSError, ValueError):
        build = {}
    cpu = (opts.server_cpus or [0])[0]
    model = re.search(r'^model name\s*:\s*(.*)$', read('/proc/cpuinfo'), re.M)
    try:
        platform_profile = read('/sys/firmware/acpi/platform_profile').strip()
    except OSError:
        platform_profile = None
    max_freq = sysfs(cpu, 'cpufreq/scaling_max_freq')
    return {
        'git': git(repo),
        'poor_loop': git(os.path.join(repo, 'subprojects', 'poor_loop')),
        'build': build,
        'kernel': os.uname().release,
        'cpu': model[1] if model else None,
        'governor': sysfs(cpu, 'cpufreq/scaling_governor'),
        'epp': sysfs(cpu, 'cpufreq/energy_performance_preference'),
        'max_mhz': int(max_freq) // 1000 if max_freq else None,
        'platform_profile': platform_profile,
        'load': read('/proc/loadavg').split()[0],
        'wrk': subprocess.run(['wrk', '-v'], capture_output=True, text=True).stdout.split(' Copyright')[0],
        'perf': opts.perf,
    }


def main():
    parser = argparse.ArgumentParser(
        description='Benchmark poor_httpd with wrk and measure what a request costs the server.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='scenarios:\n' + '\n'.join(f'  {sc.name:<10}{sc.about}' for sc in SCENARIOS) + '''

columns, per request unless noted:
  p50, p99          latency, which wrk can't measure with pipelining
  usr/req, sys/req  CPU time of the server thread in user space and in the kernel
  irq/req           interrupt and softirq time of the server CPU; over loopback most of it is TCP receive work for
                    the sockets of wrk, which runs on the CPU that sends
  wq/req, wq        CPU time of the io-wq workers, and the most workers seen
  sqe/req, cs/req   io_uring submissions, and context switches of the server thread
  ins/req           user-space instructions of the server, which unlike time don't depend on the CPU frequency
  cpu, wrk          how busy the server CPU and the CPUs of wrk were
  rss               resident memory of the server''')
    parser.add_argument('exe', metavar='POOR_HTTPD')
    parser.add_argument('scenarios', nargs='*', metavar='SCENARIO', help='scenarios to run (default: all)')
    parser.add_argument('-d', '--duration', type=int, default=5, metavar='SECONDS', help='measure for SECONDS (default: 5)')
    parser.add_argument('-w', '--warmup', type=int, default=1, metavar='SECONDS',
                        help='load the server for SECONDS before measuring (default: 1)')
    parser.add_argument('-r', '--repeat', type=int, default=1, metavar='N',
                        help='run each scenario N times, with a new server each time, and report the medians')
    parser.add_argument('-c', '--conns', type=int, default=64, metavar='N',
                        help='connections of the scenarios that set none (default: 64)')
    parser.add_argument('--idle', type=int, default=10000, metavar='N', help='idle connections of idle (default: 10000)')
    parser.add_argument('--server-cpus', type=parse_cpus, metavar='LIST',
                        help='CPUs of the server thread, e.g. 0 or 0-3, or all (default: the fastest core)')
    parser.add_argument('--worker-cpus', type=parse_cpus, metavar='LIST',
                        help='CPUs to move the io-wq workers to, or all to leave them on the CPUs of the server thread '
                             '(default: the next core)')
    parser.add_argument('--client-cpus', type=parse_cpus, metavar='LIST',
                        help='CPUs of wrk, a thread for each (default: up to four CPUs of the other cores)')
    parser.add_argument('--save', metavar='FILE', help='save the results as JSON')
    parser.add_argument('--compare', metavar='FILE', help='compare the results with saved ones')
    parser.add_argument('--profile', nargs='?', const='bench-perf', metavar='DIR',
                        help='run each scenario once more under perf record, save the profiles in DIR '
                             '(default: bench-perf) and show the top symbols')
    opts = parser.parse_args()

    names = {sc.name: sc for sc in SCENARIOS}
    unknown = [name for name in opts.scenarios if name not in names]
    if unknown:
        parser.error(f'unknown scenario {unknown[0]}, choose from {", ".join(names)}')
    if not shutil.which('wrk'):
        print('bench.py: wrk is needed, see https://github.com/wg/wrk', file=sys.stderr)
        return 77
    base = json.loads(read(opts.compare)) if opts.compare else None
    opts.exe = os.path.abspath(opts.exe)
    server_cpus, worker_cpus, client_cpus = default_cpus()
    if opts.server_cpus is None:
        opts.server_cpus = server_cpus
    if opts.worker_cpus is None:
        opts.worker_cpus = worker_cpus if opts.server_cpus == server_cpus else []
    if opts.client_cpus is None:
        opts.client_cpus = [cpu for cpu in client_cpus if cpu not in opts.server_cpus + opts.worker_cpus]
    measured = set(opts.server_cpus + opts.worker_cpus + opts.client_cpus)
    if spare := os.sched_getaffinity(0) - measured:
        os.sched_setaffinity(0, spare)
    opts.perf = perf_mode()
    hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    resource.setrlimit(resource.RLIMIT_NOFILE, (1 << 20 if hard == resource.RLIM_INFINITY else hard, hard))
    if opts.profile:
        os.makedirs(opts.profile, exist_ok=True)

    env = environment(opts)
    results = {
        'date': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'env': env,
        'options': {key: getattr(opts, key) for key in ('duration', 'warmup', 'repeat', 'conns', 'idle', 'server_cpus',
                                                         'worker_cpus', 'client_cpus')},
        'scenarios': {},
    }
    widths = print_header(opts, env)
    failed = False
    profiles = []
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, 'root')
        make_tree(root)
        for sc in [names[name] for name in opts.scenarios] or SCENARIOS:
            try:
                runs = [run(sc, opts, root, tmp) for _ in range(opts.repeat)]
                if opts.profile:
                    path = os.path.abspath(os.path.join(opts.profile, f'{sc.name}.data'))
                    profiles.append((sc.name, path, runs[0], run(sc, opts, root, tmp, profile=path)))
            except BenchError as e:
                print(row(sc.name, ['failed'], [8]))
                print('  ' + str(e).rstrip().replace('\n', '\n  '))
                failed = True
                continue
            m = median(runs)
            results['scenarios'][sc.name] = {'median': m, 'runs': runs}
            spread = [f'{m["spread"]:.1f}%'] if 'spread' in m else []
            print(row(sc.name, [cell(m) for _, _, cell in COLUMNS] + spread, widths))
            for note in notes(sc, m):
                print(f'  {note}')
            sys.stdout.flush()

    for name, path, m, profiled in profiles:
        if opts.perf == 'cpu' and opts.server_cpus:
            scope = f'CPUs {",".join(map(str, opts.server_cpus + opts.worker_cpus))}'
        elif opts.perf == 'kernel' or opts.perf == 'cpu':
            scope = 'server'
        else:
            scope = f'user space, {m["usr"] / (m["usr"] + m["sys"] + (m["irq"] or 0)) * 100:.0f}% of the server CPU'
        tasks, symbols = profile_summary(path)
        print(f'\n{name}: {path}, {num(profiled["rps"])} req/s while profiled')
        print(f'  {scope}: ' + ', '.join(f'{comm} {percent:.1f}%' for comm, percent in tasks))
        for percent, symbol in symbols:
            print(f'  {percent:>6}%  {symbol}')
    if base:
        compare(opts.compare, base, results)
    if opts.save:
        with open(opts.save, 'w') as f:
            json.dump(results, f, indent=1)
            f.write('\n')
    return 1 if failed else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
