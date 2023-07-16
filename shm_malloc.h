#ifndef SHM_MALLOC_H
#define SHM_MALLOC_H

#include<stdint.h>
#include <netinet/in.h>

struct tracker {                /* Shared memory structure to keep track of current pointer */
    size_t max_size;            // Total Size of the shared memory
    size_t used_size;           // Used Size of the shared memory
    size_t spare_off;           // Distance between the spare page and shared-memory start address 
    size_t spare_size;          // Exact size of spare page

    void * start;               // start address of the shared memory
    void * start_address;       // address returned to tenants when asked for a page
    void * avail_address;       
    void * spare_mem_start;     // address of the spare page

    int    spare_mem_clsid;     // Slabclass ID of the Victim class
    int    max_id;              // ID of tenant with the max score
    int    min_id;              // ID of tenant with the min score

    bool   spare_mem_avail;     // flag indicating if a spare page is ready
    bool   spare_requested;     // flag indicating a spare page is needed
    bool   spare_lock;          // lock preventing more than one tenant to access the spare slab


    double max_score;           // The maximum score among tenants - used for finding the Victor
    double min_score;           // The minimum score among tenants - used for finding the Victim
    double preset_share[4];     // Shows how much of shared memory each tenant is occupying

    /*** RDMA ADDITIONS***/

    struct connection * latest_conn;   // Latest connection

    bool   remote_spare_requested; // flag indicating a remote tenant needs a page
    bool   remote_spare_received;
    bool   rdma_broadcast;
    bool   remote_server_ready;
    
    struct sockaddr_in rdma_peer_addr;

    int    min_counter;
    int    max_counter;
    int    local_access_port_n;
};

extern bool rdma_page_transfer_in_progress;
extern char tracker_name[20], semaph_name[20], slab_name[20];

void init_shared_names(int x);

void * shm_malloc(size_t n);    // MAIN ALLOCATION FUNCTION
void * shm_mallocAt(size_t n, int id);  // Spare page allocation function

void * set_spare_mem(void * ptr, size_t n, int cls, int id);    // The victim uses to set the spare page info
int    get_spare_clsid();       // returns the class id of the spare page -- might be unnecessary

bool   set_rdma_access_port(int rdma_access_port); // Sets the access port for rdma access by the remote tenant
bool   set_rdma_server_info(struct sockaddr_in peer_addr, bool boo); // Sets the info for the remote server

bool   is_remote_server_available(int id);
bool   remote_spare_needed(int id);          // checks the remote spare_requested flag
bool   reset_remote_spare();
bool   is_remote_spare_received();

void   receive_remote_mem(struct connection * conn, size_t n, int id);
void   set_remote_mem(void * addr, size_t n, int id);

bool   rdma_broadcast();
bool   stop_RDMA_broadcast();

bool   set_scores(double sc1, double sc2, int id);  // Each tenant uses to send its scores to the tracker
int    compare_minID(int id,double sc);             // checks if a tenant's score was the max
int    compare_maxID(int id,double sc);             // checks if a tenant's score was the min
bool   req_spare();             // The vivtor uses to ask for a spare page
bool   spare_needed();          // checks the spare_requested flag
bool   reset_locks();           // resets spare page flags
bool   lock_spare();            // locks the spare page
bool   unlock_spare();          // unlocks the spare page
bool   is_spare_avail();        // checks the spare_mem_avail flag
bool   signal_alloc_free(int id, size_t size);      // when the victim wants to release an unallocated page from its share instead

extern struct tracker get_tracker(void);    // function returning a copy of the most recent tracker

#endif