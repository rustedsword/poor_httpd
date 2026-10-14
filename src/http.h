/* SPDX-License-Identifier: MIT */
#ifndef POOR_HTTPD_HTTP_H
#define POOR_HTTPD_HTTP_H

#include <netinet/in.h>
#include <poor_array.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

constexpr size_t HTTP_DATE_SIZE = sizeof("Sun, 06 Nov 1994 08:49:37 GMT");
constexpr size_t HTTP_LOG_DATE_SIZE = sizeof("06/Nov/1994:08:49:37 +0000");
constexpr size_t HTTP_ADDR_SIZE = INET6_ADDRSTRLEN + sizeof("[]:65535");

/* A part of a request head */
struct http_str {
	const char *ptr;
	size_t len;
};

enum http_method { HTTP_GET, HTTP_HEAD, HTTP_OTHER };

/* 'if_modified_since' is -1 if absent. 'path' starts with a slash and 'query' with a question mark, if present. */
struct http_request {
	struct http_str line, path, query, range, if_range;
	time_t if_modified_since;
	enum http_method method;
	bool keep_alive;
};

/*
 * Return the length of the request head in 'buf', up to and including the empty line, or 0 if it is incomplete.
 * Scanning resumes at '*scanned', so pass the same buffer again as it grows.
 */
size_t _http_head_length(size_t len, const char (*buf)[len], size_t *scanned);
#define http_head_length(buf, scanned) _http_head_length(ARRAY_SIZE(buf), &auto_arr(buf), scanned)

/*
 * Parse a request head. Returns 0, or the status of the error response. 'req->line' is set if the request line
 * is valid, so it can be logged.
 */
int _http_parse_request(struct http_request *req, size_t len, const char (*head)[len]);
#define http_parse_request(req, head) _http_parse_request(req, ARRAY_SIZE(head), &auto_arr(head))

/*
 * Decode a request path into 'out' as a NUL-terminated path relative to the served directory, for openat2():
 * percent-decoded, with dot segments removed and without the leading slash, or "." for the directory itself.
 * Returns 0, or the status of the error response.
 */
int _http_decode_path(struct http_str path, size_t size, char (*out)[size]);
#define http_decode_path(path, out) _http_decode_path(path, ARRAY_SIZE(out), &auto_arr(out))

/*
 * Parse a Range header value for a body of 'size' bytes. Returns 1 for a single satisfiable range [*first, *last],
 * 0 if the header must be ignored, e.g. for several ranges, or -1 if the range can't be satisfied.
 */
int http_parse_range(struct http_str value, uint64_t size, uint64_t *first, uint64_t *last);

/* Parse an IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT". Returns -1 for anything else. */
time_t http_parse_date(struct http_str value);

void http_format_date(char (*buf)[HTTP_DATE_SIZE], time_t t);
void http_format_log_date(char (*buf)[HTTP_LOG_DATE_SIZE], time_t t);
void http_format_addr(char (*buf)[HTTP_ADDR_SIZE], const struct sockaddr *addr, bool port);

const char *http_reason(int status);
const char *http_content_type(const char *path);

/* Return the response status for a failed open of the requested file */
int http_open_status(int err);

#endif
