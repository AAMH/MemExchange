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


int tracker_fd;
int slabs_fd;

sem_t * mutex;

struct tracker* track;
void* slab_start;
void* shadow_start;

static bool issender = false;
static bool bb = true;

typedef struct dummy_struct {
    int value;  
} dum;

void *thread_routine(){

    if(!issender){
        struct tracker trck = get_tracker();
        
        void * ptr = shm_malloc_spare(1046784);

        //set_spare_mem(ptr,3);
        
        if(ptr != NULL){
            printf("dummy malloced: %p\n",ptr);
        }

        int counter = 0;
        while(1){
            sleep(1); 
            counter++;
            if(counter >= 1){
                counter = -10000;
                memset(ptr,3,1046784);
                //munmap(ptr,1047840);
            }
            dum * dd = (dum*) ptr;    
            printf("counter: %d, ptr: %d\n",counter, dd->value);
        }
    }
    else
    {
        struct tracker trck = get_tracker();
        
        void * ptr1 = shm_malloc(1046784);
        memset(ptr1,1,1046784);
        
        if(ptr1 != NULL){
            printf("dummy1 malloced: %p\n",ptr1);
        }

        void * ptr2 = shm_malloc(1046784);
        memset(ptr2,1,1046784);

        if(ptr2 != NULL){
            printf("dummy2 malloced: %p\n",ptr2);
        }

        void * ptr3 = shm_malloc(1046784);
        memset(ptr3,1,1046784);


        if(ptr3 != NULL){
            printf("dummy3 malloced: %p\n",ptr3);
        }

        set_spare_mem(ptr2,3);

        int counter = 0;
        while(1){
            sleep(1); 
            counter++;
            dum * dd = (dum*) ptr1;    
            printf("counter: %d, ptr1: %d\n",counter, dd->value);
            
            if(counter >= 20){
                counter = -20000;
                //memset(ptr2,1,1046784);
                //munmap(ptr1,1047840);
                //printf("dummy released\n");
            }
            else if(bb && counter >= 10){
                //counter = -10000;
                //memset(ptr,1,1046784);
                bb = false;
                // dum * dd = (dum*) ptr1;
                // printf("pptr1: %d\n",dd->value);
                munmap(ptr2,1047840);
                printf("dummy released\n");
            }//trck = get_tracker();
            // printf("\033[2J\033[1;1H");
             //printf("Used Memory: %ld, counter: %d\n", trck.used_size,counter); 
        }
    }
    

    return NULL;
}

int main(int argc, char *argv[])
{
    if(argc >= 2){
            printf("sender\n");
            issender = true;
        }
    pthread_t tracker_tid;
    pthread_create(&tracker_tid, NULL, thread_routine, NULL);
    pthread_join(tracker_tid, NULL);
}