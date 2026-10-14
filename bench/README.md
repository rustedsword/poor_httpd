# Benchmarks

`bench.py` loads poor_httpd with [wrk](https://github.com/wg/wrk) in scenarios that each stress one part of it, and
shows what a request costs the server. The server runs unchanged and is only watched from outside: through `/proc`,
the counters of its ring in fdinfo and perf.

## Running

```sh
meson setup build-release -Dbuildtype=release
meson test -C build-release --benchmark -v
```

runs every scenario for 5 seconds after a second of warm-up, in about a minute and a half. Without wrk the
benchmark is skipped. `bench.py` takes the server, the scenarios and options, see `--help`:

```sh
bench/bench.py build-release/poor_httpd small close --save before.json
bench/bench.py build-release/poor_httpd small close --compare before.json
```

`--save` keeps every run and the environment as JSON, and `--compare` shows the changes against such a file and
names what differed between the two, like the build or the CPU frequency settings. `--repeat N` runs every scenario
N times, each with a new server, and reports the medians and the spread of requests per second. `--profile` runs
every scenario once more under `perf record`.

## Placement

Everything stays in the L3 cache of the fastest core: the server thread gets that core to itself, the io-wq workers
the next core and wrk up to four CPUs of the other cores, SMT siblings included. The harness and perf run on the
remaining CPUs. `--server-cpus`, `--worker-cpus` and `--client-cpus` change it. On a Ryzen AI 9 HX 370, with
`small`:

- Unpinned, the scheduler decides which cores and caches the server shares with wrk, which alone gave between 112k
  and 166k requests per second.
- Over loopback, the CPU that sends does the TCP receive work of the other side. With wrk on the other CCX, that work
  and the wake-ups cross caches, and the server got 110k instead of 163k.
- io-wq workers inherit the CPUs of the server thread, so with the server pinned to one CPU they take turns with the
  thread instead of running next to it. Then runs swing between 80k and 206k, and io-wq looks three times cheaper.
  The harness moves every worker to its CPUs as soon as it shows up.
- Workers on the SMT sibling of the server thread slow the thread down: 163k against 190k with a core of their own.

## Scenarios

| scenario | stresses |
|---|---|
| latency | one connection: the round trip of a request, with every wake-up on the way |
| small | a 1 KiB file over 64 keep-alive connections: the cost of a request |
| pipeline | 16 pipelined requests at a time: a request without a network round trip |
| close | a new connection for every request: accept, shutdown, linger and close |
| idle | close next to 10000 idle connections: what grows with the connections |
| index | a directory, served by its index.html: a second open and statx |
| missing | 404: the error path, without statx and read |
| notmod | 304 for If-Modified-Since: date parsing |
| range | 206 for a Range: range parsing |
| medium | a 256 KiB file: a response in several reads and sends |
| big | a 64 MiB file over 8 connections: bulk copying |
| site | the files of a web page, one after another: a mix of sizes and content types |
| log | small with the access log on, the only scenario with it |

Every scenario first makes its request once and checks the status, so a missing file can't turn `small` into a
benchmark of 404s. Differences between scenarios point at parts of the server: `index` against `small` is the cost of
a directory, `close` against `small` the cost of a connection.

## Columns

| column | meaning |
|---|---|
| req/s, MB/s | throughput, headers included |
| p50, p99 | latency measured by wrk, which can't measure it with pipelining |
| usr/req, sys/req | CPU time of the server thread per request, in user space and in the kernel |
| irq/req | interrupt and softirq time of the server CPU per request, over loopback mostly TCP receive work for wrk |
| wq/req, wq | CPU time of the io-wq workers per request, and the most workers seen |
| sqe/req | io_uring submissions per request, from the counters of the ring |
| cs/req | context switches of the server thread per request |
| ins/req | user-space instructions of the server per request |
| cpu, wrk | how busy the server CPU and the CPUs of wrk were |
| rss | resident memory of the server |

Costs per request compare across scenarios and builds. When `cpu` is near 100%, `usr/req`, `sys/req` and `irq/req` add
up to the time of a request, and req/s follows from them. Otherwise something else limits req/s, while the costs per
request still hold. A note under a scenario names what to watch: an idle server CPU, busy io-wq or wrk CPUs, other
tasks on the server CPU, wrk errors or unexpected lines in the server log. The JSON adds p90, the maximum latency,
completions per request and, with the kernel visible to perf, kernel instructions and cycles of the server thread.

## perf

`kernel.perf_event_paranoid` decides what perf sees:

- 2, the default: user space. `ins/req` works and profiles show user-space symbols.
- 1: the kernel too. Profiles show the kernel functions of the server thread and the workers.
- 0 or less: whole CPUs. Profiles sample the server and io-wq CPUs, with softirq work in ksoftirqd, idle time and
  other tasks.

perf counts on the server thread only, which runs all the user-space code: attached to the workers, it reprograms the
counters on every context switch, and the workers switch for nearly every request. That cost 10% of the requests per
second, 18% with kernel counters, and doubled the CPU time of the workers. For the same reason profiles of whole CPUs
cost 3% of the requests per second, and profiles of the server threads 7%. A profile reports its requests per second,
the share of every task and the top symbols of the time that wasn't idle; `perf report -i bench-perf/SCENARIO.data`
shows the rest. Build with `-Dc_args=-fno-omit-frame-pointer` for call graphs through the server.

## Caveats

- Time depends on the CPU frequency, instructions don't. On a laptop in a power saving profile times of the same
  build drift by a few percent, while `ins/req` stays within 1%. The header shows the cpufreq governor, EPP, maximum
  frequency, platform profile and load average.
- Tail latencies vary: `p99` of the same build ranged from 1 to 15 ms. Cores with nothing to do drop into deep idle
  states, and C3 of the HX 370 takes 350 us to leave; disabling deep idle states, e.g. with
  `cpupower idle-set -D 20`, should steady it.
- wrk corrects its latencies for coordinated omission by adding samples, which raises `p99`.
