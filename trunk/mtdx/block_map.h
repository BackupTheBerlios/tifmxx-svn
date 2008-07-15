#ifndef _BLOCK_MAP_H
#define _BLOCK_MAP_H

struct mtdx_block_map;

enum mtdx_block_map_type {
	MTDX_BLOCK_MAP_INVALID = 0,
	MTDX_BLOCK_MAP_RANDOM,
	MTDX_BLOCK_MAP_INCREMENTAL
};

struct mtdx_block_map *mtdx_block_map_alloc(unsigned int page_cnt,
					    unsigned int node_count,
					    enum mtdx_block_map_type map_type);
void mtdx_block_map_free(struct mtdx_block_map *map);
int mtdx_block_map_set_range(struct mtdx_block_map *map, unsigned int address,
			     unsigned int offset, unsigned int count);
int mtdx_block_map_check_range(struct mtdx_block_map *map, unsigned int address,
			       unsigned int offset, unsigned int count);
#endif
