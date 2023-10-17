#include <stdint.h>
#include <stddef.h>
#include "shadows.h"

/* associative array */
void shadow_assoc_init(const int hashpower_init);
shadow_item *shadow_assoc_find(const char *key, const size_t nkey, const uint32_t hv);
int shadow_assoc_insert(shadow_item *item, const uint32_t hv);
void shadow_assoc_delete(const char *key, const size_t nkey, const uint32_t hv);
void shadow_do_assoc_move_next_shadow(void);
int shadow_start_assoc_maintenance_thread(void);
void shadow_stop_assoc_maintenance_thread(void);
