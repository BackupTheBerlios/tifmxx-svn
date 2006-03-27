/* Header file for TI FlashMedia driver */
#ifndef _TIFMXX_H
#define _TIFMXX_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/pci.h>

#define CONFIG_TIFM_DEBUG 1
#ifdef CONFIG_TIFM_DEBUG
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

typedef enum {FM_NULL = 0, FM_SM = 0x01, FM_MS = 0x02, FM_SD = 0x03} tifm_device_id;

struct tifm_driver;
struct tifm_dev {
	char __iomem    *addr;
	spinlock_t      lock;
	tifm_device_id  media_id;

	void            (*eject)(struct tifm_dev *sock);
	void            (*signal_irq)(struct tifm_dev *sock, unsigned int sock_irq_status);

	struct tifm_driver *drv;
	struct device      dev;
};

struct tifm_driver {
	tifm_device_id       *id_table;	
	int                  (*probe)(struct tifm_dev *dev);
	void                 (*remove)(struct tifm_dev *dev);
	struct device_driver driver;
};

struct tifm_adapter {
	char __iomem        *addr;
	unsigned int        irq_status;
	spinlock_t          lock;
	unsigned int        id;
	unsigned int        max_sockets;
	struct tifm_dev     *sockets[4];
	struct work_struct  isr_bh;
	struct class_device cdev;
	struct device       *dev;
};

struct tifm_sd {
	struct tifm_dev     *dev;

	wait_queue_head_t   event;
	unsigned int        flags;
	unsigned int        status;
	unsigned int        fifo_status;

};

struct tifm_adapter* tifm_alloc_adapter(void);
void tifm_free_adapter(struct tifm_adapter *fm);
int tifm_add_adapter(struct tifm_adapter *fm);
void tifm_remove_adapter(struct tifm_adapter *fm);
struct tifm_dev* tifm_alloc_device(struct tifm_adapter *fm);
int tifm_register_driver(struct tifm_driver *drv);
void tifm_unregister_driver(struct tifm_driver *drv);

static inline void* tifm_get_drvdata(struct tifm_dev *dev)
{
        return dev_get_drvdata(&dev->dev);
}

static inline void tifm_set_drvdata(struct tifm_dev *dev, void *data)
{
	dev_set_drvdata(&dev->dev, data);
}

struct tifm_device_id {
	__u32 media_id;
};

#endif
