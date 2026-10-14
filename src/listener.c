/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_array.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "httpd.h"
#include "log.h"

constexpr uint64_t ACCEPT_RETRY_NS = 100'000'000;

/*
 * A listening socket. Its fd group holds the setup chain, the accept, the retry timer and, while the server has as
 * many connections as allowed, a pause; the socket is closed once none of them is left.
 */
poor_loop_fd_declare(listener_sock);

struct listener {
	listener_sock sock;
	struct poor_list_node link;
	struct server *server;
	struct poor_loop_op socket_op, opt_op, bind_op, listen_op, accept_op;
	struct poor_loop_timer retry;
	struct sockaddr_storage addr, peer;
	socklen_t addr_len, peer_len;
	const char *step;
	int err;
	bool ready, paused;
	char name[HTTP_ADDR_SIZE];
};

poor_list_define(listener_list, struct listener, link);
poor_loop_group_attach(listener_sock, struct listener, sock, opt_op, on_opt);
poor_loop_group_attach(listener_sock, struct listener, sock, bind_op, on_bind);
poor_loop_group_attach(listener_sock, struct listener, sock, listen_op, on_listen);
poor_loop_group_attach(listener_sock, struct listener, sock, accept_op, on_accept);
poor_loop_group_attach(listener_sock, struct listener, sock, retry, on_retry);

static void on_socket(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe);

static struct listener *listener_of(listener_sock *sock)
{
	return container_of(sock, struct listener, sock);
}

static struct listener *listener_new(struct server *s)
{
	struct listener *l = malloc(sizeof(*l));

	if (!l)
		return nullptr;
	l->server = s;
	poor_loop_op_init(&l->socket_op, on_socket);
	poor_loop_op_init(&l->opt_op, on_opt);
	poor_loop_op_init(&l->bind_op, on_bind);
	poor_loop_op_init(&l->listen_op, on_listen);
	poor_loop_op_init(&l->accept_op, on_accept);
	poor_loop_timer_init(&l->retry, on_retry);
	l->err = 0;
	l->ready = l->paused = false;
	poor_list_append(&s->listeners, l);
	server_acquire(s);
	return l;
}

static void listener_free(struct poor_loop *loop, struct listener *l)
{
	struct server *s = l->server;

	poor_list_remove(&s->listeners, l);
	free(l);
	server_release(loop, s);
}

static void listener_put(struct poor_loop *loop, struct listener *l)
{
	if (poor_loop_group_release(&l->sock))
		poor_loop_group_call_done(listener_sock, loop, &l->sock);
}

/* With the server full the listener pauses, holding its group, until a connection is done */
static void listener_accept(struct poor_loop *loop, struct listener *l)
{
	struct server *s = l->server;

	if (s->stopping)
		return;
	if (s->nconns >= s->cfg.max_conns) {
		if (!l->paused)
			poor_loop_group_acquire(&l->sock);
		l->paused = true;
		return;
	}
	l->peer_len = sizeof(l->peer);
	io_uring_prep_accept(need_sqe(poor_loop_group_get_sqe_or_submit(loop, &l->sock, &l->accept_op)), l->sock.fd,
			     (struct sockaddr *)&l->peer, &l->peer_len, SOCK_CLOEXEC);
}

static void listener_step(struct listener *l, const char *step, int res)
{
	if (res < 0 && !l->err) {
		l->err = res;
		l->step = step;
	}
}

/* The setup steps are linked, so they reach the kernel together and a failure cancels the rest */
static void on_socket(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	static int one = 1;
	struct listener *l = container_of(op, struct listener, socket_op);
	struct io_uring_sqe *sqe;
	int ret;

	if (cqe->res < 0 || l->server->stopping) {
		if (cqe->res < 0) {
			log_println(l->name, ": socket: ", strerror(-cqe->res));
			server_fail(loop, l->server);
		} else {
			poor_loop_close_fd_or_submit(loop, cqe->res, 0);
		}
		listener_free(loop, l);
		return;
	}
	poor_loop_fd_init(&l->sock, cqe->res);
	ret = poor_loop_check_sq_space_or_submit(loop, 3);
	if (ret)
		loop_failed("io_uring_submit", -ret);
	sqe = poor_loop_group_get_sqe(loop, &l->sock, &l->opt_op);
	io_uring_prep_cmd_sock_array(sqe, SOCKET_URING_OP_SETSOCKOPT, l->sock.fd, SOL_SOCKET, SO_REUSEADDR, array_ptr(&one));
	sqe->flags |= IOSQE_IO_LINK;
	sqe = poor_loop_group_get_sqe(loop, &l->sock, &l->bind_op);
	io_uring_prep_bind(sqe, l->sock.fd, (struct sockaddr *)&l->addr, l->addr_len);
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_prep_listen(poor_loop_group_get_sqe(loop, &l->sock, &l->listen_op), l->sock.fd, SOMAXCONN);
}

POOR_LOOP_GROUP_OP(on_opt, loop, sock, op, cqe)
{
	listener_step(listener_of(sock), "setsockopt", cqe->res);
}

POOR_LOOP_GROUP_OP(on_bind, loop, sock, op, cqe)
{
	listener_step(listener_of(sock), "bind", cqe->res);
}

POOR_LOOP_GROUP_OP(on_listen, loop, sock, op, cqe)
{
	struct listener *l = listener_of(sock);

	listener_step(l, "listen", cqe->res);
	if (l->err) {
		log_println(l->name, ": ", l->step, ": ", strerror(-l->err));
		server_fail(loop, l->server);
		return;
	}
	log_println("listening on ", l->name);
	l->ready = true;
	listener_accept(loop, l);
}

/*
 * Running out of fds or memory makes accept fail at once while a connection is queued, so it is retried later.
 * Errors of a pending connection are retried right away, and errors of the socket itself end the server.
 */
POOR_LOOP_GROUP_OP(on_accept, loop, sock, op, cqe)
{
	struct listener *l = listener_of(sock);
	struct server *s = l->server;

	switch (cqe->res) {
	case -ECANCELED:
		return;
	case -EMFILE:
	case -ENFILE:
	case -ENOBUFS:
	case -ENOMEM:
		log_println(l->name, ": accept: ", strerror(-cqe->res));
		poor_loop_group_timer_arm(loop, sock, &l->retry, poor_loop_now() + ACCEPT_RETRY_NS);
		return;
	case -EBADF:
	case -EINVAL:
	case -ENOTSOCK:
	case -EOPNOTSUPP:
		log_println(l->name, ": accept: ", strerror(-cqe->res));
		server_fail(loop, s);
		return;
	}
	if (cqe->res >= 0 && s->stopping)
		poor_loop_close_fd_or_submit(loop, cqe->res, 0);
	else if (cqe->res >= 0)
		conn_start(loop, s, cqe->res, (struct sockaddr *)&l->peer);
	listener_accept(loop, l);
}

POOR_LOOP_GROUP_TIMER(on_retry, loop, sock, timer)
{
	listener_accept(loop, listener_of(sock));
}

POOR_LOOP_FD_DONE(listener_sock, loop, sock)
{
	listener_free(loop, listener_of(sock));
}

static void listener_open(struct poor_loop *loop, struct server *s, const struct listen_addr *addr)
{
	struct listener *l = listener_new(s);

	if (!l) {
		log_println("out of memory");
		server_fail(loop, s);
		return;
	}
	l->addr = addr->addr;
	l->addr_len = addr->len;
	http_format_addr(&l->name, (struct sockaddr *)&l->addr, true);
	io_uring_prep_socket(need_sqe(poor_loop_get_sqe_or_submit(loop, &l->socket_op)), l->addr.ss_family,
			     SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
}

static void listener_adopt(struct poor_loop *loop, struct server *s, int fd)
{
	struct listener *l = listener_new(s);

	if (!l) {
		log_println("out of memory");
		server_fail(loop, s);
		return;
	}
	poor_loop_fd_init(&l->sock, fd);
	sprint_array(l->name, "fd ", fd);
	log_println("listening on ", l->name);
	l->ready = true;
	listener_accept(loop, l);
}

void listeners_start(struct poor_loop *loop, struct server *s)
{
	for (unsigned i = 0; i < s->cfg.nlisten; i++)
		listener_open(loop, s, &s->cfg.listen[i]);
	for (unsigned i = 0; i < s->cfg.nfds; i++)
		listener_adopt(loop, s, LISTEN_FDS_START + i);
}

/* A listener that is still being set up stops once its setup completes */
void listeners_stop(struct poor_loop *loop, struct server *s)
{
	poor_list_foreach_safe(&s->listeners, l) {
		if (!l->ready)
			continue;
		if (l->accept_op.pending) {
			cancel_op(loop, &l->accept_op);
		} else if (l->paused) {
			l->paused = false;
			listener_put(loop, l);
		} else if (poor_loop_group_timer_disarm(&l->sock, &l->retry)) {
			poor_loop_group_call_done(listener_sock, loop, &l->sock);
		}
	}
}

void listeners_resume(struct poor_loop *loop, struct server *s)
{
	if (s->stopping)
		return;
	poor_list_foreach_safe(&s->listeners, l) {
		if (!l->paused)
			continue;
		l->paused = false;
		listener_accept(loop, l);
		listener_put(loop, l);
	}
}
