#ifndef _LONG_MAP_H
#define _LONG_MAP_H

struct long_map;

typedef void *(long_map_alloc_t)(unsigned long);
typedef void (long_map_free_t)(void*, unsigned long);

struct long_map *long_map_create(long_map_alloc_t *alloc_fn,
				 long_map_free_t *free_fn,
				 unsigned long param);
int long_map_prealloc(struct long_map *map, unsigned int count);
void long_map_destroy(struct long_map *map);
unsigned long *long_map_get(struct long_map *map, unsigned long key);
unsigned long *long_map_insert(struct long_map *map, unsigned long key);
void long_map_move(struct long_map *map, unsigned long dst_key,
		   unsigned long src_key);
void long_map_erase(struct long_map *map, unsigned long key);
void long_map_clear(struct long_map *map);

#endif
