/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <string.h>

#include "httpd.h"
#include "log.h"

poor_loop_group_attach(server_io, struct server, io, root_op, on_root);

static struct server *server_of(server_io *io)
{
	return container_of(io, struct server, io);
}

static void on_grace(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct server *s = container_of(timer, struct server, grace);

	log_println("closing ", s->nconns, " connections");
	conns_stop(loop, s, true);
}

void server_init(struct server *s)
{
	poor_loop_group_init(&s->io);
	poor_loop_op_init(&s->root_op, on_root);
	poor_loop_timer_init(&s->grace, on_grace);
	poor_list_init(&s->listeners);
	poor_list_init(&s->conns);
	s->requests = s->bytes = 0;
	s->nconns = 0;
	s->root_fd = -1;
	s->status = 0;
	s->stopping = s->done = false;
}

void server_start(struct poor_loop *loop, struct server *s)
{
	io_uring_prep_openat(need_sqe(poor_loop_group_get_sqe_or_submit(loop, &s->io, &s->root_op)), AT_FDCWD, s->cfg.root,
			     O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
}

POOR_LOOP_GROUP_OP(on_root, loop, io, op, cqe)
{
	struct server *s = server_of(io);

	if (cqe->res < 0) {
		log_println(s->cfg.root, ": ", strerror(-cqe->res));
		s->status = 1;
		return;
	}
	s->root_fd = cqe->res;
	if (s->stopping)
		return;
	log_println("serving ", s->cfg.root);
	listeners_start(loop, s);
}

/* The grace timer is armed first, so a server that gets done right away disarms it */
void server_shutdown(struct poor_loop *loop, struct server *s, bool force)
{
	if (s->done)
		return;
	if (!s->stopping) {
		s->stopping = true;
		poor_loop_timer_arm(loop, &s->grace, poor_loop_now() + s->cfg.grace_ns);
		listeners_stop(loop, s);
	}
	conns_stop(loop, s, force);
}

void server_fail(struct poor_loop *loop, struct server *s)
{
	s->status = 1;
	server_shutdown(loop, s, false);
}

void server_acquire(struct server *s)
{
	poor_loop_group_acquire(&s->io);
}

void server_release(struct poor_loop *loop, struct server *s)
{
	if (poor_loop_group_release(&s->io))
		poor_loop_group_call_done(server_io, loop, &s->io);
}

POOR_LOOP_GROUP_DONE(server_io, loop, io)
{
	struct server *s = server_of(io);

	s->done = true;
	poor_loop_timer_disarm(&s->grace);
	if (s->root_fd >= 0) {
		poor_loop_close_fd_or_submit(loop, s->root_fd, 0);
		log_println("served ", s->requests, " requests, ", s->bytes, " bytes");
	}
	poor_loop_stop(loop);
}
