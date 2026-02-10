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
#define HASH_SIZE  1024

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

typedef struct client_t {
    int fd;
    char rb[BUF_SIZE]; // Read Buffer
    int rb_len;        // How much data is currently in the buffer

    char wb[BUF_SIZE]; // Write Buffer
    int wb_len;        // total data to send
    int wb_sent;       // already sent
} client_t;

/* ---- WRITE BUFFER QUEUE ---- */
static void queue_write(client_t *clt, int epfd, const char *data, int len)
{
    /* FIX: prevent silent overflow drop */
    if (len > BUF_SIZE - clt->wb_len) {
        /* disconnect instead of silent corruption */
        epoll_ctl(epfd, EPOLL_CTL_DEL, clt->fd, NULL);
        close(clt->fd);
        free(clt);
        return;
    }

    memcpy(clt->wb + clt->wb_len, data, len);
    clt->wb_len += len;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    ev.data.ptr = clt;

    epoll_ctl(epfd, EPOLL_CTL_MOD, clt->fd, &ev);
}

/*Hash table*/

typedef struct kv_node{
    char key[64];
    char value[64];

    struct kv_node* next;
}kv_node;

kv_node *hash_table[HASH_SIZE];

/* FIX: explicit init safety */
static void kv_init() {
    memset(hash_table, 0, sizeof(hash_table));
}

static unsigned int hash_key(const char *key)
{
    unsigned int hash = 5381;

    while (*key)
        hash = ((hash << 5) + hash) + *key++;

    return hash % HASH_SIZE;
}

void kv_set(const char *key, const char *value)
{
    /* FIX: bounds validation */
    if (!key || !value) return;
    if (strlen(key) >= 64 || strlen(value) >= 64) return;

    unsigned int idx = hash_key(key);

    kv_node *node = hash_table[idx];
    while(node){
        if(strcasecmp(node->key , key) == 0){   // find the key if already exist
            strncpy(node->value , value,63);
            node->value[63] = '\0';
            return;
        }   

        node = node->next; // check until null 
    }

    // not found  ? create a new node 
    kv_node *new_node = malloc(sizeof(kv_node)); 

    /* FIX: malloc check */
    if(!new_node) return;

    strncpy(new_node->key, key, 63);
    new_node->key[63] = '\0';

    strncpy(new_node->value , value,63);
    new_node->value[63] = '\0';

    new_node->next = hash_table[idx]; // if new then hash_table[idx] return null |key|value|null|
    hash_table[idx] = new_node;
}

const char *kv_get(const char *key)
{
    unsigned int idx  = hash_key(key);

    kv_node *node = hash_table[idx];

    while(node){

        if(strcasecmp(key , node->key) == 0){
            return node->value;
        }
        node = node->next;
    }

    return NULL;
}

void kv_delete(const char *key){
    unsigned int idx  = hash_key(key);
    kv_node *node = hash_table[idx];
    kv_node *prev = NULL;
 
    while(node){
        if(strcasecmp(key , node->key) == 0){
            if(prev) prev->next = node->next;
            else hash_table[idx] = node->next;
            free(node);
            return;
        }

        prev = node;
        node = node->next;
    }
}

/* FIX: cleanup helper */
void kv_free_all(){
    for(int i=0;i<HASH_SIZE;i++){
        kv_node *n = hash_table[i];
        while(n){
            kv_node *tmp = n;
            n = n->next;
            free(tmp);
        }
    }
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[]) {

    kv_init(); /* FIX: ensure table clean */

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

                /* ---- WRITE HANDLING ---- */
                if (events[i].events & EPOLLOUT) {

                    while (clt->wb_sent < clt->wb_len) {

                        ssize_t s = send(clt->fd,
                                         clt->wb + clt->wb_sent,
                                         clt->wb_len - clt->wb_sent,
                                         0);

                        if (s > 0) {
                            clt->wb_sent += s;
                        }
                        else if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break;
                        }
                        else {
                            goto disconnect_client;
                        }
                    }

                    if (clt->wb_sent == clt->wb_len) {
                        clt->wb_len = 0;
                        clt->wb_sent = 0;

                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLRDHUP;
                        ev.data.ptr = clt;

                        epoll_ctl(epfd, EPOLL_CTL_MOD, clt->fd, &ev);
                    }
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

                            /* FIX: malformed command detection */
                            if (*space1 == '\0') {
                                queue_write(clt, epfd, "ERROR malformed\n", 16);
                                goto shift_buffer;
                            }

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
                                if (key && value &&
                                    strlen(key) < 64 &&
                                    strlen(value) < 64) {
                                    kv_set(key , value);
                                    queue_write(clt , epfd , "OK\n" , 3);
                                } else {
                                    queue_write(clt, epfd, "ERROR: SET needs key and value\n", 31);
                                }

                            }else if(strcasecmp(cmd , "GET") == 0){
                                const char *val = kv_get(key);

                                if(val){
                                    queue_write(clt , epfd , val , strlen(val));
                                    queue_write(clt, epfd, "\n", 1);
                                } else {
                                    queue_write(clt, epfd, "Key not found\n", 14);
                                }
                            }else if (strcasecmp(cmd, "DEL") == 0) {
                                kv_delete(key);
                                queue_write(clt, epfd, "DELETED\n", 8);
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

    kv_free_all();
    close(server_fd);
    close(epfd);
    return 0;
}
