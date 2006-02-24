/* Header file for TI FlashMedia driver */
#ifndef _TIFMXX_H
#define _TIFMXX_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/mmc/host.h>

#define DRIVER_NAME "tifmxx"
#define DRIVER_VERSION "0.2"

#define CONFIG_MMC_DEBUG 1
#ifdef CONFIG_MMC_DEBUG
#define DBG(f, x...) \
        printk(KERN_DEBUG DRIVER_NAME " [%s()]: " f, __func__,## x)
#else
#define DBG(f, x...) do { } while (0)
#endif

/* Host registers (relative to pci base address): */
enum { 
	FM_SET_INTERRUPT_ENABLE   = 0x008, 
	FM_CLEAR_INTERRUPT_ENABLE = 0x00c, 
	FM_INTERRUPT_STATUS       = 0x014 };

/* Socket registers (relative to socket base address): */
enum {
	SOCK_CONTROL                   = 0x004, 
	SOCK_PRESENT_STATE             = 0x008, 
	SOCK_DMA_ADDRESS               = 0x00c, 
	SOCK_DMA_CONTROL               = 0x010, 
	SOCK_DMA_FIFO_INT_ENABLE_SET   = 0x014, 
	SOCK_DMA_FIFO_INT_ENABLE_CLEAR = 0x018, 
	SOCK_DMA_FIFO_STATUS           = 0x020, 
	SOCK_FIFO_CONTROL              = 0x024, 
	SOCK_FIFO_PAGE_SIZE            = 0x028,
	SOCK_MMCSD_COMMAND             = 0x104,
	SOCK_MMCSD_ARG_LOW             = 0x108,
	SOCK_MMCSD_ARG_HIGH            = 0x10c,
	SOCK_MMCSD_CONFIG              = 0x110, 
	SOCK_MMCSD_STATUS              = 0x114, 
	SOCK_MMCSD_INT_ENABLE          = 0x118,
	SOCK_MMCSD_COMMAND_TO          = 0x11c,
	SOCK_MMCSD_DATA_TO             = 0x120,
	SOCK_MMCSD_DATA                = 0x124,
	SOCK_MMCSD_BLOCK_LEN           = 0x128,
	SOCK_MMCSD_NUM_BLOCKS          = 0x12c,
	SOCK_MMCSD_BUFFER_CONFIG       = 0x130,
	SOCK_MMCSD_SDIO_MODE_CONFIG    = 0x138,
	SOCK_MMCSD_RESPONSE            = 0x144,
	SOCK_MMCSD_SYSTEM_CONTROL      = 0x168,
	SOCK_MMCSD_SYSTEM_STATUS       = 0x16c,
	SOCK_MS_COMMAND                = 0x184,
	SOCK_MS_DATA                   = 0x188,
	SOCK_MS_STATUS                 = 0x18c,
	SOCK_MS_SYSTEM                 = 0x190,
	SOCK_FIFO_ACCESS               = 0x200 };

/* Socket capabilities:
 *      MS_SOCKET    -> better Sony MS support on this socket
 *      XX12_SOCKET  -> xx12 devices have slightly different controls
 *      ALLOW_SD     -> use SD in addition to SDIO
 *      ALLOW_MMC    -> use MMC in addition to SDIO
 *      ALLOW_MSP    -> enable use of MS parallel mode on dedicated socket
 *      ALLOW_SM     -> enable SM use
 *      ALLOW_SMCIS  -> enable SM CIS check
 *      ALLOW_SMDLY  -> enable SM/xD insertion delay
 */

/* Volatile socket flags: 
 *      INT_B0       -> lower nibble card bit from interrupt status (mmio_base + 0x14), vara_0
 *      INT_B1       -> higher nibble card bit from interrupt status (mmio_base + 0x14), vara_1
 *      CARD_RO      -> card is read-only
 *      CARD_BUSY    -> vara_6 variable from reveng
 *      CARD_ACTIVE  -> vara_2 variable from reveng, set after card initialization success
 *      CARD_EJECTED -> vara_4 variable from reveng, set by finalizer, condition for event_1
 *      SOCK_EVENT   -> socket event (event_2)
 *      FLAG_A5      -> vara_5, probably marks unfinished io
 * These flags are probably cmmcsd specific:
 *      MMCSD_EVENT  -> card needs attention (to replace cmmcsd_event_1)
 *      MMCSD_READY  -> card ready to accept command (cmmcsd_var_14)
 *      R_INIT       -> cmmcsd_var_9, card was re-initialized after read error
 *      FLAG_V2      -> cmmcsd_var_2, bit 3 of socket register 0x114 was set and INT_B0 signalled
 */

struct tifm_driver {
	struct module        *owner;
	const char           *name;

	struct device_driver driver;
};

#endif
