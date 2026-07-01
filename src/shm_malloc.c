#include "shm_malloc.h"

void init_trck_access(sem_t ** mutex, struct tracker ** track, int * tracker_fd){
    /* open semaphore */
    if ((*mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(*mutex);
    /* open tracker */
    *tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (*tracker_fd == -1) {
        printf("tracker file not found!\n");
        return;
    }

    *track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, *tracker_fd, 0);
    if (*track == MAP_FAILED) {
        return;
    }
}

void term_trck_access(sem_t * mutex, struct tracker * track, int tracker_fd){

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);
}

void * shm_malloc(size_t n) {
    // pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
    int tracker_fd;
    int slabs_fd;

    sem_t * mutex;
    struct tracker * track;
    void * rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;
    printf("page size: %lu\n", PAGESIZE); 

    init_trck_access(&mutex, &track, &tracker_fd);
  
    // open shared slab memory region
    slabs_fd = shm_open(slab_name, O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }
    printf("slabs fd: %d\n",slabs_fd);
	
    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }

    // pthread_mutex_lock(&shm_lock);	
    printf("current max memory %lu\n", track->max_size);
    printf("requested allocation: %lu\n", n);
	
    /* align to nearest page */
    int index = n / PAGESIZE;
    n = (index + 1) * PAGESIZE;

    if (write (slabs_fd, "", 1) != 1){
	printf ("write error");
     	return NULL;
    }	

    /* get the next available address of the shared memory */  
    rptr = mmap( ((char*)track->start_address) , n,
        PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd, track->used_size);

    if (rptr == MAP_FAILED) {
        printf("mmap failed\n");
    return NULL;
    }

    // pthread_mutex_unlock(&shm_lock);
	
    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->used_size + n) {
        return NULL;
    }

    /* update allocated memory size */
    track->start_address = (long *) track->start_address - (n / 8);
    track->avail_address = rptr;	  
	track->used_size = track->used_size + (size_t)n;

    printf("return pointer %p\n", rptr);
    printf("total allocated mem(all): %lu\n", track->used_size);

    close(slabs_fd);
    term_trck_access(mutex, track, tracker_fd); 
    return rptr;
}

void * shm_malloc_spare(size_t n, int id){

    int slabs_fd;
    int tracker_fd;

    struct tracker * track;
    sem_t * mutex;
    void * rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;

    init_trck_access(&mutex, &track, &tracker_fd);

    // open shared slab memory region
    slabs_fd = shm_open(slab_name, O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }

    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }

    int index = n / PAGESIZE;
    n = (index + 1) * PAGESIZE;

    if(track->spare_size < n){
        //printf(" Available size is less than the Requested!\n");
        n = track->spare_size;
    }

    rptr = mmap(track->spare_mem_start , n,
        PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd,track->spare_off);

    if (rptr == MAP_FAILED) {
        printf("mmap failed at address: %p  requested size: %lu\n",track->spare_mem_start,n);
        return NULL;
    }

    track->preset_share[id] += n;
 
    close(slabs_fd);
    term_trck_access(mutex, track, tracker_fd); 
    return rptr;
}

void signal_alloc_free(int id, size_t size){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    int index = size / PAGESIZE;
    size = (index + 1) * PAGESIZE;    

    void* rptr = track->start_address;
    printf("address %p\n", rptr);
    track->start_address = (long *) track->start_address - (size / 8);
	track->used_size = track->used_size + size;
    printf("total allocated mem(all): %lu\n", track->used_size);

    term_trck_access(mutex, track, tracker_fd);

    set_spare_mem(rptr, size, 1, id);  
}

void set_spare_mem(void * ptr, size_t n, int cls, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->spare_off = track->start - ptr;
    track->spare_mem_start = ptr;
    track->spare_mem_clsid = cls;
    track->spare_mem_avail = true;
    track->spare_size = n;
    track->spare_mem_owner = id + 11212;

    track->preset_share[id] -= n;

    term_trck_access(mutex, track, tracker_fd); 
}

void set_scores(double sc1, double sc2, long long id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(sc2 == __DBL_MAX__ && track->min_id == id){ // has non-zero miss rate, should not be victim, was a candidate before by mistake
        track->min_id = -1;
        track->min_score = 9999999999;
        track->victim_potential = false;
    }

    if(sc2 < track->min_score && !track->min_score_lock){
        if(track->min_id != id)
            track->min_counter = 0;
        track->min_id = id;
        track->min_score = sc2;
        if(id < 99999)      /* local tenant*/
            track->victim_potential = true;
    }
    
    if(sc1 > track->max_score && !track->max_score_lock){
        if(track->max_id != id)
            track->max_counter = 0;
        track->max_id = id;
        track->max_score = sc1;

        if(track->min_id == id && !track->min_score_lock){ /* A tenant cannot be highest and lowest at the same time */
            track->min_id = -1;
            track->min_score = 9999999999;
        }
    }

    term_trck_access(mutex, track, tracker_fd);  
}

int compare_minID(long long id, double sc){   /* set_scores should be called beforehand */

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    int b = 0;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(id == track->min_id){
        track->min_score = sc;
        b = (++track->min_counter);
    }

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

int compare_maxID(long long id, double sc){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    int b = 0;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(id == track->max_id){
        track->max_score = sc;
        b = (++track->max_counter);
    }

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

int get_spare_clsid(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    int victim_id = -1;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    victim_id = track->spare_mem_clsid;
    
    term_trck_access(mutex, track, tracker_fd);  
    return victim_id;
}

int get_spare_owner(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    int victim_id = -1;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    victim_id = track->spare_mem_owner;
    
    term_trck_access(mutex, track, tracker_fd);  
    return victim_id;
}

bool req_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(track->spare_requested == false){
        track->spare_requested = true;
        b = true;
    }

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

bool req_rem_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(track->remote_spare_requested == false){
        track->remote_spare_requested = true;
        track->rdma_broadcast = true;
        b = true;
    }

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

bool spare_needed(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    b = track->spare_requested;

    term_trck_access(mutex, track, tracker_fd);  
    return b;
}

bool new_mem_available(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    b = track->spare_mem_avail;

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

void reset_locks(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->spare_mem_owner = 0;
    track->spare_mem_avail = false;
    track->spare_requested = false;
    track->remote_spare_requested = false;

    term_trck_access(mutex, track, tracker_fd); 
}

bool lock_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(track->spare_lock == false){
        track->spare_lock = true;
        b = true;
    }

    term_trck_access(mutex, track, tracker_fd);
    return b;
}

void unlock_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->spare_lock = false;

    term_trck_access(mutex, track, tracker_fd); 
}

bool r_mem_received(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    b = track->remote_spare_received;

    term_trck_access(mutex, track, tracker_fd);
    return b;
}

void receive_remote_mem(struct connection * conn, size_t n, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->latest_conn = conn;
    track->remote_spare_received = true;

    track->preset_share[id] += n;
    track->spare_size = n;

    term_trck_access(mutex, track, tracker_fd);
}


void set_remote_mem(void * addr, size_t n, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    // track->remote_addr = addr;
    // track->spare_size = n;
    track->preset_share[id] -= n;

    term_trck_access(mutex, track, tracker_fd);
}

void set_rdma_access_port(int rdma_access_port){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->local_access_port_n = rdma_access_port;

    term_trck_access(mutex, track, tracker_fd);
}

void stop_RDMA_broadcast(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->rdma_broadcast = false;

    term_trck_access(mutex, track, tracker_fd); 
}

void stop_RDMA_finish_broadcast(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->finish_broadcast = false;

    term_trck_access(mutex, track, tracker_fd); 
}

void finish_broadcast(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->finish_broadcast = true;
    track->temp_score = track->max_score;

    term_trck_access(mutex, track, tracker_fd); 
}

void set_rdma_server_info(struct sockaddr_in peer_addr, int peer_id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);

    track->remote_server_ready = true; 
    track->rdma_peer_addr = peer_addr;
    track->rdma_peer_id = peer_id;
    track->remote_spare_requested = true;

    term_trck_access(mutex, track, tracker_fd);
}

bool r_server_available(int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);

    if(track->max_id == id)   // Only the candidate tenant should be notified to get a page
        b = track->remote_server_ready;

    term_trck_access(mutex, track, tracker_fd); 
    return b;
}

bool remote_spare_needed(int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);

	if(track->min_id == id)   // Only the candidate tenant should be notified to release a page
        b = track->remote_spare_requested;

    term_trck_access(mutex, track, tracker_fd);
    return b;
}

void reset_remote_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->remote_spare_requested = false;
    track->remote_spare_received = false;
    track->remote_server_ready = false;
    memset(&(track->rdma_peer_addr), 0, sizeof(struct sockaddr_in));
    track->rdma_peer_id = 0;
    track->local_access_port_n = 0;
    track->min_counter = 0;
    track->max_counter = 0;

    term_trck_access(mutex, track, tracker_fd);
}
bool lock_max_score(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    bool b = false;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(track->max_score_lock == false){
        track->max_score_lock = true;
        b = true;
    }

    term_trck_access(mutex, track, tracker_fd);
    return b;
}

void unlock_max_score(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->max_score_lock = false;
    track->max_score = -1;
    track->max_id = -1;

    term_trck_access(mutex, track, tracker_fd); 
}

int lock_min_score(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    int b = 0;

    init_trck_access(&mutex, &track, &tracker_fd);

    if(track->min_id == -1)
        b = 1;
    else if(track->min_score_lock == false){
        track->min_score_lock = true;
        b = 2;
    }

    term_trck_access(mutex, track, tracker_fd);
    return b;
}

void unlock_min_score(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    track->min_score_lock = false;

    term_trck_access(mutex, track, tracker_fd); 
}

void reset_min_score(){
    
    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;

    init_trck_access(&mutex, &track, &tracker_fd);
	
    if(!track->min_score_lock){
        track->min_score = 9999999999;
        track->min_id = -1;
    }

    term_trck_access(mutex, track, tracker_fd); 
}

struct tracker get_tracker(void) {
	
    int tracker_fd;
    sem_t * mutex;
    struct tracker * track;
    struct tracker track2;

    init_trck_access(&mutex, &track, &tracker_fd);
    track2 = *track;
    term_trck_access(mutex, track, tracker_fd); 
    return track2;
}

void init_shared_names(int x) {
    int y = (x - 11212) / 4;
    sprintf(semaph_name,"/semaph%d",y); 
    sprintf(tracker_name,"/tracker%d",y); 
    sprintf(slab_name,"/slabs%d",y); 
}