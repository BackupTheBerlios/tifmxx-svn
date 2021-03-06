/*
 *  xD picture card ECC algorithm
 *
 *  Copyright (C) 2008 JMicron Technology Corporation <www.jmicron.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "linux/xd_card.h"

/*
 * Rather straightforward implementation of SmartMedia ECC algorithm.
 * Probably very slow.
 */

const static unsigned char xd_card_ecc_table[] = {
	0x00, 0x55, 0x56, 0x03, 0x59, 0x0C, 0x0F, 0x5A,
	0x5A, 0x0F, 0x0C, 0x59, 0x03, 0x56, 0x55, 0x00,
	0x65, 0x30, 0x33, 0x66, 0x3C, 0x69, 0x6A, 0x3F,
	0x3F, 0x6A, 0x69, 0x3C, 0x66, 0x33, 0x30, 0x65,
	0x66, 0x33, 0x30, 0x65, 0x3F, 0x6A, 0x69, 0x3C,
	0x3C, 0x69, 0x6A, 0x3F, 0x65, 0x30, 0x33, 0x66,
	0x03, 0x56, 0x55, 0x00, 0x5A, 0x0F, 0x0C, 0x59,
	0x59, 0x0C, 0x0F, 0x5A, 0x00, 0x55, 0x56, 0x03,
	0x69, 0x3C, 0x3F, 0x6A, 0x30, 0x65, 0x66, 0x33,
	0x33, 0x66, 0x65, 0x30, 0x6A, 0x3F, 0x3C, 0x69,
	0x0C, 0x59, 0x5A, 0x0F, 0x55, 0x00, 0x03, 0x56,
	0x56, 0x03, 0x00, 0x55, 0x0F, 0x5A, 0x59, 0x0C,
	0x0F, 0x5A, 0x59, 0x0C, 0x56, 0x03, 0x00, 0x55,
	0x55, 0x00, 0x03, 0x56, 0x0C, 0x59, 0x5A, 0x0F,
	0x6A, 0x3F, 0x3C, 0x69, 0x33, 0x66, 0x65, 0x30,
	0x30, 0x65, 0x66, 0x33, 0x69, 0x3C, 0x3F, 0x6A,
	0x6A, 0x3F, 0x3C, 0x69, 0x33, 0x66, 0x65, 0x30,
	0x30, 0x65, 0x66, 0x33, 0x69, 0x3C, 0x3F, 0x6A,
	0x0F, 0x5A, 0x59, 0x0C, 0x56, 0x03, 0x00, 0x55,
	0x55, 0x00, 0x03, 0x56, 0x0C, 0x59, 0x5A, 0x0F,
	0x0C, 0x59, 0x5A, 0x0F, 0x55, 0x00, 0x03, 0x56,
	0x56, 0x03, 0x00, 0x55, 0x0F, 0x5A, 0x59, 0x0C,
	0x69, 0x3C, 0x3F, 0x6A, 0x30, 0x65, 0x66, 0x33,
	0x33, 0x66, 0x65, 0x30, 0x6A, 0x3F, 0x3C, 0x69,
	0x03, 0x56, 0x55, 0x00, 0x5A, 0x0F, 0x0C, 0x59,
	0x59, 0x0C, 0x0F, 0x5A, 0x00, 0x55, 0x56, 0x03,
	0x66, 0x33, 0x30, 0x65, 0x3F, 0x6A, 0x69, 0x3C,
	0x3C, 0x69, 0x6A, 0x3F, 0x65, 0x30, 0x33, 0x66,
	0x65, 0x30, 0x33, 0x66, 0x3C, 0x69, 0x6A, 0x3F,
	0x3F, 0x6A, 0x69, 0x3C, 0x66, 0x33, 0x30, 0x65,
	0x00, 0x55, 0x56, 0x03, 0x59, 0x0C, 0x0F, 0x5A,
	0x5A, 0x0F, 0x0C, 0x59, 0x03, 0x56, 0x55, 0x00
};

#define XD_CARD_ECC_CORR 0x00555554UL

/**
 * xd_card_ecc_step - update the ecc state with some data
 * @state: ecc state (initial - 0)
 * @pos: ecc byte counter value
 * @data: pointer to some data
 * @count: length of data
 *
 * Returns 0 if more data needed or 1 if ecc can be computed
 */
int xd_card_ecc_step(unsigned int *state, unsigned int *pos,
		     unsigned char *data, unsigned int count)
{
	unsigned char c;

	while (*pos < 256) {
		c = xd_card_ecc_table[data[*pos]];
		*state ^= c & 0x3f;
		if (c & 0x40)
			*state ^= (((*pos) & 0xff) << 16)
				  | (((~(*pos)) & 0xff) << 8); 

		(*pos)++;
		count--;
		if (!count)
			break;
	}

	return (*pos) == 256;
}

/**
 * xd_card_ecc_value - turn ecc state into value
 * @state: current ecc state
 *
 * Returns ecc value in SmartMedia format
 */
unsigned int xd_card_ecc_value(unsigned int state)
{
	unsigned int a = 0x808000, b = 0x8000, cnt;
	unsigned int rv = 0;

	for (cnt = 0; cnt < 4; ++cnt) {
		if (state & a & 0xff0000)
			rv |= b;
		b >>= 1;
		if (state & a & 0xff00)
			rv |= b;
		b >>= 1;
		a >>= 1;
	}
	b = 0x80;
	for (cnt = 0; cnt < 4; ++cnt) {
		if (state & a & 0xff0000)
			rv |= b;
		b >>= 1;
		if (state & a & 0xff00)
			rv |= b;
		b >>= 1;
		a >>= 1;
	}

	rv = (~rv) & 0xffff;
	rv |= (((~(state & 0xff)) << 2) | 3) << 16;

	return rv;
}

/**
 * xd_card_fix_ecc - try to fix 1-bit error
 * @pos: error position in the original data block (returned)
 * @mask: value to xor into error position (returned)
 * @act_ecc: computed ecc of the original data block
 * @ref_ecc: assumed ecc of the original data block
 *
 * Returns 0 if no error, -1 if uncorrectable, +1 if correctable
 */
int xd_card_fix_ecc(unsigned int *pos, unsigned char *mask,
		    unsigned int act_ecc, unsigned int ref_ecc)
{
	unsigned char a = 0x80, b = 0x04, cnt;
	unsigned int b_pos = 0x00800000UL;

	if (act_ecc == ref_ecc)
		return 0;

	ref_ecc = ((ref_ecc & 0xffff) << 8) | ((ref_ecc >> 16) & 0xff);

	if (((ref_ecc ^ (ref_ecc >> 1)) & XD_CARD_ECC_CORR)
	    == XD_CARD_ECC_CORR) {
		*pos = 0;
		*mask = 0;

		for (cnt = 0; cnt < 8; ++cnt) {
			if (ref_ecc & b_pos)
				*pos |= a;
			b_pos >>= 2;
			a >>= 1;
		}

		for (cnt = 0; cnt < 3; ++cnt) {
			if (ref_ecc & b_pos)
				*mask |= b;
			b_pos >>= 2;
			b >>= 1;
		}

		*mask = 1 << *mask;
		return 1;
	}

	return -1;
}
