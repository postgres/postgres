#if defined(__EMSCRIPTEN__)

#ifndef PGLITE_COMM_H
#define PGLITE_COMM_H

#include <emscripten/emscripten.h>

volatile int querylen = 0;
volatile FILE* queryfp = NULL;

// read FROM JS
// (i guess return number of bytes written)
// ssize_t pglite_read(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ socklen_t *address_len);
//typedef ssize_t (*pglite_read_t)(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ unsigned int *address_len);
typedef ssize_t (*pglite_read_t)(void *buffer, size_t max_length);
pglite_read_t pglite_read;

// write TO JS
// (i guess return number of bytes read)
// ssize_t pglite_write(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
// typedef ssize_t (*pglite_write_t)(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
typedef ssize_t (*pglite_write_t)(void *buffer, size_t length);
pglite_write_t pglite_write;

__attribute__((export_name("set_read_write_cbs")))
void
set_read_write_cbs(pglite_read_t read_cb, pglite_write_t write_cb) {
    pglite_read = read_cb;
    pglite_write = write_cb;
}

int EMSCRIPTEN_KEEPALIVE fcntl(int __fd, int __cmd, ...) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE setsockopt(int __fd, int __level, int __optname,
	const void *__optval, socklen_t __optlen) {
		// dummy 
		return 0;
}

int EMSCRIPTEN_KEEPALIVE getsockopt(int __fd, int __level, int __optname,
	void *__restrict __optval,
	socklen_t *__restrict __optlen) {
		// dummy 
		return 0;
}

int EMSCRIPTEN_KEEPALIVE getsockname(int __fd, struct sockaddr * __addr,
	socklen_t *__restrict __len) {
		// dummy 
		return 0;
	}

ssize_t EMSCRIPTEN_KEEPALIVE
	recv(int __fd, void *__buf, size_t __n, int __flags) {
		ssize_t got = pglite_read(__buf, __n);
		return got;
	}

ssize_t EMSCRIPTEN_KEEPALIVE
	send(int __fd, const void *__buf, size_t __n, int __flags) {
		ssize_t wrote = pglite_write(__buf, __n);
		return wrote;
	}

int EMSCRIPTEN_KEEPALIVE
    connect(int socket, const struct sockaddr *address, socklen_t address_len) {
		// dummy
		return 0;
	}

struct pollfd {
    int   fd;         /* file descriptor */
    short events;     /* requested events */
	short revents;    /* returned events */
};

int EMSCRIPTEN_KEEPALIVE
    poll(struct pollfd fds[], ssize_t nfds, int timeout) {
		// dummy
		return nfds;
	}

#endif

#endif // ifndef PGLITE_COMM_H