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
#include "memcached.h"

#define TEST_NZ(x) do { if ( (x)) printf("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) printf("error: " #x " failed (returned zero/null)."); } while (0)

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
    struct ibv_mr *rdma_local_mr_r;
    struct ibv_mr *rdma_local_mr_w;
    struct ibv_mr *rdma_remote_mr;

    struct ibv_mr peer_mr;

    struct message *recv_msg;
    struct message *send_msg;

    char *rdma_local_region_r;
    char *rdma_local_region_w;
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

static const int RDMA_BUFFER_SIZE = 1024 * 1024;
static bool should_stop_polling = false;
static struct context *s_ctx = NULL;

extern void * remote_region;
extern struct rdma_cm_id *rdma_connections_head;
static pthread_mutex_t rdma_local_region_lock;
static pthread_mutex_t rdma_local_region_lock2;
static char * last_used_addr = NULL;

static int batch_size = 10;
static int curr_wr_sge = 0;
static struct ibv_sge * sge_send = NULL;
static struct ibv_send_wr * wr_list_send = NULL;

void die(const char *reason);

/* Handling Connections */
void add_rdma_connection(struct rdma_cm_id * conn);
void remove_rdma_connection(struct rdma_cm_id * conn);
struct rdma_cm_id * lookup_rdma_connection(struct sockaddr_in peer_addr);

void build_connection(struct rdma_cm_id *id);
void build_params(struct rdma_conn_param *params);
void destroy_connection(void *context);
void on_connect(void *context);

/* Handling Memory Regions */
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

/* Handling Remote Items */
remote_item* create_remote_item(u_int8_t clsid);
remote_item* slabs_remoteq_lookup(char *key, const size_t nkey);
void slabs_rdma_insert(remote_item *it);

/* HASH OPERATIONS for remote items*/
void remote_assoc_init(const int hashtable_init);
remote_item *remote_assoc_find(const char *key, const size_t nkey, const uint32_t hv);
int remote_assoc_insert(remote_item *it, const uint32_t hv);
void remote_assoc_delete(const char *key, const uint32_t hv);

/* REMOTE OPERATIONS*/
char* get_remote_item(remote_item* r_it);
void set_remote_item(remote_item *r_it, item *it);
void add_remote_set_entry(remote_item *r_it, item *it);
void send_remote_page(void *context);

#endif