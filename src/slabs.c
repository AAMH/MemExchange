//* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
//#define DEBUG_SLAB_MOVER
/* powers-of-N allocation structures */



static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
static size_t mem_malloced_remote = 0;
/* If the memory limit has been hit once. Used as a hint to decide when to
 * early-wake the LRU maintenance thread */
static bool mem_limit_reached = false;
static int power_largest;

static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;
static int prev_victim = POWER_SMALLEST;

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;

static enum realloc_dest_type {
    TENANT_SLAB,
    TENANT_LOCAL,
    TENANT_REMOTE
} realloc_destination = TENANT_SLAB, realloc_source = TENANT_SLAB;

static uint64_t total_misses = 0;
static int no_diff_miss_times = 0;
static bool greedy = false;
static bool aborted = false;
// static bool rebal_source_ext = false;       // indicating if reassigning is being used to take memory from an external source
// static bool rebal_dest_ext = false;         // indicating if reassigning is being used to give memory to an external destination
static bool is_taken_careof = false;
static int slab_shadowq_dec_victim = POWER_SMALLEST;
int pages_to_request = -1;
static int slab_to_release = -1;
static double score1 = 0;
static double score2 = 0;
static clock_t start_t = 0;
static clock_t end_t = 0;

static int total_released_pages = 0;
static int total_received_pages = 0;

/* RDMA stuff*/
struct rdma_cm_id *rdma_connections_head = NULL;
const int TIMEOUT_IN_MS = 500;
void * remote_region = NULL;

static int on_connect_request(struct rdma_cm_id *id);
static int on_addr_resolved(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_id *id);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);
static int on_route_resolved(struct rdma_cm_id *id);


void victor_waiting_routine();
/*
 * Forward Declarations
 */
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);
static void do_slabs_free_remote(char *lptr, const size_t size, unsigned int id);

/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0 || size > settings.item_size_max)
        return 0;
    while (size > slabclass[res].size)
        if (res++ == power_largest)     /* won't fit in the biggest slab */
            return power_largest;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, const bool isgreedy) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;
    greedy = isgreedy;
    mem_limit = limit;
    
    if (prealloc) {
        /* Allocate everything in a big chunk with malloc */ 
        void * ptr = shm_malloc(mem_limit);
        struct tracker  trck = get_tracker();
        mem_base = trck.avail_address;
        if(greedy)
            //mem_limit = trck.max_size;
            mem_limit = trck.preset_share[(settings.port - 11212) % 4];
        if (mem_base != NULL) {
            mem_current = mem_base;
            if(greedy)
                //mem_avail = trck.max_size - trck.used_size;
                mem_avail = mem_limit - (mem_malloced + mem_malloced_remote);
            else
                mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
    }

    memset(slabclass, 0, sizeof(slabclass));

    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
        if (slab_sizes != NULL) {
            if (slab_sizes[i-1] == 0)
                break;
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor) {
            break;
        }
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        pthread_mutex_init(&slabclass[i].shadowq_lock, NULL);
        slabclass[i].tree = new_tree();
        slabclass[i].size = size;
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        slabclass[i].shadowq_max_items = settings.shadowq_size / slabclass[i].size;
        printf("shadowq_max_items: queue %d, slab_size %d, perslab %d, shadowq_size %d\n",i,size,slabclass[i].perslab, slabclass[i].shadowq_max_items);

        if (slab_sizes == NULL)
            size *= factor;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }

    power_largest = i;
    slabclass[power_largest].size = settings.slab_chunk_size_max;
    slabclass[power_largest].perslab = settings.slab_page_size / settings.slab_chunk_size_max;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            mem_malloced = (size_t)atol(t_initial_malloc);
        }

    }

    if (prealloc) {
        slabs_preallocate(power_largest);
    }

}

static void slabs_preallocate (const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs)
            return;
        if (do_slabs_newslab(i) == 0) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max memory must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }

}

static int grow_slab_list (const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    if (p->slabs == p->list_size) {
        size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
        p->list_size = new_size;
        p->slab_list = new_list;

        p->rkey = realloc(p->rkey, new_size * sizeof(uint32_t));
        p->conn = realloc(p->conn, new_size * sizeof(struct connection *));
    }
    return 1;
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id, size_t sizee) {
    slabclass_t *p = &slabclass[id];
    int x;
    int i = sizee / p->size;
    for (x = 0; x < i; x++) {
        do_slabs_free(ptr, 0, id);
        ((item *)ptr)->page_id = p->slabs-1;
        ptr += p->size;
    }
}

static void ** split_remote_page_into_freelist(void *rptr, const unsigned int id, size_t sizee) {
    slabclass_t *p = &slabclass[id];
    int i = sizee / p->size;

    remote_item ** remote_page = (remote_item*) malloc(i * sizeof(struct _remitem *));
    memset(remote_page, 0, i * sizeof(struct _remitem *));

    remote_item * r_it = NULL;
    for (int x = 0; x < i; x++) {
        r_it = remote_page[x] = create_remote_item(id);

        r_it->address = rptr;
        r_it->next = p->rslots;
        if (r_it->next) r_it->next->prev = r_it;
        p->rslots = r_it;
        r_it->page_id = p->slabs - 1;
        r_it->slabs_clsid = id;
        
        rptr += p->size;
        p->sl_curr_r++;
    }
    
    return remote_page;
}

/* Fast FIFO queue */
static void *get_page_from_global_pool(void) {
    slabclass_t *p = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    if (p->slabs < 1) {
        return NULL;
    }
    char *ret = p->slab_list[p->slabs - 1];
    p->slabs--;
    return ret;
}

static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
        ? settings.slab_page_size
        : p->size * p->perslab;
    char *ptr; 

    if(greedy && !mem_limit_reached){   
        struct tracker trck = get_tracker();     
        // mem_avail = trck.max_size - trck.used_size;
        // mem_limit = mem_malloced + mem_avail ;
        mem_limit = trck.preset_share[(settings.port - 11212) % 4];
        mem_avail = mem_limit - (mem_malloced + mem_malloced_remote);
        printf("free mem(this): %lu\n", mem_avail);  
    }

    if ((mem_limit && mem_malloced + mem_malloced_remote + len > mem_limit && p->slabs > 0
        && g->slabs == 0)) {
            mem_limit_reached = true;
            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return 0;
        }

    if(mem_limit_reached)
        return 0;
    
    if ((grow_slab_list(id) == 0) ||
        (((ptr = get_page_from_global_pool()) == NULL) &&
        ((ptr = memory_allocate((size_t)len)) == 0))) {

        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);
    split_slab_page_into_freelist(ptr, id, len);

    p->slab_list[p->slabs++] = ptr;
    p->rkey[p->slabs-1] = NULL;
    p->conn[p->slabs-1] = NULL;
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

/*@null@*/
static void *do_slabs_alloc(const size_t size, unsigned int id, uint64_t *total_bytes,
        unsigned int flags) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }
    p = &slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);
    if (total_bytes != NULL) {
        *total_bytes = p->requested;
    }

    assert(size <= p->size);
    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (p->sl_curr == 0 && flags != SLABS_ALLOC_NO_NEWPAGE) {
        do_slabs_newslab(id);
    }
        
    if (p->sl_curr != 0) {
        /* return off our freelist */
        it = (item *)p->slots;
        p->slots = it->next;
        if (it->next) it->next->prev = 0;
        /* Kill flag and initialize refcount here for lock safety in slab
            * mover's freeness detection. */
        it->it_flags &= ~ITEM_SLABBED;
        it->refcount = 1;
        p->sl_curr--;
        ret = (void *)it;
    } else if (p->sl_curr_r != 0) {
        remote_item * r_it = (remote_item*) p->rslots;
        p->rslots = r_it->next;
        if (r_it->next) r_it->next->prev = 0;

        it = (item *) malloc(p->size); /* Allocating a temp item. Released after the response to client */
        memset(it, 0, p->size);
        it->it_flags &= ~ITEM_SLABBED;
        it->refcount = 1;
        it->page_id = r_it->page_id;
        it->r_it = r_it;
        it->slabs_clsid = r_it->slabs_clsid;
        
        p->sl_curr_r--;
        ret = (void *)it;
    } 
    else {
        ret = NULL;
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

static void do_slabs_free_chunked(item *it, const size_t size) {
    item_chunk *chunk = (item_chunk *) ITEM_data(it);
    slabclass_t *p;

    it->it_flags = ITEM_SLABBED;
    it->slabs_clsid = 0;
    it->prev = 0;
    // header object's original classid is stored in chunk.
    p = &slabclass[chunk->orig_clsid];
    if (chunk->next) {
        chunk = chunk->next;
        chunk->prev = 0;
    } else {
        // header with no attached chunk
        chunk = NULL;
    }

    // return the header object.
    // TODO: This is in three places, here and in do_slabs_free().
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
    p->sl_curr++;
    // TODO: macro
    p->requested -= it->nkey + 1 + it->nsuffix + sizeof(item) + sizeof(item_chunk);
    if (settings.use_cas) {
        p->requested -= sizeof(uint64_t);
    }

    item_chunk *next_chunk;
    while (chunk) {
        assert(chunk->it_flags == ITEM_CHUNK);
        chunk->it_flags = ITEM_SLABBED;
        p = &slabclass[chunk->slabs_clsid];
        chunk->slabs_clsid = 0;
        next_chunk = chunk->next;

        chunk->prev = 0;
        chunk->next = p->slots;
        if (chunk->next) chunk->next->prev = chunk;
        p->slots = chunk;
        p->sl_curr++;
        p->requested -= chunk->size + sizeof(item_chunk);

        chunk = next_chunk;
    }

    return;
}


static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

    it = (item *)ptr;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        it->it_flags = ITEM_SLABBED;
        it->slabs_clsid = 0;
        it->prev = 0;
        it->next = p->slots;
        if (it->next) it->next->prev = it;
        p->slots = it;

        p->sl_curr++;
        p->requested -= size;
    } else {
        do_slabs_free_chunked(it, size);
    }
    return;
}

static void do_slabs_free_chunked_remote(item *it, const size_t size) {
    item_chunk *chunk = (item_chunk *) ITEM_data(it);
    slabclass_t *p;

    it->it_flags = ITEM_SLABBED;
    it->slabs_clsid = 0;
    it->prev = 0;
    // header object's original classid is stored in chunk.
    p = &slabclass[chunk->orig_clsid];
    if (chunk->next) {
        chunk = chunk->next;
        chunk->prev = 0;
    } else {
        // header with no attached chunk
        chunk = NULL;
    }

    // return the header object.
    // TODO: This is in three places, here and in do_slabs_free().
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
    p->sl_curr++;
    // TODO: macro
    p->requested -= it->nkey + 1 + it->nsuffix + sizeof(item) + sizeof(item_chunk);
    if (settings.use_cas) {
        p->requested -= sizeof(uint64_t);
    }

    item_chunk *next_chunk;
    while (chunk) {
        assert(chunk->it_flags == ITEM_CHUNK);
        chunk->it_flags = ITEM_SLABBED;
        p = &slabclass[chunk->slabs_clsid];
        chunk->slabs_clsid = 0;
        next_chunk = chunk->next;

        chunk->prev = 0;
        chunk->next = p->slots;
        if (chunk->next) chunk->next->prev = chunk;
        p->slots = chunk;
        p->sl_curr++;
        p->requested -= chunk->size + sizeof(item_chunk);

        chunk = next_chunk;
    }

    return;
}

static void do_slabs_free_remote(char *lptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    p = &slabclass[id];

    it = (item *)lptr;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        it->it_flags = ITEM_SLABBED;
        it->slabs_clsid = 0;
        it->prev = 0;
        it->page_id = p->slabs-1; 

        p->sl_curr++;
        p->requested -= size;
    } else {
        //do_slabs_free_chunked_remote(it, size);
    }
    return;
}

static int nz_strcmp(int nzlength, const char *nz, const char *z) {
    int zlength=strlen(z);
    return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    if (add_stats != NULL) {
        if (!stat_type) {
            /* prepare general statistics for the engine */
            STATS_LOCK();
            APPEND_STAT("bytes", "%llu", (unsigned long long)stats_state.curr_bytes);
            APPEND_STAT("curr_items", "%llu", (unsigned long long)stats_state.curr_items);
            APPEND_STAT("total_items", "%llu", (unsigned long long)stats.total_items);
            STATS_UNLOCK();
            if (settings.slab_automove > 0) {
                pthread_mutex_lock(&slabs_lock);
                APPEND_STAT("slab_global_page_pool", "%u", slabclass[SLAB_GLOBAL_PAGE_POOL].slabs);
                pthread_mutex_unlock(&slabs_lock);
            }
            item_stats_totals(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "items") == 0) {
            item_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "slabs") == 0) {
            slabs_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes") == 0) {
            item_stats_sizes(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_enable") == 0) {
            item_stats_sizes_enable(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_disable") == 0) {
            item_stats_sizes_disable(add_stats, c);
        } else {
            ret = false;
        }
    } else {
        ret = false;
    }

    return ret;
}

/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;

    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);

    total = 0;
    for(i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;
            
            for(int j = 0;j < p->slabs; j++){
            APPEND_NUM_STAT(i, "slab pointer", "%p", p->slab_list[j]);
            } 

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            APPEND_NUM_STAT(i, "used_chunks", "%u",
                            slabs*perslab - p->sl_curr - p->sl_curr_r);
            APPEND_NUM_STAT(i, "free_local_chunks", "%u", p->sl_curr);
            APPEND_NUM_STAT(i, "free_remote_chunks", "%u", p->sl_curr_r);
            /* Stat is dead, but displaying zero instead of removing it. */
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", 0);
            APPEND_NUM_STAT(i, "mem_requested", "%llu",
                            (unsigned long long)p->requested);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "shadowq_hits", "%llu",
                     (unsigned long long)thread_stats.slab_stats[i].shadowq_hits);
            APPEND_NUM_STAT(i, "q_misses", "%llu",
                     (unsigned long long)thread_stats.slab_stats[i].q_misses);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)mem_malloced);
    add_stats(NULL, 0, NULL, 0, c);
}

static void *memory_allocate(size_t size) {
    void *ret;
    
    if (mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */  
        ret = shm_malloc(size);
        size_t PAGESIZE = sysconf(_SC_PAGESIZE);
        int index = size/PAGESIZE;
        size = (index+1) * PAGESIZE;   
        mem_malloced += size;
        printf("total allocated mem(this): %lu\n", mem_malloced);
  
    
    } else {
        ret = mem_current;

        if (size > mem_avail) {
            return NULL;
        }

        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }
    if(greedy){
        struct tracker trck = get_tracker();
        // mem_avail = trck.max_size - trck.used_size;
        // mem_limit = mem_malloced + mem_avail ;
        mem_limit = trck.preset_share[(settings.port - 11212) % 4];
        mem_avail = mem_limit - (mem_malloced + mem_malloced_remote);
    }
    return ret;
}

/* Must only be used if all pages are item_size_max */
static void memory_release() {
    void *p = NULL;
    if (mem_base != NULL)
        return;

    if (!settings.slab_reassign)
        return;

    while (mem_malloced > mem_limit &&
            (p = get_page_from_global_pool()) != NULL) {
    //    free(p);
        munmap(p,settings.item_size_max);
        mem_malloced -= settings.item_size_max;
    }
}



void *slabs_alloc(size_t size, unsigned int id, uint64_t *total_bytes,
        unsigned int flags) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id, total_bytes, flags);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_stats(add_stats, c);
    pthread_mutex_unlock(&slabs_lock);
}

static bool do_slabs_adjust_mem_limit(size_t new_mem_limit) {
    /* Cannot adjust memory limit at runtime if prealloc'ed */
    if (mem_base != NULL)
        return false;
    settings.maxbytes = new_mem_limit;
    mem_limit = new_mem_limit;
    mem_limit_reached = false; /* Will reset on next alloc */
    memory_release(); /* free what might already be in the global pool */
    return true;
}

bool slabs_adjust_mem_limit(size_t new_mem_limit) {
    bool ret;
    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_adjust_mem_limit(new_mem_limit);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)
{
    pthread_mutex_lock(&slabs_lock);
    slabclass_t *p;
    if (id < POWER_SMALLEST || id > power_largest) {
        fprintf(stderr, "Internal error! Invalid slab class\n");
        abort();
    }

    p = &slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&slabs_lock);
}

unsigned int slabs_available_chunks(const unsigned int id, bool *mem_flag,
        uint64_t *total_bytes, unsigned int *chunks_perslab) {
    unsigned int ret;
    slabclass_t *p;

    pthread_mutex_lock(&slabs_lock);
    p = &slabclass[id];
    ret = p->sl_curr;
    if (mem_flag != NULL)
        *mem_flag = mem_limit_reached;
    if (total_bytes != NULL)
        *total_bytes = p->requested;
    if (chunks_perslab != NULL)
        *chunks_perslab = p->perslab;
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

/* The slabber system could avoid needing to understand much, if anything,
 * about items if callbacks were strategically used. Due to how the slab mover
 * works, certain flag bits can only be adjusted while holding the slabs lock.
 * Using these functions, isolate sections of code needing this and turn them
 * into callbacks when an interface becomes more obvious.
 */
void slabs_mlock(void) {
    pthread_mutex_lock(&slabs_lock);
}

void slabs_munlock(void) {
    pthread_mutex_unlock(&slabs_lock);
}

static pthread_cond_t slab_rebalance_cond = PTHREAD_COND_INITIALIZER;
static volatile int do_run_slab_thread = 1;
static volatile int do_run_slab_rebalance_thread = 1;

#define DEFAULT_SLAB_BULK_CHECK 1
int slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;

static int slab_rebalance_start(void) {
    slabclass_t *s_cls;
    int no_go = 0;

    pthread_mutex_lock(&slabs_lock);

    /* source is not external AND it's not just releasing for someone else*/
    if(!realloc_source && !realloc_destination){
        if (slab_rebal.s_clsid < POWER_SMALLEST ||
            slab_rebal.s_clsid > power_largest  ||
            slab_rebal.d_clsid < SLAB_GLOBAL_PAGE_POOL ||
            slab_rebal.d_clsid > power_largest  ||
            slab_rebal.s_clsid == slab_rebal.d_clsid)
            no_go = -2;

         s_cls = &slabclass[slab_rebal.s_clsid];

        if (s_cls->slabs < 2)
            no_go = -3;

        if (!grow_slab_list(slab_rebal.d_clsid)) {
            no_go = -1;
        }
    }
    if(realloc_source)
        if(!grow_slab_list(slab_rebal.d_clsid)) {
            no_go = -1;
        }
    s_cls = &slabclass[slab_rebal.s_clsid];

    if(realloc_source && s_cls->slabs == 0)
        s_cls = &slabclass[slab_rebal.d_clsid];

    if (no_go != 0) {
        pthread_mutex_unlock(&slabs_lock);
        return no_go; /* Should use a wrapper function... */
    }

    /* Always kill the first available slab page as it is most likely to
        * contain the oldest items
        */
    if(realloc_destination)
        slab_rebal.slab_start = s_cls->slab_list[slab_to_release];
    else
        slab_rebal.slab_start = s_cls->slab_list[0];

    slab_rebal.slab_end   = (char *)slab_rebal.slab_start +
        (s_cls->size * s_cls->perslab);
    slab_rebal.slab_pos   = slab_rebal.slab_start;
    slab_rebal.done       = 0;

    /* Also tells do_item_get to search for items in this slab */
    slab_rebalance_signal = 2;
        
    if(realloc_destination && !is_taken_careof){
        printf("releasing address range: %p to %p \n",slab_rebal.slab_start,slab_rebal.slab_end);
        is_taken_careof = true;
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "Started a slab rebalance\n");
    }

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats_state.slab_reassign_running = true;
    STATS_UNLOCK();

    return 0;
}

/* CALLED WITH slabs_lock HELD */
static void *slab_rebalance_alloc(const size_t size, unsigned int id) {
    slabclass_t *s_cls;
    s_cls = &slabclass[slab_rebal.s_clsid];
    int x;
    item *new_it = NULL;

    for (x = 0; x < s_cls->perslab; x++) {
        if(s_cls->sl_curr != 0)
            new_it = do_slabs_alloc(size, id, NULL, SLABS_ALLOC_NO_NEWPAGE);
        /* check that memory isn't within the range to clear */
        if (new_it == NULL) {
            break;
        }
        if ((void *)new_it >= slab_rebal.slab_start
            && (void *)new_it < slab_rebal.slab_end) {
            /* Pulled something we intend to free. Mark it as freed since
             * we've already done the work of unlinking it from the freelist.
             */
            s_cls->requested -= size;
            new_it->refcount = 0;
            new_it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
            memcpy(ITEM_key(new_it), "deadbeef", 8);
#endif
            new_it = NULL;
            slab_rebal.inline_reclaim++;
        } else {
            break;
        }
    }
    return new_it;
}

/* CALLED WITH slabs_lock HELD */
/* detatches item/chunk from freelist. */
static void slab_rebalance_cut_free(slabclass_t *s_cls, item *it) {
    /* Ensure this was on the freelist and nothing else. */
    assert(it->it_flags == ITEM_SLABBED);
    if (s_cls->slots == it) {
        s_cls->slots = it->next;
    }
    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    s_cls->sl_curr--;
}

enum move_status {
    MOVE_PASS=0, MOVE_FROM_SLAB, MOVE_FROM_LRU, MOVE_BUSY, MOVE_LOCKED
};

/* refcount == 0 is safe since nobody can incr while item_lock is held.
 * refcount != 0 is impossible since flags/etc can be modified in other
 * threads. instead, note we found a busy one and bail. logic in do_item_get
 * will prevent busy items from continuing to be busy
 * NOTE: This is checking it_flags outside of an item lock. I believe this
 * works since it_flags is 8 bits, and we're only ever comparing a single bit
 * regardless. ITEM_SLABBED bit will always be correct since we're holding the
 * lock which modifies that bit. ITEM_LINKED won't exist if we're between an
 * item having ITEM_SLABBED removed, and the key hasn't been added to the item
 * yet. The memory barrier from the slabs lock should order the key write and the
 * flags to the item?
 * If ITEM_LINKED did exist and was just removed, but we still see it, that's
 * still safe since it will have a valid key, which we then lock, and then
 * recheck everything.
 * This may not be safe on all platforms; If not, slabs_alloc() will need to
 * seed the item key while holding slabs_lock.
 */
static int slab_rebalance_move(void) {
    slabclass_t *s_cls;
    int x;
    int was_busy = 0;
    int refcount = 0;
    uint32_t hv;
    void *hold_lock;
    enum move_status status = MOVE_PASS;

    pthread_mutex_lock(&slabs_lock);

    if(!realloc_source){
    s_cls = &slabclass[slab_rebal.s_clsid];

    for (x = 0; x < slab_bulk_check; x++) {
        hv = 0;
        hold_lock = NULL;
        item *it = slab_rebal.slab_pos;
        item_chunk *ch = NULL;
        status = MOVE_PASS;
        if (it->it_flags & ITEM_CHUNK) {
            /* This chunk is a chained part of a larger item. */
            ch = (item_chunk *) it;
            /* Instead, we use the head chunk to find the item and effectively
             * lock the entire structure. If a chunk has ITEM_CHUNK flag, its
             * head cannot be slabbed, so the normal routine is safe. */
            it = ch->head;
            assert(it->it_flags & ITEM_CHUNKED);
        }
        /* ITEM_FETCHED when ITEM_SLABBED is overloaded to mean we've cleared
         * the chunk for move. Only these two flags should exist.
         */
        if (it->it_flags != (ITEM_SLABBED|ITEM_FETCHED)) {
            /* ITEM_SLABBED can only be added/removed under the slabs_lock */
            if (it->it_flags & ITEM_SLABBED) {
                assert(ch == NULL);
                slab_rebalance_cut_free(s_cls, it);
                status = MOVE_FROM_SLAB;
            } else if ((it->it_flags & ITEM_LINKED) != 0) {
                /* If it doesn't have ITEM_SLABBED, the item could be in any
                 * state on its way to being freed or written to. If no
                 * ITEM_SLABBED, but it's had ITEM_LINKED, it must be active
                 * and have the key written to it already.
                 */
                hv = hash(ITEM_key(it), it->nkey);
                if ((hold_lock = item_trylock(hv)) == NULL) {
                    status = MOVE_LOCKED;
                } else {
                    refcount = refcount_incr(it);
                    if (refcount == 2) { /* item is linked but not busy */
                        /* Double check ITEM_LINKED flag here, since we're
                         * past a memory barrier from the mutex. */
                        if ((it->it_flags & ITEM_LINKED) != 0) {
                            status = MOVE_FROM_LRU;
                        } else {
                            /* refcount == 1 + !ITEM_LINKED means the item is being
                             * uploaded to, or was just unlinked but hasn't been freed
                             * yet. Let it bleed off on its own and try again later */
                            status = MOVE_BUSY;
                        }
                    } else {
                        if (settings.verbose > 2) {
                            fprintf(stderr, "Slab reassign hit a busy item: refcount: %d (%d -> %d)\n",
                                it->refcount, slab_rebal.s_clsid, slab_rebal.d_clsid);
                        }
                        status = MOVE_BUSY;
                    }
                    /* Item lock must be held while modifying refcount */
                    if (status == MOVE_BUSY) {
                        refcount_decr(it);
                        item_trylock_unlock(hold_lock);
                    }
                }
            } else {
                /* See above comment. No ITEM_SLABBED or ITEM_LINKED. Mark
                 * busy and wait for item to complete its upload. */
                status = MOVE_BUSY;
            }
        }

        int save_item = 0;
        item *new_it = NULL;
        size_t ntotal = 0;
        switch (status) {
            case MOVE_FROM_LRU:
                /* Lock order is LRU locks -> slabs_lock. unlink uses LRU lock.
                 * We only need to hold the slabs_lock while initially looking
                 * at an item, and at this point we have an exclusive refcount
                 * (2) + the item is locked. Drop slabs lock, drop item to
                 * refcount 1 (just our own, then fall through and wipe it
                 */
                /* Check if expired or flushed */
                ntotal = ITEM_ntotal(it);
                /* REQUIRES slabs_lock: CHECK FOR cls->sl_curr > 0 */
                if (ch == NULL && (it->it_flags & ITEM_CHUNKED)) {
                    /* Chunked should be identical to non-chunked, except we need
                     * to swap out ntotal for the head-chunk-total. */
                    ntotal = s_cls->size;
                }
                if ((it->exptime != 0 && it->exptime < current_time)
                    || item_is_flushed(it)) {
                    /* Expired, don't save. */
                    save_item = 0;
                } else if (ch == NULL &&
                        (new_it = slab_rebalance_alloc(ntotal, slab_rebal.s_clsid)) == NULL) {
                    /* Not a chunk of an item, and nomem. */
                    save_item = 0;
                    slab_rebal.evictions_nomem++;
                } else if (ch != NULL &&
                        (new_it = slab_rebalance_alloc(s_cls->size, slab_rebal.s_clsid)) == NULL) {
                    /* Is a chunk of an item, and nomem. */
                    save_item = 0;
                    slab_rebal.evictions_nomem++;
                } else {
                    /* Was whatever it was, and we have memory for it. */
                    save_item = 1;
                }
                pthread_mutex_unlock(&slabs_lock);
                unsigned int requested_adjust = 0;
                if (save_item) {
                    if (ch == NULL) {
                        assert((new_it->it_flags & ITEM_CHUNKED) == 0);
                        /* if free memory, memcpy. clear prev/next/h_bucket */
                        memcpy(new_it, it, ntotal);
                        new_it->prev = 0;
                        new_it->next = 0;
                        new_it->h_next = 0;
                        /* These are definitely required. else fails assert */
                        new_it->it_flags &= ~ITEM_LINKED;
                        new_it->refcount = 0;
                        do_item_replace(it, new_it, hv);
                        /* Need to walk the chunks and repoint head  */
                        if (new_it->it_flags & ITEM_CHUNKED) {
                            item_chunk *fch = (item_chunk *) ITEM_data(new_it);
                            fch->next->prev = fch;
                            while (fch) {
                                fch->head = new_it;
                                fch = fch->next;
                            }
                        }
                        it->refcount = 0;
                        it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
                        memcpy(ITEM_key(it), "deadbeef", 8);
#endif
                        slab_rebal.rescues++;
                        requested_adjust = ntotal;
                    } else {
                        item_chunk *nch = (item_chunk *) new_it;
                        /* Chunks always have head chunk (the main it) */
                        ch->prev->next = nch;
                        if (ch->next)
                            ch->next->prev = nch;
                        memcpy(nch, ch, ch->used + sizeof(item_chunk));
                        ch->refcount = 0;
                        ch->it_flags = ITEM_SLABBED|ITEM_FETCHED;
                        slab_rebal.chunk_rescues++;
#ifdef DEBUG_SLAB_MOVER
                        memcpy(ITEM_key((item *)ch), "deadbeef", 8);
#endif
                        refcount_decr(it);
                        requested_adjust = s_cls->size;
                    }
                } else {
                    /* restore ntotal in case we tried saving a head chunk. */
                    ntotal = ITEM_ntotal(it);
                    do_item_unlink(it, hv);
                    slabs_free(it, ntotal, slab_rebal.s_clsid);
                    /* Swing around again later to remove it from the freelist. */
                    slab_rebal.busy_items++;
                    was_busy++;
                }
                item_trylock_unlock(hold_lock);
                pthread_mutex_lock(&slabs_lock);
                /* Always remove the ntotal, as we added it in during
                 * do_slabs_alloc() when copying the item.
                 */
                s_cls->requested -= requested_adjust;
                break;
            case MOVE_FROM_SLAB:
                it->refcount = 0;
                it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
                memcpy(ITEM_key(it), "deadbeef", 8);
#endif
                break;
            case MOVE_BUSY:
            case MOVE_LOCKED:
                slab_rebal.busy_items++;
                was_busy++;
                break;
            case MOVE_PASS:
                break;
        }

        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }

    if (slab_rebal.slab_pos >= slab_rebal.slab_end) {
        /* Some items were busy, start again from the top */
        if (slab_rebal.busy_items) {
            slab_rebal.slab_pos = slab_rebal.slab_start;
            STATS_LOCK();
            stats.slab_reassign_busy_items += slab_rebal.busy_items;
            STATS_UNLOCK();
            slab_rebal.busy_items = 0;
        } else {
            slab_rebal.done++;
        }
    }
    }
    else{
        slab_rebal.done++;      
    }

    pthread_mutex_unlock(&slabs_lock);

    return was_busy;
}

size_t adjust_size(size_t si){
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    int index = si/PAGESIZE;
    return (index+1) * PAGESIZE; 
}

static void slab_rebalance_finish(void) {
    slabclass_t *s_cls;
    slabclass_t *d_cls;
    int x;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;
    uint32_t chunk_rescues;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &slabclass[slab_rebal.s_clsid];
    if(slab_rebal.d_clsid != -2)        // When destination is external
        d_cls = &slabclass[slab_rebal.d_clsid];

#ifdef DEBUG_SLAB_MOVER
    /* If the algorithm is broken, live items can sneak in. */
    slab_rebal.slab_pos = slab_rebal.slab_start;
    while (1) {
        item *it = slab_rebal.slab_pos;
        assert(it->it_flags == (ITEM_SLABBED|ITEM_FETCHED));
        assert(memcmp(ITEM_key(it), "deadbeef", 8) == 0);
        it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }
#endif

    /* At this point the stolen slab is completely clear.
     * shuffle the page list backwards and decrement.
     */

    void * pp = NULL;
    if(!realloc_source){      // No need to do this when we are in receiver side of reassigning
        s_cls->slabs--;
        pp = s_cls->slab_list[slab_to_release];
        for (x = slab_to_release; x < s_cls->slabs; x++) {
            s_cls->slab_list[x] = s_cls->slab_list[x + 1];
            s_cls->hits[x] = s_cls->hits[x + 1];
            s_cls->rkey[x] = s_cls->rkey[x + 1];
            s_cls->conn[x] = s_cls->conn[x + 1];

            char * ptr = s_cls->slab_list[x];
            if(s_cls->conn[x] == NULL)                      /* we can't change the page_id */
                for (int y = 0; y < s_cls->perslab; y++){   /* for remote pages, unless we */
                    if(((item *)ptr)->page_id > 0)          /* perform a remote operation  */
                        ((item *)ptr)->page_id--;           /* -- might add in the future  */
                    ptr += s_cls->size;
                }
            else{
                remote_item ** r_ptr = (remote_item **) ptr;
                for (int y = 0; y < s_cls->perslab; y++){   /* But we can change it for the */
                    if(r_ptr[y]->page_id > 0)               /* r_it structs we have in lieu */
                        r_ptr[y]->page_id--;                /* of the original items        */
                }
            }
        }
        s_cls->slab_list[s_cls->slabs] = NULL;
        s_cls->hits[s_cls->slabs] = 0;
        s_cls->rkey[s_cls->slabs] = NULL;
        s_cls->conn[s_cls->slabs] = NULL;

        if(realloc_destination){
            size_t size = adjust_size((size_t)(s_cls->size * s_cls->perslab));

            printf("    address: %p \n",pp);

            if(realloc_destination != TENANT_REMOTE){
                set_spare_mem(pp,size,slab_rebal.s_clsid,(settings.port - 11212) % 4);
                munmap(pp,size);
            }
            else{
                set_remote_mem(pp, size,(settings.port - 11212) % 4);
                remote_region = pp;
                start_rdma_client();
            }
        }
    }
    /* taking care of destination */
    if(!realloc_destination){

        size_t si = (size_t)(d_cls->size * d_cls->perslab);
        si = adjust_size(si);
        
        struct tracker trck = get_tracker();            
        if(trck.spare_size < si){
            printf(" Adjusting..\n");
            si = trck.spare_size;
        }

        if(realloc_source == TENANT_LOCAL){

            item * ppp = (item *)shm_malloc_spare(si,(settings.port - 11212) % 4);
            memset(ppp, 0, si);
            d_cls->slab_list[d_cls->slabs++] = ppp;
            d_cls->rkey[d_cls->slabs-1] = NULL;
            d_cls->conn[d_cls->slabs-1] = NULL;
            printf("    address: %p \n",ppp); 
            int temp = d_cls->sl_curr;
            split_slab_page_into_freelist(d_cls->slab_list[d_cls->slabs-1], slab_rebal.d_clsid,si);
            printf("%d items added\n",d_cls->sl_curr - temp);
        }
        else if(realloc_source == TENANT_REMOTE){

            struct connection *conn = trck.latest_conn;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(trck.rdma_peer_addr.sin_addr), ip, INET_ADDRSTRLEN);

            printf("  Memory address: %p,  rkey: %u, IP: %s , ID: %d\n", conn->peer_mr.addr, conn->peer_mr.rkey, ip, conn->peer_identifier); 
            int temp = d_cls->sl_curr_r;
            d_cls->slab_list[d_cls->slabs++] = split_remote_page_into_freelist(conn->peer_mr.addr, slab_rebal.d_clsid, si);
            d_cls->rkey[d_cls->slabs-1] = conn->peer_mr.rkey;
            d_cls->conn[d_cls->slabs-1] = conn;
            printf("%d remote items added\n",d_cls->sl_curr_r - temp);
        }
        else{   
            d_cls->slab_list[d_cls->slabs++] = slab_rebal.slab_start;
            /* Don't need to split the page into chunks if we're just storing it */
            if (slab_rebal.d_clsid > SLAB_GLOBAL_PAGE_POOL) {
                memset(d_cls->slab_list[d_cls->slabs-1], 0, (size_t)(s_cls->size * s_cls->perslab));
                split_slab_page_into_freelist(d_cls->slab_list[d_cls->slabs-1], slab_rebal.d_clsid,(size_t)(s_cls->size * s_cls->perslab));
            } else if (slab_rebal.d_clsid == SLAB_GLOBAL_PAGE_POOL) {
                /* mem_malloc'ed might be higher than mem_limit. */
                memory_release();
            }
        }
    }
    
    /* adjust memory limit since a slab was reassigned to/from external source/destination */
    if(realloc_source || realloc_destination){
        size_t si = adjust_size((size_t)(s_cls->size * s_cls->perslab));
        struct tracker trck = get_tracker();
            
        if(realloc_source == TENANT_LOCAL){
            if(trck.spare_size < si)
                si = trck.spare_size;
            mem_malloced += si; 
        }
        else if(realloc_source == TENANT_REMOTE){
            mem_malloced_remote += 1024 * 1024 /*si*/; 
        }
        else 
           mem_malloced -= si;   
        //mem_avail = trck.max_size - trck.used_size;
        //mem_limit = mem_malloced + mem_avail ;
        mem_limit = trck.preset_share[(settings.port - 11212) % 4];
        mem_avail = mem_limit - (mem_malloced + mem_malloced_remote);
        unlock_spare();
        printf("total allocated mem(this-local): %lu   (this-remote): %lu\n\n", mem_malloced, mem_malloced_remote);
    }

    slab_rebal.done       = 0;
    slab_rebal.s_clsid    = 0;
    slab_rebal.slab_start = NULL;
    slab_rebal.slab_end   = NULL;
    slab_rebal.slab_pos   = NULL;
    evictions_nomem    = slab_rebal.evictions_nomem;
    inline_reclaim = slab_rebal.inline_reclaim;
    rescues   = slab_rebal.rescues;
    chunk_rescues = slab_rebal.chunk_rescues;
    slab_rebal.evictions_nomem    = 0;
    slab_rebal.inline_reclaim = 0;
    slab_rebal.rescues  = 0;


    if(realloc_source){
        reset_locks();
        if(realloc_source == TENANT_REMOTE)
            reset_remote_spare();
        slab_rebalance_signal = 10;
        rdma_transfer_state = TRY_TO_BROADCAST;
    }
    else{
        if(realloc_destination == TENANT_REMOTE)
            reset_remote_spare();
        unlock_min_score();
        reset_min_score();
        rdma_transfer_state = DONE;
        slab_to_release = -1;
        slab_rebalance_signal = 0;
        slab_rebal.d_clsid    = 0;
    }
    realloc_source = TENANT_SLAB;
    realloc_destination = TENANT_SLAB;

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slabs_moved++;
    stats.slab_reassign_rescues += rescues;
    stats.slab_reassign_evictions_nomem += evictions_nomem;
    stats.slab_reassign_inline_reclaim += inline_reclaim;
    stats.slab_reassign_chunk_rescues += chunk_rescues;
    stats_state.slab_reassign_running = false;
    STATS_UNLOCK();

    if (settings.verbose > 1) {
        fprintf(stderr, "finished a slab move\n");
    }
}

/* Slab mover thread.
 * Sits waiting for a condition to jump off and shovel some memory about
 */
static void *slab_rebalance_thread(void *arg) {
    int was_busy = 0;
    /* So we first pass into cond_wait with the mutex held */
    mutex_lock(&slabs_rebalance_lock);

    while (do_run_slab_rebalance_thread) {
        if (slab_rebalance_signal == 1) {
            if (slab_rebalance_start() < 0) {
                /* Handle errors with more specifity as required. */
                slab_rebalance_signal = 0;
            }

            was_busy = 0;
        } else if(slab_rebalance_signal == 10){
            victor_waiting_routine();
        } else if(slab_rebalance_signal && slab_rebal.slab_start != NULL) {
            was_busy = slab_rebalance_move();
        }

        if (slab_rebal.done) {
            slab_rebalance_finish();
        } else if (was_busy) {
            /* Stuck waiting for some items to unlock, so slow down a bit
             * to give them a chance to free up */
            usleep(50);
        }
        if (slab_rebalance_signal == 0) {
            /* always hold this lock while we're running */
            pthread_cond_wait(&slab_rebalance_cond, &slabs_rebalance_lock);
        }
    }
    return NULL;
}

/* Iterate at most once through the slab classes and pick a "random" source.
 * I like this better than calling rand() since rand() is slow enough that we
 * can just check all of the classes once instead.
 */
static int slabs_reassign_pick_any(int dst) {
    static int cur = POWER_SMALLEST - 1;
    int tries = power_largest - POWER_SMALLEST + 1;
    for (; tries > 0; tries--) {
        cur++;
        if (cur > power_largest)
            cur = POWER_SMALLEST;
        if (cur == dst)
            continue;
        if (slabclass[cur].slabs > 1) {
            return cur;
        }
    }
    return -1;
}

static enum reassign_result_type do_slabs_reassign(int src, int dst) {
    if (slab_rebalance_signal != 0)
        return REASSIGN_RUNNING;

    if(src == 1000){    //  does not know the source clsid, thread should enter the state of "waiting" for a page to be released 
        slab_rebalance_signal = 10;
        slab_rebal.d_clsid = dst;
        pthread_cond_signal(&slab_rebalance_cond);
        return REASSIGN_OK;
    }

    if (src == dst)
        if(!realloc_source)   // two slabs being in the same class is not a problem when they are from different tenants 
            return REASSIGN_SRC_DST_SAME;

    /* Special indicator to choose ourselves. */
    if (src == -1) {
        src = slabs_reassign_pick_any(dst);
        /* TODO: If we end up back at -1, return a new error type */
    }

    /* Indicating that victim is from an external source */
    if(realloc_source){
         if (dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
            return REASSIGN_BADCLASS;
    }
    else if(realloc_destination){
        if (src < POWER_SMALLEST        || src > power_largest )
            return REASSIGN_BADCLASS;

        if (src != 1 && slabclass[src].slabs < 2)
            return REASSIGN_NOSPARE;
    }
    else{
        if (src < POWER_SMALLEST        || src > power_largest ||
            dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
            return REASSIGN_BADCLASS;

        if (src != 1 && slabclass[src].slabs < 2)
            return REASSIGN_NOSPARE;

    }

    slab_rebal.s_clsid = src;
    slab_rebal.d_clsid = dst;

    slab_rebalance_signal = 1;
    pthread_cond_signal(&slab_rebalance_cond);
    
    return REASSIGN_OK;
}

enum reassign_result_type slabs_reassign(int src, int dst) {
    enum reassign_result_type ret;
    if (pthread_mutex_trylock(&slabs_rebalance_lock) != 0) {
        return REASSIGN_RUNNING;
    }
    ret = do_slabs_reassign(src, dst);
    pthread_mutex_unlock(&slabs_rebalance_lock);
    return ret;
}

/* If we hold this lock, rebalancer can't wake up or move */
void slabs_rebalancer_pause(void) {
    pthread_mutex_lock(&slabs_rebalance_lock);
}

void slabs_rebalancer_resume(void) {
    pthread_mutex_unlock(&slabs_rebalance_lock);
}

static pthread_t rebalance_tid;

int start_slab_maintenance_thread(void) {
    int ret;
    slab_rebalance_signal = 0;
    slab_rebal.slab_start = NULL;
    char *env = getenv("MEMCACHED_SLAB_BULK_CHECK");
    if (env != NULL) {
        slab_bulk_check = atoi(env);
        if (slab_bulk_check == 0) {
            slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;
        }
    }

    if (pthread_cond_init(&slab_rebalance_cond, NULL) != 0) {
        fprintf(stderr, "Can't intiialize rebalance condition\n");
        return -1;
    }
    pthread_mutex_init(&slabs_rebalance_lock, NULL);

    if ((ret = pthread_create(&rebalance_tid, NULL,
                              slab_rebalance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

int start_slab_rebalance_thread(void) {
     int ret;
     slab_rebalance_signal = 0;
     slab_rebal.slab_start = NULL;
            srand(time(0));

     if ((ret = pthread_create(&rebalance_tid, NULL,
                           slab_rebalance_thread, NULL)) != 0) {
         fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
         return -1;
     }
     return 0;
 }


/* The maintenance thread is on a sleep/loop cycle, so it should join after a
 * short wait */
void stop_slab_maintenance_thread(void) {
    mutex_lock(&slabs_rebalance_lock);
    do_run_slab_thread = 0;
    do_run_slab_rebalance_thread = 0;
    pthread_cond_signal(&slab_rebalance_cond);
    pthread_mutex_unlock(&slabs_rebalance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(rebalance_tid, NULL);
}

void stop_slab_rebalance_thread(void) {
     mutex_lock(&slabs_rebalance_lock);
     do_run_slab_thread = 0;
     do_run_slab_rebalance_thread = 0;
     pthread_cond_signal(&slab_rebalance_cond);
     pthread_mutex_unlock(&slabs_rebalance_lock);
     pthread_join(rebalance_tid, NULL);
}

static inline void now_epoch_sec_nsec(long *sec, long *nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = ts.tv_sec;
    *nsec = ts.tv_nsec;
}


void slabs_stats_file_write(FILE *f2){

    long sec, nsec;
    now_epoch_sec_nsec(&sec, &nsec);
    fprintf(f2,"%ld.%09ld,", sec, nsec);

    fprintf(f2,"%.2f,",(double)mem_malloced_remote / 1047840);
    fprintf(f2,"%.2f,",(double)mem_malloced / 1047840);
    for(int i = 2; i <= 9; i++) {
        slabclass_t *p = &slabclass[i];
        fprintf(f2,"%.2f,",(double)(p->slabs * p->perslab * p->size) / 1047840);
        fflush(f2);
    }

    fseek(f2,-1,SEEK_CUR);
    fprintf(f2,"\n");
}

void force_reassign(int d){
    realloc_source = TENANT_SLAB; 
    realloc_destination   = TENANT_SLAB;
    int counter = 0; //make sure we are not stuck in case there are no more slabs left
    do {
        if (counter++ > power_largest - POWER_SMALLEST + 1)
            break;
        prev_victim = ((prev_victim + 1 - POWER_SMALLEST) % (power_largest - POWER_SMALLEST + 1)) + POWER_SMALLEST; //round robin
    } while ((prev_victim == d) || (slabclass[prev_victim].slabs <= 1));

    printf("Internal REASSIGN: class %d  TO  class %d\n", prev_victim, d); 
    do_slabs_reassign(prev_victim,d);
}

void check_timeout(){
    if(start_t == 0)
        start_t = clock();
    end_t = clock();
    float seconds = (end_t - start_t) / CLOCKS_PER_SEC;

    if(seconds > 5 && pages_to_request != 0 && 
        (get_tracker().remote_server_ready == false ||  rdma_transfer_state == PAGE_TRANSFER_IN_PROGRESS)){
        //aborted = true;
        printf("Timeout. Trying again..\n");
        reset_locks();
        reset_remote_spare();//??
        unlock_spare();//??
        unlock_min_score();//??
        reset_min_score();//??
        start_t = 0;
        end_t = 0;
        rdma_transfer_state = TRY_TO_BROADCAST;
    }
}

void victor_waiting_routine(){ // Victor tenant runs this routine while waiting for new pages

    if(pages_to_request == 0 || aborted){ // finished
        finish_broadcast();
        realloc_source = TENANT_SLAB;
        realloc_destination   = TENANT_SLAB;
        aborted = false;
        time_elapsed = 0;  
        rdma_transfer_state = DONE;
        reset_locks();
        reset_remote_spare();
        unlock_max_score();
        slab_rebalance_signal = 0;  
    }
    else {
        check_timeout();
        
        switch (rdma_transfer_state) {
        case TRY_TO_BROADCAST:
            if(req_rem_spare())
                rdma_transfer_state = BROADCASTING;
            else{
                printf("Can't broadcast! Trying locally...\n");
                rdma_transfer_state = BROADCASTING;//BROADCASTING_FAILED;
            }
            req_spare();
            break;

        case BROADCASTING:
            if(r_server_available(settings.port))
                start_rdma_server();
            /* no break */
        case BROADCASTING_FAILED:
            if(new_mem_available()){       
                lock_spare();
                start_t = 0;
                end_t = 0;
                pages_to_request--;
                total_received_pages++;
                printf("\nExternal REASSIGN: Received 1 page(s) for class %d, Needs %d more\n", slab_rebal.d_clsid,pages_to_request);
                slab_rebal.s_clsid = get_spare_clsid();
                realloc_source = TENANT_LOCAL;
                realloc_destination   = TENANT_SLAB;
                is_taken_careof  = false;    
                slab_rebalance_signal = 1;
            }
            break;

        case PAGE_TRANSFER_IN_PROGRESS:
            if(r_mem_received()){
                pages_to_request--;
                total_received_pages++;
                printf("\nREMOTE REASSIGN: Received 1 page(s) for class %d, Needs %d more\n", slab_rebal.d_clsid,pages_to_request);
                realloc_source = TENANT_REMOTE;
                realloc_destination   = TENANT_SLAB;
                slab_rebalance_signal = 1;
                start_t = 0;
                end_t = 0;
            }
            break;

        case DONE:
        default:
            break;
        }
    }
}

/* RDMA connection routines */

void * poll_cm(struct rdma_event_channel *ec){

    struct rdma_cm_event *event = NULL;
    struct rdma_cm_event event_copy;

    while (rdma_get_cm_event(ec, &event) == 0){
        memcpy(&event_copy, event, sizeof(*event));
        rdma_ack_cm_event(event);

        if (on_event(&event_copy))
            break;
    }
    rdma_destroy_event_channel(ec);
}

static int get_local_rdma_ip(char *out, size_t outlen)
{
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0)
        return -1;

    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;

        char buf[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
            continue;

        if (strncmp(buf, "10.10.1.", 7) == 0)
        {
            strncpy(out, buf, outlen);
            out[outlen - 1] = '\0';
            freeifaddrs(ifaddr);
            return 0;
        }
    }
    freeifaddrs(ifaddr);
    return -1;
}

void start_rdma_server(){

    struct rdma_cm_id * c = lookup_rdma_connection(get_tracker().rdma_peer_addr, get_tracker().rdma_peer_id);
    if(c){
        set_rdma_access_port(ntohs(rdma_get_src_port(c)));
        post_receives(c->context);
        rdma_transfer_state = PAGE_TRANSFER_IN_PROGRESS;
        return;
    }

    struct sockaddr_in addr;
    struct rdma_cm_id *conn = NULL;
    struct rdma_event_channel *ec = NULL;
    uint16_t port = 0;

    /* Server-side preparation - All tenants are servers by default */

    printf("\nListening for connections ");
    TEST_Z(ec = rdma_create_event_channel());
    TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));

    char local_ip[INET_ADDRSTRLEN];
    if (get_local_rdma_ip(local_ip, sizeof(local_ip)) != 0){
        fprintf(stderr, "ERROR: cannot find local 10.10.1.x address for RDMA bind\n");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    inet_pton(AF_INET, local_ip, &addr.sin_addr);

    TEST_NZ(rdma_bind_addr(conn, (struct sockaddr *)&addr));
    TEST_NZ(rdma_listen(conn, 10));

    port = ntohs(rdma_get_src_port(conn));
    printf("on port: %d\n", port);
    set_rdma_access_port(port);

    conn->context = malloc(sizeof(struct connection));
    ((struct connection *)conn->context)->peer_identifier = get_tracker().rdma_peer_id;
    
    TEST_NZ(pthread_create(&((struct connection *)conn->context)->cm_poller_thread, NULL, poll_cm, ec));

    rdma_transfer_state = PAGE_TRANSFER_IN_PROGRESS;
}

void start_rdma_client(){

    struct rdma_cm_id * c = lookup_rdma_connection(get_tracker().rdma_peer_addr, get_tracker().rdma_peer_id);
    if(c){
        rdma_transfer_state = PAGE_TRANSFER_IN_PROGRESS;
        set_rdma_access_port(ntohs(rdma_get_src_port(c)));
        send_remote_page(c->context);
        return;
    }

    rdma_transfer_state = PAGE_TRANSFER_IN_PROGRESS;

    char ip[INET_ADDRSTRLEN], port[INET_ADDRSTRLEN];
    struct addrinfo *addrinf;
    struct rdma_cm_id *conn = NULL;
    struct rdma_event_channel *ec = NULL;

    /* Client-side preparation - A server that wants to access another server i.e., a client */

    TEST_Z(ec = rdma_create_event_channel());
    TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));

    struct sockaddr_in sa = get_tracker().rdma_peer_addr;
    inet_ntop( AF_INET, &sa.sin_addr, ip, INET_ADDRSTRLEN );
    sprintf(port, "%d", get_tracker().rdma_peer_id);

    printf("\n--Initiating access to a remote server. IP: %s, Port: %s\n",ip, port);
    TEST_NZ(getaddrinfo(ip, port, NULL, &addrinf));

    char local_ip[INET_ADDRSTRLEN];
    if (get_local_rdma_ip(local_ip, sizeof(local_ip)) != 0) {
        fprintf(stderr, "ERROR: cannot find local 10.10.1.x address for RDMA bind\n");
        exit(1);
    }

    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = 0;
    inet_pton(AF_INET, local_ip, &src.sin_addr);

    TEST_NZ(rdma_bind_addr(conn, (struct sockaddr *)&src));
    TEST_NZ(rdma_resolve_addr(conn, (struct sockaddr *)&src, addrinf->ai_addr, TIMEOUT_IN_MS));

    conn->context = malloc(sizeof(struct connection));
    ((struct connection *)conn->context)->peer_identifier = get_tracker().rdma_peer_id;
    
    TEST_NZ(pthread_create(&((struct connection *)conn->context)->cm_poller_thread, NULL, poll_cm, ec));
}

int on_addr_resolved(struct rdma_cm_id *id){
    printf("address resolved.");
    build_connection(id);
    sprintf(get_local_message_region(id->context), "message from active/client side with pid %d", getpid());
    TEST_NZ(rdma_resolve_route(id, TIMEOUT_IN_MS));

    return 0;
}

int on_route_resolved(struct rdma_cm_id *id){
    printf(" route resolved.\n");
    struct rdma_conn_param cm_params;
    build_params(&cm_params);
    TEST_NZ(rdma_connect(id, &cm_params));

    return 0;
}

int on_connect_request(struct rdma_cm_id *id){
    struct rdma_conn_param cm_params;
    printf("received connection request.\n");
    build_connection(id);
    build_params(&cm_params);
    sprintf(get_local_message_region(id->context), "message from passive/server side with pid %d", getpid());
    TEST_NZ(rdma_accept(id, &cm_params));

    return 0;
}

int on_connection(struct rdma_cm_id *id){
    on_connect(id->context);
    send_remote_page(id->context);
    return 0;
}

int on_disconnect(struct rdma_cm_id *id){
    printf("peer disconnected.\n");
    remove_rdma_connection(id);
    destroy_connection(id->context);
    return 1;
}

int on_event(struct rdma_cm_event *event){
    int r = 0;

    if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
        r = on_connect_request(event->id);
    else if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)
        r = on_addr_resolved(event->id);
    else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)
        r = on_route_resolved(event->id);
    else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
        r = on_connection(event->id);
    else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
        r = on_disconnect(event->id);
    else {
        fprintf(stderr, "on_event: %d\n", event->event);
        die("on_event: unknown event.");
    }

    return r;
}

void calculate_scores(uint64_t misses, double missRatio){

    // if(mem_malloced_remote + mem_malloced > 1100 * 1024 * 1024) return; // temporary, used for stress test

    double highest_mu = 0, lowest_mu = 1000000000, mu = 0;
    int temp = 0, cls_id = 0, cls_id2 = 0, page_id2 = -1, shadow_page_id = -1;

    if(isnan(missRatio))
        missRatio = 0;

    if(slab_rebalance_signal == 0){

        for(int i = POWER_SMALLEST;i <= power_largest;i++)
            if(slabclass[i].slabs > 0)
                for(int j = 999;j >= 0;j--){
                    if(slabclass[i].shadowq_hits[j] > /*(j+1) * */ SHADOWQ_HIT_THRESHOLD){
                        temp = 0;
                        for(int k = j;k >= 0;k--)
                            temp += slabclass[i].shadowq_hits[k];
                        mu = (double) temp / (j+1);
                        if(mu > highest_mu){
                            highest_mu = mu;
                            cls_id = i;
                            shadow_page_id = j;
                        }
                    }
                }

        for(int i = POWER_SMALLEST;i <= power_largest;i++){
            if(i != prev_victim && slabclass[i].slabs > 10)
                for(int j = 0;j <= slabclass[i].slabs - 10;j++){
                    if(mem_avail > 2000000){
                        lowest_mu = 0;
                        cls_id2 = -2;
                        page_id2 = -2;
                        break;
                    }
                    else if(slabclass[i].hits[j] < lowest_mu && slabclass[i].conn[j] == NULL){
                        lowest_mu = slabclass[i].hits[j];
                        cls_id2 = i;
                        page_id2 = j;
                        if(lowest_mu == 0)
                            break;
                    }
                }
            if(lowest_mu == 0 || cls_id2 == -2)
                break;
        }

        /* CALCULATE SCORES */
        score1 = (misses - total_misses) * highest_mu * 
                        (time_elapsed * (total_released_pages + 1)) / 
                        (total_received_pages * ((mem_malloced + mem_malloced_remote) / 1000000) + 1);
        score2 = (misses - total_misses) * lowest_mu * 
                        (time_elapsed * total_released_pages) / 
                        (total_received_pages * ((mem_malloced + mem_malloced_remote) / 1000000) + 1);
        score2 = (score2) ? score2 : missRatio;
        if(score1 || missRatio || ((misses - total_misses == 0) && (lowest_mu != 0)))     /* A non-zero score1 means the tenant is in need of memory */
            score2 = __DBL_MAX__;   /* Making sure it is not going to release any */

        // if(misses - total_misses != 0 && score1 == 0) { score1 = (double) (rand() % 10) + 1; shadow_page_id = 10; cls_id = 30;} // temporaray, used for stress test
        
        double myscore = (double) (rand() % 10) + 1; // testing purposes
        set_scores(score1, score2, settings.port);

        if(!score1 && (spare_needed() || remote_spare_needed(settings.port)) && rdma_transfer_state == DONE){
    
            if(compare_minID(settings.port, score2) > 3 && !new_mem_available()){   // I'm the Victim

                if(cls_id2 != 0 && page_id2 != -1 && lock_spare()){
                    if(cls_id2 == -2 && page_id2 == -2){
                        if(remote_spare_needed(settings.port) && !spare_needed()){
                            total_released_pages++;
                            printf("\nExternal REASSIGN: Releasing Memory (Free Space) \n");
                            do_slabs_newslab(1);
                            is_taken_careof = false;
                            realloc_destination  = TENANT_REMOTE;
                            cls_id2 = 1;
                            page_id2 = 0;
                            slab_to_release = page_id2;
                            printf("\nExternal REASSIGN: Releasing Memory from class %d (slab %d)\n", cls_id2, page_id2);
                            do_slabs_reassign(cls_id2,-2);  // -2 means it's located in another tenant
                        }
                        else {
                            total_released_pages++;
                            printf("\nExternal REASSIGN: Releasing Memory (Free Space) \n");
                            signal_alloc_free((settings.port - 11212) % 4,1047840);
                            struct tracker trck = get_tracker();
                            mem_limit = trck.preset_share[(settings.port - 11212) % 4];
                            //mem_limit -= ( (slabclass[1].size * slabclass[1].perslab/(sysconf(_SC_PAGESIZE)) ) + 1) * (sysconf(_SC_PAGESIZE));
                            mem_avail = mem_limit - (mem_malloced + mem_malloced_remote);
                            printf("total allocated mem(this-local): %lu   (this-remote): %lu\n", mem_malloced, mem_malloced_remote);
                            printf("free mem(this): %lu\n", mem_avail);
                            unlock_spare(); 
                        } 
                    }
                    else {
                        is_taken_careof = false;
                        realloc_destination  = TENANT_LOCAL;
                        slab_to_release = page_id2;
                        prev_victim = cls_id2;
                        total_released_pages++;
                        if(remote_spare_needed(settings.port) && !spare_needed())
                            realloc_destination = TENANT_REMOTE;
                        printf("\nExternal REASSIGN: Releasing Memory from class %d (slab %d)\n", cls_id2, page_id2);
                        do_slabs_reassign(cls_id2,-2);  // -2 means it's located in another tenant
                    }
                }
            }
        }
        else{
            if(compare_maxID(settings.port, score1) > 10 && score1 != 0){ // Victor

                if(cls_id != 0 && shadow_page_id != -1 && req_spare() && lock_max_score()){
                    for(int k = shadow_page_id;k >= 0;k--)
                        slabclass[cls_id].shadowq_hits[k] = 0;
                    
                    pages_to_request = shadow_page_id + 1; //(shadow_page_id < 10) ? shadow_page_id + 1 : 10;
                    start_t = 0;
                    end_t = 0;
                    rdma_transfer_state = TRY_TO_BROADCAST;
                    printf("\nExternal REASSIGN: Requested %d page(s) for class %d\n",pages_to_request, cls_id);
                    do_slabs_reassign(1000,cls_id);

                }
            }
        }

        // if(new_mem_available() && (get_spare_owner() == settings.port)){ /* For an unknown reason, the released page was not used */
        //     sleep(1);
        //     if(new_mem_available() && (get_spare_owner() == settings.port) && lock_spare()){
        //         printf("! A page was WASTED. Resetting...\n");
        //         reset_locks();
        //         reset_remote_spare();
        //         unlock_spare();
        //     }
        // }
    }
    else{
        if(slab_rebalance_signal == 10 && total_misses == misses)
            if((++no_diff_miss_times) >= 5){
                    pages_to_request = 0;
                    no_diff_miss_times = 0;
                    aborted = true;
                    printf("\nExternal REASSIGN: Aborted! No MISSES.\n");
            }
    }
    if(total_misses != misses)
        no_diff_miss_times = 0;
    total_misses = misses;        
}

void incr_slab_hits(uint8_t clsid, uint8_t slabid){
    if(slabid < 3999){
        slabclass_t *p = &slabclass[clsid];
        p->hits[slabid]++;
    }
}

 void* get_slabclass(int id){ return &slabclass[id]; }
 shadow_item* get_shadowq_head(unsigned int id) { return (slabclass[id].shadowq_head); }
 void set_shadowq_head(shadow_item *elem, unsigned int id) { slabclass[id].shadowq_head = elem; }
 shadow_item* get_shadowq_tail(unsigned int id) { return (slabclass[id].shadowq_tail); }
 void set_shadowq_tail(shadow_item *elem, unsigned int id) { slabclass[id].shadowq_tail = elem; }
 unsigned int get_shadowq_max_items(unsigned int id) { return (slabclass[id].shadowq_max_items); }
 unsigned int get_shadowq_size(unsigned int id) { return (slabclass[id].shadowq_size); }
 void dec_shadowq_size(unsigned int id) { slabclass[id].shadowq_size--; }
 void inc_shadowq_size(unsigned int id) { slabclass[id].shadowq_size++; }
