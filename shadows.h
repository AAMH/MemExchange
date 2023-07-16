#include <stdio.h>
#include <stdbool.h>
#define SHADOWQ_HIT_THRESHOLD 256


typedef struct _avl_node_t { 
    struct _avl_node_t *parent;
	struct _avl_node_t *left;
    struct _avl_node_t *right; 

    struct timeval       time;
    uint32_t             weight;

	int                  height; 
} node_t; 

typedef struct _avl_tree_t {
    node_t *root;
} tree_t;

typedef struct _shadow_item_t {
    struct _shadow_item_t *next;
    struct _shadow_item_t *prev;
    struct _shadow_item_t *h_next;    /* hash chain next */
    uint8_t                nkey;      /* key length, w/terminating null and padding */
    uint8_t                slabs_clsid;
    char                   *key;
    struct timeval         last_seen_time;
} shadow_item;

typedef struct {
    unsigned int size;      /* sizes of items */
    unsigned int perslab;   /* how many items per slab */

    void *slots;           /* list of item ptrs */
    unsigned int sl_curr;   /* total free items in list */

    unsigned int slabs;     /* how many slabs were allocated for this class */

    void **slab_list;       /* array of slab pointers */
    unsigned int list_size; /* size of prev array */

    size_t requested; /* The number of requested bytes */

    uint32_t hits[4000];

/*** Remote memory access Additions ***/
    uint32_t *rkey;
    struct connection **conn;
    void *rslots;
    
/*** shadow queue Additions ***/
    shadow_item *shadowq_head;
    shadow_item *shadowq_tail;
    unsigned int shadowq_size;
    uint32_t shadowq_max_items;
    uint32_t shadowq_hits[4000];
    uint32_t q_misses;

    tree_t *tree;

} slabclass_t;

static pthread_mutex_t shadow_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tree_lock = PTHREAD_MUTEX_INITIALIZER;

extern int time_elapsed;
extern int timee;

void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid);
void remove_shadowq_item(shadow_item *elem, node_t * node);
void evict_shadowq_item(shadow_item *shadowq_it);

tree_t *new_tree();
node_t *search_tree(node_t *root, struct timeval key);
void insert_tree_node(tree_t *t, node_t *n);
void delete_tree_node(tree_t *t, node_t *z);
void fix_weights(node_t *root, node_t *node);
int  calculate_reuse_distance(node_t *root, node_t *node);