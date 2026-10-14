/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <getopt.h>
#include <limits.h>
#include <poor_array.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "httpd.h"
#include "log.h"

constexpr unsigned SIGNAL_BUFS = 4;
constexpr int SIGNAL_BUF_GROUP = 0;
constexpr uint64_t NS_PER_SEC = 1'000'000'000;
constexpr uint64_t DRAIN_TIMEOUT_NS = NS_PER_SEC;

/*
 * Signals are read from a signalfd with one multishot read, into buffers the kernel picks from a buffer ring. The
 * first SIGINT or SIGTERM shuts the server down gracefully, the second closes all connections and unblocks the
 * signals, so a third one kills the process.
 */
poor_loop_fd_declare(signal_file);

struct signals {
	signal_file file;
	struct poor_loop_op read_op;
	struct io_uring_buf_ring *ring;
	struct server *server;
	sigset_t set;
	unsigned count;
	bool stopping, closed;
	struct signalfd_siginfo bufs[SIGNAL_BUFS];
};

poor_loop_group_attach(signal_file, struct signals, file, read_op, on_signal);

static const char usage[] = "Usage: poor_httpd [OPTIONS] [DIR]\n"
			    "Serve the files in DIR, or in the current directory, over HTTP.\n"
			    "\n"
			    "  -l, --listen ADDR:PORT      listen on ADDR:PORT, e.g. 127.0.0.1:8080, [::1]:8080 or :8080\n"
			    "                              for all addresses; can be repeated (default: 127.0.0.1:8080)\n"
			    "  -t, --timeout SECONDS       close connections that make no progress for SECONDS (default: 10)\n"
			    "  -g, --grace SECONDS         on SIGINT or SIGTERM, let responses finish for up to SECONDS\n"
			    "                              (default: 10)\n"
			    "  -c, --max-connections N     serve at most N connections at once (default: 1024)\n"
			    "  -q, --quiet                 do not log requests\n"
			    "  -h, --help                  show this help\n"
			    "\n"
			    "Sockets passed with systemd socket activation are served too.";

[[noreturn]] void loop_failed(const char *what, int err)
{
	dprintln(STDERR_FILENO, "poor_httpd: ", what, ": ", strerror(err));
	exit(1);
}

static void signals_recycle(struct signals *sig, unsigned id)
{
	io_uring_buf_ring_add_array(sig->ring, array_ptr(&sig->bufs[id]), id, io_uring_buf_ring_mask(SIGNAL_BUFS), 0);
	io_uring_buf_ring_advance(sig->ring, 1);
}

static void signals_read(struct poor_loop *loop, struct signals *sig)
{
	io_uring_prep_read_multishot(need_sqe(poor_loop_group_get_sqe_or_submit(loop, &sig->file, &sig->read_op)), sig->file.fd,
				     0, 0, SIGNAL_BUF_GROUP);
}

/* Running out of buffers ends the multishot read, so it is armed again */
POOR_LOOP_GROUP_OP(on_signal, loop, file, op, cqe)
{
	struct signals *sig = container_of(file, struct signals, file);
	unsigned id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

	if (cqe->flags & IORING_CQE_F_BUFFER) {
		if (cqe->res > 0 && !sig->stopping) {
			log_println(strsignal(sig->bufs[id].ssi_signo),
				    sig->count ? ": closing all connections" : ": shutting down");
			if (sig->count++)
				sigprocmask(SIG_UNBLOCK, &sig->set, nullptr);
			server_shutdown(loop, sig->server, sig->count > 1);
		}
		signals_recycle(sig, id);
	}
	if (op->pending || sig->stopping)
		return;
	if (cqe->res < 0 && cqe->res != -ENOBUFS)
		log_println("signalfd: ", strerror(-cqe->res));
	else
		signals_read(loop, sig);
}

POOR_LOOP_FD_DONE(signal_file, loop, file)
{
	struct signals *sig = container_of(file, struct signals, file);

	io_uring_free_buf_ring(poor_loop_ring(loop), sig->ring, SIGNAL_BUFS, SIGNAL_BUF_GROUP);
	sig->closed = true;
}

[[nodiscard]] static int signals_start(struct poor_loop *loop, struct signals *sig, struct server *s)
{
	int fd, ret;

	sigemptyset(&sig->set);
	sigaddset(&sig->set, SIGINT);
	sigaddset(&sig->set, SIGTERM);
	sigprocmask(SIG_BLOCK, &sig->set, nullptr);
	fd = signalfd(-1, &sig->set, SFD_CLOEXEC);
	if (fd < 0)
		return -errno;
	sig->ring = io_uring_setup_buf_ring(poor_loop_ring(loop), SIGNAL_BUFS, SIGNAL_BUF_GROUP, 0, &ret);
	if (!sig->ring) {
		poor_loop_close_fd_or_submit(loop, fd, 0);
		return ret;
	}
	foreach_array_index(sig->bufs, i)
		signals_recycle(sig, i);
	poor_loop_fd_init(&sig->file, fd);
	poor_loop_op_init(&sig->read_op, on_signal);
	sig->server = s;
	sig->count = 0;
	sig->stopping = sig->closed = false;
	signals_read(loop, sig);
	return 0;
}

/* The fd group closes the signalfd once the read is cancelled */
static void signals_stop(struct poor_loop *loop, struct signals *sig)
{
	sig->stopping = true;
	if (sig->read_op.pending)
		cancel_op(loop, &sig->read_op);
}

/* Each of these is needed, and the newest, bind and listen, came in Linux 6.11 */
static bool probe(struct poor_loop *loop)
{
	static const struct {
		int op;
		const char *name;
	} ops[] = {
		{ IORING_OP_OPENAT, "OPENAT" },
		{ IORING_OP_OPENAT2, "OPENAT2" },
		{ IORING_OP_STATX, "STATX" },
		{ IORING_OP_READ, "READ" },
		{ IORING_OP_READ_MULTISHOT, "READ_MULTISHOT" },
		{ IORING_OP_WRITE, "WRITE" },
		{ IORING_OP_SOCKET, "SOCKET" },
		{ IORING_OP_URING_CMD, "URING_CMD" },
		{ IORING_OP_BIND, "BIND" },
		{ IORING_OP_LISTEN, "LISTEN" },
		{ IORING_OP_ACCEPT, "ACCEPT" },
		{ IORING_OP_RECV, "RECV" },
		{ IORING_OP_SEND, "SEND" },
		{ IORING_OP_ASYNC_CANCEL, "ASYNC_CANCEL" },
		{ IORING_OP_CLOSE, "CLOSE" },
	};
	struct io_uring_probe *p = io_uring_get_probe_ring(poor_loop_ring(loop));
	bool ok = p;

	if (!p)
		log_println("io_uring: probing operations failed");
	foreach_array_ref(ops, o) {
		if (p && !io_uring_opcode_supported(p, o->op)) {
			log_println("io_uring: IORING_OP_", o->name, " is not supported, Linux 6.11 or newer is needed");
			ok = false;
		}
	}
	io_uring_free_probe(p);
	return ok;
}

static bool parse_number(const char *s, unsigned long max, unsigned long *n)
{
	char *end;

	errno = 0;
	*n = strtoul(s, &end, 10);
	return *s >= '0' && *s <= '9' && !*end && !errno && *n <= max;
}

/* ADDR:PORT, [ADDR]:PORT for IPv6, or :PORT for all IPv4 and IPv6 addresses */
static bool parse_listen(const char *arg, struct listen_addr *a)
{
	const char *colon = strrchr(arg, ':');
	char host[INET6_ADDRSTRLEN];
	struct in6_addr addr6;
	struct in_addr addr;
	unsigned long port;
	size_t len;

	if (!colon || !parse_number(colon + 1, 65535, &port) || !port)
		return false;
	len = colon - arg;
	if (len >= 2 && arg[0] == '[' && arg[len - 1] == ']') {
		arg++;
		len -= 2;
	}
	if (len >= sizeof(host))
		return false;
	memcpy(host, arg, len);
	host[len] = '\0';
	if (len && inet_pton(AF_INET, host, &addr) == 1) {
		*(struct sockaddr_in *)&a->addr =
			(struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = addr };
		a->len = sizeof(struct sockaddr_in);
		return true;
	}
	if (!len)
		addr6 = in6addr_any;
	else if (inet_pton(AF_INET6, host, &addr6) != 1)
		return false;
	*(struct sockaddr_in6 *)&a->addr =
		(struct sockaddr_in6){ .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = addr6 };
	a->len = sizeof(struct sockaddr_in6);
	return true;
}

/* sd_listen_fds(): the sockets are for us only if LISTEN_PID names this process */
static unsigned listen_fds(void)
{
	const char *pid = getenv("LISTEN_PID"), *fds = getenv("LISTEN_FDS");
	unsigned long p, n;

	if (!pid || !fds || !parse_number(pid, LONG_MAX, &p) || p != (unsigned long)getpid() || !parse_number(fds, 1024, &n))
		return 0;
	return n;
}

static int invalid(int opt, const char *arg)
{
	log_println("poor_httpd: invalid argument for -", (char)opt, ": ", arg);
	return 2;
}

static void config_init(struct config *cfg)
{
	cfg->root = ".";
	cfg->timeout_ns = cfg->grace_ns = 10 * NS_PER_SEC;
	cfg->max_conns = 1024;
	cfg->nlisten = 0;
	cfg->quiet = false;
	cfg->nfds = listen_fds();
}

/* Returns -1 to run the server, or the exit status */
static int parse_options(struct poor_loop *loop, struct config *cfg, int argc, char **argv)
{
	static const struct option options[] = {
		{ "listen", required_argument, nullptr, 'l' },
		{ "timeout", required_argument, nullptr, 't' },
		{ "grace", required_argument, nullptr, 'g' },
		{ "max-connections", required_argument, nullptr, 'c' },
		{ "quiet", no_argument, nullptr, 'q' },
		{ "help", no_argument, nullptr, 'h' },
		{},
	};
	unsigned long n;
	int opt;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, ":l:t:g:c:qh", options, nullptr)) != -1) {
		switch (opt) {
		case 'l':
			if (cfg->nlisten == MAX_LISTEN) {
				log_println("poor_httpd: at most ", MAX_LISTEN, " addresses can be given");
				return 2;
			}
			if (!parse_listen(optarg, &cfg->listen[cfg->nlisten++]))
				return invalid(opt, optarg);
			break;
		case 't':
		case 'g':
			if (!parse_number(optarg, 86400, &n) || (opt == 't' && !n))
				return invalid(opt, optarg);
			*(opt == 't' ? &cfg->timeout_ns : &cfg->grace_ns) = n * NS_PER_SEC;
			break;
		case 'c':
			if (!parse_number(optarg, UINT_MAX, &n) || !n)
				return invalid(opt, optarg);
			cfg->max_conns = n;
			break;
		case 'q':
			cfg->quiet = true;
			break;
		case 'h':
			log_init(loop, STDOUT_FILENO);
			log_println(usage);
			return 0;
		case ':':
			log_println("poor_httpd: ", argv[optind - 1], " needs an argument");
			return 2;
		default:
			log_println("poor_httpd: unknown option ", argv[optind - 1], "\nTry 'poor_httpd --help'.");
			return 2;
		}
	}
	if (optind < argc)
		cfg->root = argv[optind++];
	if (optind < argc) {
		log_println("poor_httpd: only one directory can be served");
		return 2;
	}
	if (!cfg->nlisten && !cfg->nfds)
		parse_listen("127.0.0.1:8080", &cfg->listen[cfg->nlisten++]);
	return -1;
}

/* Runs until the server is done and the signalfd is closed */
static int run(struct poor_loop *loop, struct server *s, struct signals *sig)
{
	int ret;

	if (!probe(loop))
		return 1;
	ret = signals_start(loop, sig, s);
	if (ret) {
		log_println("signalfd: ", strerror(-ret));
		return 1;
	}
	server_start(loop, s);
	ret = poor_loop_run(loop);
	signals_stop(loop, sig);
	while (!ret && !sig->closed)
		ret = poor_loop_run_once(loop);
	if (ret) {
		log_println("io_uring: ", strerror(-ret));
		return 1;
	}
	return s->status;
}

/* Write out the log, then wait for the closes, which nobody tracks */
static int finish(struct poor_loop *loop, int status)
{
	int ret = 0;

	while (!ret && log_pending())
		ret = poor_loop_run_once(loop);
	if (!ret)
		ret = poor_loop_drain(loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	poor_loop_exit(loop);
	return ret ? 1 : status;
}

int main(int argc, char **argv)
{
	struct io_uring_params params = {
		.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
			 IORING_SETUP_DEFER_TASKRUN,
	};
	struct signals signals;
	struct server server;
	struct poor_loop loop;
	int ret;

	signal(SIGPIPE, SIG_IGN);
	ret = poor_loop_init(&loop, 4096, &params);
	if (ret)
		loop_failed("io_uring_queue_init", -ret);
	io_uring_register_ring_fd(poor_loop_ring(&loop));
	log_init(&loop, STDERR_FILENO);
	poor_loop_log_function_set(log_poor_loop);
	server_init(&server);
	config_init(&server.cfg);
	ret = parse_options(&loop, &server.cfg, argc, argv);
	if (ret < 0)
		ret = run(&loop, &server, &signals);
	return finish(&loop, ret);
}
