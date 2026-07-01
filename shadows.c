#include "assert.h"
#include "memcached.h"
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

node_t *new_tree_node(struct timeval key) 
{
	node_t *node = malloc(sizeof(*node));
    assert(node);
    node->parent = NULL;
	node->time = key; 
	node->left = NULL; 
	node->right = NULL; 
    node->weight = 0;
	node->height = 1; // new node is initially added as leaf 

	return node; 
} 

tree_t *new_tree(void)
{
    tree_t *t = malloc(sizeof(*t));
    assert(t);

    t->root = NULL;
    pthread_mutex_init(&t->lock, NULL);

    return t;
}

int max(int a, int b)
{
    if(a > b)
        return a;
    return b;
}

int height(node_t *n)
{
    if(n == NULL)
        return -1;
    return n->height;
}

node_t *minimum(tree_t *t, node_t *x)
{
    while(x->left != NULL)
        x = x->left;
    return x;
}

void left_rotate(tree_t *t, node_t *x)
{
    node_t *y = x->right;
    x->right = y->left;

    if(y->left != NULL){
        y->left->parent = x;
    }

    y->parent = x->parent;

    if(x->parent == NULL){ // x is root
        t->root = y;
    }
    else if(x == x->parent->left){ // x is left child
        x->parent->left = y;
    }
    else { // x is right child
        x->parent->right = y;
    }

    y->left = x;
    x->parent = y;

    x->height = 1 + max(height(x->left), height(x->right));
    y->height = 1 + max(height(y->left), height(y->right));
}

void right_rotate(tree_t *t, node_t *x)
{
    node_t *y = x->left;
    x->left = y->right;

    if(y->right != NULL){
        y->right->parent = x;
    }
    
    y->parent = x->parent;
    
    if(x->parent == NULL){ // x is root
        t->root = y;
    }
    else if(x == x->parent->right){ // x is left child
        x->parent->right = y;
    }
    else { // x is right child
        x->parent->left = y;
    }

    y->right = x;
    x->parent = y;

    x->height = 1 + max(height(x->left), height(x->right));
    y->height = 1 + max(height(y->left), height(y->right));
}

int balance_factor(node_t *n)
{
    if(n == NULL)
        return 0;
    return(height(n->left) - height(n->right));
}

static int compare_nodes(const node_t *a, const node_t *b)
{
    if (a->time.tv_sec != b->time.tv_sec)
        return (a->time.tv_sec < b->time.tv_sec) ? -1 : 1;

    if (a->time.tv_usec != b->time.tv_usec)
        return (a->time.tv_usec < b->time.tv_usec) ? -1 : 1;

    if (a->shadowItem < b->shadowItem)
        return -1;
    if (a->shadowItem > b->shadowItem)
        return 1;

    return 0;
}

void insert_tree_node(tree_t *t, node_t *n)
{
    node_t *y = NULL;
    node_t *temp = t->root;

    while(temp != NULL){
        y = temp;
        int cmp = compare_nodes(n, temp);

        if (cmp < 0) {
            temp = temp->left;
        } else {
            temp->weight++;
            temp = temp->right;
        }
    }
    n->parent = y;

    if(y == NULL) //newly added node is root
        t->root = n;
    else {
        int cmp = compare_nodes(n, y);
        if (cmp < 0)
            y->left = n;
        else
            y->right = n;
    }

    node_t *z = n;

    while(y != NULL){
        y->height = 1 + max(height(y->left), height(y->right));

        node_t *x = y->parent;
        int bf = x ? balance_factor(x) : 0;

        if(x && (bf <= -2 || bf >= 2)){ //grandparent is unbalanced

            if(y == x->left){
                if(z == x->left->left) //case 1
                    right_rotate(t, x);

                else if(z == x->left->right){ //case 3
                    left_rotate(t, y);
                    right_rotate(t, x);
                }
            }
            else if(y == x->right){
                if(z == x->right->right) //case 2
                    left_rotate(t, x);

                else if(z == x->right->left){ //case 4
                    right_rotate(t, y);
                    left_rotate(t, x);
                }
            }
            break;
        }

        y = y->parent;
        z = z->parent;
    }
}

void transplant(tree_t *t, node_t *u, node_t *v) // replaces u by v
{
    if(u->parent == NULL) // u is root
        t->root = v;
    else if(u == u->parent->left) // u is left child
        u->parent->left = v;
    else // u is right child
        u->parent->right = v;

    if(v != NULL)
        v->parent = u->parent;
}

void delete_fixup(tree_t *t, node_t *n)
{
    node_t *p = n;

    while(p != NULL){
        p->height = 1 + max(height(p->left), height(p->right));

        if(balance_factor(p) <= -2 || balance_factor(p) >= 2){ //grandparent is unbalanced
            node_t *x, *y, *z;
            x = p;

            //taller child of x will be y
            if(x->left && x->right){
                if(x->left->height > x->right->height)
                    y = x->left;
                else
                    y = x->right;
            } 
            else if(!x->left)
                y = x->right;
            else if(!x->right)
                y = x->left;

            //taller child of y will be z
            if(y->left && y->right){
                if(y->left->height > y->right->height){
                    z = y->left;
                }
                else if(y->left->height < y->right->height){
                    z = y->right;
                }
                else { //same height, go for single rotation
                    if(y == x->left)
                        z = y->left;
                    else
                        z = y->right;
                }
            }
            else if(!y->left)
                z = y->right;
            else if(!y->right)
                z = y->left;

            if(y == x->left){
                if(z == x->left->left) //case 1
                    right_rotate(t, x);

                else if(z == x->left->right){//case 3
                    left_rotate(t, y);
                    right_rotate(t, x);
                }
            }
            else if(y == x->right){
                if(z == x->right->right) //case 2
                    left_rotate(t, x);

                else if(z == x->right->left){//case 4
                    right_rotate(t, y);
                    left_rotate(t, x);
                }
            }
        }
        p = p->parent;
    }
}

void delete_tree_node(tree_t *t, node_t *z)
{
    if(z->left == NULL) {
        transplant(t, z, z->right);
        if(z->right != NULL)
            delete_fixup(t, z->right);
        free(z);
    }
    else if(z->right == NULL) {
        transplant(t, z, z->left);
        if(z->left != NULL)
            delete_fixup(t, z->left);
        free(z);
    }
    else {
        node_t *y = minimum(t, z->right); //minimum element in right subtree
        if(y->parent != z){
            transplant(t, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(t, z, y);
        y->left = z->left;
        y->left->parent = y;
        if(y != NULL)
            delete_fixup(t, y);
        free(z);
    }
}

node_t *search_tree(node_t *root, shadow_item *it)  // Not needed anymore, shadow items have direct reference to their tree node
{
    while (root != NULL) {
        int cmp = compare_nodes(root, it);

        if (cmp == 0)            return root;
        else if (cmp < 0) root = root->right;
        else               root = root->left;
    }

    return NULL;
}

// A utility function for preorder traversal of the tree. 
// The function returns the weight of the sub-tree
int preOrder(node_t *node) 
{
	if (node == NULL)
        return 0;
    else
        return (1+ preOrder(node->left) + preOrder(node->right)); 
} 

void fix_weights(node_t *root, node_t *node)
{
    while(node != root){
        if(node == (node->parent)->right)
            node->parent->weight--;
        node = node->parent;
    }
}

int calculate_reuse_distance(node_t *root, node_t *node)
{
    int x = node->weight;
    while(node != root){
	if(node->parent == NULL)
            break;
        if(node == (node->parent)->left)
            x += (node->parent)->weight + 1;
        node = node->parent;
    }
    return x;
}

#define KEY_PAD 4

shadow_item* create_shadow_item(item *it, uint8_t clsid, uint16_t nkey)
{
    assert(it);

    shadow_item *shadow_it = calloc(1, sizeof(*shadow_it));
    if (!shadow_it) return NULL;

    shadow_it->key = malloc((size_t)nkey + 1 + KEY_PAD);
    if (!shadow_it->key) { free(shadow_it); return NULL; }

    memcpy(shadow_it->key, ITEM_key(it), nkey);
    shadow_it->key[nkey] = '\0';
    memset(shadow_it->key + nkey + 1, 0, KEY_PAD);

    shadow_it->nkey = nkey;
    shadow_it->slabs_clsid = clsid;

    atomic_init(&shadow_it->refcnt, 1); // structure-owned ref
    shadow_it->dead = false;
    shadow_it->tree_node = NULL;

    return shadow_it;
}

void remove_shadowq_item_locked(shadow_item *elem)
{
    // p->shadowq_lock must already be held

    if (elem->prev) elem->prev->next = elem->next;
    if (elem->next) elem->next->prev = elem->prev;

    if (get_shadowq_head(elem->slabs_clsid) == elem)
        set_shadowq_head(elem->next, elem->slabs_clsid);

    if (get_shadowq_tail(elem->slabs_clsid) == elem)
        set_shadowq_tail(elem->prev, elem->slabs_clsid);

    dec_shadowq_size(elem->slabs_clsid);

    elem->prev = NULL;
    elem->next = NULL;
}

void insert_shadowq_item_locked(shadow_item *elem)
{
    // p->shadowq_lock must already be held

    uint8_t slabs_clsid = elem->slabs_clsid;

    elem->prev = NULL;
    shadow_item *old_head = get_shadowq_head(slabs_clsid);

    elem->next = old_head;
    if (old_head) old_head->prev = elem;
    else set_shadowq_tail(elem, slabs_clsid);

    set_shadowq_head(elem, slabs_clsid);
    inc_shadowq_size(slabs_clsid);
}

void insert_shadowq_item(shadow_item *elem)
{
    assert(elem);
    uint8_t clsid = elem->slabs_clsid;
    slabclass_t *p = get_slabclass(clsid);

    // Update timestamp (no shared state yet)
    gettimeofday(&elem->last_seen_time, NULL);

    // Insert into tree (tree is separate)
    pthread_mutex_lock(&p->tree->lock);
    node_t *tn = new_tree_node(elem->last_seen_time);
    tn->shadowItem = elem;
    elem->tree_node = tn;
    insert_tree_node(p->tree, tn);
    pthread_mutex_unlock(&p->tree->lock);

    // Queue insert + possible eviction is ONE critical section
    pthread_mutex_lock(&p->shadowq_lock);

    insert_shadowq_item_locked(elem);

    // Evict while still holding the lock so head/tail pointers are stable
    if (get_shadowq_size(clsid) > get_shadowq_max_items(clsid)) {
        shadow_item *victim = get_shadowq_tail(clsid);
        if (victim) {
            // evict will unlink while we're locked
            evict_shadowq_item_locked(victim);
        }
    }

    pthread_mutex_unlock(&p->shadowq_lock);
}

void remove_shadowq_item(shadow_item *elem)
{
    slabclass_t *p = get_slabclass(elem->slabs_clsid);
    pthread_mutex_lock(&p->shadowq_lock);
    remove_shadowq_item_locked(elem);
    pthread_mutex_unlock(&p->shadowq_lock);
}

static void shadow_maybe_free(shadow_item *it)
{
    // called when refcnt already reached 0
    free(it->key);
    free(it);
}

void evict_shadowq_item_locked(shadow_item *victim)
{
    if (victim->dead) return;
    // p->shadowq_lock must be held
    // We will temporarily take tree lock and hash lock (in allowed order).

    slabclass_t *p = get_slabclass(victim->slabs_clsid);

    // 1) unlink from queue
    remove_shadowq_item_locked(victim);

    // 2) remove from tree (if present)
    pthread_mutex_lock(&p->tree->lock);
    node_t *node = victim->tree_node;
    if (node) {
        fix_weights(p->tree->root, node);
        delete_tree_node(p->tree, node);
        victim->tree_node = NULL;
    }
    pthread_mutex_unlock(&p->tree->lock);

    // 3) remove from hash + mark dead + drop structure ref
    uint32_t hv = hash(victim->key, victim->nkey);
    pthread_mutex_t *hlk = shadow_hash_lock_for_hv(hv);
    pthread_mutex_lock(hlk);

    victim->dead = true;
    shadow_assoc_delete(victim->key, victim->nkey, hv);

    unsigned prev = atomic_fetch_sub_explicit(&victim->refcnt, 1, memory_order_relaxed);
    bool free_now = (prev == 1); // now refcnt==0

    pthread_mutex_unlock(hlk);

    if (free_now) shadow_maybe_free(victim);
}

void shadow_release(shadow_item *it, uint32_t hv)
{
    pthread_mutex_t *hlk = shadow_hash_lock_for_hv(hv);
    pthread_mutex_lock(hlk);

    unsigned prev = atomic_fetch_sub_explicit(&it->refcnt, 1, memory_order_relaxed);
    bool free_now = (prev == 1) && it->dead;

    pthread_mutex_unlock(hlk);

    if (free_now) shadow_maybe_free(it);
}

shadow_item *shadow_find_and_pin(const char *key, size_t nkey, uint32_t hv)
{
    pthread_mutex_t *hlk = shadow_hash_lock_for_hv(hv);
    pthread_mutex_lock(hlk);

    shadow_item *it = shadow_assoc_find(key, nkey, hv);
    if (it && !it->dead) {
        atomic_fetch_add_explicit(&it->refcnt, 1, memory_order_relaxed);
    } else {
        it = NULL;
    }

    pthread_mutex_unlock(hlk);
    return it;
}

shadow_item* slabs_shadowq_lookup(char *key, const size_t nkey)
{
    uint32_t hv = hash(key, nkey);
    shadow_item *shadow_it = shadow_find_and_pin(key, nkey, hv);
    if (!shadow_it) return NULL;

    slabclass_t *p = get_slabclass(shadow_it->slabs_clsid);

    // We need queue + tree stable while we compute and move-to-head.
    pthread_mutex_lock(&p->shadowq_lock);
    pthread_mutex_lock(&p->tree->lock);

    node_t *node = shadow_it->tree_node;
    if (!node) {
        pthread_mutex_unlock(&p->tree->lock);
        pthread_mutex_unlock(&p->shadowq_lock);
        shadow_release(shadow_it, hv);
        return NULL;
    }

    int y = calculate_reuse_distance(p->tree->root, node);
    int x = y / p->perslab;

    fix_weights(p->tree->root, node);
    delete_tree_node(p->tree, node);
    shadow_it->tree_node = NULL;

    // Move-to-head in the queue (no re-locking)
    remove_shadowq_item_locked(shadow_it);

    // Update last_seen_time and reinsert into tree + queue as the newest
    gettimeofday(&shadow_it->last_seen_time, NULL);

    node_t *tn = new_tree_node(shadow_it->last_seen_time);
    tn->shadowItem = shadow_it;
    shadow_it->tree_node = tn;
    insert_tree_node(p->tree, tn);

    insert_shadowq_item_locked(shadow_it);

    // Counters
    if (x >= 0 && x <= 3999) p->shadowq_hits[x]++;
    p->q_misses++;

    pthread_mutex_unlock(&p->tree->lock);
    pthread_mutex_unlock(&p->shadowq_lock);

    shadow_release(shadow_it, hv);
    return shadow_it;
}