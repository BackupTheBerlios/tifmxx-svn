#!/bin/bash
sync
insmod ./memstick.ko
insmod ./mspro_block.ko
insmod ./tifm_core.ko
insmod ./tifm_7xx1.ko
insmod ./tifm_ms.ko
#insmod ./tifm_sd.ko
