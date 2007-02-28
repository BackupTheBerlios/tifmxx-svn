#!/bin/bash
sync
#insmod ./mmc_core.ko
#insmod ./mmc_block.ko
#insmod ./memstick.ko
insmod ./tifm_core.ko
insmod ./tifm_7xx1.ko
#insmod ./tifm_ms.ko
insmod ./tifm_sd.ko no_dma=1
