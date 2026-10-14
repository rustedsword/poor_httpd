/* SPDX-License-Identifier: MIT */
#ifndef POOR_HTTPD_LOG_H
#define POOR_HTTPD_LOG_H

#include <poor_array.h>
#include <poor_loop.h>
#include <poor_stdio.h>
#include <stdarg.h>

/*
 * Asynchronous logger. log_println() takes the same arguments as println() and formats the line into one buffer
 * while the loop writes the other one. A line that doesn't fit is dropped and counted.
 */
constexpr size_t LOG_BUF_SIZE = 65536;

struct log_buf {
	size_t len;
	char data[LOG_BUF_SIZE];
};

struct logger {
	struct poor_loop *loop;
	struct poor_loop_op op;
	struct log_buf *fill, *busy;
	size_t written;
	uint64_t dropped;
	int fd;
	struct log_buf bufs[2];
};

extern struct logger logger;

#define log_println(...) log_commit(sprintln_array(arrview_cfront(logger.fill->len, logger.fill->data), __VA_ARGS__))

/* Log to 'fd'. Call it again only when nothing is pending. */
void log_init(struct poor_loop *loop, int fd);
void log_commit(int len);
bool log_pending(void);

/* A poor_loop_log_fn */
void log_poor_loop(struct poor_loop *loop, const char *fmt, va_list ap);

#endif
