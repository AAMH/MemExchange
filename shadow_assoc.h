#include <stdint.h>
#include <stddef.h>
#include "shadows.h"

#define SHADOW_LOCK_POWER 12                 // 4096 stripes; tune
#define SHADOW_LOCKS (1U << SHADOW_LOCK_POWER)

static pthread_mutex_t shadow_locks[SHADOW_LOCKS];
static inline pthread_mutex_t *shadow_hash_lock_for_hv(uint32_t hv) { return &shadow_locks[hv & (SHADOW_LOCKS - 1)]; }

void shadow_locks_init(void);

/* associative array */
void         shadow_assoc_init(const int hashpower_init);
int          shadow_assoc_insert(shadow_item *item, const uint32_t hv);
shadow_item *shadow_assoc_find(const char *key, const size_t nkey, const uint32_t hv);
void         shadow_assoc_delete(const char *key, const size_t nkey, const uint32_t hv);

void         shadow_do_assoc_move_next_shadow(void);
int          shadow_start_assoc_maintenance_thread(void);
void         shadow_stop_assoc_maintenance_thread(void);