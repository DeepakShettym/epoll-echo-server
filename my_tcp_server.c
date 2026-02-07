#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#define MAX_EVENTS 1024
#define BUF_SIZE   4096

/* ---------------- utility ---------------- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[]) {

    typedef struct client_t {
        int fd;
        char rb[BUF_SIZE]; // Read Buffer
        int rb_len;        // How much data is currently in the buffer
    } client_t;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);

    /* 1. Create listening socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(1);
    }

    set_nonblocking(server_fd);

    /* 2. Create epoll */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        exit(1);
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; 
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    struct epoll_event events[MAX_EVENTS];

    printf("Server listening on port %d\n", port);

    /* 3. Event loop */
    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
           
            /* New connections */
            if (events[i].data.ptr == NULL) {
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(client_fd);

                    client_t *clt = malloc(sizeof(client_t));
                    if(!clt){
                        perror("malloc");
                        close(client_fd);
                        continue;
                    }

                    clt->fd = client_fd;
                    clt->rb_len = 0;
                    memset(clt->rb , 0 , BUF_SIZE);

                    struct epoll_event cev;  // used to preserve data of the client
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    /* EPOLLIN  : there is data to read ?
                       EPOLLRDHUP : is the clent disconnected ?
                    */
                    cev.data.ptr = clt; // if structure use : *ptr  | if fd use : fd 

                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev); // epoll start watching this fd
                }
            }
            /* Client socket */
            else {
                struct client_t *clt = events[i].data.ptr;

                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, clt->fd, NULL);
                    close(clt->fd);
                    printf("client disconnected FD(%d)\n", clt->fd);
                    free(clt);
                    continue;
                }

                /* Read all available data */
                while (1) {
                    int currLen = clt->rb_len; // Bookmark old position
                    ssize_t r = recv(clt->fd, clt->rb + clt->rb_len, sizeof(clt->rb) - clt->rb_len - 1, 0);
                    
                    if (r > 0) {
                        clt->rb_len += r;
                        clt->rb[clt->rb_len] = '\0';

                        // echo everything (full history) to the client
                        send(clt->fd, clt->rb, clt->rb_len, 0);
                        
                        // print only the new chunk to the server log
                        printf("client says FD(%d) : %s \n", clt->fd, clt->rb + currLen);
                    } else if (r == 0) {
                        printf("client disconnected(r == 0) FD(%d)\n", clt->fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, clt->fd, NULL);
                        close(clt->fd);
                        free(clt);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                            
                        epoll_ctl(epfd, EPOLL_CTL_DEL, clt->fd, NULL);
                        close(clt->fd);
                        free(clt);
                        break;
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epfd);
    return 0;
}
