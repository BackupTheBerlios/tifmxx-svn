#ifndef _XD_CARD_H
#define _XD_CARD_H

#include "mtdx_common.h"

struct xd_card_id1 {
	unsigned char maker_code;
	unsigned char device_code;
	unsigned char option_code1;
	unsigned char option_code2;
} __attribute__((packed));

struct xd_card_id2 {
	unsigned char characteristics_code;
#define XD_CARD_CELL_TYPE_MASK   0xc0
#define XD_CARD_MULTI_BLOCK_MASK 0x30

	unsigned char vendor_code1;
	unsigned char size_code;
	unsigned char vendor_code2;
	unsigned char vendor_code3;
} __attribute__((packed));

struct xd_card_id3 {
	unsigned char vendor_code1;
	unsigned char vendor_code2;
	unsigned char id_code;
	unsigned char vendor_code3;
} __attribute__((packed));

struct xd_card_extra {
	unsigned int   reserved;
	unsigned char  data_status;
	unsigned char  block_status;
	unsigned short addr1;
	unsigned char  ecc_hi[3];
	unsigned short addr2; /* this should be identical to addr1 */
	unsigned char  ecc_lo[3];
} __attribute__((packed));

struct xd_card_idi {
	unsigned char vendor_code1[6];
	unsigned char serial_number[20];
	unsigned char model_number[40];
	unsigned char vendor_code2[62];
} __attribute__((packed));

enum xd_card_cmd {
	XD_CARD_CMD_INPUT       = 0x80,
	XD_CARD_CMD_READ1       = 0x00,
	XD_CARD_CMD_READ2       = 0x01,
	XD_CARD_CMD_READ3       = 0x50,
	XD_CARD_CMD_RESET       = 0xff,
	XD_CARD_CMD_PAGE_PROG   = 0x10,
	XD_CARD_CMD_DUMMY_PROG  = 0x11,
	XD_CARD_CMD_MULTI_PROG  = 0x15,
	XD_CARD_CMD_ERASE_SET   = 0x60,
	XD_CARD_CMD_ERASE_START = 0xd0,
	XD_CARD_CMD_STATUS1     = 0x70,
	XD_CARD_CMD_STATUS2     = 0x71,
	XD_CARD_CMD_ID1         = 0x90,
	XD_CARD_CMD_ID2         = 0x91,
	XD_CARD_CMD_ID3         = 0x9a
};

static inline int xd_card_is_id_cmd(enum xd_card_cmd cmd)
{
	return (cmd == XD_CARD_CMD_ID1) || (cmd == XD_CARD_CMD_ID2)
	       || (cmd == XD_CARD_CMD_ID1);
}

static inline int xd_card_is_data_cmd(enum xd_card_cmd cmd)
{
	return (cmd == XD_CARD_CMD_INPUT) || (cmd == XD_CARD_CMD_READ1);
}

struct xd_card_req {
	enum xd_card_cmd cmd;
	unsigned char    status;
#define XD_CARD_STTS_RW      0x80
#define XD_CARD_STTS_READY   0x40
#define XD_CARD_STTS_D3_FAIL 0x10
#define XD_CARD_STTS_D2_FAIL 0x08
#define XD_CARD_STTS_D1_FAIL 0x04
#define XD_CARD_STTS_D0_FAIL 0x02
#define XD_CARD_STTS_FAIL    0x01

	union {
		unsigned long long address;
		char               id_reg[8];
	};

	struct mtdx_oob *req_oob;
	struct scatterlist sg;
};

struct xd_host {
	struct device      *dev;
	struct mutex       lock;
	struct work_struct media_checker;
	unsigned int       caps;
#define XD_CARD_CAP_AUTO_ECC   1
#define XD_CARD_CAP_AUTO_CMD   2
#define XD_CARD_CAP_SPLIT_PAGE 4

	struct mtdx_dev    *card;
	char               host_data[];
};

void xd_host_detect_media(struct xd_host *host);
void xd_host_eject_media(struct xd_host *host);
int xd_host_suspend(struct xd_host *host);
void xd_host_resume(struct xd_host *host);

/* struct xd_host will be set as host driver's device drvdata. Host driver
 * can use host_data field to store the rest of it's private bits.
 */
struct xd_host *xd_card_alloc_host(struct device *dev, unsigned int extra);
void xd_host_free(struct xd_host *host);


static inline void *xd_host_private(struct xd_host *host)
{
	return (void *)host->host_data;
}

static inline struct xd_host *xd_host_from_dev(struct device *dev)
{
	return dev_get_drvdata(dev);
}

#endif
