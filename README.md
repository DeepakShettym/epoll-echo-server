# High-Performance Epoll Echo Server

A lightweight, event-driven TCP echo server written in C using the Linux `epoll` API. This server handles multiple concurrent connections efficiently using non-blocking I/O and a single-threaded event loop.

## Features
* **Epoll-based:** Uses Linux-specific `epoll` for scalable I/O multiplexing.
* **Non-blocking I/O:** Utilizes `O_NONBLOCK` to ensure the server never hangs on a single slow client.
* **Level-Triggered:** Robust event handling for incoming data and new connections.
* **Edge-Case Handling:** Correctly manages `EAGAIN`, `EWOULDBLOCK`, and client disconnections (`EPOLLRDHUP`).

## Requirements
* Linux OS (Epoll is Linux-specific)
* GCC Compiler
* Make (optional)

## Usage

### Compilation
```bash
gcc -o server main.c


### Sample output

deepak@deepak-ThinkPad-E14-Gen-6:~/networking$ ./my_tcp_server 8080

Server listening on port 8080
clent says FD(5): hey server ...
 
clent says FD(6): yo man how are u
 
clent says FD(7): iam iron man
 
clent says FD(8): iam siderman
 

