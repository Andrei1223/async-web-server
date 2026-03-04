# Asynchronous Web Server

## Description

This project is a HTTP web server implemented in C for the Linux operating system. The server is designed to handle multiple concurrent
client connections efficiently by leveraging advanced I/O paradigms, including asynchronous file operations, non-blocking sockets, and
zero-copy data transfer. It serves as a practical implementation of high-concurrency network programming.

## Objectives

* Master the **epoll** multiplexing API for handling thousands of concurrent connections.
* Implement **Zero-Copy** data transfers using the `sendfile` system call for static resources.
* Develop skills in **Asynchronous I/O (AIO)** using `io_submit` and related syscalls for dynamic content simulation.
* Design and manage complex per-connection **State Machines** to track non-blocking network transitions.

## Features

### 1. I/O Multiplexing (epoll)
The server uses an event-driven architecture. Instead of spawning a thread per connection, it uses `epoll` to monitor multiple file
descriptors, reacting only when data is ready to be read or written.



### 2. Static vs. Dynamic Content
The server distinguishes between two types of requests based on the directory path:

* **Static Files (`/static/`)**: Transmitted using **Zero-Copying** via `sendfile()`. This bypasses the need to copy data between kernel
and user space, significantly reducing CPU usage.

* **Dynamic Files (`/dynamic/`)**: Simulates post-processing requirements by using **Linux AIO**. Files are read from disk asynchronously
and pushed to non-blocking sockets once the data buffer is ready.

### 3. Connection State Machine
To handle non-blocking operations, each connection maintains a state (e.g., `STATE_RECEIVING_REQUEST`, `STATE_SENDING_HEADER`, `STATE_SENDING_DATA`).
The server transitions between these states based on `epoll` events.

## API & Protocol Support

* **HTTP/1.1**: Minimal implementation supporting `GET` requests.
* **Response Codes**: Returns `200 OK` for valid files and `404 Not Found` for invalid paths.
* **HTTP Parser**: Integrates the [Node.js HTTP Parser](https://github.com/nodejs/http-parser) for robust request header processing.

## Project Structure

* **`skel/`**: Contains the server core (`aws.c`), assignment headers (`aws.h`), and the HTTP parser library.
* **`tests/`**: Automated test suite including functional tests and memory leak (Valgrind) checks.
* **`AWS_DOCUMENT_ROOT`**: The root directory from which files are served.

## Building and Running

### Compilation
Navigate to the `skel/` directory and use `make`:
```
cd skel/
make
```

### Running the Server
The server listens on the port defined by AWS_LISTEN_PORT in the header files.

```
./aws
```

## Testing

The test suite includes 35 functional tests and 13 memory check tests. To run all tests:
```
cd tests/
make check
```

## Requirements
 - **OS:** Linux (required for epoll and AIO support).
 - **Tools:** gcc, make, valgrind, wget.


