/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_loop_uring_array.h>

#include "httpd.h"
#include "log.h"

struct logger logger;

static void log_write(void)
{
	struct log_buf *b = logger.busy;

	io_uring_prep_write_array(need_sqe(poor_loop_get_sqe_or_submit(logger.loop, &logger.op)), logger.fd,
				  arrview_cfront(logger.written, arrview_first(b->len, b->data)), -1);
}

static void log_flush(void)
{
	if (logger.op.pending || !logger.fill->len)
		return;
	logger.busy = logger.fill;
	logger.fill = logger.busy == &logger.bufs[0] ? &logger.bufs[1] : &logger.bufs[0];
	logger.written = 0;
	log_write();
}

/* A failed write drops the rest of the buffer: there is nowhere to report it */
static void on_write(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	uint64_t dropped = logger.dropped;

	if (cqe->res > 0 && (logger.written += cqe->res) < logger.busy->len) {
		log_write();
		return;
	}
	logger.busy->len = 0;
	if (dropped) {
		logger.dropped = 0;
		log_println("log: dropped ", dropped, " lines");
	}
	log_flush();
}

void log_init(struct poor_loop *loop, int fd)
{
	logger.loop = loop;
	poor_loop_op_init(&logger.op, on_write);
	logger.fill = &logger.bufs[0];
	logger.bufs[0].len = logger.bufs[1].len = 0;
	logger.dropped = 0;
	logger.fd = fd;
}

/* The fill buffer always keeps a byte free, for the terminating NUL and so its view is never empty */
void log_commit(int len)
{
	if (len < 0 || (size_t)len >= LOG_BUF_SIZE - logger.fill->len) {
		logger.dropped++;
		return;
	}
	logger.fill->len += len;
	log_flush();
}

bool log_pending(void)
{
	return logger.op.pending || logger.fill->len;
}

void log_poor_loop(struct poor_loop *, const char *fmt, va_list ap)
{
	char msg[256];

	vsnprintf_array(msg, fmt, ap);
	log_println("poor_loop: ", msg);
}
