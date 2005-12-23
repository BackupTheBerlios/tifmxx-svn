#ifndef _TIFMXX_H
#define _TIFMXX_H

#include <linux/spinlock.h>
#include <linux/workqueue.h>
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
 * 	CARD_PRESENT -> assume that card is present
 *      CARD_RO      -> card is read-only
 *	MS_SOCKET    -> better Sony MS support on this socket
 *      XX12_SOCKET  -> xx12 devices have slightly different controls
 */
enum { CARD_PRESENT = 0x1, CARD_RO = 0x2, MS_SOCKET = 0x4, XX12_SOCKET = 0x8 };

/* Host flags:
 */
enum { CLOSING = 0x80000000 };

/* Socket states:
 */
enum { P_IDLE = 0, P_POWER = 1 };

/* There is some waiting associated with some socket operations 
 * (up to 100 msec in worst case). So it's better to have independent
 * work items for each socket.
 */
struct tifmxx_data;

struct tifmxx_sock_data
{
	rwlock_t                 lock;
	struct tifmxx_data       *host;  // pointer to parent host
	char __iomem             *sock_addr;  // pointer to socket registers

	struct scsi_cmnd         *srb;        // current command

	unsigned int             sock_status; // current socket status
	struct work_struct       do_scsi_cmd;

	unsigned int             flags;
	unsigned int             media_id;

	unsigned int             p_state;

	void                     (*finalize)(struct tifmxx_sock_data *sock);
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
	unsigned int             flags;
};


#endif
