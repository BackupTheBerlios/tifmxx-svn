/*
 *  MTDX core data functionality
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "mtdx_data.h"

void mtdx_oob_init(struct mtdx_oob *m_oob, char *o_buf,
		   unsigned int mode, unsigned int inc)
{
	if (mode <= MTDX_OOB_MODE_TWO) {
		m_oob->dptr = o_buf;
		m_oob->dptr |= mode;
	} else {
		m_oob->dptr = NULL;
		return;
	}
	m_oob->pos = 0;
	m_oob->inc = inc;
}

char *mtdx_oob_get_entry(struct mtdx_oob *m_oob, unsigned int idx)
{
	char *data = m_oob->dptr & ~3UL;
	unsigned int mode = m_oob->dptr & 3UL;

	if (!data)
		return NULL;

	switch (mode) {
	case MTDX_OOB_MODE_ALL:
		m_oob->pos = idx * m_oob->inc;
		return &data[m_oob->pos];
	case MTDX_OOB_MODE_ONE:
		return data;
	case MTDX_OOB_MODE_TWO:
		if (!m_oob->pos) {
			m_oob->pos = m_oob->inc;
			return data;
		} else {
			return &data[m_oob->pos];
		}
	default:
		return NULL;
	}
}

char *mtdx_oob_get_next(struct mtdx_oob *m_oob)
{
	char *data = m_oob->dptr & ~3UL;
	unsigned int mode = m_oob->dptr & 3UL;

	if (!data)
		return NULL;

	switch (mode) {
	case MTDX_OOB_MODE_ALL:
		data = &data[m_oob->pos];
		m_oob->pos += m_oob->inc;
		return data;
	case MTDX_OOB_MODE_ONE:
		return data;
	case MTDX_OOB_MODE_TWO:
		if (!m_oob->pos) {
			m_oob->pos = m_oob->inc;
			return data;
		} else {
			return &data[m_oob->pos];
		}
	default:
		return NULL;
	}
}
