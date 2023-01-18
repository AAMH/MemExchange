/* 
    Initializes shared memory sections for memcached
    arguments: amount of memory being allocated in bytes
*/
#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <pthread.h>
#include "shm_malloc.c"

#define n 15

int tracker_fd[n];
int slabs_fd[n];
sem_t * mutex[n];

struct tracker* track[n];
void* slab_start[n];

char tracker_name[20], semaph_name[20], slab_name[20];

void *thread_routine(){

    struct tracker trck = get_tracker();
    
    int counter = 0;
    while(1){
        sleep(1); 
        counter++;
        printf("\033[2J\033[1;1H");
        printf("Used Memory: %ld, counter: %d\n", trck.used_size,counter); 
    }

    return NULL;
}

int main(int argc, char **argv)
{

    size_t mem_allocated;
    
    /* Parse arguments */
    if(argc == 1) {
        printf("No memory size specified, defaulting to 2GB\n");
        mem_allocated = 2147483648;
    }
    else if(argc > 2) {
        printf("Too many arguments, please only input the amount of shared memory in MB\n");
        exit(1);
    }
    else {
        mem_allocated = (size_t)(atoi(argv[1]))*1024*1024;
        printf("argument 1:%s\n", argv[1]);
    }

    /* Get maximum system shared memory*/
    FILE *f = fopen("/proc/sys/kernel/shmmax", "r");
    size_t max_sz = 0;
    char line[100];
    if (f == NULL)
    {
        printf("no shared memory max file found, defaulting to 2GB\n");
        max_sz = 2147483648;
    }
    else {
        fgets(line, sizeof(line), f);
        //printf("line: %s\n", line);
        max_sz = (size_t) atoi( line );
        //printf("max: %lu\n", (long) max_sz);
        fclose(f);
    }
    if (mem_allocated > max_sz-sizeof(struct tracker))
    {
        printf("exceeded maximum shared memory size, current max (in bytes): %d\n",(int)(max_sz-sizeof(struct tracker)));
        exit(1);
    }

    printf("Allocated memory for each shared-memory region(MB): %lu\n", (long) mem_allocated/1024/1024);
    
    /* Initialize Semaphore */
    
    for(int i = 0;i < n;i++){
        sprintf(semaph_name,"/semaph%d",i);
        if ((mutex[i] = sem_open(semaph_name, O_CREAT, 0644, 1)) == SEM_FAILED) {
            perror("semaphore initilization failed");
            exit(1);
        }
        semaph_name[0] = '\n';
    }
    
    /* Initialize tracker and shared memory */

    for(int i = 0;i < n;i++){
        sprintf(tracker_name,"/tracker%d",i);
        tracker_fd[i] = shm_open(tracker_name, O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        tracker_name[0] = '\n';
        
        sprintf(slab_name,"/slabs%d",i);
        slabs_fd[i] = shm_open(slab_name, O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        slab_name[0] = '\n';

        if (tracker_fd[i] == -1) {
            perror("couldn\'t create tracker struct\n");
            exit(1);
        }
        if (slabs_fd[i] == -1) {
            perror("couldn\'t create shared slab\n");
            exit(1);
        }
        if (ftruncate(tracker_fd[i], sizeof(struct tracker)) == -1) {
            perror("error truncating tracker\n");
            exit(1);
        }
        if (ftruncate(slabs_fd[i], mem_allocated) == -1) {
            perror("error truncating shared slab\n");
            exit(1);
        }
    }

    /* Map shared memory tracker object */

    for(int i = 0;i < n;i++){
        track[i] = mmap(NULL, sizeof(struct tracker),
            PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd[i], 0);
        
        track[i]->max_size = mem_allocated;
        track[i]->used_size = 0;
        
        track[i]->spare_mem_clsid = -1;
        track[i]->spare_mem_start = NULL;
        track[i]->spare_mem_avail = false;
        track[i]->spare_size = 0;

        track[i]->min_score = 9999999999;          
        track[i]->min_id = -1;
        track[i]->max_score = -1;          
        track[i]->max_id = -1;
    }

    track[0]->preset_share[0] = 0.5 * mem_allocated;
    track[0]->preset_share[1] = 0.5 * mem_allocated;
    track[0]->preset_share[2] = 0.23 * mem_allocated;
    track[0]->preset_share[3] = 0.256 * mem_allocated;

    track[1]->preset_share[0] = 0.257 * mem_allocated;
    track[1]->preset_share[1] = 0.257 * mem_allocated;
    track[1]->preset_share[2] = 0.23 * mem_allocated;
    track[1]->preset_share[3] = 0.256 * mem_allocated;

    track[2]->preset_share[0] = 0.257 * mem_allocated;
    track[2]->preset_share[1] = 0.257 * mem_allocated;
    track[2]->preset_share[2] = 0.23 * mem_allocated;
    track[2]->preset_share[3] = 0.256 * mem_allocated;

    track[3]->preset_share[0] = 0.257 * mem_allocated;
    track[3]->preset_share[1] = 0.257 * mem_allocated;
    track[3]->preset_share[2] = 0.23 * mem_allocated;
    track[3]->preset_share[3] = 0.256 * mem_allocated;

    track[4]->preset_share[0] = 0.257 * mem_allocated;
    track[4]->preset_share[1] = 0.257 * mem_allocated;
    track[4]->preset_share[2] = 0.23 * mem_allocated;
    track[4]->preset_share[3] = 0.256 * mem_allocated;

    track[5]->preset_share[0] = 0.257 * mem_allocated;
    track[5]->preset_share[1] = 0.257 * mem_allocated;
    track[5]->preset_share[2] = 0.23 * mem_allocated;
    track[5]->preset_share[3] = 0.256 * mem_allocated;

    track[6]->preset_share[0] = 0.257 * mem_allocated;
    track[6]->preset_share[1] = 0.257 * mem_allocated;
    track[6]->preset_share[2] = 0.23 * mem_allocated;
    track[6]->preset_share[3] = 0.256 * mem_allocated;

    track[7]->preset_share[0] = 0.257 * mem_allocated;
    track[7]->preset_share[1] = 0.257 * mem_allocated;
    track[7]->preset_share[2] = 0.23 * mem_allocated;
    track[7]->preset_share[3] = 0.256 * mem_allocated;

    track[8]->preset_share[0] = 0.257 * mem_allocated;
    track[8]->preset_share[1] = 0.257 * mem_allocated;
    track[8]->preset_share[2] = 0.23 * mem_allocated;
    track[8]->preset_share[3] = 0.256 * mem_allocated;
    
    printf("tracking segments initialized\n");

    /* Map shared memory slab segment */
    for(int i = 0;i < n;i++){
        slab_start[i] = mmap(NULL, mem_allocated, PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd[i], 0); 

        track[i]->start_address = slab_start[i];
        track[i]->start = slab_start[i];

        if (track[i] == MAP_FAILED) {
            perror("mapping tracker failed\n");
        }
        if (slab_start[i] == MAP_FAILED) {
            perror("mapping slab failed\n");
        }
    
        printf("slab %d starts on %p\n", i, track[i]->start_address);
    }

    printf("shared slabs initialized\n");

    for(int i = 0;i < n;i++){
        close(slabs_fd[i]);
        close(tracker_fd[i]);
    }

    // pthread_t tracker_tid;
    // pthread_create(&tracker_tid, NULL, thread_routine, NULL);
    // pthread_join(tracker_tid, NULL);
}