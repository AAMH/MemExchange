#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include "shm_malloc.h"
#include <pthread.h>
#include <sys/stat.h>
#include <semaphore.h>

char tracker_name[20], semaph_name[20], slab_name[20];

void * shm_malloc(size_t n) {
//    pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
    int tracker_fd;
    int slabs_fd;

    sem_t * mutex;
    struct tracker* track;
    void* rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;
    printf("page size: %lu\n", PAGESIZE); 

    // open semaphore
    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);
	
    // open tracker  
    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    printf("tracker fd: %d\n",tracker_fd);    
    if (tracker_fd == -1) {
        printf("tracker file not found\n");
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
  
    // open shared slab memory region
    slabs_fd = shm_open(slab_name, O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }
    printf("slabs fd: %d\n",slabs_fd);
	
    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }
	
    /* align to nearest page */

 //   pthread_mutex_lock(&shm_lock);	
    printf("current max memory %lu\n", track->max_size);
    printf("requested allocation: %lu\n", n);
	
    int index = n/PAGESIZE;
    n = (index+1) * PAGESIZE;    
    if (write (slabs_fd, "", 1) != 1)
    {
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

//    pthread_mutex_unlock(&shm_lock);
	
    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->used_size + n) {
        return NULL;
    }

    /* update allocated memory size */
    track->start_address = (long *) track->start_address - (n/8);
    track->avail_address = rptr;	  
	track->used_size = track->used_size + (size_t)n;

    printf("return pointer %p\n", rptr);
    printf("total allocated mem(all): %lu\n", track->used_size);

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    close(slabs_fd);
    sem_post(mutex); 
    return rptr;
}

void * shm_mallocAt(size_t n, int id){

    int slabs_fd;
    int tracker_fd;

    struct tracker* track;
    sem_t * mutex;
    void* rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;

    // open semaphore
    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);
  
    // open shared slab memory region
    slabs_fd = shm_open(slab_name, O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }

    // open tracker  
    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found\n");
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }

    int index = n/PAGESIZE;
    n = (index+1) * PAGESIZE;

    if(track->spare_size < n){
        printf(" Available size is less than the Requested!\n");
        n = track->spare_size;
    }

    rptr = mmap(track->spare_mem_start , n,
        PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd,track->spare_off);

    if (rptr == MAP_FAILED) {
        printf("mmap failed at address: %p  requested size: %lu\n",track->spare_mem_start,n);
        return NULL;
    }

    track->preset_share[id] += n;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    close(slabs_fd);
    sem_post(mutex); 
    return rptr;
}

bool signal_alloc_free(int id, size_t size){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    int index = size/PAGESIZE;
    size = (index+1) * PAGESIZE;    

    void* rptr = track->start_address;
    printf("address %p\n", rptr);
    track->start_address = (long *) track->start_address - (size/8);
	track->used_size = track->used_size + size;
    printf("total allocated mem(all): %lu\n", track->used_size);

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);

    set_spare_mem(rptr,size,1,id);  
    return b;
}

void * set_spare_mem(void * ptr, size_t n, int cls, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    void * victim;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    track->spare_off = track->start - ptr;
    track->spare_mem_start = ptr;
    track->spare_mem_clsid = cls;
    track->spare_mem_avail = true;
    track->spare_size = n;

    track->preset_share[id] -= n;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return victim;
}

bool set_scores(double sc1, double sc2, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    if(sc2 < track->min_score){
        track->min_score = sc2;
        track->min_id = id;
    }
    
    if(sc1 > track->max_score){
        track->max_score = sc1;
        track->max_id = id;

        if(track->min_id == id){
            track->min_id == -1;
            track->min_score = 9999999999;
        }
    }

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool compare_minID(int id, double sc){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    if(id == track->min_id){
        b = true;
        track->min_score = sc;
    }

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool compare_maxID(int id, double sc){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    if(id == track->max_id){
        b = true;
        track->max_score = sc;
    }

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

int get_spare_clsid(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    int victim_id = -1;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return -1;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return -1;
    }
	
    victim_id = track->spare_mem_clsid;
    
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return victim_id;
}

bool req_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    if(track->spare_requested == false){
        track->spare_requested = true;
        b = true;
    }
    else{
        b = false; 
    }

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool spare_needed(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    b = track->spare_requested;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool is_spare_avail(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    b = track->spare_mem_avail;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool reset_locks(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    track->spare_mem_avail = false;
    track->spare_requested = false;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool lock_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    if(track->spare_lock == false){
        track->spare_lock = true;
        b = true;
    }
    else{
        b = false; 
    }

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool unlock_spare(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    track->spare_lock = false;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

struct tracker get_tracker(void) {
	
    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    struct tracker track2;

    if ((mutex = sem_open(semaph_name, 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open(tracker_name,  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return track2;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ , MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return track2;
    }
	
    track2 = *track;
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return track2;
}

void init_shared_names(int x) {
    int y = (x - 11212) / 4;
    sprintf(semaph_name,"/semaph%d",y); 
    sprintf(tracker_name,"/tracker%d",y); 
    sprintf(slab_name,"/slabs%d",y); 
}