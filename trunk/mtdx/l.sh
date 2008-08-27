#!/bin/bash
sync
insmod ../driver/memstick.ko
insmod ./mtdx_core.ko
insmod ./long_map.ko
insmod ./rand_peb_alloc.ko
insmod ./sg_bounce.ko
insmod ./ftl_simple.ko
insmod ./mtdx_block.ko
insmod ./ms_block.ko
#insmod ./ms_block.ko
insmod ../driver/jmb38x_ms.ko
#insmod ./flash_bd.ko
#insmod ./xd_card.ko
#insmod ./jmb38x_xd.ko
#insmod ./tifm_core.ko
#insmod ./tifm_7xx1.ko
#insmod ./tifm_ms.ko
#insmod ./tifm_sd.ko
