#include "rdma_util.h"

struct rdma_cm_id * lookup_rdma_connection(struct sockaddr_in peer_addr){

    if(!rdma_connections_head)
        return NULL;

    struct connection * curr = (struct connection *)rdma_connections_head->context;
    struct rdma_cm_id * ret  = NULL;

    struct sockaddr_in* curr_addr = (struct sockaddr_in *)rdma_get_peer_addr(curr->id);

    // char ip[INET_ADDRSTRLEN], ip_string[INET_ADDRSTRLEN];;
    // inet_ntop( AF_INET, &curr_addr->sin_addr, ip, INET_ADDRSTRLEN );
    // inet_ntop( AF_INET, &peer_addr.sin_addr, ip_string, INET_ADDRSTRLEN );

    // printf("head connection ip: %s, looked up ip: %s\n\n", ip, ip_string);

    while(curr->next && curr_addr->sin_addr.s_addr != peer_addr.sin_addr.s_addr){
        curr = curr->next;
        curr_addr = (struct sockaddr_in *)rdma_get_peer_addr(curr->id);
    }
    
    if(curr_addr->sin_addr.s_addr == peer_addr.sin_addr.s_addr)
        ret = curr->id;

    return ret;
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

    s_ctx = NULL;
    should_stop_polling = false;

    build_context(id->verbs);
    build_qp_attr(&qp_attr);

    TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));

    //id->context = conn = (struct connection *)malloc(sizeof(struct connection));

    conn->id = id;
    conn->qp = id->qp;

    conn->send_state = SS_INIT;
    conn->recv_state = RS_INIT;

    conn->connected = 0;

    register_memory(conn);
    post_receives(conn);
}

void build_context(struct ibv_context *verbs){
    if (s_ctx) {
        if (s_ctx->ctx != verbs)
        die("cannot handle events in more than one context.");

        return;
    }

    s_ctx = (struct context *)malloc(sizeof(struct context));

    s_ctx->ctx = verbs;

    TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
    TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
    TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
    TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));

    TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));
}

void build_params(struct rdma_conn_param *params){
    memset(params, 0, sizeof(*params));

    params->initiator_depth = params->responder_resources = 1;
    params->rnr_retry_count = 7; /* infinite retry */
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr){
    memset(qp_attr, 0, sizeof(*qp_attr));

    qp_attr->send_cq = s_ctx->cq;
    qp_attr->recv_cq = s_ctx->cq;
    qp_attr->qp_type = IBV_QPT_RC;

    qp_attr->cap.max_send_wr = 1000;
    qp_attr->cap.max_recv_wr = 1000;
    qp_attr->cap.max_send_sge = 10;
    qp_attr->cap.max_recv_sge = 10;
}

void destroy_connection(void *context){
    struct connection *conn = (struct connection *)context;

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
        should_stop_polling = true;
    }

    if (wc->opcode & IBV_WC_RECV){

        if (conn->recv_msg->type == MSG_R_MR){

            conn->recv_state = RS_MR_RECV;
            memcpy(&conn->peer_mr, &conn->recv_msg->data.mr, sizeof(conn->peer_mr));
            receive_remote_mem(conn, RDMA_BUFFER_SIZE, (get_tracker().max_id  - 11212) % 4); /// ???

            printf("Page allocated in remote tenant. ");
            rdma_page_transfer_in_progress = false;
        /*
            struct ibv_send_wr wr, *bad_wr = NULL;
            struct ibv_sge sge;

            if (s_mode == M_WRITE)
                printf("Attempting to write remotely...\n");
            else
                printf("Attempting to read remotely...\n");

            memset(&wr, 0, sizeof(wr));

            wr.wr_id = (uintptr_t)conn;
            wr.opcode = (s_mode == M_WRITE) ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = (uintptr_t)conn->peer_mr.addr;
            wr.wr.rdma.rkey = conn->peer_mr.rkey;

            memset(conn->rdma_local_region, 0, RDMA_BUFFER_SIZE);
            sge.addr = (uintptr_t)conn->rdma_local_region;
            sge.length = RDMA_BUFFER_SIZE;
            sge.lkey = conn->rdma_local_mr->lkey;

            TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
            
            conn->send_msg->type = MSG_DONE;
            send_message(conn);
            post_receives(conn); 
        */

        }
        else if (conn->recv_msg->type == MSG_DONE){
            conn->recv_state = RS_DONE_RECV;
        }
    } 
    else{
        
        if (conn->send_msg->type == MSG_L_MR){
            //printf("Local MR sent.\n");
            rdma_page_transfer_in_progress = false;
        }

        if (conn->send_msg->type == MSG_R_MR){
            printf("Page allocated to remote tenant.\n");
            rdma_page_transfer_in_progress = false;
        }

        if (conn->send_msg->type == MSG_DONE){
            conn->send_state = SS_DONE_SENT;
        }

    }

    if (conn->send_state == SS_DONE_SENT && conn->recv_state == RS_DONE_RECV){ // All connections should be kept alive. These states are unlikely to happen.
        printf("Peer is ready to disconnect. %s\n", get_peer_message_region(conn));
        rdma_disconnect(conn->id);
        should_stop_polling = true;
    }
}

void on_connect(void *context){
    ((struct connection *)context)->connected = 1;
}

void * poll_cq(void *ctx){
    int batch_size = 10;
    int i = 0;
    struct ibv_cq *cq;
    struct ibv_wc *wc = (struct ibv_wc *) malloc(batch_size * sizeof(struct ibv_wc));
    memset(wc, 0, batch_size * sizeof(struct ibv_wc));

    while (1) {
        TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
        ibv_ack_cq_events(cq, 1);
        TEST_NZ(ibv_req_notify_cq(cq, 0));

        while (ibv_poll_cq(cq, batch_size, wc)){
            i = 0;
            while((wc + i)->byte_len != 0)
                on_completion(wc + (i++));
            memset(wc, 0, batch_size * sizeof(struct ibv_wc));
        }

        if (should_stop_polling)
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

    //TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
    if(ibv_post_recv(conn->qp, &wr, &bad_wr) && errno != 0){
        printf("ibv_post_recv failed(1). ERROR: %s\n", strerror(errno));
    }
}

void register_memory(struct connection *conn){
    conn->send_msg = malloc(sizeof(struct message));
    conn->recv_msg = malloc(sizeof(struct message));

    conn->rdma_local_region_r = malloc(RDMA_BUFFER_SIZE);
    conn->rdma_local_region_w = malloc(RDMA_BUFFER_SIZE);

    last_used_addr = conn->rdma_local_region_r;

    sge_send = (struct ibv_sge *) malloc(sizeof(struct ibv_sge) * batch_size);
    wr_list_send = (struct ibv_send_wr *) malloc(sizeof(struct ibv_send_wr) * batch_size);

    TEST_Z(conn->send_mr = ibv_reg_mr(
        s_ctx->pd, 
        conn->send_msg, 
        sizeof(struct message), 
        0));

    TEST_Z(conn->recv_mr = ibv_reg_mr(
        s_ctx->pd, 
        conn->recv_msg, 
        sizeof(struct message), 
        IBV_ACCESS_LOCAL_WRITE));

    TEST_Z(conn->rdma_local_mr_r = ibv_reg_mr(
        s_ctx->pd, 
        conn->rdma_local_region_r, 
        RDMA_BUFFER_SIZE, 
        IBV_ACCESS_LOCAL_WRITE));
    
    TEST_Z(conn->rdma_local_mr_w = ibv_reg_mr(
        s_ctx->pd, 
        conn->rdma_local_region_w, 
        RDMA_BUFFER_SIZE, 
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

    //while (!conn->connected);

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
        s_ctx->pd, 
        conn->rdma_remote_region, 
        RDMA_BUFFER_SIZE, 
       (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)));

    memset(conn->rdma_remote_region, 0, RDMA_BUFFER_SIZE);
        
    conn->send_msg->type = MSG_R_MR;
    memcpy(&conn->send_msg->data.mr, conn->rdma_remote_mr, sizeof(struct ibv_mr));

    send_message(conn);

    remote_region = NULL;
}

char* get_remote_item(remote_item* r_it){     /* contact the remote host and get the item*/

    slabclass_t *p = (slabclass_t *) get_slabclass(r_it->slabs_clsid);
    struct connection *conn = p->conn[r_it->page_id];

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;

    //printf("Attempting to read remotely...\n");

    memset(&wr, 0, sizeof(wr));

    conn->send_msg->type = MSG_L_MR;

    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = (uintptr_t)r_it->address;
    wr.wr.rdma.rkey = p->rkey[r_it->page_id];

    last_used_addr += p->size;
    if(last_used_addr + p->size >= conn->rdma_local_region_r + RDMA_BUFFER_SIZE)
        last_used_addr = conn->rdma_local_region_r;

    memset(last_used_addr, 0, p->size);
    sge.addr = (uintptr_t)last_used_addr;
    sge.length = p->size;
    sge.lkey = conn->rdma_local_mr_r->lkey;
        
    if(ibv_post_send(conn->qp, &wr, &bad_wr)){
        printf("ibv_post_send failed(0). ERROR: %d: %s\n", errno, strerror(errno));
    }

    while(((item *)last_used_addr)->nbytes == 0)
        nanosleep(&((struct timespec){0,1}), &((struct timespec){0,0}));
        
    return last_used_addr;
}

void add_remote_set_entry(remote_item *r_it, item *it){

    slabclass_t *p = (slabclass_t *) get_slabclass(r_it->slabs_clsid);
    struct connection *conn = p->conn[r_it->page_id];
    struct ibv_send_wr *bad_wr_send = NULL;

    if(curr_wr_sge == 0){
        memset(sge_send, 0, sizeof(struct ibv_sge) * batch_size);
        memset(wr_list_send, 0, sizeof(struct ibv_send_wr) * batch_size);
    }
    else
        wr_list_send[curr_wr_sge - 1].next = &wr_list_send[curr_wr_sge];
        
    wr_list_send[curr_wr_sge].wr_id = (uintptr_t)conn;
    wr_list_send[curr_wr_sge].opcode = IBV_WR_RDMA_WRITE;
    wr_list_send[curr_wr_sge].sg_list = &sge_send[curr_wr_sge];
    wr_list_send[curr_wr_sge].num_sge = 1;
    wr_list_send[curr_wr_sge].send_flags = IBV_SEND_SIGNALED;
    wr_list_send[curr_wr_sge].wr.rdma.remote_addr = (uintptr_t)r_it->address;
    wr_list_send[curr_wr_sge].wr.rdma.rkey = p->rkey[r_it->page_id];

    memcpy(conn->rdma_local_region_w + curr_wr_sge * p->size, it, p->size);
    sge_send[curr_wr_sge].addr = (uintptr_t)conn->rdma_local_region_w + curr_wr_sge * p->size;
    sge_send[curr_wr_sge].length = p->size;
    sge_send[curr_wr_sge++].lkey = conn->rdma_local_mr_w->lkey;

    if(curr_wr_sge >= batch_size){
        curr_wr_sge = 0;
        conn->send_msg->type = MSG_L_MR;
        if(ibv_post_send(conn->qp, wr_list_send, &bad_wr_send) && errno != 0){
            printf("ibv_post_send failed(1). ERROR: %d: %s\n", errno, strerror(errno));
        }
    }

}

void set_remote_item(remote_item *r_it, item *it){

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