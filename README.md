# poor_httpd

A static file HTTP server on top of [poor_loop](https://github.com/rustedsword/poor_loop), the minimal io_uring
event loop. It is a demonstrator for poor_loop, and a usable server: it serves a directory over HTTP/1.1 with
keep-alive, pipelining, ranges, conditional requests, timeouts and graceful shutdown.

Every operation goes through io_uring: opening the directory and the files, creating, binding and listening on
sockets, accepting, reading, sending, logging, reading signals and closing. The server makes no synchronous I/O
calls.

## Build

Requires Linux 6.11+, a C23 compiler, Meson 1.4+, liburing 2.7+, poor_loop and
[poor_base](https://github.com/rustedsword/poor_base). Installed ones are preferred, otherwise poor_loop comes from
the `subprojects/poor_loop` submodule, with its own liburing and poor_base:

```sh
git submodule update --init subprojects/poor_loop
git -C subprojects/poor_loop submodule update --init subprojects/poor_base subprojects/liburing/liburing
meson setup build
meson compile -C build
```

`meson test -C build` runs the tests, which need Python 3. `meson install -C build --skip-subprojects` installs
just the server.

## Usage

```
poor_httpd [OPTIONS] [DIR]

  -l, --listen ADDR:PORT      listen on ADDR:PORT, e.g. 127.0.0.1:8080, [::1]:8080 or :8080
                              for all addresses; can be repeated (default: 127.0.0.1:8080)
  -t, --timeout SECONDS       close connections that make no progress for SECONDS (default: 10)
  -g, --grace SECONDS         on SIGINT or SIGTERM, let responses finish for up to SECONDS
                              (default: 10)
  -c, --max-connections N     serve at most N connections at once (default: 1024)
  -q, --quiet                 do not log requests
  -h, --help                  show this help
```

A directory is served by its `index.html`. Files are opened with `RESOLVE_BENEATH`, so symlinks can't lead out of
DIR. Requests are logged to stderr in the Common Log Format. The first SIGINT or SIGTERM stops accepting and lets
responses in progress finish, the second closes all connections.

It also takes sockets from systemd socket activation:

```ini
# poor_httpd.socket
[Socket]
ListenStream=8080

[Install]
WantedBy=sockets.target
```

```ini
# poor_httpd.service
[Service]
ExecStart=/usr/local/bin/poor_httpd /srv/www
```

## How it uses poor_loop

- `src/server.c`: a group held by the directory open, every listener and every connection. When it is done, the
  server stops the loop.
- `src/listener.c`: socket setup as one linked chain, reserved with `poor_loop_check_sq_space_or_submit()`, and
  an accept with a retry timer, all in an fd group that closes the socket. At the connection limit a listener
  pauses by holding its group.
- `src/conn.c`: an fd group for the socket and one for the served file, tied together with
  `poor_loop_group_acquire()`, `poor_loop_group_release()` and `poor_loop_group_call_done()`, a group timer for
  timeouts and untracked cancels to end a connection.
- `src/main.c`: a multishot read on a signalfd into a buffer ring set up through `poor_loop_ring()`, and
  `poor_loop_run()`, `poor_loop_run_once()` and `poor_loop_drain()` for a clean exit.
- `src/log.c`: an asynchronous logger that also takes poor_loop's own messages through
  `poor_loop_log_function_set()`.
