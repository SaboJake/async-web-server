// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#ifdef funny_style_checker
#include <cstdio>
#endif

#define DEBUG_LOG(...) printf(__VA_ARGS__)

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	char *header;

	switch (conn->state) {
	case STATE_SENDING_HEADER:
		header = "HTTP/1.1 200 OK\r\n"
				 "Content-Type: text/html\r\n"
				 "Connection: keep-alive\r\n"
				 "\r\n";
		break;

	case STATE_SENDING_404:
		header = "HTTP/1.1 404 Not Found\r\n"
				 "Content-Type: text/html\r\n"
				 "Connection: close\r\n"
				 "\r\n";
		break;

	case STATE_SENDING_DATA:
		header = "HTTP/1.1 200 OK\r\n"
				 "Content-Type: application/octet-stream\r\n"
				 "Connection: keep-alive\r\n"
				 "\r\n";
		break;

	case STATE_DATA_SENT:
		header = "HTTP/1.1 200 OK\r\n"
				 "Content-Type: text/html\r\n"
				 "Connection: keep-alive\r\n"
				 "\r\n";
		break;

	case STATE_404_SENT:
		header = "HTTP/1.1 404 Not Found\r\n"
				 "Content-Type: text/html\r\n"
				 "Connection: close\r\n"
				 "\r\n";
		break;

	default:
		header = "HTTP/1.1 500 Internal Server Error\r\n"
				 "Content-Type: text/html\r\n"
				 "Connection: close\r\n"
				 "\r\n";
		break;
	}

	snprintf(conn->send_buffer, BUFSIZ, "%s", header);
	conn->send_len = strlen(header);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	conn->state = STATE_SENDING_404;
	connection_prepare_send_reply_header(conn);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	char *static_dir = "static";
	char *dynamic_dir = "dynamic";

	if (strstr(conn->request_path, static_dir) != NULL)
		return RESOURCE_TYPE_STATIC;
	else if (strstr(conn->request_path, dynamic_dir) != NULL)
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = malloc(sizeof(struct connection));

	if (conn == NULL) {
		ERR("malloc");
		return NULL;
	}

	conn->sockfd = sockfd;
	conn->fd = -1;
	conn->eventfd = -1;
	conn->file_size = 0;
	conn->recv_len = 0;
	conn->send_len = 0;
	conn->send_pos = 0;
	conn->file_pos = 0;
	conn->async_read_len = 0;
	conn->have_path = 0;
	conn->res_type = RESOURCE_TYPE_NONE;
	conn->state = STATE_INITIAL;

	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	dlog(LOG_INFO, "Connection created: %p\n", conn);

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	if (conn->file_pos == 0 && io_setup(10, &conn->ctx) < 0) {
		ERR("io_setup");
		return;
	}

	io_set_eventfd(&conn->iocb, conn->eventfd);
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);
	conn->file_pos += BUFSIZ;

	conn->piocb[0] = &conn->iocb;
	if (io_submit(conn->ctx, 1, conn->piocb) < 0) {
		ERR("io_submit");
		return;
	}
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	if (conn->sockfd != -1)
		close(conn->sockfd);
	if (conn->fd != -1)
		close(conn->fd);
	if (conn->eventfd != -1)
		close(conn->eventfd);

	dlog(LOG_INFO, "Connection removed: %p\n", conn);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	/* TODO: Accept new connection. */
	int sockfd = accept(listenfd, (struct sockaddr *)&addr, &len);

	if (sockfd < 0) {
		ERR("accept");
		return;
	}
	/* TODO: Set socket to be non-blocking. */
	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0) {
		ERR("fcntl");
		close(sockfd);
		return;
	}
	/* TODO: Instantiate new connection handler. */
	struct connection *conn = connection_create(sockfd);

	if (conn == NULL) {
		close(sockfd);
		return;
	}
	/* TODO: Add socket to epoll. */
	if (w_epoll_add_ptr_in(epollfd, sockfd, conn) < 0) {
		ERR("w_epoll_add_ptr_in");
		connection_remove(conn);
		return;
	}
	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	conn->recv_len = 0;

	while (1) {
		ssize_t received = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);

		if (received <= 0)
			break;

		conn->recv_len += received;
	}
	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	dlog(LOG_INFO, "Opening file: %s\n", conn->request_path);

	/* Open file */
	int fd = open(conn->request_path + 1, O_RDONLY);

	if (fd < 0) {
		ERR("open");
		return -1;
	}

	/* Get file information */
	struct stat st;

	if (fstat(fd, &st) < 0) {
		ERR("fstat");
		close(fd);
		return -1;
	}

	/* Update connection fields */
	conn->fd = fd;
	conn->file_size = st.st_size;
	conn->file_pos = 0;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	struct io_event events[10];

	if (io_getevents(conn->ctx, 1, 10, events, NULL) < 0) {
		ERR("io_getevents");
		return;
	}

	conn->send_len = events[0].res;
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	conn->request_parser.data = conn;
	size_t check = http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	if (check != conn->recv_len)
		return -1;

	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	int fd = open(conn->request_path + 1, O_RDONLY);

	if (fd < 0) {
		ERR("open");
		return STATE_404_SENT;
	}

	struct stat st;

	if (fstat(fd, &st) < 0) {
		ERR("fstat");
		close(fd);
		return STATE_404_SENT;
	}

	conn->send_len = st.st_size;
	while (conn->send_len > 0) {
		ssize_t sent = sendfile(conn->sockfd, fd, NULL, conn->send_len);

		if (sent > 0)
			conn->send_len -= sent;
	}

	close(fd);
	return STATE_DATA_SENT;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	ssize_t total = 0;

	while (conn->send_len > 0) {
		ssize_t sent = send(conn->sockfd, conn->send_buffer + total, conn->send_len, 0);

		if (sent < 0) {
			ERR("send");
		} else {
			conn->send_len -= sent;
			total += sent;
		}
	}

	if (w_epoll_update_ptr_in(epollfd, conn->sockfd, conn) < 0) {
		ERR("w_epoll_update_ptr_in");
		connection_remove(conn);
		return -1;
	}

	return total;
}

#define BIG_BUFSIZE 2097153

int connection_send_dynamic(struct connection *conn)
{
	while (conn->file_pos < BIG_BUFSIZE) {
		connection_start_async_io(conn);
		connection_complete_async_io(conn);
		if (conn->send_len == 0)
			break;
		connection_send_data(conn);
	}

	return 0;
}

void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	if (conn == NULL) {
		dlog(LOG_INFO, "Connection is NULL\n");
		return;
	}

	dlog(LOG_INFO, "Handling conn: %p\n", conn);
	dlog(LOG_INFO, "Handling input, state: %d\n", conn->state);

	receive_data(conn);

	/* Parse the HTTP header. */
	if (parse_header(conn) < 0) {
		/* Send 404 for unparseable header. */
		connection_prepare_send_404(conn);
		conn->state = STATE_SENDING_HEADER;
		connection_send_data(conn);
		connection_remove(conn);
		return;
	}

	/* Determine resource type and handle accordingly. */
	conn->res_type = connection_get_resource_type(conn);

	if (connection_open_file(conn) < 0) {
		/* File not found, send 404. */
		connection_prepare_send_404(conn);
		conn->state = STATE_SENDING_HEADER;
		connection_send_data(conn);
	} else if (conn->res_type == RESOURCE_TYPE_STATIC) {
		/* Handle static resource. */
		conn->state = STATE_SENDING_HEADER;
		connection_prepare_send_reply_header(conn);
		connection_send_data(conn);
		connection_send_static(conn);
	} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
		/* Handle dynamic resource. */
		conn->state = STATE_SENDING_DATA;
		conn->file_pos = 0;
		connection_prepare_send_reply_header(conn);
		connection_send_dynamic(conn);
	} else {
		/* Resource type unknown, send 404. */
		connection_prepare_send_404(conn);
		conn->state = STATE_SENDING_HEADER;
		connection_send_data(conn);
	}

	/* Close the connection. */
	connection_remove(conn);
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if (conn == NULL) {
		dlog(LOG_INFO, "Connection is NULL\n");
		return;
	}

	dlog(LOG_INFO, "Handling conn: %p\n", conn);
	dlog(LOG_INFO, "Handling client, event: %u\n", event);

	if (event & EPOLLIN)
		handle_input(conn);

	if (event & EPOLLOUT)
		dlog(LOG_INFO, "Should handle output\n");
}

int main(void)
{
	/* TODO: Initialize asynchronous operations. */
	epollfd = w_epoll_create();
	if (epollfd < 0) {
		ERR("w_epoll_create");
		return 1;
	}

	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(8888, DEFAULT_LISTEN_BACKLOG);
	if (listenfd < 0) {
		ERR("tcp_create_listener");
		return 1;
	}

	/* TODO: Add server socket to epoll object*/
	if (w_epoll_add_fd_in(epollfd, listenfd) < 0) {
		ERR("w_epoll_add_fd_in");
		return 1;
	}

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;
		/* TODO: Wait for events. */
		if (w_epoll_wait_infinite(epollfd, &rev) < 0) {
			ERR("w_epoll_wait_infinite");
			return 1;
		}
		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			dlog(LOG_INFO, "New connection\n");
			handle_new_connection();
		} else {
			dlog(LOG_INFO, "Client event\n");
			handle_client(rev.events, rev.data.ptr);
		}
	}

	return 0;
}
