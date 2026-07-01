/* 
    Unlinks the shared memory segment
*/
#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>

#define n 1

char tracker_name[20], semaph_name[20], slab_name[20];

int main () {

    for(int i = 0;i < n;i++){
        sprintf(tracker_name,"/tracker%d",i);
        shm_unlink(tracker_name);
        tracker_name[0] = '\n';

        sprintf(slab_name,"/slabs%d",i);
        shm_unlink(slab_name);
        slab_name[0] = '\n';

        sprintf(semaph_name,"/semaph%d",i);
        sem_unlink(semaph_name);
        semaph_name[0] = '\n';
    }
}