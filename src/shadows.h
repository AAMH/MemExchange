#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#define SHADOWQ_HIT_THRESHOLD 256

struct _avl_node_t;  // forward declaration
typedef struct _avl_node_t node_t;
typedef struct _shadow_item_t {
    struct _shadow_item_t *next;
    struct _shadow_item_t *prev;
    struct _shadow_item_t *h_next;    /* hash chain next */

    uint16_t               nkey;      /* key length, w/terminating null and padding */
    uint8_t                slabs_clsid;

    char                  *key;
    struct timeval         last_seen_time;

    struct _avl_node_t    *tree_node;

    atomic_uint            refcnt;   // number of active users
    bool                   dead;     // removed from hash/queue, waiting to be freed
} shadow_item;

typedef struct _avl_node_t { 
    struct _avl_node_t *parent;
	struct _avl_node_t *left;
    struct _avl_node_t *right; 

    struct timeval      time;
    shadow_item        *shadowItem; 
    uint32_t            weight;
	uint32_t            height; 
} node_t;

typedef struct _avl_tree_t {
    node_t *root;
    pthread_mutex_t lock;
} tree_t;

extern int time_elapsed;

/* SHADOW QUEUE OPERATION */
shadow_item* shadow_find_and_pin(const char *key, size_t nkey, uint32_t hv);
void shadow_release(shadow_item *it, uint32_t hv);

void insert_shadowq_item(shadow_item *elem);
void remove_shadowq_item(shadow_item *elem);
/* internal locked helpers */
void insert_shadowq_item_locked(shadow_item *elem);
void remove_shadowq_item_locked(shadow_item *elem);
void evict_shadowq_item_locked(shadow_item *victim);

shadow_item* slabs_shadowq_lookup(char *key, const size_t nkey);

/* AVL TREE MAINTENANCE OPERATION */
tree_t *new_tree();
node_t *new_tree_node(struct timeval key);
node_t *search_tree(node_t *root, shadow_item * it);
void insert_tree_node(tree_t *t, node_t *n);
void delete_tree_node(tree_t *t, node_t *z);
void fix_weights(node_t *root, node_t *node);
int  calculate_reuse_distance(node_t *root, node_t *node);