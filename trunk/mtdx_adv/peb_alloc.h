/*
 *  MTDX block allocator interface
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _PEB_ALLOC_H
#define _PEB_ALLOC_H

/*
 * Generic interface for block allocators. By default (at creation time) all
 * blocks are believed used. During the block scan (or equivalent activity)
 * user of the allocator will call put_peb for each free block encountered,
 * marking it with an appropriate status. When the user needs a free block,
 * he will call get_peb with <*dirty> unset to get a free, already erased
 * block. If no such blocks are found, allocator may still return a block,
 * which needs erasing first (this will be reflected in the <*dirty> value).
 * User may call get_peb specifically with <*dirty> set, to get a useful block
 * that must be erased first, if explicit garbage collection is desirable.
 *
 *
 */

#define MTDX_PEB_ALLOC_ALL 0xffffffff

struct mtdx_peb_alloc {
	const struct mtdx_geo *geo;

	unsigned int (*get_peb)(struct mtdx_peb_alloc *bal,
				unsigned int zone, int *dirty);
	void         (*put_peb)(struct mtdx_peb_alloc *bal, unsigned int peb,
				int dirty);
	void         (*reset)(struct mtdx_peb_alloc *bal, unsigned int zone);
	void         (*free)(struct mtdx_peb_alloc *bal);
};

static inline unsigned int mtdx_get_peb(struct mtdx_peb_alloc *bal,
					unsigned int zone, int *dirty)
{
	return bal->get_peb(bal, zone, dirty);
}

static inline void mtdx_put_peb(struct mtdx_peb_alloc *bal, unsigned int peb,
				int dirty)
{
	bal->put_peb(bal, peb, dirty);
}

static inline void mtdx_peb_alloc_reset(struct mtdx_peb_alloc *bal,
					unsigned int zone)
{
	if (bal->reset)
		bal->reset(bal, zone);
}

static inline void mtdx_peb_alloc_free(struct mtdx_peb_alloc *bal)
{
	if (bal)
		bal->free(bal);
}

struct mtdx_peb_alloc *mtdx_rand_peb_alloc(const struct mtdx_geo *geo);

#endif
