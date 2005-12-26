/* Header file for TI FlashMedia driver */
#ifndef _TIFMXX_H
#define _TIFMXX_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>

#define DRIVER_NAME "tifmxx"
#define DRIVER_VERSION "0.1"
#define MAX_SUPPORTED_SOCKETS 4

#define FM_DEBUG(x...) printk(KERN_DEBUG DRIVER_NAME ": " x)

/* Socket flags: 
 *      INT_B0       -> lower nibble card bit from interrupt status (mmio_base + 0x14), vara_0
 *      INT_B1       -> higher nibble card bit from interrupt status (mmio_base + 0x14), vara_1
 *      CARD_PRESENT -> assume that card is present
 *      CARD_RO      -> card is read-only
 *      CARD_BUSY    -> vara_6 variable from reveng
 *      CARD_READY   -> vara_2 variable from reveng, set after card initialization success
 *      CARD_REMOVED -> vara_4 variable from reveng, set by detection routine on card removal, condition for event_1
 *      CARD_EVENT   -> card needs attention (to replace cmmcsd_event_1)
 * -- Flags above 0x8000 are carried over on card insertion/removal
 *      MS_SOCKET    -> better Sony MS support on this socket
 *      XX12_SOCKET  -> xx12 devices have slightly different controls
 *      ALLOW_SD     -> use SD in addition to SDIO
 *      ALLOW_MMC    -> use MMC in addition to SDIO
 *      ALLOW_MSP    -> enable use of MS parallel mode on dedicated socket
 *      ALLOW_SM     -> enable SM use
 *      ALLOW_SMCIS  -> enable SM CIS check
 *      ALLOW_SMDLY  -> enable SM/xD insertion delay
 */

enum { INT_B0 = 0x1, INT_B1 = 0x2, CARD_PRESENT = 0x4, CARD_RO = 0x8, CARD_BUSY = 0x40, CARD_READY = 0x80,
       CARD_REMOVED = 0x100, CARD_EVENT = 0x200, MS_SOCKET = 0x10000, XX12_SOCKET = 0x20000, ALLOW_SD = 0x40000,
       ALLOW_MMC = 0x80000, ALLOW_MSP = 0x100000, ALLOW_SM = 0x200000, ALLOW_SMCIS = 0x400000, 
       ALLOW_SMDLY = 0x800000 };

struct tifmxx_data;

struct tifmxx_sm_data
{
};

struct tifmxx_ms_data
{
};

struct tifmxx_mmcsd_data
{
	unsigned int       r_var_2;
	unsigned int       r_var_5;
	unsigned int       r_var_10;
	unsigned int       r_var_11;

	unsigned int       clk_speed;
};

struct tifmxx_sock_data
{
	spinlock_t               lock;
	struct Scsi_Host         *host;       // pointer to parent host
	char __iomem             *sock_addr;  // pointer to socket registers

	struct scsi_cmnd         *srb;        // current command

	unsigned int             sock_id;     //socket number, same as scsi target

	struct work_struct       do_scsi_cmd;

	unsigned int             flags;
	unsigned int             media_id;
	
	wait_queue_head_t        irq_ack; // reveng: event_2 signalled from interrupt, event_1 signalled on
					  // shutdown/remove, most functions wait on both

	unsigned int             r_var_2;
	
	
	union
	{
		struct tifmxx_sm_data    *sm_p;
		struct tifmxx_ms_data    *ms_p;
		struct tifmxx_mmcsd_data *mmcsd_p;
	};

	void                     (*clean_up)(struct tifmxx_sock_data *sock);
	void                     (*process_irq)(struct tifmxx_sock_data *sock);
	void                     (*signal_irq)(struct tifmxx_sock_data *sock, unsigned int card_irq_status);
};

struct tifmxx_data
{
	rwlock_t                 lock;
	struct pci_dev           *dev;	
	char __iomem             *mmio_base;

	unsigned int             irq_status;
	unsigned int             max_sockets;
	struct tifmxx_sock_data  *sockets[MAX_SUPPORTED_SOCKETS];
	
	struct work_struct       isr_bh;
};

void tifmxx_mmcsd_init(struct tifmxx_sock_data *sock);
void tifmxx_eval_scsi(void *data);

/* Additional Sense Code (ASC) used */
#define NO_ADDED_SENSE        0x00
#define UNRECOVERED_READ_ERR  0x11
#define INVALID_OPCODE        0x20
#define ADDR_OUT_OF_RANGE     0x21
#define INVALID_FIELD_IN_CDB  0x24
#define POWERON_RESET         0x29
#define SAVING_PARAMS_UNSUP   0x39
#define THRESHHOLD_EXCEEDED   0x5d


#endif
