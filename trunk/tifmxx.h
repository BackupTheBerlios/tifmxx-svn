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
 *      CARD_ACTIVE  -> vara_2 variable from reveng, set after card initialization success
 *      CARD_REMOVED -> vara_4 variable from reveng, set by detection routine on card removal, condition for event_1
 *      SOCK_EVENT   -> socket event (event_2)
 *      CARD_EVENT   -> card needs attention (to replace cmmcsd_event_1)
 *      CARD_READY   -> card ready to accept command (cmmcsd_var_14)
 *      FLAG_A5      -> vara_5, probably marks unfinished io
 *      R_INIT       -> cmmcsd_var_9, card was re-inited after read error
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

enum { INT_B0 = 0x1, INT_B1 = 0x2, CARD_PRESENT = 0x4, CARD_RO = 0x8, CARD_BUSY = 0x10, CARD_ACTIVE = 0x20,
       CARD_REMOVED = 0x40, SOCK_EVENT = 0x80, CARD_EVENT = 0x100, CARD_READY = 0x200, FLAG_A5 = 0x400,
       R_INIT = 0x800, MS_SOCKET = 0x10000, XX12_SOCKET = 0x20000, ALLOW_SD = 0x40000, ALLOW_MMC = 0x80000, 
       ALLOW_MSP = 0x100000, ALLOW_SM = 0x200000, ALLOW_SMCIS = 0x400000, ALLOW_SMDLY = 0x800000 };

struct tifmxx_data;

struct tifmxx_sm_data
{
};

struct tifmxx_ms_data
{
};

/* Command flags:
 * CMD_DIR -> write = 0 / read = 1 
 * CMD_APP -> normal command = 0 / SD escaped command = 1
 */

enum { CMD_DIR = 0x1, CMD_APP = 0x2, CMD_RESP = 0x4, CMD_BLKM = 0x8 };

struct tifmxx_mmcsd_ecmd
{
	unsigned int cmd_index;
	unsigned int cmd_arg;
	unsigned int dtx_length;
	unsigned int resp_type;
	unsigned int flags; 
};

struct tifmxx_mmcsd_data
{
	unsigned int             r_var_2;
	unsigned int             r_var_10;
	unsigned int             r_var_11;
	unsigned int             cmd_status;

	unsigned int             rca;

	unsigned char            mid;
	unsigned short           oid;
	char                     pnm[7];
	unsigned char            rev;
	unsigned int             psn;	
	
	unsigned int             clk_speed; /* clocks per bit */
	unsigned int             read_block_len;
	unsigned int             log_read_block_len;
	unsigned int             write_block_len;
	unsigned int             log_write_block_len;
	unsigned int             read_time_out;
	unsigned int             write_time_out;

	size_t                   size;
	size_t                   blocks;

	struct tifmxx_mmcsd_ecmd *active_cmd;
	unsigned int             dma_pages_processed;
	unsigned int             dma_pages_total;
};

struct tifmxx_sock_data
{
	spinlock_t               lock;
	struct Scsi_Host         *host;       // pointer to parent host
	char __iomem             *sock_addr;  // pointer to socket registers

	struct scsi_cmnd         *srb;        // current command

	unsigned int             sock_id;     // socket number, same as scsi target

	struct work_struct       do_scsi_cmd;

	unsigned int             flags;
	unsigned int             media_id;
	
	wait_queue_head_t        irq_ack; // reveng: event_2 signalled from interrupt, event_1 signalled on
					  // shutdown/remove, most functions wait on both

	unsigned int             clk_freq;
	unsigned int             sock_status; // r_var_2

	int                      lba_start; // r_var_5 
	int                      total_sector_count; // r_var_3
	int                      res_sector_count; // r_var_10
	int                      cur_sector; // r_var_4
	
	union
	{
		struct tifmxx_sm_data    *sm_p;
		struct tifmxx_ms_data    *ms_p;
		struct tifmxx_mmcsd_data *mmcsd_p;
	};

	void                     (*clean_up)(struct tifmxx_sock_data *sock);
	void                     (*process_irq)(struct tifmxx_sock_data *sock);
	void                     (*signal_irq)(struct tifmxx_sock_data *sock, unsigned int card_irq_status);
	int                      (*init_card)(struct tifmxx_sock_data *sock);
	int                      (*close_write)(struct tifmxx_sock_data *sock);
	int                      (*read_sect)(struct tifmxx_sock_data *sock, int lba_start, int* sector_count,
					      int* dma_page_count, int resume);
	int                      (*write_sect)(struct tifmxx_sock_data *sock, int lba_start, int* sector_count,
					       int* dma_page_count, int resume);
};

struct tifmxx_data
{
	struct pci_dev           *dev;	
	char __iomem             *mmio_base;

	unsigned int             irq_status;
	unsigned int             max_sockets;
	struct tifmxx_sock_data  sockets[MAX_SUPPORTED_SOCKETS];
	
	struct work_struct       isr_bh;
};

unsigned int tifmxx_get_media_id(struct tifmxx_sock_data *sock);
void tifmxx_set_media_id(struct tifmxx_sock_data *sock, unsigned int media_id);
void tifmxx_set_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask);
void tifmxx_clear_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask);
int tifmxx_test_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask);

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
