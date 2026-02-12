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
#include <time.h>

#define MAX_EVENTS 1024
#define BUF_SIZE   4096
#define HASH_SIZE  1024
#define MAX_KV     3   // ✅ capacity limit

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
static int queue_write(client_t *clt, int epfd, const char *data, int len)
{
    /* FIX: prevent silent overflow drop */
    if (len > BUF_SIZE - clt->wb_len) {
        /* FIX: signal caller to disconnect safely */
        return -1;
    }

    memcpy(clt->wb + clt->wb_len, data, len);
    clt->wb_len += len;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    ev.data.ptr = clt;

    epoll_ctl(epfd, EPOLL_CTL_MOD, clt->fd, &ev);

    return 0;
}

static long long now_sec(){
    return time(NULL);
}

/*Hash table*/

typedef struct kv_node{
    char key[64];
    char value[64];
    long long expiry;   // TTL support
    struct kv_node* next;

    struct kv_node *lru_prev;   // ✅ LRU
    struct kv_node *lru_next;   // ✅ LRU
}kv_node;

kv_node *hash_table[HASH_SIZE];

int kv_count = 0;
int current_hash_size = HASH_SIZE;

/* ✅ LRU HEAD/TAIL */
kv_node *lru_head = NULL;
kv_node *lru_tail = NULL;

/* FIX: explicit init safety */
static void kv_init() {
    memset(hash_table, 0, sizeof(hash_table));
    lru_head = lru_tail = NULL;   // ✅ reset LRU
}

void kv_delete(const char *key);

/* ---------------- LRU HELPERS ---------------- */

static void lru_add_to_head(kv_node *node) {
    node->lru_prev = NULL;
    node->lru_next = lru_head;

    if (lru_head)
        lru_head->lru_prev = node;

    lru_head = node;

    if (!lru_tail)
        lru_tail = node;
}

static void lru_remove(kv_node *node) {
    if (node->lru_prev)
        node->lru_prev->lru_next = node->lru_next;
    else
        lru_head = node->lru_next;

    if (node->lru_next)
        node->lru_next->lru_prev = node->lru_prev;
    else
        lru_tail = node->lru_prev;

    node->lru_prev = node->lru_next = NULL;
}

static void lru_move_to_head(kv_node *node) {
    if (lru_head == node)
        return;

    lru_remove(node);
    lru_add_to_head(node);
}

/* ------------------------------------------------ */

static unsigned int hash_key(const char *key)
{
    unsigned int hash = 5381;

    while (*key)
        hash = ((hash << 5) + hash) + *key++;

    return hash % HASH_SIZE;
}

void kv_set(const char *key, const char *value, int ttl)
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
            node->expiry = ttl ? now_sec() + ttl : 0;  // TTL update

            /* ✅ LRU update */
            lru_move_to_head(node);
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

    new_node->expiry = ttl ? now_sec() + ttl : 0; // TTL insert

    new_node->next = hash_table[idx]; // if new then hash_table[idx] return null |key|value|null|
    hash_table[idx] = new_node;

    /* ✅ LRU insert */
    lru_add_to_head(new_node);

    kv_count++;

    /* ✅ Eviction */
    if (kv_count > MAX_KV) {
        kv_node *victim = lru_tail;
        if (victim) {
            kv_delete(victim->key);
        }
    }
}

const char *kv_get(const char *key)
{
    unsigned int idx  = hash_key(key);

    kv_node *node = hash_table[idx];

    while(node){

        if(strcasecmp(key , node->key) == 0){

            /* TTL CHECK */
            if(node->expiry && node->expiry < now_sec()){
                kv_delete(key);
                return NULL;
            }

            /* ✅ LRU update */
            lru_move_to_head(node);

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

            /* ✅ remove from LRU */
            lru_remove(node);

            free(node);
            kv_count--;
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
                                if (queue_write(clt, epfd, "ERROR malformed\n", 16) < 0)
                                    goto disconnect_client;
                                goto shift_buffer;
                            }

                            *space1 = '\0';

                            char *key = space1 + 1;

                            char *space2 = key;
                            while (*space2 != ' ' && *space2 != '\0') space2++;

                            char *value = NULL;
                            int ttl = 0;

                            if (*space2 == ' ') {
                                *space2 = '\0';
                                value = space2 + 1;

                                char *ex = strstr(value, " EX ");
                                if (ex) {
                                    *ex = '\0';
                                    ttl = atoi(ex + 4);
                                }
                            }

                            /* ---- command handling ---- */

                            if(strcasecmp(cmd , "SET") == 0){
                                if (key && value &&
                                    strlen(key) < 64 &&
                                    strlen(value) < 64 &&
                                    strlen(key) > 0 &&
                                    strlen(value) > 0) {

                                    kv_set(key , value, ttl);

                                    if (queue_write(clt , epfd , "OK\n" , 3) < 0)
                                        goto disconnect_client;
                                } else {
                                    if (queue_write(clt, epfd, "ERROR: SET needs key and value\n", 31) < 0)
                                        goto disconnect_client;
                                }

                            }else if(strcasecmp(cmd , "GET") == 0){
                                const char *val = kv_get(key);

                                if(val){
                                    if (queue_write(clt , epfd , val , strlen(val)) < 0)
                                        goto disconnect_client;
                                    if (queue_write(clt, epfd, "\n", 1) < 0)
                                        goto disconnect_client;
                                } else {
                                    if (queue_write(clt, epfd, "Key not found\n", 14) < 0)
                                        goto disconnect_client;
                                }
                            }else if (strcasecmp(cmd, "DEL") == 0) {
                                kv_delete(key);
                                if (queue_write(clt, epfd, "DELETED\n", 8) < 0)
                                    goto disconnect_client;
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
