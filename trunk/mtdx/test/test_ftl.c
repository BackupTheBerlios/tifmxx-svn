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

struct btm_oob *blocks;

void *request_thread(void *data)
{
	pthread_mutex_lock(&req_lock);
	while (1) {
		while (!btm_req_dev)
			pthread_cond_wait(&next_req, &req_lock);

		
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
