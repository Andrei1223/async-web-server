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

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

// my implementation of strcpy for the coding style warning
char *my_strcpy(char *dest, const char *src)
{
	char *originalDest = dest;

	while ((*dest++ = *src++) != '\0')
		;

	// return the pointer
	return originalDest;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	snprintf(conn->send_buffer, sizeof(conn->send_buffer),
			"HTTP/1.1 200 OK\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n",
			conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;
}

void get_filename(struct connection *conn)
{
	// get the address of the first slash
	char *first_slash = strchr(conn->recv_buffer, '/');

	if (first_slash) {
		// get the address of the 2nd slash
		char *second_slash = strchr(first_slash + 1, '/');

		if (second_slash) {
			char *first_char = second_slash;
			char *second_char = NULL;

			// get the end point of the file name(find the first space)
			second_char = strchr(first_char, ' ');

			if (second_char)
				strncpy(conn->filename, second_slash + 1, second_char - first_char - 1);
		}
	}
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	snprintf(conn->send_buffer, sizeof(conn->send_buffer),
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Type: text/html\r\n"
			"Connection: close\r\n\r\n");

	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	char *type = strstr(conn->request_path, "static");

	// if static type
	if (type)
		return RESOURCE_TYPE_STATIC;
	else
		return RESOURCE_TYPE_DYNAMIC;

	printf("Error not a valid path\n");
	// unkown type of resource
	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	// allocate memory
	struct connection *conn;

	conn = (struct connection *)malloc(sizeof(struct connection));
	DIE(conn == NULL, "malloc"); // check for errors

	// set the socket fd
	conn->sockfd = sockfd;

	// set to 0 each buffer
	memset(conn->filename, 0, BUFSIZ);
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	// create non blocking event fd
	conn->eventfd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->eventfd == -1, "eventfd"); // check for errors

	conn->state = STATE_INITIAL;
	conn->have_path = 0;
	conn->recv_len = 0;
	conn->send_len = 0;
	conn->send_pos = 0;
	conn->file_pos = 0;
	conn->async_read_len = 0;

	conn->ctx = ctx;

	// return the new connection
	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	int result;
	struct iocb *iocb;
	io_context_t ctx;

	// set the values
	iocb = &(conn->iocb);
	ctx = conn->ctx;

	// init the async read operation
	io_prep_pread(iocb, conn->fd, conn->recv_buffer, BUFSIZ, conn->recv_len);

	// set the context
	iocb->data = conn;
	iocb->aio_fildes = conn->fd;

	// submit the operation
	result = io_submit(ctx, 1, &iocb);
	DIE(result != 1, "io_submit");

	// set the new connection state
	conn->state = STATE_ASYNC_ONGOING;
}


void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	 // complete asynchronous read operation
	struct io_event events[1];
	int ret = io_getevents(conn->ctx, 1, 1, events, NULL);

	if (ret == 1) {
		conn->async_read_len = events[0].res;
		conn->send_pos = 0;
		conn->recv_len += events[0].res;

		// send all the data that has been read
		while ((ret = connection_send_dynamic(conn))) {
			if (ret <= 0)
				break;
		}

		// send the read data to the socket
		if (conn->state == STATE_ASYNC_ONGOING)
			connection_start_async_io(conn);
	}
}

int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Send data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	int ret = send(conn->sockfd, conn->recv_buffer + conn->send_pos, conn->async_read_len, MSG_NOSIGNAL | MSG_DONTWAIT);

	// modify values to compute
	conn->send_pos += ret;
	conn->async_read_len -= ret;
	conn->send_len += ret;

	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// send would block
			conn->state = STATE_CONNECTION_CLOSED;
		} else {
			// handle errors
			DIE(1, "send");
			return -1;
		}
	}

	// if all the data has been sent
	if (conn->send_len == conn->file_size) {
		conn->state = STATE_CONNECTION_CLOSED;
		return -1;
	}

	return ret;
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	int result;
	struct epoll_event ev;

	// remove the connection from the epoll
	ev.events = EPOLLOUT;
	ev.data.ptr = (void *)conn;

	result = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(result == -1, "epoll_ctl");

	// set the status to closed
	conn->state = STATE_CONNECTION_CLOSED;

	// close the socket
	close(conn->sockfd);

	close(conn->fd);

	// free the memory
	free(conn);

	conn = NULL;
}

void handle_new_connection(void)
{
	// variables
	int result, client_socket, flags; // for copying the socket s flags
	struct sockaddr_in client_addr;
	struct connection *conn; // used as a connection

	/* TODO: Handle a new connection request on the server socket. */
	socklen_t addr_len = sizeof(struct sockaddr_in);

	/* TODO: Accept new connection. */
	client_socket = accept(listenfd, (struct sockaddr *)&client_addr, &addr_len);
	DIE(client_socket == -1, "accept");

	/* TODO: Set socket to be non-blocking. */
	flags = fcntl(client_socket, F_GETFL, 0); // get the flags for the socket fd
	DIE(flags < 0, "fcntl"); // check for errors

	// set flags for socket with the non-blocking flag
	result = fcntl(client_socket, F_SETFL, flags | O_NONBLOCK, 0);
	DIE(result < 0, "fcntl"); // check for errors

	/* TODO: Instantiate new connection handler. */
	conn = connection_create(client_socket);

	/* TODO: Add socket to epoll. */
	result = w_epoll_add_ptr_in(epollfd, client_socket, conn);
	DIE(result < 0, "w_epoll_add_ptr_in");

	/* TODO: Initialize HTTP_REQUEST parser. */
	// init the parser
	http_parser_init(&(conn->request_parser), HTTP_REQUEST);
	conn->state = STATE_INITIAL;
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	// set initial value
	size_t bytes_received = 1;
	const char *http_format = "\r\n\r\n";

	if (bytes_received > 0 && conn->recv_len < sizeof(conn->recv_buffer) - 1) {
		// read the info from the socket
		bytes_received = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, sizeof(conn->recv_buffer) - conn->recv_len - 1, 0);

		// change the count of read bytes
		conn->recv_len += bytes_received;
	}

	conn->recv_buffer[conn->recv_len + 1] = '\0';

	// check if the http request is done
	if (strstr(conn->recv_buffer, http_format) != NULL)
		conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	char file_path[300];

	// get the resource type
	conn->res_type = connection_get_resource_type(conn);

	// check the type of file
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		my_strcpy(file_path, AWS_ABS_STATIC_FOLDER);
		get_filename(conn);
		my_strcpy(file_path + strlen(file_path), conn->filename);
	} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
		my_strcpy(file_path, AWS_ABS_DYNAMIC_FOLDER);
		get_filename(conn);
		my_strcpy(file_path + strlen(file_path), conn->filename);
	}

	// check the filename is valid
	int filefd = open(file_path, O_RDONLY), rc;
	struct stat file_stat;

	// call fstat to get the length of the file
	conn->fd = filefd;

	if (conn->fd == -1) {
		conn->state = STATE_SENDING_404;
		return -1;
	}

	// obtain the size of the file
	rc = fstat(conn->fd, &file_stat);

	// should not enter this
	if (rc == -1) {
		DIE(rc < 0, "fstat");
		close(conn->fd);
		connection_remove(conn);
		return -1;
	}

	// set the file size
	conn->file_size = file_stat.st_size;
	conn->state = STATE_SENDING_HEADER;
	conn->recv_len = 0; // reset the recv len

	return filefd;
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
	int bytes_parsed = http_parser_execute(&(conn->request_parser),
											&settings_on_path,
											conn->recv_buffer,
											conn->recv_len);
	return bytes_parsed;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	int bytes = 0;

	// reset the byts count
	bytes = 0;
	off_t offset = conn->send_pos;
	// send the data
	if (conn->state == STATE_SENDING_DATA && conn->send_pos < conn->file_size) {
		// send the content of the file
		bytes += sendfile(conn->sockfd, conn->fd, &offset, BUFSIZ);
		conn->send_pos += bytes;
		conn->state = STATE_SENDING_DATA;
	}

	// check if all the data has been sent
	if (conn->state == STATE_SENDING_DATA && conn->send_pos == conn->file_size)
		conn->state = STATE_CONNECTION_CLOSED;

	return conn->state;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	size_t bytes_sent = 0;

	// open the file
	if (conn->state == STATE_REQUEST_RECEIVED)
		connection_open_file(conn);

	// send header for 404 status
	if (conn->state == STATE_SENDING_404) {
		// if the state is STATE_SENDING_404 prepare and send the 404 header
		if (conn->send_len == 0)
			connection_prepare_send_404(conn);

		// send the HTTP header
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, strlen(conn->send_buffer + conn->send_pos), 0);

		// update the position in the file
		conn->send_pos += bytes_sent;

		// check if the header has been sent
		if (conn->send_pos == conn->send_len) {
			conn->state = STATE_CONNECTION_CLOSED;
			// reset the counter
			conn->send_pos = 0;
			conn->send_len = 0;
		}
	}

	// send header for normal file
	if (conn->state == STATE_SENDING_HEADER) {
		// if the state is STATE_SENDING_404, prepare and send the 404 header

		if (conn->send_len == 0)
			connection_prepare_send_reply_header(conn);

		// Send the HTTP header.
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, strlen(conn->send_buffer + conn->send_pos), 0);

		// update the position in the file
		conn->send_pos += bytes_sent;

		// check if the header has been sent
		if (conn->send_pos == conn->send_len) {
			conn->state = STATE_SENDING_DATA;
			// reset the counter
			conn->send_pos = 0;
			conn->send_len = 0;
		}
	}

	// send data
	if (conn->state == STATE_SENDING_DATA) {
		// TODO add check for type of operation
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			conn->state = connection_send_static(conn);
		} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			conn->state = STATE_ASYNC_ONGOING;
			conn->send_len = 0;
			// start the async operation
			connection_start_async_io(conn);
		}
	}

	return bytes_sent;
}

void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	switch (conn->state) {
	case STATE_INITIAL:
		// set the state to read from socket
		conn->state = STATE_RECEIVING_DATA;
		break;
	case STATE_RECEIVING_DATA:
		// read the data
		receive_data(conn);
		break;
	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;
	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	switch (conn->state) {
	case STATE_INITIAL:
		// set the state to read from socket
		conn->state = STATE_RECEIVING_DATA;
		break;
	case STATE_SENDING_404:
		connection_send_data(conn);
		break;
	case STATE_ASYNC_ONGOING:
		// check if the operation has ended
		connection_complete_async_io(conn);
		break;
	case STATE_REQUEST_RECEIVED:
		parse_header(conn);
		connection_send_data(conn);
		break;
	case STATE_SENDING_DATA:
		connection_send_data(conn);
		break;
	case STATE_SENDING_HEADER:
		connection_send_data(conn);
		break;
	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	int result;

	if (event & EPOLLIN) {
		// the data is ready to be read
		handle_input(conn);

		if (conn->state == STATE_REQUEST_RECEIVED) {
			result = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(result < 0, "w_epoll_update_ptr_out");
		}
	} else if (event & EPOLLOUT) {
		// the socket is ready to recieve data
		handle_output(conn);

		if (conn->state == STATE_CONNECTION_CLOSED)
			connection_remove(conn);

	} else if (event & EPOLLHUP) {
		// the connection has ended
		connection_remove(conn);
	}
}

int main(void)
{
	int rc;
	/* TODO: Initialize asynchronous operations. */

	/* TODO: Initialize multiplexing. */
	// get the epoll file descriptor
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* TODO: Create server socket. */
	// get the server socket file descriptor
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, 0);
	DIE(listenfd < 0, "tcp_create_listener");

	/* TODO: Add server socket to epoll object*/
	rc = w_epoll_add_fd_inout(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_inout");

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	rc = io_setup(128, &ctx);
	DIE(rc == -1, "io_setup");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		// wait for events
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		// check for new connection
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN) {
				// create new connection
				handle_new_connection();
			}
		} else {
			// if old connection
			handle_client(rev.events, (struct connection *)rev.data.ptr);
		}
	}

	io_destroy(ctx);

	return 0;
}
