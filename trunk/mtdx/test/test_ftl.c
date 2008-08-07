#include "../mtdx_common.h"
#include <linux/module.h>
#include <stdio.h>

struct mtdx_driver *test_driver;

pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t next_req = PTHREAD_COND_INITIALIZER;
struct mtdx_dev *btm_req_dev = NULL;

struct btm_oob {
	unsigned int log_block;
	enum mtdx_page_status status;
};

struct mtdx_dev_geo btm_geo = {
	.zone_size_log = 8,
	.log_block_cnt = 2000,
	.phy_block_cnt = 2048,
	.page_cnt = 4,
	.page_size = 512,
	.oob_size = sizeof(struct btm_oob),
	.fill_value = 0xff
};

char *flat_space;
char *trans_space;

struct btm_oob *pages;

int btm_trans_data(struct mtdx_request *req, int dir)
{
	struct scatterlist sg;
	unsigned int c_pos = req->phy_block
			     * (btm_geo.page_cnt * btm_geo.page_size);
	unsigned int t_len, r_len = req->length;
	int rc;

	c_pos += req->offset;

	while (!(rc = btm_req_dev->get_data_buf_sg(req, &sg))) {
		t_len = min(sg.length, r_len);

		if (dir)
			memcpy(trans_space + c_pos, sg_virt(&sg), t_len);
		else
			memcpy(sg_virt(&sg), trans_space + c_pos, t_len);

		r_len -= t_len;
		c_pos += t_len;
	}
	return rc;
}

int btm_trans_oob(struct mtdx_request *req, int dir)
{
	unsigned int c_pos = req->phy_block * btm_geo.page_cnt;
	unsigned int cnt = req->length / btm_geo.page_size;
	char *oob_buf;

	c_pos += req->offset / btm_geo.page_size;

	for (; cnt; --cnt) {
		oob_buf = btm_req_dev->get_oob_buf(req);
		if (!oob_buf)
			return -ENOMEM;

		if (dir)
			memcpy(&pages[c_pos], oob_buf,
			       sizeof(struct btm_oob));
		else
			memcpy(oob_buf, &pages[c_pos],
			       sizeof(struct btm_oob));
		c_pos++;
	}
	return 0;
}

void *request_thread(void *data)
{
	struct mtdx_request *req;
	unsigned int cnt, src_off, src_blk;
	int rc;

	pthread_mutex_lock(&req_lock);
	while (1) {
		while (!btm_req_dev)
			pthread_cond_wait(&next_req, &req_lock);

		while ((req = btm_req_dev->get_request(btm_req_dev))) {
			printf("req cmd %x, block %x, %x:%x\n", req->cmd,
			       req->phy_block, req->offset, req->length);
			if ((req->offset % btm_geo.page_size)
			    || (req->length % btm_geo.page_size))
				printf("unaligned offset/length!\n");
			if ((req->offset
			     > (btm_geo.page_cnt * btm_geo.page_size))
			    || ((req->offset + req->length)
				> (btm_geo.page_cnt * btm_geo.page_size)))
				printf("invalid offset/length!\n");

			switch (req->cmd) {
			case MTDX_CMD_READ:
				rc = btm_trans_data(req, 0);
				if (!rc)
					rc = btm_trans_oob(req, 0);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_READ_DATA:
				rc = btm_trans_data(req, 0);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_READ_OOB:
				rc = btm_trans_oob(req, 0);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_ERASE:
				memset(trans_space
				       + req->phy_block * btm_geo.page_size,
				       btm_geo.fill_value,
				       btm_geo.page_cnt * btm_geo.page_size);
				memset(&pages[req->phy_block
					      * btm_geo.page_size],
				       btm_geo.fill_value,
				       btm_geo.page_cnt
				       * sizeof(struct btm_oob));

				btm_req_dev->end_request(req, 0, 0);
				break;
			case MTDX_CMD_WRITE:
				rc = btm_trans_data(req, 1);
				if (!rc)
					rc = btm_trans_oob(req, 1);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_WRITE_DATA:
				rc = btm_trans_data(req, 1);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_WRITE_OOB:
				rc = btm_trans_oob(req, 1);

				btm_req_dev->end_request(req, rc, req->length);
				break;
			case MTDX_CMD_SELECT:
				printf("select block %x, stat %x\n",
				       req->phy_block,
				       pages[req->phy_block * btm_geo.page_cnt]
					    .status);
				pages[req->phy_block * btm_geo.page_cnt].status
					= MTDX_PAGE_SMAPPED;
				btm_req_dev->end_request(req, 0, 0);
				break;
			case MTDX_CMD_INVALIDATE:
				printf("invalidate block %x, stat %x\n",
				       req->phy_block,
				       pages[req->phy_block * btm_geo.page_cnt]
					    .status);
				pages[req->phy_block * btm_geo.page_cnt].status
					= MTDX_PAGE_INVALID;
				btm_req_dev->end_request(req, 0, 0);
				break;
			case MTDX_CMD_COPY:
				rc = btm_req_dev->get_copy_source(req, &src_blk,
								  &src_off);
				if (rc)
					btm_req_dev->end_request(req, rc, 0);

				memcpy(trans_space
				       + req->phy_block * btm_geo.page_size +
				       req->offset,
				       trans_space + src_blk * btm_geo.page_size
				       + src_off,
				       req->length);
				btm_req_dev->end_request(req, 0, req->length);
				break;
			default:
				btm_req_dev->end_request(req, -EINVAL, 0);
			}
		}
		btm_req_dev = NULL;
	}
	pthread_mutex_unlock(&req_lock);
}

int btm_new_req(struct mtdx_dev *this_dev, struct mtdx_dev *req_dev)
{
	pthread_mutex_lock(&req_lock);
	if (req_dev) {
		printf("crap\n");
		pthread_mutex_unlock(&req_lock);
		return -EBUSY;
	}
	btm_req_dev = req_dev;
	pthread_cond_signal(&next_req);
	pthread_mutex_unlock(&req_lock);
}

int btm_oob_to_info(struct mtdx_dev *this_dev, struct mtdx_page_info *p_info,
		    void *oob)
{
	struct btm_oob *b_oob = oob;

	p_info->log_block = b_oob->log_block;
	p_info->status = b_oob->status;
	return 0;
}

int btm_info_to_oob(struct mtdx_dev *this_dev, void *oob,
		    struct mtdx_page_info *p_info)
{
	struct btm_oob *b_oob = oob;

	b_oob->log_block = p_info->log_block;
	b_oob->status = p_info->status;
	return 0;
}

static int btm_get_param(struct mtdx_dev *this_dev,
			 enum mtdx_param param, void *val)
{
	switch (param) {
	case MTDX_PARAM_GEO: {
		memcpy(val, &btm_geo, sizeof(btm_geo));
		return 0;
	}
	case MTDX_PARAM_SPECIAL_BLOCKS: {
		return 0;
	}
	case MTDX_PARAM_READ_ONLY: {
		int *rv = val;
		*rv = 0;
		return 0;
	}
	case MTDX_PARAM_DEV_SUFFIX: {
		char *rv = val;
		sprintf(rv, "%d", this_dev->ord);
		return 0;
	}
	case MTDX_PARAM_DMA_MASK: {
		return 0;
	}
	default:
		return -EINVAL;
	}
}

struct mtdx_dev btm_dev = {
	.id = {
		MTDX_WMODE_PAGE_PEB_INC, MTDX_WMODE_NONE, MTDX_RMODE_PAGE_PEB,
		MTDX_RMODE_NONE, MTDX_TYPE_MEDIA, MTDX_ID_MEDIA_MEMORYSTICK
	},
	.new_request = btm_new_req,
	.oob_to_info = btm_oob_to_info,
	.info_to_oob = btm_info_to_oob,
	.get_param = btm_get_param
};

struct mtdx_dev ftl_dev = {
	.id = {
		MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB_INC, MTDX_RMODE_PAGE,
		MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE
	},
	.dev = {
		.parent = &btm_dev.dev
	}
};

struct mtdx_dev top_dev = {
	.get_request = top_get_request,
	.end_request = top_end_request,
	.get_data_buf_sg = top_get_data_buf_sg,
	.dev = {
		.parent = &ftl_dev.dev
	}
};

int mtdx_register_driver(struct mtdx_driver *drv)
{
	test_driver = drv;
	printf("Loading module\n");
	return 0;
}

void mtdx_unregister_driver(struct mtdx_driver *drv)
{
}

unsigned int random32(void)
{
	unsigned int rv = random();

	RAND_bytes(&rv, 4);
	return rv;
}

int device_register(struct device *dev)
{
	return 0;
}

int main(int argc, char **argv)
{
	init_module();
	test_driver->probe(&ftl_dev);

}
