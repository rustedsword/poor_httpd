/* SPDX-License-Identifier: MIT */
#ifndef POOR_HTTPD_H
#define POOR_HTTPD_H

#include <errno.h>
#include <poor_list.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_loop_group.h>
#include <sys/socket.h>

constexpr unsigned MAX_LISTEN = 16;
constexpr int LISTEN_FDS_START = 3;

struct listen_addr {
	struct sockaddr_storage addr;
	socklen_t len;
};

/* 'nfds' sockets from systemd socket activation start at LISTEN_FDS_START */
struct config {
	const char *root;
	uint64_t timeout_ns, grace_ns;
	unsigned max_conns, nlisten, nfds;
	bool quiet;
	struct listen_addr listen[MAX_LISTEN];
};

poor_loop_group_declare(server_io);
poor_list_declare(listener_list);
poor_list_declare(conn_list);

/*
 * The server group is held by the root directory open, every listener and every connection. Once all of them are
 * gone, the server is done and stops the loop.
 */
struct server {
	server_io io;
	struct poor_loop_op root_op;
	struct poor_loop_timer grace;
	listener_list listeners;
	conn_list conns;
	struct config cfg;
	uint64_t requests, bytes;
	unsigned nconns;
	int root_fd, status;
	bool stopping, done;
};

/* Report a failure of the loop itself and exit: there is no asynchronous way left to report it */
[[noreturn]] void loop_failed(const char *what, int err);

static inline struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe)
		loop_failed("io_uring_submit", errno);
	return sqe;
}

static inline void cancel_op(struct poor_loop *loop, struct poor_loop_op *op)
{
	io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(loop)), op, 0);
}

void server_init(struct server *s);
void server_start(struct poor_loop *loop, struct server *s);

/* Stop accepting and let responses in progress finish, or with 'force' close all connections */
void server_shutdown(struct poor_loop *loop, struct server *s, bool force);
void server_fail(struct poor_loop *loop, struct server *s);
void server_acquire(struct server *s);
void server_release(struct poor_loop *loop, struct server *s);

void listeners_start(struct poor_loop *loop, struct server *s);
void listeners_stop(struct poor_loop *loop, struct server *s);
void listeners_resume(struct poor_loop *loop, struct server *s);

void conn_start(struct poor_loop *loop, struct server *s, int fd, const struct sockaddr *peer);
void conns_stop(struct poor_loop *loop, struct server *s, bool force);

#endif
