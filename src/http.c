/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <poor_stdio.h>
#include <string.h>
#include <strings.h>

#include "http.h"

static constexpr bool delimiter[128] = {
	['"'] = true, ['('] = true,  [')'] = true, [','] = true, ['/'] = true, [':'] = true,
	[';'] = true, ['<'] = true,  ['='] = true, ['>'] = true, ['?'] = true, ['@'] = true,
	['['] = true, ['\\'] = true, [']'] = true, ['{'] = true, ['}'] = true,
};

static bool is_tchar(unsigned char ch)
{
	return ch > ' ' && ch < 0x7f && !delimiter[ch];
}

static bool is_token(struct http_str s)
{
	if (!s.len)
		return false;
	for (size_t i = 0; i < s.len; i++)
		if (!is_tchar(s.ptr[i]))
			return false;
	return true;
}

static bool is_value(struct http_str s)
{
	for (size_t i = 0; i < s.len; i++)
		if (((unsigned char)s.ptr[i] < ' ' && s.ptr[i] != '\t') || s.ptr[i] == 0x7f)
			return false;
	return true;
}

static bool is_target(struct http_str s)
{
	if (!s.len)
		return false;
	for (size_t i = 0; i < s.len; i++)
		if ((unsigned char)s.ptr[i] <= ' ' || (unsigned char)s.ptr[i] >= 0x7f)
			return false;
	return true;
}

static bool str_eq(struct http_str s, const char *lit)
{
	return s.len == strlen(lit) && !memcmp(s.ptr, lit, s.len);
}

static bool str_is(struct http_str s, const char *lit)
{
	return s.len == strlen(lit) && !strncasecmp(s.ptr, lit, s.len);
}

static bool str_has_prefix(struct http_str s, const char *prefix)
{
	size_t len = strlen(prefix);

	return s.len >= len && !strncasecmp(s.ptr, prefix, len);
}

static struct http_str str_from(struct http_str s, size_t off)
{
	return (struct http_str){ s.ptr + off, s.len - off };
}

static struct http_str str_trim(struct http_str s)
{
	while (s.len && (*s.ptr == ' ' || *s.ptr == '\t'))
		s = str_from(s, 1);
	while (s.len && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t'))
		s.len--;
	return s;
}

/* Split 's' at the first 'sep': return the part before it and leave the rest in 's' */
static struct http_str str_split(struct http_str *s, char sep)
{
	const char *at = memchr(s->ptr, sep, s->len);
	struct http_str head = { s->ptr, at ? (size_t)(at - s->ptr) : s->len };

	*s = at ? str_from(*s, head.len + 1) : str_from(*s, s->len);
	return head;
}

static bool str_to_u64(struct http_str s, uint64_t *v)
{
	if (!s.len)
		return false;
	*v = 0;
	for (size_t i = 0; i < s.len; i++) {
		if (s.ptr[i] < '0' || s.ptr[i] > '9' || *v > (UINT64_MAX - 9) / 10)
			return false;
		*v = *v * 10 + (s.ptr[i] - '0');
	}
	return true;
}

size_t _http_head_length(size_t len, const char (*buf)[len], size_t *scanned)
{
	for (const char *end = *buf + len, *at = *buf + *scanned; (at = memchr(at, '\n', end - at)); at++) {
		size_t i = at - *buf;

		if ((i >= 1 && (*buf)[i - 1] == '\n') || (i >= 2 && (*buf)[i - 1] == '\r' && (*buf)[i - 2] == '\n'))
			return i + 1;
	}
	*scanned = len;
	return 0;
}

/* The head ends with an empty line, so there is always a line feed ahead */
static struct http_str next_line(struct http_str *head)
{
	struct http_str line = str_split(head, '\n');

	if (line.len && line.ptr[line.len - 1] == '\r')
		line.len--;
	return line;
}

static int parse_request_line(struct http_request *req, struct http_str line, bool *http11)
{
	struct http_str rest = line, method = str_split(&rest, ' '), target = str_split(&rest, ' '), version = rest;
	const char *question;

	if (!is_token(method) || !is_target(target) || version.len != 8 || memcmp(version.ptr, "HTTP/", 5) ||
	    version.ptr[5] < '0' || version.ptr[5] > '9' || version.ptr[6] != '.' || version.ptr[7] < '0' || version.ptr[7] > '9')
		return 400;
	req->line = line;
	if (version.ptr[5] != '1')
		return 505;
	*http11 = version.ptr[7] != '0';
	if (str_eq(method, "GET"))
		req->method = HTTP_GET;
	else if (str_eq(method, "HEAD"))
		req->method = HTTP_HEAD;
	/* An absolute-form target, sent to proxies, carries the scheme and the authority before the path */
	if (str_has_prefix(target, "http://") || str_has_prefix(target, "https://")) {
		target = str_from(target, str_has_prefix(target, "http://") ? 7 : 8);
		while (target.len && *target.ptr != '/' && *target.ptr != '?')
			target = str_from(target, 1);
		if (!target.len || *target.ptr == '?') {
			req->path = (struct http_str){ "/", 1 };
			req->query = target;
			return 0;
		}
	}
	if (*target.ptr != '/')
		return 400;
	question = memchr(target.ptr, '?', target.len);
	req->path = (struct http_str){ target.ptr, question ? (size_t)(question - target.ptr) : target.len };
	req->query = str_from(target, req->path.len);
	return 0;
}

static void parse_connection(struct http_str value, bool *close, bool *keep_alive)
{
	while (value.len) {
		struct http_str option = str_trim(str_split(&value, ','));

		*close |= str_is(option, "close");
		*keep_alive |= str_is(option, "keep-alive");
	}
}

int _http_parse_request(struct http_request *req, size_t len, const char (*head)[len])
{
	struct http_str rest = { *head, len }, field, name, value;
	bool http11 = false, close = false, keep_alive = false;
	unsigned hosts = 0;
	uint64_t length;
	int status;

	req->line = req->path = req->query = req->range = req->if_range = (struct http_str){ "", 0 };
	req->if_modified_since = -1;
	req->method = HTTP_OTHER;
	req->keep_alive = false;
	status = parse_request_line(req, next_line(&rest), &http11);
	if (status)
		return status;
	while ((field = next_line(&rest)).len) {
		value = field;
		name = str_split(&value, ':');
		value = str_trim(value);
		if (name.len == field.len || !is_token(name) || !is_value(value))
			return 400;
		if (str_is(name, "host"))
			hosts++;
		else if (str_is(name, "connection"))
			parse_connection(value, &close, &keep_alive);
		else if (str_is(name, "content-length") && !str_to_u64(value, &length))
			return 400;
		else if (str_is(name, "content-length") && length)
			return 413;
		else if (str_is(name, "transfer-encoding"))
			return 501;
		else if (str_is(name, "if-modified-since"))
			req->if_modified_since = http_parse_date(value);
		else if (str_is(name, "range"))
			req->range = value;
		else if (str_is(name, "if-range"))
			req->if_range = value;
	}
	if (hosts > 1 || (http11 && !hosts))
		return 400;
	req->keep_alive = !close && (http11 || keep_alive);
	return 0;
}

static int hex_digit(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/*
 * A percent-encoded slash separates segments like a plain one: decoding comes first, so "%2e%2e" can't escape the
 * served directory either. openat2() with RESOLVE_BENEATH guards against symlinks that point outside of it.
 */
int _http_decode_path(struct http_str path, size_t size, char (*out)[size])
{
	char decoded[path.len];
	size_t len = 0, start, end = 0, n = 0;

	for (size_t i = 0; i < path.len; i++, n++) {
		decoded[n] = path.ptr[i];
		if (path.ptr[i] != '%')
			continue;
		if (i + 2 >= path.len || hex_digit(path.ptr[i + 1]) < 0 || hex_digit(path.ptr[i + 2]) < 0)
			return 400;
		decoded[n] = (char)(hex_digit(path.ptr[i + 1]) << 4 | hex_digit(path.ptr[i + 2]));
		if (!decoded[n])
			return 400;
		i += 2;
	}
	while (end < n) {
		for (start = end; start < n && decoded[start] == '/'; start++)
			;
		for (end = start; end < n && decoded[end] != '/'; end++)
			;
		if (end == start || (end - start == 1 && decoded[start] == '.'))
			continue;
		if (end - start == 2 && decoded[start] == '.' && decoded[start + 1] == '.') {
			while (len && (*out)[len - 1] != '/')
				len--;
			len -= len > 0;
			continue;
		}
		if (len + 1 + (end - start) >= size)
			return 414;
		if (len)
			(*out)[len++] = '/';
		memcpy(*out + len, decoded + start, end - start);
		len += end - start;
	}
	if (!len)
		(*out)[len++] = '.';
	(*out)[len] = '\0';
	return 0;
}

int http_parse_range(struct http_str value, uint64_t size, uint64_t *first, uint64_t *last)
{
	uint64_t from, to = UINT64_MAX;
	struct http_str start, end;
	const char *dash;

	if (!str_has_prefix(value, "bytes=") || memchr(value.ptr, ',', value.len))
		return 0;
	value = str_trim(str_from(value, 6));
	dash = memchr(value.ptr, '-', value.len);
	if (!dash)
		return 0;
	start = (struct http_str){ value.ptr, (size_t)(dash - value.ptr) };
	end = str_from(value, start.len + 1);
	if (!start.len) {
		if (!str_to_u64(end, &to))
			return 0;
		if (!to || !size)
			return -1;
		*first = to < size ? size - to : 0;
		*last = size - 1;
		return 1;
	}
	if (!str_to_u64(start, &from) || (end.len && (!str_to_u64(end, &to) || to < from)))
		return 0;
	if (from >= size)
		return -1;
	*first = from;
	*last = to < size ? to : size - 1;
	return 1;
}

time_t http_parse_date(struct http_str value)
{
	char date[HTTP_DATE_SIZE];
	const char *end;
	struct tm tm;

	if (value.len != ARRAY_SIZE(date) - 1)
		return -1;
	memcpy(date, value.ptr, value.len);
	date[value.len] = '\0';
	end = strptime(date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
	return end && !*end ? timegm(&tm) : -1;
}

void http_format_date(char (*buf)[HTTP_DATE_SIZE], time_t t)
{
	struct tm tm;

	strftime(*buf, ARRAY_SIZE(buf), "%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&t, &tm));
}

void http_format_log_date(char (*buf)[HTTP_LOG_DATE_SIZE], time_t t)
{
	struct tm tm;

	strftime(*buf, ARRAY_SIZE(buf), "%d/%b/%Y:%H:%M:%S +0000", gmtime_r(&t, &tm));
}

/* IPv4 clients of a dual-stack listener show up as IPv4-mapped IPv6 addresses, print them as IPv4 */
void http_format_addr(char (*buf)[HTTP_ADDR_SIZE], const struct sockaddr *addr, bool port)
{
	const struct sockaddr_in6 *in6 = (const void *)addr;
	const struct sockaddr_in *in = (const void *)addr;
	char host[INET6_ADDRSTRLEN];

	if (addr->sa_family == AF_INET6 && !IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
		inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
		if (port)
			sprint_array(buf, "[", host, "]:", ntohs(in6->sin6_port));
		else
			sprint_array(buf, host);
	} else if (addr->sa_family == AF_INET6 || addr->sa_family == AF_INET) {
		inet_ntop(AF_INET, addr->sa_family == AF_INET ? (const void *)&in->sin_addr : &in6->sin6_addr.s6_addr[12], host,
			  sizeof(host));
		if (port)
			sprint_array(buf, host, ":", ntohs(addr->sa_family == AF_INET ? in->sin_port : in6->sin6_port));
		else
			sprint_array(buf, host);
	} else {
		sprint_array(buf, "local");
	}
}

const char *http_reason(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 206:
		return "Partial Content";
	case 301:
		return "Moved Permanently";
	case 304:
		return "Not Modified";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Content Too Large";
	case 414:
		return "URI Too Long";
	case 416:
		return "Range Not Satisfiable";
	case 431:
		return "Request Header Fields Too Large";
	case 501:
		return "Not Implemented";
	case 503:
		return "Service Unavailable";
	case 505:
		return "HTTP Version Not Supported";
	default:
		return "Internal Server Error";
	}
}

static const struct {
	const char *ext, *type;
} content_types[] = {
	{ "html", "text/html; charset=utf-8" },
	{ "htm", "text/html; charset=utf-8" },
	{ "css", "text/css; charset=utf-8" },
	{ "js", "text/javascript; charset=utf-8" },
	{ "mjs", "text/javascript; charset=utf-8" },
	{ "json", "application/json" },
	{ "map", "application/json" },
	{ "txt", "text/plain; charset=utf-8" },
	{ "md", "text/markdown; charset=utf-8" },
	{ "csv", "text/csv; charset=utf-8" },
	{ "xml", "application/xml" },
	{ "svg", "image/svg+xml" },
	{ "png", "image/png" },
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif", "image/gif" },
	{ "webp", "image/webp" },
	{ "avif", "image/avif" },
	{ "ico", "image/vnd.microsoft.icon" },
	{ "pdf", "application/pdf" },
	{ "wasm", "application/wasm" },
	{ "mp4", "video/mp4" },
	{ "webm", "video/webm" },
	{ "ogg", "audio/ogg" },
	{ "mp3", "audio/mpeg" },
	{ "wav", "audio/wav" },
	{ "flac", "audio/flac" },
	{ "woff", "font/woff" },
	{ "woff2", "font/woff2" },
	{ "ttf", "font/ttf" },
	{ "otf", "font/otf" },
	{ "zip", "application/zip" },
	{ "gz", "application/gzip" },
	{ "tar", "application/x-tar" },
	{ "xz", "application/x-xz" },
	{ "zst", "application/zstd" },
};

const char *http_content_type(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (!dot || strchr(dot, '/'))
		return "application/octet-stream";
	foreach_array_ref(content_types, t)
		if (!strcasecmp(dot + 1, t->ext))
			return t->type;
	return "application/octet-stream";
}

int http_open_status(int err)
{
	switch (err) {
	case ENOENT:
	case ENOTDIR:
	case EXDEV:
	case ELOOP:
	case ENAMETOOLONG:
		return 404;
	case EACCES:
	case EPERM:
	case ENXIO:
	case ENODEV:
		return 403;
	case EMFILE:
	case ENFILE:
	case ENOMEM:
		return 503;
	default:
		return 500;
	}
}
