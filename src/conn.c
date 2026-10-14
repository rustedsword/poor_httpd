/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/openat2.h>
#include <netinet/tcp.h>
#include <poor_array.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "http.h"
#include "httpd.h"
#include "log.h"

constexpr size_t IN_SIZE = 8192;
constexpr size_t OUT_SIZE = 65536;
constexpr size_t PATH_SIZE = 4096;
constexpr uint64_t LINGER_NS = 2'000'000'000;

/*
 * A connection has an fd group for its socket and one for the file it serves. The open file holds the socket group
 * and a response in progress holds the file group, so the file closes after its last read and the socket after
 * everything else. Requests are parsed in place in 'in', which keeps pipelined ones for later, and responses go
 * out through 'out', the file in chunks that are read and then sent.
 */
poor_loop_fd_declare(conn_sock);
poor_loop_fd_declare(conn_file);

struct conn {
	conn_sock sock;
	conn_file file;
	struct poor_list_node link;
	struct server *server;
	struct poor_loop_op recv_op, send_op, open_op, shutdown_op, nodelay_op, stat_op, read_op;
	struct poor_loop_timer timer;
	struct http_request req;
	struct statx stx;
	uint64_t offset, left, body_len;
	size_t in_len, head_len, scanned, out_len, out_sent;
	int status;
	bool busy, keep_alive, lingering, closing, file_held, index;
	char peer[HTTP_ADDR_SIZE];
	char path[PATH_SIZE];
	char in[IN_SIZE];
	char out[OUT_SIZE];
};

poor_list_define(conn_list, struct conn, link);
poor_loop_group_attach(conn_sock, struct conn, sock, recv_op, on_recv);
poor_loop_group_attach(conn_sock, struct conn, sock, send_op, on_send);
poor_loop_group_attach(conn_sock, struct conn, sock, open_op, on_open);
poor_loop_group_attach(conn_sock, struct conn, sock, shutdown_op, on_shutdown);
poor_loop_group_attach(conn_sock, struct conn, sock, nodelay_op, on_nodelay);
poor_loop_group_attach(conn_sock, struct conn, sock, timer, on_timeout);
poor_loop_group_attach(conn_file, struct conn, file, stat_op, on_stat);
poor_loop_group_attach(conn_file, struct conn, file, read_op, on_read);

/* The kernel reads it when the open is submitted */
static struct open_how open_how = {
	.flags = O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC,
	.mode = 0,
	.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS,
};

/* The kernel reads it when the setsockopt is submitted */
static int one = 1;

#define out_print(c, ...) ((c)->out_len += sprint_array(arrview_cfront((c)->out_len, (c)->out), __VA_ARGS__))

static void conn_parse(struct poor_loop *loop, struct conn *c);

static struct conn *conn_of_sock(conn_sock *sock)
{
	return container_of(sock, struct conn, sock);
}

static struct conn *conn_of_file(conn_file *file)
{
	return container_of(file, struct conn, file);
}

static struct io_uring_sqe *sock_sqe(struct poor_loop *loop, struct conn *c, struct poor_loop_op *op)
{
	return need_sqe(poor_loop_group_get_sqe_or_submit(loop, &c->sock, op));
}

static struct io_uring_sqe *file_sqe(struct poor_loop *loop, struct conn *c, struct poor_loop_op *op)
{
	return need_sqe(poor_loop_group_get_sqe_or_submit(loop, &c->file, op));
}

/* May free the connection */
static void conn_put(struct poor_loop *loop, struct conn *c)
{
	if (poor_loop_group_release(&c->sock))
		poor_loop_group_call_done(conn_sock, loop, &c->sock);
}

/* Inside a callback of the file group, its op still counts, so this never closes the file right away */
static void conn_file_release(struct poor_loop *loop, struct conn *c)
{
	if (!c->file_held)
		return;
	c->file_held = false;
	if (poor_loop_group_release(&c->file))
		poor_loop_group_call_done(conn_file, loop, &c->file);
}

static void conn_wait(struct poor_loop *loop, struct conn *c)
{
	poor_loop_group_timer_arm(loop, &c->sock, &c->timer, poor_loop_now() + c->server->cfg.timeout_ns);
}

/*
 * Cancel what waits on the socket and let pending file operations finish. The extra hold keeps the connection
 * while the timer and the file are let go. May free the connection.
 */
static void conn_end(struct poor_loop *loop, struct conn *c)
{
	if (c->closing)
		return;
	c->closing = true;
	poor_loop_group_acquire(&c->sock);
	if (c->recv_op.pending)
		cancel_op(loop, &c->recv_op);
	if (c->send_op.pending)
		cancel_op(loop, &c->send_op);
	if (c->shutdown_op.pending)
		cancel_op(loop, &c->shutdown_op);
	(void)poor_loop_group_timer_disarm(&c->sock, &c->timer);
	conn_file_release(loop, c);
	conn_put(loop, c);
}

/*
 * Closing a socket with unread data resets the connection, which can destroy the response before the client reads
 * it. So the sending side is shut down, and what the client still sends is dropped until it closes too.
 */
static void conn_linger(struct poor_loop *loop, struct conn *c)
{
	uint64_t timeout = c->server->cfg.timeout_ns < LINGER_NS ? c->server->cfg.timeout_ns : LINGER_NS;

	c->lingering = true;
	poor_loop_group_timer_arm(loop, &c->sock, &c->timer, poor_loop_now() + timeout);
	io_uring_prep_shutdown(sock_sqe(loop, c, &c->shutdown_op), c->sock.fd, SHUT_WR);
	io_uring_prep_recv_array(sock_sqe(loop, c, &c->recv_op), c->sock.fd, c->in, 0);
}

static void conn_send(struct poor_loop *loop, struct conn *c)
{
	io_uring_prep_send_array(sock_sqe(loop, c, &c->send_op), c->sock.fd,
				 arrview_cfront(c->out_sent, arrview_first(c->out_len, c->out)),
				 MSG_NOSIGNAL | (c->left ? MSG_MORE : 0));
}

static void conn_status(struct conn *c, int status)
{
	char date[HTTP_DATE_SIZE];

	http_format_date(&date, time(nullptr));
	c->status = status;
	c->out_len = c->out_sent = 0;
	out_print(c, "HTTP/1.1 ", status, " ", http_reason(status), "\r\nServer: poor_httpd\r\nDate: ", date, "\r\n");
}

static void conn_end_head(struct conn *c)
{
	out_print(c, "Connection: ", c->keep_alive ? "keep-alive" : "close", "\r\n\r\n");
}

static void conn_error(struct poor_loop *loop, struct conn *c, int status)
{
	const char *reason = http_reason(status);
	char page[256];
	int len = sprint_array(page, "<!DOCTYPE html>\n<title>", status, " ", reason, "</title>\n<h1>", status, " ", reason,
			       "</h1>\n");

	conn_status(c, status);
	if (status == 301)
		out_print(c, "Location: ", fmt_p(c->req.path.ptr, (int)c->req.path.len), "/",
			  fmt_p(c->req.query.ptr, (int)c->req.query.len), "\r\n");
	if (status == 405)
		out_print(c, "Allow: GET, HEAD\r\n");
	out_print(c, "Content-Type: text/html; charset=utf-8\r\nContent-Length: ", len, "\r\n");
	conn_end_head(c);
	c->body_len = 0;
	if (c->req.method != HTTP_HEAD) {
		out_print(c, page);
		c->body_len = len;
	}
	c->left = 0;
	conn_send(loop, c);
}

static void conn_open(struct poor_loop *loop, struct conn *c)
{
	io_uring_prep_openat2(sock_sqe(loop, c, &c->open_op), c->server->root_fd, c->path, &open_how);
}

static void conn_request(struct poor_loop *loop, struct conn *c)
{
	int status = http_parse_request(&c->req, arrview_first(c->head_len, c->in));

	c->busy = true;
	c->keep_alive = c->req.keep_alive && !c->server->stopping;
	conn_wait(loop, c);
	if (!status && c->req.method == HTTP_OTHER)
		status = 405;
	if (!status)
		status = http_decode_path(c->req.path, c->path);
	if (status)
		conn_error(loop, c, status);
	else
		conn_open(loop, c);
}

static void conn_parse(struct poor_loop *loop, struct conn *c)
{
	size_t skip = 0;

	while (skip < c->in_len && (c->in[skip] == '\r' || c->in[skip] == '\n'))
		skip++;
	if (skip && skip < c->in_len)
		array_remove_view(arrview_first(c->in_len, c->in), arrview_first(skip, c->in));
	c->in_len -= skip;
	if (c->in_len)
		c->head_len = http_head_length(arrview_first(c->in_len, c->in), &c->scanned);
	if (c->head_len) {
		conn_request(loop, c);
	} else if (c->in_len == IN_SIZE) {
		c->keep_alive = false;
		conn_error(loop, c, 431);
	} else {
		io_uring_prep_recv_array(sock_sqe(loop, c, &c->recv_op), c->sock.fd, arrview_cfront(c->in_len, c->in), 0);
	}
}

static void conn_next(struct poor_loop *loop, struct conn *c)
{
	c->req.line.len = 0;
	c->req.method = HTTP_OTHER;
	c->head_len = c->scanned = 0;
	c->index = false;
	conn_wait(loop, c);
	conn_parse(loop, c);
}

static void conn_log(struct conn *c)
{
	char when[HTTP_LOG_DATE_SIZE];

	c->server->requests++;
	c->server->bytes += c->body_len;
	if (c->server->cfg.quiet)
		return;
	http_format_log_date(&when, time(nullptr));
	if (c->req.line.len)
		log_println(c->peer, " - - [", when, "] \"", fmt_p(c->req.line.ptr, (int)c->req.line.len), "\" ", c->status, " ",
			    c->body_len);
	else
		log_println(c->peer, " - - [", when, "] \"-\" ", c->status, " ", c->body_len);
}

static void conn_done(struct poor_loop *loop, struct conn *c)
{
	conn_log(c);
	c->busy = false;
	if (!c->keep_alive || c->server->stopping) {
		conn_linger(loop, c);
		return;
	}
	if (c->head_len < c->in_len)
		array_remove_view(arrview_first(c->in_len, c->in), arrview_first(c->head_len, c->in));
	c->in_len -= c->head_len;
	conn_next(loop, c);
}

static void conn_read(struct poor_loop *loop, struct conn *c)
{
	size_t room = OUT_SIZE - c->out_len, len = c->left < room ? c->left : room;

	io_uring_prep_read_array(file_sqe(loop, c, &c->read_op), c->file.fd, arrview(c->out_len, len, c->out), c->offset);
}

static bool conn_unmodified(const struct conn *c)
{
	return c->req.if_modified_since >= 0 && c->stx.stx_mtime.tv_sec <= c->req.if_modified_since;
}

/* A Range only counts if an If-Range next to it names the current modification time */
static int conn_range(const struct conn *c, const char (*modified)[HTTP_DATE_SIZE], uint64_t *first, uint64_t *last)
{
	if (!c->req.range.len || (c->req.if_range.len && (c->req.if_range.len != strlen(*modified) ||
							  memcmp(c->req.if_range.ptr, *modified, c->req.if_range.len))))
		return 0;
	return http_parse_range(c->req.range, c->stx.stx_size, first, last);
}

static void conn_respond(struct poor_loop *loop, struct conn *c)
{
	uint64_t size = c->stx.stx_size, first = 0, last = size - 1;
	char modified[HTTP_DATE_SIZE];
	int range = 0;

	http_format_date(&modified, c->stx.stx_mtime.tv_sec);
	if (!conn_unmodified(c))
		range = conn_range(c, &modified, &first, &last);
	conn_status(c, conn_unmodified(c) ? 304 : range > 0 ? 206 : range < 0 ? 416 : 200);
	out_print(c, "Last-Modified: ", modified, "\r\nAccept-Ranges: bytes\r\n");
	c->body_len = 0;
	if (c->status == 416) {
		out_print(c, "Content-Range: bytes */", size, "\r\nContent-Length: 0\r\n");
	} else if (c->status != 304) {
		if (c->status == 206)
			out_print(c, "Content-Range: bytes ", first, "-", last, "/", size, "\r\n");
		c->body_len = size ? last - first + 1 : 0;
		out_print(c, "Content-Type: ", http_content_type(c->path), "\r\nContent-Length: ", c->body_len, "\r\n");
	}
	conn_end_head(c);
	if (c->req.method == HTTP_HEAD)
		c->body_len = 0;
	c->offset = first;
	c->left = c->body_len;
	if (!c->left) {
		conn_file_release(loop, c);
		conn_send(loop, c);
		return;
	}
	conn_read(loop, c);
}

/* A directory is served by its index.html, under a path that ends with a slash so relative links work */
static void conn_directory(struct poor_loop *loop, struct conn *c)
{
	size_t len = strcmp(c->path, ".") ? strlen(c->path) : 0;
	int ret;

	conn_file_release(loop, c);
	if (c->index) {
		conn_error(loop, c, 403);
		return;
	}
	if (c->req.path.ptr[c->req.path.len - 1] != '/') {
		conn_error(loop, c, 301);
		return;
	}
	ret = sprint_array(arrview_cfront(len, c->path), len ? "/" : "", "index.html");
	if ((size_t)ret >= PATH_SIZE - len) {
		conn_error(loop, c, 414);
		return;
	}
	c->index = true;
	conn_open(loop, c);
}

POOR_LOOP_GROUP_OP(on_recv, loop, sock, op, cqe)
{
	struct conn *c = conn_of_sock(sock);

	if (c->closing)
		return;
	if (cqe->res <= 0) {
		conn_end(loop, c);
	} else if (c->lingering) {
		io_uring_prep_recv_array(sock_sqe(loop, c, &c->recv_op), c->sock.fd, c->in, 0);
	} else {
		c->in_len += cqe->res;
		conn_parse(loop, c);
	}
}

POOR_LOOP_GROUP_OP(on_shutdown, loop, sock, op, cqe)
{
	if (cqe->res < 0)
		conn_end(loop, conn_of_sock(sock));
}

/* A UNIX socket from systemd has no TCP_NODELAY, so the result is ignored */
POOR_LOOP_GROUP_OP(on_nodelay, loop, sock, op, cqe)
{
}

POOR_LOOP_GROUP_OP(on_open, loop, sock, op, cqe)
{
	struct conn *c = conn_of_sock(sock);

	if (c->closing) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(loop, cqe->res, IOSQE_ASYNC);
		return;
	}
	if (cqe->res < 0) {
		conn_error(loop, c, c->index && cqe->res == -ENOENT ? 403 : http_open_status(-cqe->res));
		return;
	}
	poor_loop_fd_init(&c->file, cqe->res);
	poor_loop_group_acquire(sock);
	poor_loop_group_acquire(&c->file);
	c->file_held = true;
	io_uring_prep_statx(file_sqe(loop, c, &c->stat_op), c->file.fd, "", AT_EMPTY_PATH, STATX_TYPE | STATX_SIZE | STATX_MTIME,
			    &c->stx);
}

POOR_LOOP_GROUP_OP(on_stat, loop, file, op, cqe)
{
	struct conn *c = conn_of_file(file);

	if (c->closing)
		return;
	if (cqe->res < 0) {
		conn_file_release(loop, c);
		conn_error(loop, c, 500);
	} else if (S_ISDIR(c->stx.stx_mode)) {
		conn_directory(loop, c);
	} else if (!S_ISREG(c->stx.stx_mode)) {
		conn_file_release(loop, c);
		conn_error(loop, c, 403);
	} else {
		conn_respond(loop, c);
	}
}

/* The response head promised the length, so a file that shrank ends the connection */
POOR_LOOP_GROUP_OP(on_read, loop, file, op, cqe)
{
	struct conn *c = conn_of_file(file);

	if (c->closing)
		return;
	if (cqe->res <= 0) {
		log_println(c->peer, ": ", c->path, ": ", cqe->res ? strerror(-cqe->res) : "file got shorter");
		conn_end(loop, c);
		return;
	}
	c->out_len += cqe->res;
	c->offset += cqe->res;
	c->left -= cqe->res;
	if (!c->left)
		conn_file_release(loop, c);
	conn_send(loop, c);
}

POOR_LOOP_GROUP_OP(on_send, loop, sock, op, cqe)
{
	struct conn *c = conn_of_sock(sock);

	if (c->closing)
		return;
	if (cqe->res < 0) {
		conn_end(loop, c);
		return;
	}
	conn_wait(loop, c);
	c->out_sent += cqe->res;
	if (c->out_sent < c->out_len) {
		conn_send(loop, c);
		return;
	}
	c->out_len = c->out_sent = 0;
	if (c->left)
		conn_read(loop, c);
	else
		conn_done(loop, c);
}

POOR_LOOP_GROUP_TIMER(on_timeout, loop, sock, timer)
{
	struct conn *c = conn_of_sock(sock);

	if (!c->lingering && (c->busy || c->in_len))
		log_println(c->peer, ": timed out");
	conn_end(loop, c);
}

POOR_LOOP_FD_DONE(conn_file, loop, file)
{
	conn_put(loop, conn_of_file(file));
}

POOR_LOOP_FD_DONE(conn_sock, loop, sock)
{
	struct conn *c = conn_of_sock(sock);
	struct server *s = c->server;

	poor_list_remove(&s->conns, c);
	s->nconns--;
	free(c);
	listeners_resume(loop, s);
	server_release(loop, s);
}

void conn_start(struct poor_loop *loop, struct server *s, int fd, const struct sockaddr *peer)
{
	struct conn *c = malloc(sizeof(*c));

	if (!c) {
		log_println("out of memory");
		poor_loop_close_fd_or_submit(loop, fd, IOSQE_ASYNC);
		return;
	}
	poor_loop_fd_init(&c->sock, fd);
	c->server = s;
	poor_loop_op_init(&c->recv_op, on_recv);
	poor_loop_op_init(&c->send_op, on_send);
	poor_loop_op_init(&c->open_op, on_open);
	poor_loop_op_init(&c->shutdown_op, on_shutdown);
	poor_loop_op_init(&c->nodelay_op, on_nodelay);
	poor_loop_op_init(&c->stat_op, on_stat);
	poor_loop_op_init(&c->read_op, on_read);
	poor_loop_timer_init(&c->timer, on_timeout);
	c->in_len = c->out_len = c->out_sent = 0;
	c->busy = c->lingering = c->closing = c->file_held = false;
	http_format_addr(&c->peer, peer, false);
	poor_list_append(&s->conns, c);
	s->nconns++;
	server_acquire(s);
	/* Without TCP_NODELAY the end of a response waits for an ACK of what came before it, and clients delay ACKs */
	io_uring_prep_cmd_sock_array(sock_sqe(loop, c, &c->nodelay_op), SOCKET_URING_OP_SETSOCKOPT, fd, IPPROTO_TCP, TCP_NODELAY,
				     array_ptr(&one));
	conn_next(loop, c);
}

void conns_stop(struct poor_loop *loop, struct server *s, bool force)
{
	poor_list_foreach_safe(&s->conns, c) {
		if (force || !c->busy)
			conn_end(loop, c);
		else
			c->keep_alive = false;
	}
}
