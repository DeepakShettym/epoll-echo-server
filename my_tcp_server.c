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

typedef struct {
  char key[64];
  char value[64];  
}entry_t;

entry_t database[100];
int db_count = 0;

/* ---------------- main ---------------- */

int main(int argc, char *argv[]) {

    typedef struct client_t {
        int fd;
        char rb[BUF_SIZE]; // Read Buffer
        int rb_len;        // How much data is currently in the buffer

        char wb[BUF_SIZE]; // Write Buffer
        int wb_len;        // total data to send
        int wb_sent;       // already sent
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
                    memset(clt->rb , 0 , BUF_SIZE); // OK here (not hot path)

                    clt->wb_len = 0;
                    clt->wb_sent = 0;

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
disconnect_client:
                    epoll_ctl(epfd, EPOLL_CTL_DEL, clt->fd, NULL);
                    close(clt->fd);
                    printf("client disconnected FD(%d)\n", clt->fd);
                    free(clt);
                    continue;
                }

                /* Read all available data */
                while (1) {
                    ssize_t r = recv(clt->fd, clt->rb + clt->rb_len,
                                     sizeof(clt->rb) - clt->rb_len - 1, 0);
                    
                    if (r > 0) {
                        clt->rb_len += r;
                        clt->rb[clt->rb_len] = '\0';

                        /* ---- MULTI COMMAND PARSING LOOP ---- */
                        while (1) {

                            char *newline = memchr(clt->rb, '\n', clt->rb_len);
                            if (!newline) break; // No full command yet

                            *newline = '\0';

                            /* ---- parsing ---- */

                            char *cmd = clt->rb;

                            char *space1 = clt->rb;
                            while (*space1 != ' ' && *space1 != '\0') space1++;

                            if (*space1 == '\0') goto shift_buffer; // incomplete command

                            *space1 = '\0';

                            char *key = space1 + 1;

                            char *space2 = key;
                            while (*space2 != ' ' && *space2 != '\0') space2++;

                            char *value = NULL;

                            if (*space2 == ' ') {
                                *space2 = '\0';
                                value = space2 + 1;
                            }

                            /* ---- command handling ---- */

                            if(strcasecmp(cmd , "SET") == 0){

                                if (!value) {
                                    send(clt->fd, "ERROR SET needs value\n", 23, 0);
                                    goto shift_buffer;
                                }

                                int idx = -1;

                                for(int j = 0 ; j < db_count ; j++){
                                    if(strcasecmp(database[j].key , key) == 0){
                                        idx = j;
                                        break;
                                    }
                                }

                                if(idx == -1){
                                    if(db_count < 100){
                                        strncpy(database[db_count].key , key , 63);
                                        database[db_count].key[63] = '\0';

                                        strncpy(database[db_count].value , value, 63);
                                        database[db_count].value[63] = '\0';

                                        send(clt->fd , "OK\n",3,0);
                                        db_count++;
                                        printf("stored %s : %s \n" , key , value);
                                    }else{
                                        send(clt->fd , "dbisfull\n",9,0);
                                    }
                                }else if(idx > -1){
                                    strncpy(database[idx].value , value ,63);
                                    database[idx].value[63] = '\0';

                                    send(clt->fd , "OK\n",3,0);
                                    printf("updated %s : %s \n" , key , value);
                                }

                            }else if(strcasecmp(cmd , "GET") == 0){

                                int found = 0;
                                for(int k = 0 ; k < db_count ; k++){
                                    if(strcasecmp(key , database[k].key) == 0){
                                        send(clt->fd , database[k].value ,
                                             strlen(database[k].value),0);
                                        send(clt->fd, "\n", 1, 0);
                                        found = 1;
                                        break;
                                    }
                                }

                                if(!found)
                                    send(clt->fd , "Key not found \n" , 15 , 0);  
                            }

shift_buffer:
                            /* ---- SHIFT BUFFER LEFT ---- */
                            int consumed = (newline - clt->rb) + 1;
                            memmove(clt->rb, clt->rb + consumed, clt->rb_len - consumed);
                            clt->rb_len -= consumed;
                            clt->rb[clt->rb_len] = '\0';
                        }
                    }
                    else if (r == 0) {
                        goto disconnect_client;
                    }
                    else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        goto disconnect_client;
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epfd);
    return 0;
}
