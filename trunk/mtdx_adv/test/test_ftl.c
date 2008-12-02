#include "../mtdx_common.h"
#include <pthread.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <stdio.h>
#include <stdlib.h>

struct mtdx_driver *test_driver;

pthread_mutex_t req_lock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
wait_queue_head_t next_req_wq;
struct mtdx_dev *btm_req_dev = NULL;
struct mtdx_dev_queue btm_dev_queue;

struct btm_oob {
	unsigned int log_block;
	enum mtdx_page_status status;
};

struct mtdx_geo btm_geo = {
	.zone_cnt = 2,
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

void btm_trans_data(struct mtdx_request *req, int dir)
{
	struct scatterlist sg;
	unsigned int c_pos = req->phy.b_addr
			     * (btm_geo.page_cnt * btm_geo.page_size);
	unsigned int t_len, r_len = req->length;
	struct bio_vec b_vec;
	int rc;

	c_pos += req->phy.offset;

	while (r_len) {
		mtdx_data_iter_get_bvec(req->req_data, &b_vec, req->length);
		if (dir)
			memcpy(trans_space + c_pos,
			       b_vec.bv_page + b_vec.bv_offset,
			       b_vec.bv_len);
		else
			memcpy(b_vec.bv_page + b_vec.bv_offset,
			       trans_space + c_pos,
			       b_vec.bv_len);

		r_len -= b_vec.bv_len;
		c_pos += b_vec.bv_len;
	}

	printf("trans data end");
}

int btm_trans_oob(struct mtdx_request *req, int dir)
{
	unsigned int c_pos = req->phy.b_addr * btm_geo.page_cnt;
	unsigned int cnt = req->length / btm_geo.page_size;
	char *oob_buf;

	c_pos += req->phy.offset / btm_geo.page_size;

	for (; cnt; --cnt) {
		oob_buf = mtdx_oob_get_next(req->req_oob);
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

void btm_complete_req(struct mtdx_request *req, int error, unsigned int count)
{
	pthread_mutex_unlock(&req_lock);
	btm_req_dev->end_request(btm_req_dev, req, count, error, 0);
	pthread_mutex_lock(&req_lock);
}

void *request_thread(void *data)
{
	struct mtdx_request *req;
	unsigned int cnt, src_off, src_blk;
	unsigned int block_size = btm_geo.page_cnt * btm_geo.page_size;
	int rc;

	printf("rt: thread created\n");
	while (1) {
		wait_event_interruptible(next_req_wq, btm_req_dev != NULL);
		pthread_mutex_lock(&req_lock);
		if (!btm_req_dev) {
			pthread_mutex_unlock(&req_lock);
			continue;
		}

		printf("rt: got request dev\n");
		while ((req = btm_req_dev->get_request(btm_req_dev))) {
			printf("rt: req cmd %x, block %x, %x:%x\n", req->cmd,
			       req->phy.b_addr, req->phy.offset, req->length);
			if ((req->phy.offset % btm_geo.page_size)
			    || (req->length % btm_geo.page_size)) {
				printf("rt: unaligned offset/length!\n");
				exit(1);
			}
			if ((req->phy.offset
			     > (btm_geo.page_cnt * btm_geo.page_size))
			    || ((req->phy.offset + req->length)
				> (btm_geo.page_cnt * btm_geo.page_size))) {
				printf("rt: invalid offset/length!\n");
				exit(2);
			}

			switch (req->cmd) {
			case MTDX_CMD_READ:
				if (req->req_data)
					btm_trans_data(req, 0);

				if (req->req_oob)
					btm_trans_oob(req, 0);

				btm_complete_req(req, rc, req->length);
				break;
			case MTDX_CMD_ERASE:
				printf("rt: erase %x\n", req->phy.b_addr);
				memset(trans_space
				       + req->phy.b_addr * block_size,
				       btm_geo.fill_value, block_size);
				for (cnt = req->phy.b_addr * btm_geo.page_cnt;
				     cnt < ((req->phy.b_addr + 1)
					    * btm_geo.page_cnt);
				     ++cnt) {
					pages[cnt].status = MTDX_PAGE_ERASED;
					pages[cnt].log_block = MTDX_INVALID_BLOCK;
				}

				btm_complete_req(req, 0, 0);
				break;
			case MTDX_CMD_WRITE:
				if (req->req_data)
					btm_trans_data(req, 1);
				printf("rt: write data\n");
				if (req->req_oob)
					btm_trans_oob(req, 1);

				printf("rt: write oob\n");
				btm_complete_req(req, rc, req->length);
				break;
			case MTDX_CMD_COPY:
				src_blk = req->copy.b_addr;
				src_off = req->copy.offset;

				memcpy(trans_space
				       + req->phy.b_addr * block_size +
				       req->phy.offset,
				       trans_space + src_blk * block_size
				       + src_off,
				       req->length);
				btm_complete_req(req, 0, req->length);
				break;
			default:
				btm_complete_req(req, -EINVAL, 0);
			}
		}
		btm_req_dev = NULL;
		printf("rt: x1\n");
		pthread_mutex_unlock(&req_lock);
	}
	printf("rt: x2\n");
}

void btm_new_req(struct mtdx_dev *this_dev, struct mtdx_dev *req_dev)
{
	get_device(&req_dev->dev);
	mtdx_dev_queue_push_back(&btm_dev_queue, req_dev);
	printf("request set\n");
	wake_up(&next_req_wq);
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
	.dev = {
		.bus_id = "btm"
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
		.bus_id = "ftl",
		.parent = &btm_dev.dev
	}
};

char *top_data_buf;
unsigned int top_pos;
unsigned int top_size;
pthread_mutex_t top_lock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
wait_queue_head_t top_cond_wq;
unsigned int top_req_done = 0;

static int top_get_data_buf_sg(struct mtdx_request *req,
			       struct scatterlist *sg)
{
	pthread_mutex_lock(&top_lock);
	if (top_pos >= top_size) {
		pthread_mutex_unlock(&top_lock);
		return -EAGAIN;
	}

	sg->page = top_data_buf;
	sg->offset = top_pos;
	sg->length = top_size - top_pos;

	top_pos += sg->length;
	pthread_mutex_unlock(&top_lock);
	return 0;
}

static void top_end_request(struct mtdx_dev *this_dev, struct mtdx_request *req,
			    unsigned int count, int dst_error,
			    int src_error)
{
	pthread_mutex_lock(&top_lock);
	printf("top end request, count %x, error %d\n", count, dst_error);

	if (top_req_done != 1) {
		printf("bad top_end_request %d\n", top_req_done);
		pthread_mutex_unlock(&top_lock);
		exit(1);
	}
	top_req_done = 2;
	pthread_mutex_unlock(&top_lock);
	wake_up(&top_cond_wq);
}

static struct mtdx_request *top_get_request(struct mtdx_dev *mdev);

struct mtdx_dev top_dev = {
	.get_request = top_get_request,
	.end_request = top_end_request,
	.dev = {
		.parent = &ftl_dev.dev
	}
};

struct mtdx_request top_req = {
//	.src_dev = &top_dev,
//	.phy_block = MTDX_INVALID_BLOCK
};

static struct mtdx_request *top_get_request(struct mtdx_dev *mdev)
{
	struct mtdx_request *rv;

	pthread_mutex_lock(&top_lock);
	printf("top get request\n");

	if (!top_req_done) {
		rv = &top_req;
		top_req_done++;
	} else
		rv = NULL;

	printf("top get request %p\n", rv);
	pthread_mutex_unlock(&top_lock);
	return rv;
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
	unsigned int t_cnt, err_cnt = 0;
	unsigned int off, size;
	unsigned int media_size = btm_geo.phy_block_cnt * btm_geo.page_cnt
				  * btm_geo.page_size;
	char *data_w, *data_r;
	pthread_t req_t;
	int rc;

	init_waitqueue_head(&next_req_wq);
	init_waitqueue_head(&top_cond_wq);
	mtdx_dev_queue_init(&btm_dev_queue);

	rc = pthread_create(&req_t, NULL, request_thread, NULL);
	if (rc)
		return rc;

	trans_space = malloc(media_size);
	flat_space = malloc(media_size);
	memset(trans_space, btm_geo.fill_value, media_size);
	memset(flat_space, btm_geo.fill_value, media_size);
	pages = calloc(btm_geo.phy_block_cnt * btm_geo.page_cnt,
		       sizeof(struct btm_oob));

	for (rc = 0; rc < (btm_geo.phy_block_cnt * btm_geo.page_cnt); ++rc)
		pages[rc].status = MTDX_PAGE_UNMAPPED;

	rc = 0;
	init_module();
	test_driver->probe(&ftl_dev);

	for (t_cnt = 7; t_cnt; --t_cnt) {
		do {
			off = random32() % (btm_geo.log_block_cnt
					    * btm_geo.page_cnt);
			size = random32() % (btm_geo.log_block_cnt
					     * btm_geo.page_cnt - off);
		} while (!size);
		//off = 0;
		//size = 1;

		top_size = size * btm_geo.page_size;

		data_w = malloc(top_size);
		RAND_bytes(data_w, top_size);
		data_r = calloc(1, top_size);

		top_req.logical = off / btm_geo.page_cnt;
		top_req.phy.offset = (off % btm_geo.page_cnt)
				     * btm_geo.page_size;
		top_req.length = top_size;

		top_pos = 0;
		top_req.cmd = MTDX_CMD_WRITE;
		top_data_buf = data_w;
		top_req_done = 0;

		printf("Writing %x sectors at %x\n", size, off);

		ftl_dev.new_request(&ftl_dev, &top_dev);
		printf("top write issue\n");

		wait_event_interruptible(top_cond_wq, top_req_done >= 2);

		printf("Write signalled %d\n", top_req_done);
		top_pos = 0;
		top_req.cmd = MTDX_CMD_READ;
		top_data_buf = data_r;
		top_req_done = 0;

		memcpy(flat_space + (off * btm_geo.page_size), data_w,
		       top_size);

		printf("Reading %x sectors at %x\n", size, off);
		ftl_dev.new_request(&ftl_dev, &top_dev);
		printf("top read issue\n");

		wait_event_interruptible(top_cond_wq, top_req_done >= 2);
		printf("Read signalled %d\n", top_req_done);
/*
		printf("data_w %04x, %04x, %04x\n", *(int*)data_w,
		       *(int*)(data_w + 512), *(int*)(data_w + 1024));
		printf("data_r %04x, %04x, %04x\n", *(int*)data_r,
		       *(int*)(data_r + 512), *(int*)(data_r + 1024));
*/
		if (memcmp(data_w, data_r, top_size)) {
			printf("read/write err - %d\n", t_cnt);
			break;
		} else
			printf("req OK!\n");

clean_up:
		free(data_w);
		free(data_r);
	}

	printf("no more requests\n");
	fflush(NULL);
	test_driver->remove(&ftl_dev);

	printf("c1\n");
	pthread_cancel(req_t);
	printf("c2\n");
	kfree(flat_space);
	kfree(trans_space);
	kfree(pages);

struct btm_oob *pages;
	cleanup_module();
}
