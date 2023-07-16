#ifndef RDMA_UTIL_H
#define RDMA_UTIL_H

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <rdma/rdma_cma.h>
#include "shm_malloc.h"
#include "logger.h"

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks */
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock */
    struct _stritem *h_next;    /* hash chain next */
    rel_time_t      time;       /* least recent access */
    rel_time_t      exptime;    /* expire time */
    int             nbytes;     /* size of data */
    unsigned short  refcount;
    uint8_t         nsuffix;    /* length of flags-and-length string */
    uint8_t         it_flags;   /* ITEM_* above */
    uint8_t         slabs_clsid;/* which slab class we're in */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    uint16_t        page_id;    /* indicating the ID of the page containing this item */

    struct _remitem *r_it;      /* NULL: item is Normal. NOT NULL: this is a temp item created for local operations
                                 * in lieu of a remote item; should be commited to the remote host */

    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS. */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS */
    /* then null-terminated key */
    /* then " flags length\r\n" (no terminating null) */
    /* then data with terminating \r\n (no terminating null; it's binary!) */
} item;

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

static const int RDMA_BUFFER_SIZE = 1024 * 1024;
static bool should_stop_polling = false;
static struct context *s_ctx = NULL;

extern void * remote_region;
extern struct rdma_cm_id *rdma_connections_head;

struct message {
    enum {
        MSG_L_MR,   // Local  MR
        MSG_R_MR,   // Remote MR
        MSG_DONE
    } type;

    union {
        struct ibv_mr mr;
     } data;
};

struct context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;

    pthread_t cq_poller_thread;
};

struct connection {
    struct rdma_cm_id *id;
    struct ibv_qp *qp;

    int connected;

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;
    struct ibv_mr *rdma_local_mr;
    struct ibv_mr *rdma_remote_mr;

    struct ibv_mr peer_mr;

    struct message *recv_msg;
    struct message *send_msg;

    char *rdma_local_region;
    char *rdma_remote_region;

    struct connection *next;
    struct connection *prev; 
    enum {
        SS_INIT,
        SS_MR_SENT,
        SS_RDMA_SENT,
        SS_DONE_SENT
    } send_state;

    enum {
        RS_INIT,
        RS_MR_RECV,
        RS_DONE_RECV
    } recv_state;

    pthread_t cm_poller_thread;
};

typedef struct _remitem {
    void            * address;     /* remote address of the item - RKEY can be obtained from the owner page */
    struct _remitem * next;
    struct _remitem * prev;
    struct _remitem * h_next;      /* hash chain next */
    
    char            * key;        /* item key*/
    uint8_t           nkey;       /* key length, w/terminating null and padding */
    uint8_t           slabs_clsid;
    uint16_t          page_id;    /* page id owning this item */

} remote_item;


void die(const char *reason);


void add_rdma_connection(struct rdma_cm_id * conn);
void remove_rdma_connection(struct rdma_cm_id * conn);
struct rdma_cm_id * lookup_rdma_connection(struct sockaddr_in peer_addr);

void build_connection(struct rdma_cm_id *id);
void build_params(struct rdma_conn_param *params);
void destroy_connection(void *context);
void on_connect(void *context);
void send_mr(void *context);

void * get_local_message_region(void *context);
char * get_peer_message_region(struct connection *conn);

void build_context(struct ibv_context *verbs);
void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
void on_completion(struct ibv_wc *);
void * poll_cq(void *);
void * poll_cm(struct rdma_event_channel *ec);
void post_receives(struct connection *conn);
void register_memory(struct connection *conn);
void send_message(struct connection *conn);

/* HASH OPERATIONS for remote items*/
void rdma_assoc_init(const int hashtable_init);
remote_item *rdma_assoc_find(const char *key, const size_t nkey, const uint32_t hv);
int rdma_assoc_insert(remote_item *it, const uint32_t hv);
void rdma_assoc_delete(const char *key, const uint32_t hv);

remote_item* slabs_rdma_lookup(char *key, const size_t nkey);
void slabs_rdma_insert(remote_item *it);

/* REMOTE OPERATIONS*/
item* get_remote_item(remote_item* r_it);
void set_remote_item(item *it);
void send_remote_page(void *context);

#endif