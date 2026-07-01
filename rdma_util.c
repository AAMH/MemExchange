#include "rdma_util.h"
#include <sched.h>
#include <stdatomic.h>

struct rdma_cm_id * lookup_rdma_connection(struct sockaddr_in peer_addr, int peer_id){

    if(!rdma_connections_head)
        return NULL;

    struct connection * curr = (struct connection *)rdma_connections_head->context;
    struct sockaddr_in* curr_addr = (struct sockaddr_in *)rdma_get_peer_addr(rdma_connections_head);

    if(curr_addr->sin_addr.s_addr == peer_addr.sin_addr.s_addr && curr->peer_identifier == peer_id)
        return curr->id;

    while(curr->next){
        curr = curr->next;
        curr_addr = (struct sockaddr_in *)rdma_get_peer_addr(curr->id);

        if(curr_addr->sin_addr.s_addr == peer_addr.sin_addr.s_addr && curr->peer_identifier == peer_id)
            return curr->id;
    }
    
    return NULL;
}

void add_rdma_connection(struct rdma_cm_id * conn){

    if(rdma_connections_head){
        struct connection * curr = (struct connection *)rdma_connections_head->context;
        while(curr->next)
            curr = curr->next;
        curr->next = conn->context;
        curr->next->prev = curr;
        curr->next->next = NULL;
    }
    else{
        rdma_connections_head = conn;
        ((struct connection *)(conn->context))->next = NULL;
        ((struct connection *)(conn->context))->prev = NULL;
    }
    
}

void remove_rdma_connection(struct rdma_cm_id * conn){

    struct connection * curr = (struct connection *)rdma_connections_head->context;

    if(curr->id == conn){    // removing the head
        if(curr->next){
            rdma_connections_head = curr->next->id;
            curr->next->prev = NULL;
        }
        else
            rdma_connections_head = NULL;
        return;
    }

    while(curr->next && curr->id != conn)
        curr = curr->next;
    if(curr->id == conn){
        if(curr->prev)
            curr->prev->next = curr->next;
        if(curr->next)
            curr->next->prev = curr->prev;
    }
}

void die(const char *reason){
    fprintf(stderr, "%s\n", reason);
    exit(EXIT_FAILURE);
}

void build_connection(struct rdma_cm_id *id){
    struct connection *conn = id->context;
    struct ibv_qp_init_attr qp_attr;

    conn->s_ctx = NULL;
    conn->should_stop_polling = false;
    conn->last_used_addr = NULL;

    build_context(id->verbs, conn);
    build_qp_attr(&qp_attr, conn);

    TEST_NZ(rdma_create_qp(id, conn->s_ctx->pd, &qp_attr));

    //id->context = conn = (struct connection *)malloc(sizeof(struct connection));

    conn->id = id;
    conn->qp = id->qp;

    conn->send_state = SS_INIT;
    conn->recv_state = RS_INIT;

    conn->connected = 0;

    register_memory(conn);
    post_receives(conn);
    add_rdma_connection(id);
}

void build_context(struct ibv_context *verbs, struct connection *conn){
    if (conn->s_ctx) {
        if (conn->s_ctx->ctx != verbs)
        die("cannot handle events in more than one context.");

        return;
    }

    conn->s_ctx = (struct context *)malloc(sizeof(struct context));

    conn->s_ctx->ctx = verbs;

    TEST_Z(conn->s_ctx->pd = ibv_alloc_pd(conn->s_ctx->ctx));
    TEST_Z(conn->s_ctx->comp_channel = ibv_create_comp_channel(conn->s_ctx->ctx));
    TEST_Z(conn->s_ctx->cq = ibv_create_cq(conn->s_ctx->ctx, 16384, NULL, conn->s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
    TEST_NZ(ibv_req_notify_cq(conn->s_ctx->cq, 0));

    TEST_NZ(pthread_create(&conn->s_ctx->cq_poller_thread, NULL, poll_cq, conn));
}

void build_params(struct rdma_conn_param *params){
    memset(params, 0, sizeof(*params));

    params->initiator_depth = params->responder_resources = 1;
    params->rnr_retry_count = 7; /* infinite retry */
    params->retry_count = 7;
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr, struct connection *conn){
    memset(qp_attr, 0, sizeof(*qp_attr));

    qp_attr->send_cq = conn->s_ctx->cq;
    qp_attr->recv_cq = conn->s_ctx->cq;
    qp_attr->qp_type = IBV_QPT_RC;

    qp_attr->cap.max_send_wr = 256;
    qp_attr->cap.max_recv_wr = 256;
    qp_attr->cap.max_send_sge = 1;
    qp_attr->cap.max_recv_sge = 1;
}

void destroy_connection(void *context){
    struct connection *conn = (struct connection *)context;

    conn->should_stop_polling = true;

    rdma_destroy_qp(conn->id);

    ibv_dereg_mr(conn->send_mr);
    ibv_dereg_mr(conn->recv_mr);
    ibv_dereg_mr(conn->rdma_local_mr_w);
    ibv_dereg_mr(conn->rdma_local_mr_r);
    //ibv_dereg_mr(conn->rdma_remote_mr);

    free(conn->send_msg);
    free(conn->recv_msg);
    free(conn->rdma_local_region_r);
    free(conn->rdma_local_region_w);
    //free(conn->rdma_remote_region);

    rdma_destroy_id(conn->id);

    free(conn);
}

void * get_local_message_region(void *context){
    return ((struct connection *)context)->rdma_local_region_r;
}

 char * get_peer_message_region(struct connection *conn){
    return conn->rdma_remote_region;
}

void on_completion(struct ibv_wc *wc){
    struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

    if (wc->status != IBV_WC_SUCCESS){
        printf("Failed. Status: %d - %s \n", wc->status, ibv_wc_status_str(wc->status));
        conn->should_stop_polling = true;
    }

    if (wc->opcode & IBV_WC_RECV){

        if (conn->recv_msg->type == MSG_R_MR){

            conn->recv_state = RS_MR_RECV;
            memcpy(&conn->peer_mr, &conn->recv_msg->data.mr, sizeof(conn->peer_mr));
            receive_remote_mem(conn, RDMA_BUFFER_SIZE, (get_tracker().max_id  - 11212) % 4); // ?
            printf("Page allocated in remote tenant. ");
        }
        else if (conn->recv_msg->type == MSG_DONE)
            conn->recv_state = RS_DONE_RECV;
    } 
    else{
        // if (conn->send_msg->type == MSG_L_MR)
        //     printf("Local MR sent.\n");

        if (conn->send_msg->type == MSG_R_MR){
            printf("Page allocated to remote tenant.\n");
            rdma_transfer_state = DONE;
        }

        if (conn->send_msg->type == MSG_DONE)
            conn->send_state = SS_DONE_SENT;
    }

    if (conn->send_state == SS_DONE_SENT && conn->recv_state == RS_DONE_RECV){ // All connections should be kept alive. These states are unlikely to happen.
        printf("Peer is ready to disconnect. %s\n", get_peer_message_region(conn));
        rdma_disconnect(conn->id);
        conn->should_stop_polling = true;
    }
}

void on_connect(void *context){
    ((struct connection *)context)->connected = 1;
}

void * poll_cq(struct connection *conn){
    int batch_size = 10;
    int i = 0;
    struct ibv_cq *cq;
    void *ev_ctx;
    struct ibv_wc *wc = (struct ibv_wc *) malloc(batch_size * sizeof(struct ibv_wc));
    memset(wc, 0, batch_size * sizeof(struct ibv_wc));

    while (1) {
        TEST_NZ(ibv_get_cq_event(conn->s_ctx->comp_channel, &cq, &ev_ctx));
        ibv_ack_cq_events(cq, 1);
        TEST_NZ(ibv_req_notify_cq(cq, 0));

        while (ibv_poll_cq(cq, batch_size, wc)){
            i = 0;
            while((wc + i)->byte_len != 0)
                on_completion(wc + (i++));
            memset(wc, 0, batch_size * sizeof(struct ibv_wc));
        }

        if (conn->should_stop_polling)
            break;
    }

    return NULL;
}

void post_receives(struct connection *conn){
    struct ibv_recv_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    wr.wr_id = (uintptr_t)conn;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    sge.addr = (uintptr_t)conn->recv_msg;
    sge.length = sizeof(struct message);
    sge.lkey = conn->recv_mr->lkey;

    if(ibv_post_recv(conn->qp, &wr, &bad_wr) && errno != 0){
        printf("ibv_post_recv failed(1). ERROR: %s\n", strerror(errno));
    }
}

void register_memory(struct connection *conn){
    conn->send_msg = malloc(sizeof(struct message));
    conn->recv_msg = malloc(sizeof(struct message));

    conn->rdma_local_region_r = malloc(RDMA_BUFFER_SIZE * 10);
    conn->rdma_local_region_w = malloc(RDMA_BUFFER_SIZE * 10);

    conn->last_used_addr = conn->rdma_local_region_r;

    TEST_Z(conn->send_mr = ibv_reg_mr(
        conn->s_ctx->pd, 
        conn->send_msg, 
        sizeof(struct message), 
        0));

    TEST_Z(conn->recv_mr = ibv_reg_mr(
        conn->s_ctx->pd, 
        conn->recv_msg, 
        sizeof(struct message), 
        IBV_ACCESS_LOCAL_WRITE));

    TEST_Z(conn->rdma_local_mr_r = ibv_reg_mr(
        conn->s_ctx->pd, 
        conn->rdma_local_region_r, 
        RDMA_BUFFER_SIZE * 10, 
        IBV_ACCESS_LOCAL_WRITE));
    
    TEST_Z(conn->rdma_local_mr_w = ibv_reg_mr(
        conn->s_ctx->pd, 
        conn->rdma_local_region_w, 
        RDMA_BUFFER_SIZE * 10, 
        IBV_ACCESS_LOCAL_WRITE));
}

void send_message(struct connection *conn){
    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    memset(&wr, 0, sizeof(wr));

    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = (uintptr_t)conn->send_msg;
    sge.length = sizeof(struct message);
    sge.lkey = conn->send_mr->lkey;

    if(ibv_post_send(conn->qp, &wr, &bad_wr) && errno != 0){
        printf("ibv_post_send failed(2). ERROR: %s\n", strerror(errno));
    }
}

void send_mr(void *context){
    struct connection *conn = (struct connection *)context;

    conn->send_msg->type = MSG_R_MR;
    memcpy(&conn->send_msg->data.mr, conn->rdma_remote_mr, sizeof(struct ibv_mr));

    send_message(conn);
}

void send_remote_page(void *context) {    /* registers the latest released page as remote region and sends to remote tenant*/
    struct connection *conn = (struct connection *)context;
    if(remote_region == NULL)
        return;
    else
        conn->rdma_remote_region = remote_region;

    TEST_Z(conn->rdma_remote_mr = ibv_reg_mr(
        conn->s_ctx->pd, 
        conn->rdma_remote_region, 
        RDMA_BUFFER_SIZE, 
       (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)));

    memset(conn->rdma_remote_region, 0, RDMA_BUFFER_SIZE);
        
    conn->send_msg->type = MSG_R_MR;
    memcpy(&conn->send_msg->data.mr, conn->rdma_remote_mr, sizeof(struct ibv_mr));

    send_message(conn);

    remote_region = NULL;
}

static inline void now_epoch_sec_nsec(long *sec, long *nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = ts.tv_sec;
    *nsec = ts.tv_nsec;
}

// -----------------------------
// Overload protection knobs
// -----------------------------
#define MAX_OUTSTANDING_RDMA  256

// EWMA timeout thresholds (ppm = parts-per-million; 1.0 == 1,000,000)
#define PPM                   1000000u
#define PPM_HIGH              200000u   // enter overload at >= 20% timeouts
#define PPM_LOW               50000u    // exit overload at <= 5% timeouts

// EWMA update smoothing (alpha = 0.05)
#define ALPHA_PPM             50000u

// Time-decay so a tenant can recover even if it stops issuing RDMA
#define DECAY_INTERVAL_NS     50000000LL   // 50 ms between decay steps
#define DECAY_FACTOR_PPM      980000u      // multiply ewma by 0.98 each step

// Optional: allow occasional probe RDMA while overloaded (prevents "stuck" cases)
#define ENABLE_PROBES         1
#define PROBE_INTERVAL_NS     20000000LL   // allow 1 probe every 20 ms

// Per-request wait cap
#define WAIT_TIMEOUT_NS       100000LL     // 0.1 ms

// -----------------------------
// Global state (per victim / per process)
// -----------------------------
static _Atomic int64_t  victim_outstanding_rdma = 0;
static _Atomic int      rdma_overloaded = 0;

static _Atomic uint32_t ewma_timeout_ppm = 0;

// time-based decay + probes use time in ns
static _Atomic uint64_t last_decay_ns = 0;
#if ENABLE_PROBES
static _Atomic uint64_t last_probe_ns = 0;
#endif

// -----------------------------
// Helpers
// -----------------------------
static inline uint32_t ewma_update(uint32_t old, uint32_t sample_ppm, uint32_t alpha_ppm) {
    // new = alpha*sample + (1-alpha)*old, all in ppm
    uint64_t newv = (uint64_t)alpha_ppm * sample_ppm + (uint64_t)(PPM - alpha_ppm) * old;
    return (uint32_t)(newv / PPM);
}

static inline void ewma_push_sample(uint32_t sample_ppm) {
    uint32_t oldv, newv;
    do {
        oldv = atomic_load(&ewma_timeout_ppm);
        newv = ewma_update(oldv, sample_ppm, ALPHA_PPM);
    } while (!atomic_compare_exchange_weak(&ewma_timeout_ppm, &oldv, newv));
}

static inline uint64_t now_ns(void) {
    long s, ns;
    now_epoch_sec_nsec(&s, &ns);
    return (uint64_t)s * 1000000000ULL + (uint64_t)ns;
}

// Decay EWMA over time so a tenant can recover even if it stops issuing RDMA.
static inline void ewma_time_decay(void) {
    uint64_t t = now_ns();

    uint64_t last = atomic_load(&last_decay_ns);
    if (t - last < (uint64_t)DECAY_INTERVAL_NS) return;

    // best effort: only one thread performs decay step per interval
    if (!atomic_compare_exchange_strong(&last_decay_ns, &last, t)) return;

    uint32_t oldv, newv;
    do {
        oldv = atomic_load(&ewma_timeout_ppm);
        newv = (uint32_t)(((uint64_t)oldv * DECAY_FACTOR_PPM) / PPM); // multiply by 0.98
    } while (!atomic_compare_exchange_weak(&ewma_timeout_ppm, &oldv, newv));
}

#if ENABLE_PROBES
// While overloaded, allow a single probe RDMA occasionally so we can observe recovery
// even if time decay is slow or thresholds are tight.
static inline int allow_probe(void) {
    uint64_t t = now_ns();
    uint64_t lp = atomic_load(&last_probe_ns);
    if (t - lp < (uint64_t)PROBE_INTERVAL_NS) return 0;
    return atomic_compare_exchange_strong(&last_probe_ns, &lp, t);
}
#endif

// Reserve an outstanding slot safely (race-proof).
// Returns 1 if reserved, 0 if cap reached.
static inline int reserve_outstanding_slot(void) {
    int64_t prev = atomic_fetch_add(&victim_outstanding_rdma, 1);
    if (prev >= MAX_OUTSTANDING_RDMA) {
        atomic_fetch_sub(&victim_outstanding_rdma, 1);
        return 0;
    }
    return 1;
}

static inline void release_outstanding_slot(void) {
    atomic_fetch_sub(&victim_outstanding_rdma, 1);
}

char* get_remote_item(remote_item* r_it) {   /* RDMA_READ remote item */

    // 1) Let EWMA recover over time even if no RDMA is issued.
    ewma_time_decay();

    // 2) Overload latch with hysteresis.
    uint32_t ewma_now = atomic_load(&ewma_timeout_ppm);

    if (atomic_load(&rdma_overloaded)) {
        if (ewma_now <= PPM_LOW) {
            atomic_store(&rdma_overloaded, 0);
        } else {
#if ENABLE_PROBES
            // Allow rare probes while overloaded; otherwise fail fast.
            if (!allow_probe()) return NULL;
#else
            return NULL;
#endif
        }
    } else {
        if (ewma_now >= PPM_HIGH) {
            atomic_store(&rdma_overloaded, 1);
#if ENABLE_PROBES
            // still allow a probe right after entering overload? usually "no"
            // return NULL;
            if (!allow_probe()) return NULL;
#else
            return NULL;
#endif
        }
    }

    // 3) Outstanding cap (race-proof reserve).
    if (!reserve_outstanding_slot())
        return NULL;

    // ---- RDMA READ setup ----
    slabclass_t *p = (slabclass_t *) get_slabclass(r_it->slabs_clsid);
    struct connection *conn = p->conn[r_it->page_id];

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;
    memset(&wr, 0, sizeof(wr));

    conn->send_msg->type = MSG_L_MR;

    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = (uintptr_t)r_it->address;
    wr.wr.rdma.rkey = p->rkey[r_it->page_id];

    if (prevsize < p->size)
        conn->last_used_addr += p->size;
    else
        conn->last_used_addr += prevsize;

    if (conn->last_used_addr + p->size >= conn->rdma_local_region_r + RDMA_BUFFER_SIZE * 10)
        conn->last_used_addr = conn->rdma_local_region_r;

    prevsize = p->size;

    memset(conn->last_used_addr, 0, p->size);

    sge.addr = (uintptr_t)conn->last_used_addr;
    sge.length = p->size;
    sge.lkey = conn->rdma_local_mr_r->lkey;

    if (ibv_post_send(conn->qp, &wr, &bad_wr)) {
        // Treat as a "timeout/failure" sample
        ewma_push_sample(PPM);
        release_outstanding_slot();
        // printf("ibv_post_send failed. errno=%d (%s)\n", errno, strerror(errno));
        return NULL;
    }

    // 4) Wait up to WAIT_TIMEOUT_NS for readiness
    long w0s, w0ns, w1s, w1ns;
    now_epoch_sec_nsec(&w0s, &w0ns);

    while (((item *)conn->last_used_addr)->nbytes == 0) {/* can't progress unless the whole item */
        sched_yield();                                    /* being read via RDMA_READ is ready */

        now_epoch_sec_nsec(&w1s, &w1ns);
        int64_t waited_ns = (w1s - w0s) * 1000000000LL + (w1ns - w0ns);

        if (waited_ns >= WAIT_TIMEOUT_NS) {
            // Timeout sample
            ewma_push_sample(PPM);
            release_outstanding_slot();
            return NULL;
        }
    }

    // Success sample
    ewma_push_sample(0);
    release_outstanding_slot();
    return conn->last_used_addr;
}

void set_remote_item(remote_item *r_it, item *it){  /* Write the temp item to remote memory 
                                                       w/o notifying the host (RDMA_WRITE) */
    slabclass_t *p = (slabclass_t *) get_slabclass(r_it->slabs_clsid);
    struct connection *conn = p->conn[r_it->page_id];

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    //printf("Attempting to write remotely...\n");

    memset(&wr, 0, sizeof(wr));

    conn->send_msg->type = MSG_L_MR;

    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = (uintptr_t)r_it->address;
    wr.wr.rdma.rkey = p->rkey[r_it->page_id];

    memcpy(conn->rdma_local_region_w, it, p->size);
    sge.addr = (uintptr_t)conn->rdma_local_region_w;
    sge.length = p->size;
    sge.lkey = conn->rdma_local_mr_w->lkey;

    if(ibv_post_send(conn->qp, &wr, &bad_wr) && errno != 0){
        printf("ibv_post_send failed(1). ERROR: %d: %s\n", errno, strerror(errno));
    }
}

remote_item* create_remote_item(u_int8_t clsid){
    
    remote_item* remote_it = (remote_item*) malloc(sizeof(struct _remitem));
    memset(remote_it, 0, sizeof(struct _remitem));
    assert(remote_it);
    remote_it->next = NULL;
    remote_it->prev = NULL;
    remote_it->h_next = NULL;
    remote_it->slabs_clsid = clsid;
    //printf("Succesfully created remote item: %s\n", remote_it->key);
    return remote_it;
}

remote_item* slabs_remoteq_lookup(char *key, const size_t nkey){
    uint32_t hv = hash(key, nkey);
    remote_item* remote_it = remote_assoc_find(key, nkey, hv);
    return remote_it;
}

void slabs_rdma_insert(remote_item *it) {
    uint32_t hv = hash(it->key, it->nkey);
    remote_assoc_insert(it, hv);
} 