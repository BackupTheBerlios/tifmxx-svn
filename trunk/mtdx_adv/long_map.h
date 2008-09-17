#ifndef _LONG_MAP_H
#define _LONG_MAP_H

struct long_map;

typedef void *(long_map_alloc_t)(unsigned long);
typedef void (long_map_free_t)(void*, unsigned long);

struct long_map *long_map_create(unsigned int nr, long_map_alloc_t *alloc_fn,
				 long_map_free_t *free_fn, unsigned long param);
void long_map_destroy(struct long_map *map);
unsigned long *long_map_find(struct long_map *map, unsigned long key);
unsigned long *long_map_insert(struct long_map *map, unsigned long key);
void long_map_erase(struct long_map *map, unsigned long key);
void long_map_clear(struct long_map *map);

#endif
