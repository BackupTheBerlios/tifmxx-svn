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
 *	MS_SOCKET    -> better Sony MS support on this socket
 */
enum { CARD_PRESENT = 0x1, MS_SOCKET = 0x2 };

/* There is some waiting associated with some socket operations 
 * (up to 100 msec in worst case). So it's better to have independent
 * tasklets for each socket.
 */
struct tifmxx_sock_data
{
	spinlock_t               lock;
	char __iomem             *sock_addr;  // pointer to socket registers

	struct scsi_cmnd         *srb;        // current command

	unsigned                 sock_status; // current socket status
	struct work_struct       work_q;
	unsigned                 flags;
};

struct tifmxx_data
{
	spinlock_t               lock;
	struct pci_dev           *dev;	
	char __iomem             *mmio_base;

	unsigned                 max_sockets;
	struct tifmxx_sock_data  *sockets[MAX_SUPPORTED_SOCKETS];
};


#endif
