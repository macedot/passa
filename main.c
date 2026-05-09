#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <liburing.h>
#include "simd.h"

#ifndef MAX_CONNS
#define MAX_CONNS 4096
#endif

#define RING_ENTRIES 16384

#define OP_ACCEPT   0
#define OP_CONNECT  1
#define OP_RECV_C2B 2
#define OP_SEND_C2B 3
#define OP_RECV_B2C 4
#define OP_SEND_B2C 5

/* 32-byte conn struct: two per cache line */
struct conn {
	uint8_t *buf_c2b;
	uint8_t *buf_b2c;
	uint32_t generation;
	int client_fd;
	int backend_fd;
	int16_t backend_idx;
	uint16_t pending;
	uint8_t has_bufs;
	uint8_t closing;
	uint8_t pad[2];
};

static struct io_uring ring;
static int listen_fd = -1;

static struct conn conns[MAX_CONNS];
static int free_stack[MAX_CONNS];
static int free_top;

static char **upstreams;
static int num_upstreams;
static int rr;
static size_t buf_size;

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int parse_upstreams(const char *input, char ***out_upstreams,
			   int *out_count)
{
	size_t len = strlen(input);
	int positions[256];
	int n = simd_find_commas(input, len, positions, 256);
	int count = n + 1;

	char **upstreams = calloc((size_t)count, sizeof(char *));
	if (!upstreams)
		return -1;

	int start = 0;
	for (int i = 0; i <= n; i++) {
		int end = (i < n) ? positions[i] : (int)len;
		int seg_len = end - start;
		while (seg_len > 0 && input[start] == ' ') {
			start++;
			seg_len--;
		}
		while (seg_len > 0 && input[start + seg_len - 1] == ' ') {
			seg_len--;
		}
		upstreams[i] = malloc((size_t)seg_len + 1);
		if (!upstreams[i])
			abort();
		simd_memcpy(upstreams[i], input + start, (size_t)seg_len);
		upstreams[i][seg_len] = '\0';
		start = end + 1;
	}

	*out_upstreams = upstreams;
	*out_count = count;
	return count;
}

static int make_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
	    0)
		goto fail;

	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		goto fail;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto fail;

	struct sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		goto fail;
	}
	if (listen(fd, 1024) < 0) {
		perror("listen");
		goto fail;
	}
	return fd;

fail:
	close(fd);
	return -1;
}

#define conn_idx(c) ((int)((c) - conns))

static inline __attribute__((const)) uint64_t ud_pack(uint8_t op,
					      uint32_t conn_id,
					      uint32_t gen)
{
	return ((uint64_t)gen << 32) | ((uint64_t)conn_id << 8) | op;
}

static inline __attribute__((always_inline)) struct io_uring_sqe *get_sqe(void)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	if (__builtin_expect_with_probability(!sqe, 0, 0.05)) {
		io_uring_submit(&ring);
		sqe = io_uring_get_sqe(&ring);
	}
	return sqe;
}

static inline __attribute__((always_inline, hot)) struct conn *conn_alloc(void)
{
	if (__builtin_expect_with_probability(free_top <= 0, 0, 0.01))
		return NULL;

	int idx = free_stack[--free_top];
	struct conn *c = &conns[idx];

	c->generation++;
	c->closing = 0;
	c->pending = 0;
	c->client_fd = -1;
	c->backend_fd = -1;
	c->backend_idx = -1;

	if (__builtin_expect_with_probability(!c->has_bufs, 0, 0.1)) {
		if (posix_memalign((void **)&c->buf_c2b, 32, buf_size) != 0)
			abort();
		if (posix_memalign((void **)&c->buf_b2c, 32, buf_size) != 0)
			abort();
		simd_memset(c->buf_c2b, 0, buf_size);
		simd_memset(c->buf_b2c, 0, buf_size);
		c->has_bufs = 1;
	}
	return c;
}

static inline __attribute__((always_inline, hot)) void conn_free(struct conn *c)
{
	if (c->client_fd >= 0) {
		close(c->client_fd);
		c->client_fd = -1;
	}
	if (c->backend_fd >= 0) {
		close(c->backend_fd);
		c->backend_fd = -1;
	}
	c->closing = 0;
	c->pending = 0;

	free_stack[free_top++] = conn_idx(c);
}

static inline __attribute__((always_inline, hot)) void conn_maybe_close(struct conn *c)
{
	if (c->closing && c->pending == 0)
		conn_free(c);
}

static inline __attribute__((always_inline)) void submit_accept(void)
{
	struct io_uring_sqe *sqe = get_sqe();
	if (__builtin_expect(!sqe, 0))
		return;
	io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL,
				       SOCK_NONBLOCK);
	io_uring_sqe_set_data64(sqe, ud_pack(OP_ACCEPT, 0, 0));
}

static inline __attribute__((always_inline)) void submit_connect(struct conn *c)
{
	int idx = rr++ % num_upstreams;
	c->backend_idx = (int16_t)idx;
	const char *path = upstreams[idx];

	c->backend_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (__builtin_expect(c->backend_fd < 0, 0)) {
		conn_free(c);
		return;
	}

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	struct io_uring_sqe *sqe = get_sqe();
	if (__builtin_expect(!sqe, 0)) {
		conn_free(c);
		return;
	}

	io_uring_prep_connect(sqe, c->backend_fd, (struct sockaddr *)&addr,
			      sizeof(addr));
	io_uring_sqe_set_data64(sqe,
				ud_pack(OP_CONNECT, conn_idx(c),
					c->generation));
	c->pending++;
}

static inline __attribute__((always_inline)) void submit_recv(struct conn *c,
					       int from_client)
{
	struct io_uring_sqe *sqe = get_sqe();
	if (__builtin_expect(!sqe, 0))
		return;

	int fd = from_client ? c->client_fd : c->backend_fd;
	uint8_t *buf = from_client ? c->buf_c2b : c->buf_b2c;
	uint8_t op = from_client ? OP_RECV_C2B : OP_RECV_B2C;

	io_uring_prep_recv(sqe, fd, buf, buf_size, 0);
	io_uring_sqe_set_data64(sqe,
				ud_pack(op, conn_idx(c), c->generation));
	c->pending++;
}

static inline __attribute__((always_inline)) void submit_send(struct conn *c,
					       int to_backend,
					       size_t len)
{
	struct io_uring_sqe *sqe = get_sqe();
	if (__builtin_expect(!sqe, 0))
		return;

	int fd = to_backend ? c->backend_fd : c->client_fd;
	uint8_t *buf = to_backend ? c->buf_c2b : c->buf_b2c;
	uint8_t op = to_backend ? OP_SEND_C2B : OP_SEND_B2C;

	io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
	io_uring_sqe_set_data64(sqe,
				ud_pack(op, conn_idx(c), c->generation));
	c->pending++;
}

static __attribute__((hot)) void handle_cqe(struct io_uring_cqe * restrict cqe)
{
	uint64_t ud = io_uring_cqe_get_data64(cqe);
	uint8_t op = ud & 0xFF;

	if (__builtin_expect(op == OP_ACCEPT, 1)) {
		int client_fd = cqe->res;
		if (__builtin_expect(client_fd >= 0, 1)) {
			int nodelay = 1;
			setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
				   &nodelay, sizeof(nodelay));

			struct conn *c = conn_alloc();
			if (__builtin_expect(!!c, 1)) {
				c->client_fd = client_fd;
				submit_connect(c);
			} else {
				close(client_fd);
			}
		}
		if (__builtin_expect(!(cqe->flags & IORING_CQE_F_MORE), 0))
			submit_accept();
		return;
	}

	uint32_t cid = (uint32_t) ((ud >> 8) & 0xFFFFFF);
	uint32_t gen = (uint32_t) (ud >> 32);

	if (__builtin_expect_with_probability(cid >= MAX_CONNS, 0, 0.0))
		return;

	struct conn *c = &conns[cid];
	if (__builtin_expect_with_probability(c->generation != gen, 0, 0.001))
		return;

	c->pending--;

	if (__builtin_expect_with_probability(c->closing, 0, 0.05)) {
		conn_maybe_close(c);
		return;
	}

	if (__builtin_expect_with_probability(c->client_fd < 0, 0, 0.001))
		return;

	if (op == OP_CONNECT) {
		if (__builtin_expect(cqe->res < 0, 0)) {
			c->closing = 1;
			conn_maybe_close(c);
			return;
		}
		submit_recv(c, 1);
		submit_recv(c, 0);
		return;
	}

	if (op == OP_RECV_C2B) {
		if (__builtin_expect(cqe->res <= 0, 0)) {
			c->closing = 1;
			conn_maybe_close(c);
			return;
		}
		submit_send(c, 1, (size_t) cqe->res);
		return;
	}

	if (op == OP_SEND_C2B) {
		if (__builtin_expect(cqe->res < 0, 0)) {
			c->closing = 1;
			conn_maybe_close(c);
			return;
		}
		submit_recv(c, 1);
		return;
	}

	if (op == OP_RECV_B2C) {
		if (__builtin_expect(cqe->res <= 0, 0)) {
			c->closing = 1;
			conn_maybe_close(c);
			return;
		}
		submit_send(c, 0, (size_t) cqe->res);
		return;
	}

	if (op == OP_SEND_B2C) {
		if (__builtin_expect(cqe->res < 0, 0)) {
			c->closing = 1;
			conn_maybe_close(c);
			return;
		}
		submit_recv(c, 0);
		return;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGPIPE, SIG_IGN);

	int port = 8080;
	const char *port_env = getenv("PORT");
	if (port_env)
		port = atoi(port_env);

	buf_size = 4096;
	const char *buf_env = getenv("BUF_SIZE");
	if (buf_env)
		buf_size = (size_t) atol(buf_env);

	const char *upstreams_env = getenv("UPSTREAMS");
	if (!upstreams_env) {
		fprintf(stderr, "UPSTREAMS env var required\n");
		return 1;
	}

	if (parse_upstreams(upstreams_env, &upstreams, &num_upstreams) < 0
	    || num_upstreams == 0) {
		fprintf(stderr, "Failed to parse UPSTREAMS\n");
		return 1;
	}

	struct io_uring_params params = { 0 };
	params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
	if (io_uring_queue_init_params(RING_ENTRIES, &ring, &params) < 0) {
		fprintf(stderr, "io_uring_queue_init failed\n");
		return 1;
	}

	listen_fd = make_listener(port);
	if (listen_fd < 0) {
		fprintf(stderr, "failed to create listener\n");
		io_uring_queue_exit(&ring);
		return 1;
	}

	for (int i = 0; i < MAX_CONNS; i++) {
		conns[i].client_fd = -1;
		conns[i].backend_fd = -1;
		conns[i].has_bufs = 0;
		conns[i].closing = 0;
		conns[i].pending = 0;
		conns[i].generation = 0;
		free_stack[i] = i;
	}
	free_top = MAX_CONNS;

	submit_accept();

	while (running) {
		int ret = io_uring_submit_and_wait(&ring, 1);
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			break;
		}

		unsigned int head;
		struct io_uring_cqe *cqe;
		unsigned int count = 0;

		io_uring_for_each_cqe(&ring, head, cqe) {
			++count;
			handle_cqe(cqe);
		}
		if (count)
			io_uring_cq_advance(&ring, count);
	}

	io_uring_queue_exit(&ring);
	close(listen_fd);

	for (int i = 0; i < num_upstreams; i++)
		free(upstreams[i]);
	free(upstreams);

	for (int i = 0; i < MAX_CONNS; i++) {
		if (conns[i].has_bufs) {
			free(conns[i].buf_c2b);
			free(conns[i].buf_b2c);
		}
	}

	return 0;
}
