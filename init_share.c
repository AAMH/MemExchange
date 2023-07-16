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
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include "shm_malloc.h"

#define n 15
#define S_PORT 33333
#define MAXLINE 1024
#define MULTICAST_GROUP "239.0.0.1"

int sockfd;
double score = 0;

int tracker_fd[n];
int slabs_fd[n];
sem_t * mutex[n];

struct tracker* track[n];
void* slab_start[n];

void remove_all_chars(char* str, char c) {
    char *pr = str, *pw = str;
    while (*pr) {
        *pw = *pr++;
        pw += (*pw != c);
    }
    *pw = '\0';
}

void send_msg(char *addr, char *mess){
                      
    struct sockaddr_in sendAddr;      
                 
    /* Construct local address structure */
    memset(&sendAddr, 0, sizeof(sendAddr));   
    sendAddr.sin_family = AF_INET;                 
    sendAddr.sin_addr.s_addr = inet_addr(addr);//INADDR_ANY;//
    sendAddr.sin_port = htons(S_PORT);

    char score_str[50];
    snprintf(score_str, 50, "%f", get_tracker().max_score);
    size_t len = strlen(score_str);
    char *msg = malloc(len + 2);
    memset(msg,0,len+2);
    msg[0] = 'H';
    strcat(msg, score_str);

    /* Send message to client */
    if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sendAddr, sizeof(sendAddr)) != sizeof(msg)){
        perror("SEND ERROR");
            exit(1);
    }
    //should_broadcast = false;
}

void *sender_routine(){

    //should_broadcast = page_needed;
    int counter = 0;
    while(1){
        sleep(1); 
        // printf("\033[2J\033[1;1H");
        if(get_tracker().rdma_broadcast){
            printf("-Multicasting...\n");
            send_msg(MULTICAST_GROUP,"Hello");
            stop_RDMA_broadcast();
        }
        else{
            // if((counter++) > 1){
            //     should_broadcast = true;
            //     counter = 0;
            // }
        }
    }

    return NULL;
}

void receive_msg(){

    struct sockaddr_in sender_addr;
    memset(&sender_addr, 0, sizeof(sender_addr));

    char buffer[MAXLINE];
   
    socklen_t len = sizeof(sender_addr);

    int m = recvfrom(sockfd, (char *)buffer, MAXLINE, 
                MSG_WAITALL, ( struct sockaddr *) &sender_addr,
                &len);
    
    if(m < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){ 
            //reset_remote_spare();
            fprintf(stderr, "No outstanding requests.\n");
            return;
        }
    }

    char str[INET_ADDRSTRLEN], ip_string[INET_ADDRSTRLEN];
    inet_ntop( AF_INET, &sender_addr.sin_addr, str, INET_ADDRSTRLEN );
    inet_ntop( AF_INET, &sender_addr.sin_addr, ip_string, INET_ADDRSTRLEN );
    buffer[m] = '\0';
    remove_all_chars(str, '.');

    //printf("Received msg: %s\n",  buffer);
    if(buffer[0] == 'H'){ /* A remote tenant has sent us their max score. Let's compare with our maximum*/
        buffer[0]='0';
        printf("A tenant located at %s needs a page. Their score is: %f\n", str, atof(buffer));
        set_scores(atof(buffer), INT_MAX, atoi(str));

        if(compare_maxID(atoi(str), atof(buffer)) > 0){ // The remote tenant score is higher than our local score. Let's send them our candidate
            struct tracker trck = get_tracker();
            char score[50];
            snprintf(score, 50, "%f", trck.min_score);
            size_t len = strlen(score);
            char *msg = malloc(len + 2);
            memset(msg,0,len+2);
            msg[0] = 's';
            strcat(msg, score);
            //msg[len+1] = '\0';

            if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != sizeof(msg)){
                    fprintf(stderr, "couldn't send back the score: %s", msg);
                    exit(1);
            }
            free(msg);
        }
    }
    else if(buffer[0] == 's'){ /* A remote tenant has sent us their min score. Let's compare with our minimum*/
        buffer[0]='0';
        printf("A tenant located at %s has a spare page. Their score is: %f\n", str, atof(buffer));
        set_scores(INT_MIN, atof(buffer), atoi(str));

        if(compare_minID(atoi(str), atof(buffer)) > 0){ // Ask the remote tenant to send back a memory region

            set_rdma_server_info(sender_addr, false);
            while(!get_tracker().local_access_port_n){}
            char access_port_n[10];
            snprintf(access_port_n, 10, "%d",get_tracker().local_access_port_n);
            size_t len = strlen(access_port_n);
            char *msg = malloc(len + 2);
            memset(msg,0,len+2);
            msg[0] = 'p';
            strcat(msg, access_port_n);
            //msg[len+1] = '\0';

            if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != sizeof(msg)){
                fprintf(stderr, "couldn't officially ask for the memory region: port is %s", access_port_n);
                exit(1);
            }
            free(msg);
            stop_RDMA_broadcast();
        }
    }
    else if(buffer[0] == 'p'){ /* The requester has responded with the rdma port we should connect to*/

        buffer[0]='0';
        printf("The tenant will take it from here...\n\n");
        sender_addr.sin_port = atoi(buffer);
        set_rdma_server_info(sender_addr, true);
        
    }
}

void *receiver_routine(){

    while(1){
        sleep(1); 
        // printf("\033[2J\033[1;1H");
        printf("---Waiting for a message...\n");
        receive_msg();
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
        track[i]->remote_spare_requested = false;
    }

    track[0]->preset_share[0] = 1 * mem_allocated;
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

    sprintf(semaph_name,"/semaph%d",0); 
    sprintf(tracker_name,"/tracker%d",0); 
    sprintf(slab_name,"/slabs%d",0); 
    bool page_needed = true;//spare_needed();
    //struct tracker trck = get_tracker();
     
    struct sockaddr_in servaddr;  
    memset(&servaddr, 0, sizeof(servaddr));
       
    // Filling server information
    servaddr.sin_family    = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(S_PORT);
    
    const int enable = 1, loop_back = 0;
    struct ip_mreqn mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_address.s_addr = INADDR_ANY;
    mreq.imr_ifindex = 0;

    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    // Creating socket file descriptor
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Setting socket options
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&timeout,sizeof(timeout)) < 0)
        perror("setsockopt(SO_RCVTIMEO) failed");
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed");
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0)
        perror("setsockopt(SO_REUSEPORT) failed");
    if(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&loop_back, sizeof(loop_back)) < 0)
        perror("setsockopt(IP_MULTICAST_LOOP) failed");
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        perror("setsockopt(IP_ADD_SOURCE_MEMBERSHIP) failed");

    // Bind the socket with the server address
    if ( bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 ){
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    printf("socket initialized\nlistening...\n\n");

    pthread_t sender_tid, receiver_tid;
    pthread_create(&sender_tid, NULL, sender_routine, NULL);
    pthread_create(&receiver_tid, NULL, receiver_routine, NULL);
    pthread_join(receiver_tid, NULL);
    pthread_join(sender_tid, NULL);
}