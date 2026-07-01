/* 
    Initializes shared memory sections for memcached
    arguments: amount of memory being allocated in bytes
*/
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <limits.h>
#include <string.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <inttypes.h>
#include "shm_malloc.h"

#define total_shared_regions 1
#define S_PORT 33333
#define MAXLINE 100
#define MULTICAST_GROUP "239.0.0.1"
#define MAX_DOUBLES 4

#define LOG_DIR "/users/AMH/"
#define LOG_PATH_MAX 256
#define MSG_MAX 64

FILE *f_rtt;

int mc_id = 0 ; // multicast packet identifier: 0 <= mc_id < 10
static struct timespec start_ts;
long sec, nsec; // timestamp of the initiating/first MTC message, used to find MTC RTT

int sockfd;
bool client = false, waiting = false;

int tracker_fd[total_shared_regions];
int slabs_fd[total_shared_regions];
sem_t * mutex[total_shared_regions];
struct tracker * track[total_shared_regions];
void * slab_start[total_shared_regions];

void make_log_filename(char *path,
                       size_t path_len,
                       struct in_addr local_ip){
    char ip_str[INET_ADDRSTRLEN];  // 16 bytes max for IPv4

    inet_ntop(AF_INET, &local_ip, ip_str, sizeof(ip_str));

    snprintf(path, path_len,
             LOG_DIR "%s_rtt.csv",
             ip_str);
}

static inline void now_epoch_sec_nsec(long *sec, long *nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = ts.tv_sec;
    *nsec = ts.tv_nsec;
}

static inline void log_rtt_ns(
    FILE *f,
    long cur_sec,  long cur_nsec,
    long old_sec,  long old_nsec,
    int64_t wait_ns, int status)
{
    int64_t rtt_ns =
        (int64_t)(cur_sec  - old_sec)  * 1000000000LL +
        (int64_t)(cur_nsec - old_nsec);

    fprintf(f, "%ld.%09ld,%" PRId64 ",%d\n",
            cur_sec, cur_nsec,
            rtt_ns - wait_ns, status); // deducting wait_ns: time spent by candidate for the min_score to become available
    
            fflush(f); // comment during the main runs
}

static int pick_iface_by_prefix(const char *prefix,
                                char *out_ifname, size_t ifname_len,
                                unsigned int *out_ifindex,
                                struct in_addr *out_ip)
{
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return -1;

    int rc = -1;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!ifa->ifa_name) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ipbuf[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf))) continue;

        if (strncmp(ipbuf, prefix, strlen(prefix)) == 0) {
            strncpy(out_ifname, ifa->ifa_name, ifname_len);
            out_ifname[ifname_len - 1] = '\0';

            *out_ifindex = if_nametoindex(out_ifname);
            *out_ip = sa->sin_addr;

            rc = (*out_ifindex == 0) ? -1 : 0;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return rc;
}

char * remove_char(const char *pr, char c) { /* removes all occurrences of c */ 
    size_t len = strlen(pr);
    char * out = malloc(len + 1);
    if (!out) return NULL;

    char *pw = out;

    while (*pr) {
        if (*pr != c)
            *pw++ = *pr;
        pr++;
    }
    *pw = '\0';

    return out;
}

void start_timeout_timer() {
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
}

bool check_timeout() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed =
        (now.tv_sec - start_ts.tv_sec) +
        (now.tv_nsec - start_ts.tv_nsec) / 1e9;

    return elapsed > 5.0;
}

void send_msg(char *addr, char mess){

    struct sockaddr_in sendAddr;      
                 
    /* Construct local address structure */
    memset(&sendAddr, 0, sizeof(sendAddr));   
    sendAddr.sin_family = AF_INET;                 
    sendAddr.sin_addr.s_addr = inet_addr(addr);
    sendAddr.sin_port = htons(S_PORT);

    char score_str[50];
    if(mess == 'H')
        snprintf(score_str, 50, "%f", get_tracker().max_score);
    else if(mess == 'B')
        snprintf(score_str, 50, "%f", get_tracker().temp_score);
    size_t len = strlen(score_str);
    char * msg = NULL;

    if(mess == 'H'){
        msg = malloc(len + 3);
        memset(msg, 0, len + 3);
        msg[0] = 'H';
        msg[1] = mc_id + '0';
    }
    else if(mess == 'B'){
        msg = malloc(len + 2);
        memset(msg, 0, len + 2);
        msg[0] = 'B';
    }
    strcat(msg, score_str);
    
    now_epoch_sec_nsec(&sec, &nsec);

    /* MULTICAST */
    if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sendAddr, sizeof(sendAddr)) != sizeof(msg)){
        perror("ERROR: MULTICAST failed.");
        exit(1);
    }
}

void recv_msg(){
    struct sockaddr_in sender_addr;
    memset(&sender_addr, 0, sizeof(sender_addr));
    socklen_t len = sizeof(sender_addr);

    char buffer[MSG_MAX];

    printf("Waiting for a packet...");
    int m = recvfrom(sockfd, (char *)buffer, MAXLINE, 
                MSG_WAITALL, ( struct sockaddr *) &sender_addr,
                &len);
    long cur_sec, cur_nsec;
    now_epoch_sec_nsec(&cur_sec, &cur_nsec);

    if(m < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){ 
            //fprintf(stderr, "No outstanding requests.\n");
            return;
        }
    }
    buffer[m] = '\0';
    printf("Received. Type: %c\n", buffer[0]);
    //printf("Received msg: %s\n",  buffer);

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
    long long sender_id = atoll(remove_char(addr_str, '.')); // IP digits w/ no dots

    char r_mc_id;
    /* Potential Victim */
    if(buffer[0] == 'H' && !waiting){ /* A remote tenant has sent us their max score. Let's compare with our maximum*/
        r_mc_id = buffer[1];
        buffer[0] = '0';
        buffer[1] = '0';
        printf("- A tenant at %s needs a page (score: %f) -> ", addr_str, atof(buffer));

        set_scores(atof(buffer), __DBL_MAX__, sender_id);

        if(!get_tracker().victim_potential){
            printf("No candidates on this node.\n");
            return;
        }

        if(compare_maxID(sender_id, atof(buffer)) > 0){ // The remote tenant score is higher than our local score. Let's send them our candidate
            int64_t wait_ns = 0;
            long w0s, w0ns, w1s, w1ns;

            now_epoch_sec_nsec(&w0s, &w0ns);
            do{
                int q = lock_min_score();
                if(q == 2)
                    break;
                else if(q == 1 || q == 0)   /* wait for our min_score to become available */
                    usleep(1000);   /* can be reduced by increasing freq of calculate_score routine */
            }
            while(1);
            now_epoch_sec_nsec(&w1s, &w1ns);

            wait_ns = (int64_t)(w1s - w0s) * 1000000000LL + (w1ns - w0ns);
            if (wait_ns > 999999999999LL) wait_ns = 999999999999LL; // cap to 999 seconds

            struct tracker trck = get_tracker();
            char msg[MSG_MAX];
            size_t msg_len = 0;

            msg[msg_len++] = 's';
            msg[msg_len++] = r_mc_id;

            int n = snprintf(msg + msg_len, MSG_MAX - msg_len,
                            ",%.5f,%" PRId64 "\n",
                            trck.min_score, wait_ns);

            /* check overflow */
            if (n < 0 || (size_t)n >= MSG_MAX - msg_len) {
                printf("- response overflowed during construction: not sent.\n");
                return;
            }

            msg_len += n;

            if (sendto(sockfd, msg, msg_len, 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != msg_len){
                fprintf(stderr, "ERROR: could not send our score: %s", msg);
                exit(1);
            }
            printf("Candidate (score: %f) sent.\n", trck.min_score);
            start_timeout_timer();
            waiting = true;
        }
        else printf("Lower than current highest score in the cluster. Candiate not sent.\n");
    }
    /* Victor */
    else if(buffer[0] == 's'){ /* A tracker has sent us their candidate. Let's decide */
        char type;
        double min_score;
        int64_t wait_ns;
        
        int parsed = sscanf(buffer, "%c%c,%lf,%" SCNd64, &type, &r_mc_id, &min_score, &wait_ns);

        if (parsed != 4) {      /* malformed message */
            printf("- malformed response received: ignored.\n");
            return;
        }

        printf("* Tracker at %s has a candidate (score: %f) -> ", addr_str, min_score);
        char msg = 'r'; 

        if(r_mc_id != mc_id + '0'){     /* Received a response to an outdated request */
            printf("Rejected (OLD REQUEST)\n");
            log_rtt_ns(f_rtt, cur_sec, cur_nsec, sec, nsec, wait_ns, 2);     
        }
        else{
            set_scores(INT_MIN, min_score, sender_id);

            if(compare_minID(sender_id, min_score) != 0 && lock_spare()){   // ACCEPT 
                printf("Accepted\n");
                log_rtt_ns(f_rtt, cur_sec, cur_nsec, sec, nsec, wait_ns, 0);
                msg = 'x';
                if((++mc_id) > 9) mc_id = 0;    // reset sequence id
            }
            else {  // REJECT
                printf("Rejected\n");
                log_rtt_ns(f_rtt, cur_sec, cur_nsec, sec, nsec, wait_ns, 1);
            }
        }

        if (sendto(sockfd, &msg, sizeof(msg), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != sizeof(msg)){
            fprintf(stderr, "ERROR: could not send response to candidate.");
            exit(1);
        }
    }
    else if(buffer[0] == 'p'){ /* The peer sent us the rdma port we should connect to */
        buffer[0] = '0';
        printf("Received the remote port #.\n\n");
        set_rdma_server_info(sender_addr, atoi(buffer));

        if(client)
            waiting = false;
        else{
            while(!get_tracker().local_access_port_n); // Wait for the listener port to be determined.
            char access_port_n[10];
            snprintf(access_port_n, 10, "%d", get_tracker().local_access_port_n);
            size_t len = strlen(access_port_n);
            char *msg = malloc(len + 2);
            memset(msg, 0, len+2);
            msg[0] = 'p';
            strcat(msg, access_port_n);

            if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != sizeof(msg)){
                fprintf(stderr, "ERROR: could not request the memory region. port #: %s", access_port_n);
                exit(1);
            }
            free(msg);
            unlock_min_score();
            reset_min_score();
        }            
    }
    else if(buffer[0] == 'x'){ /* The requester accepted */
        client = true;
        char access_port_n[10];
        snprintf(access_port_n, 10, "%llu", get_tracker().min_id);
        printf("my port #: %s\n", access_port_n);
        size_t len = strlen(access_port_n);
        char * msg = malloc(len + 2);
        memset(msg, 0, len+2);
        msg[0] = 'p';
        strcat(msg, access_port_n);

        if (sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&sender_addr, sizeof(sender_addr)) != sizeof(msg)){
            fprintf(stderr, "ERROR: could not send our port #: %s", access_port_n);
            exit(1);
        }
        free(msg);
    }
    else if(buffer[0] == 'r'){ /* The requester rejected */
        waiting = false;
        printf("Rejected.\n");
        unlock_min_score();
    }
    else if(buffer[0] == 'B'){
        buffer[0] = '0';
        printf("The tenant at %s finished reallocation (score: %f).\n", addr_str, atof(buffer));
        
        if(compare_maxID(sender_id, atof(buffer)) > 0) unlock_max_score();
    }
}

void *receiver_routine(){

    while(1){
        //printf("\033[2J\033[1;1H");
        //printf("---Waiting for a message...\n");
        recv_msg();
    }
    
    return NULL;
}

void *sender_routine(void *arg) {
    (void)arg;

    while (1) {

        if (get_tracker().rdma_broadcast) {
            stop_RDMA_broadcast();
            client = false;
            send_msg(MULTICAST_GROUP, 'H');
        }

        if (get_tracker().finish_broadcast) {
            stop_RDMA_finish_broadcast();
            send_msg(MULTICAST_GROUP, 'B');
        }

        if (waiting && check_timeout()) {
            printf("TIMEOUT waiting for the requester.\n");
            unlock_min_score();
            waiting = false;
        }

        usleep(1000);  // 1 ms sleep
    }

    return NULL;
}
/*
Argument Order:
    First argument: Always an integer as memory size.
    Last argument: Always a string, either "MTC_ON" or "MTC_OFF".
    Up to 4 double numbers in between as tenants' shares.
*/
int main(int argc, char **argv)
{

    size_t mem_allocated;
    
    double t1_ratio = 1;
    double t2_ratio = 0;
    double t3_ratio = 0;
    double t4_ratio = 0;

    bool MTC_mode = true;

    // Ensure the minimum and maximum number of arguments are within valid range
    if (argc < 3 || argc > (2 + MAX_DOUBLES)) {
        printf("Usage: %s <int> [<double1> <double2> <double3> <double4>] <MTC_ON|MTC_OFF>\n", argv[0]);
        return 1;
    }

    // Parse first argument as memory size
    mem_allocated = (size_t)(atoi(argv[1]))*1024*1024;

    // Parse last argument, which must be "MTC_ON" or "MTC_OFF"
    char *last_arg = argv[argc - 1];
    if (strcmp(last_arg, "MTC_ON") != 0 && strcmp(last_arg, "MTC_OFF") != 0) {
        printf("Error: Last argument must be 'MTC_ON' or 'MTC_OFF'.\n");
        return 1;
    }

    if(strcmp(last_arg, "MTC_OFF") == 0)
        MTC_mode = false;

    // Parse up to 4 double numbers in between as the share of each tenant
    double tenant_shares[MAX_DOUBLES] = {0};
    int double_count = 0;

    for (int i = 2; i < argc - 1; i++) {
        tenant_shares[double_count] = strtod(argv[i], NULL);
        double_count++;
    }

    printf("Tenant shares (%d): ", double_count);
    for (int i = 0; i < double_count; i++) {
        printf("%f ", tenant_shares[i]);
    }
    printf("\n");
    printf("Protocol: %s\n", last_arg);

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
        max_sz = (size_t) atoi( line );
        fclose(f);
    }
    if (mem_allocated > max_sz-sizeof(struct tracker))
    {
        printf("exceeded maximum shared memory size, current max (in bytes): %d\n",(int)(max_sz-sizeof(struct tracker)));
        exit(1);
    }

    printf("\nShared-memory size: %lu MB\n", (long) mem_allocated/1024/1024);
    
    /* Initialize Semaphore */
    
    for(int i = 0;i < total_shared_regions;i++){
        sprintf(semaph_name,"/semaph%d",i);
        if ((mutex[i] = sem_open(semaph_name, O_CREAT, 0644, 1)) == SEM_FAILED) {
            perror("semaphore initilization failed");
            exit(1);
        }
        semaph_name[0] = '\n';
    }
    
    /* Initialize tracker and shared memory */

    for(int i = 0;i < total_shared_regions;i++){
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

    for(int i = 0;i < total_shared_regions;i++){
        track[i] = mmap(NULL, sizeof(struct tracker),
            PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd[i], 0);
        
        track[i]->max_size = mem_allocated;
        track[i]->used_size = 0;
        
        track[i]->spare_mem_clsid = -1;
        track[i]->spare_mem_start = NULL;
        track[i]->spare_mem_avail = false;
        track[i]->spare_size = 0;

        track[i]->min_score_lock = false;
        track[i]->max_score_lock = false;
        track[i]->min_score = 9999999999;          
        track[i]->min_id = -1;
        track[i]->max_score = -1;          
        track[i]->max_id = -1;
        track[i]->remote_spare_requested = false;

        track[i]->preset_share[0] = tenant_shares[0] * mem_allocated;
        track[i]->preset_share[1] = tenant_shares[1] * mem_allocated;
        track[i]->preset_share[2] = tenant_shares[2] * mem_allocated;
        track[i]->preset_share[3] = tenant_shares[3] * mem_allocated;
    }
    
    printf("Trackers initialized.\n");

    /* Map shared memory slab segment */
    for(int i = 0;i < total_shared_regions;i++){
        slab_start[i] = mmap(NULL, mem_allocated, PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd[i], 0); 

        track[i]->start_address = slab_start[i];
        track[i]->start = slab_start[i];

        if (track[i] == MAP_FAILED) {
            perror("mapping tracker failed\n");
        }
        if (slab_start[i] == MAP_FAILED) {
            perror("mapping slab failed\n");
        }
    
        printf("Starting address for %d's memory: %p\n", i, track[i]->start_address);
    }

    printf("Memory initialized.\n");

    for(int i = 0;i < total_shared_regions;i++){
        close(slabs_fd[i]);
        close(tracker_fd[i]);
    }

    sprintf(semaph_name,"/semaph%d",0); 
    sprintf(tracker_name,"/tracker%d",0); 
    sprintf(slab_name,"/slabs%d",0); 
     
    if(MTC_mode){ 
        struct sockaddr_in servaddr;
        memset(&servaddr, 0, sizeof(servaddr));

        /* Auto-detect the 10.10.1.x interface + IP (experiment network) */
        char ifname[IFNAMSIZ];
        unsigned int ifindex = 0;
        struct in_addr local_ip;

        if (pick_iface_by_prefix("10.10.1.", ifname, sizeof(ifname), &ifindex, &local_ip) != 0) {
            fprintf(stderr, "ERROR: could not find a 10.10.1.x interface/IP for multicast\n");
            exit(EXIT_FAILURE);
        }

        char logfile[LOG_PATH_MAX];

        make_log_filename(logfile, sizeof(logfile), local_ip);

        f_rtt = fopen(logfile, "w");
        if (!f_rtt) {
            perror("fopen");
            exit(1);
        }
        setvbuf(f_rtt, NULL, _IOFBF, 1 << 20);  // 1MB buffer
        fprintf(f_rtt, "ts,rtt_ns,status\n");

        printf("MTC multicast using iface=%s ifindex=%u ip=%s\n",
            ifname, ifindex, inet_ntoa(local_ip));

        /* Filling server information (bind for receiving multicast) */
        servaddr.sin_family = AF_INET;              // IPv4
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port = htons(S_PORT);

        const int enable = 1, loop_back = 0;

        /* Multicast membership request: JOIN on the 10.10.1.x interface */
        struct ip_mreqn mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
        mreq.imr_address = local_ip;          // local interface IPv4
        mreq.imr_ifindex = (int)ifindex;      // local interface index

        /* Creating socket file descriptor */
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }

        /* Setting socket options */
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
            perror("setsockopt(SO_REUSEADDR) failed");
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) < 0)
            perror("setsockopt(SO_REUSEPORT) failed");
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_back, sizeof(loop_back)) < 0)
            perror("setsockopt(IP_MULTICAST_LOOP) failed");

        /* IMPORTANT: Force outgoing multicast to use 10.10.1.x interface/IP */
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &local_ip, sizeof(local_ip)) < 0)
            perror("setsockopt(IP_MULTICAST_IF) failed");

        /* Join the multicast group on that interface */
        if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
            perror("setsockopt(IP_ADD_MEMBERSHIP) failed");

        /* Bind the socket with the server address (receive side) */
        if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            perror("bind failed");
            exit(EXIT_FAILURE);
        }

        printf("Socket initialized. Listening...\n\n");


        pthread_t sender_tid, receiver_tid;
        pthread_create(&sender_tid, NULL, sender_routine, NULL);
        pthread_create(&receiver_tid, NULL, receiver_routine, NULL);
        pthread_join(receiver_tid, NULL);
        pthread_join(sender_tid, NULL);
    }
}
