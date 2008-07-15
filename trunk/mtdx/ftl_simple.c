/*
 *  MTDX simple FTL
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "mtdx_common.h"
#include <linux/module.h>

#define DRIVER_NAME "ftl_simple"

static int mtdx_ftl_simple_probe(struct mtdx_dev *mdev)
{
}

static void mtdx_ftl_simple_remove(struct mtdx_dev *mdev)
{
}

#ifdef CONFIG_PM

static int mtdx_ftl_simple_suspend(struct mtdx_dev *mdev, pm_message_t state)
{
}

static int mtdx_ftl_simple_resume(struct mtdx_dev *mdev)
{
}

#else

#define mtdx_ftl_simple_suspend NULL
#define mtdx_ftl_simple_resume NULL

#endif /* CONFIG_PM */

static struct mtdx_device_id mtdx_ftl_simple_id_tbl[] = {
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB_INC, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PEB, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{}
};

static struct mtdx_driver mtdx_ftl_simple_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = mtdx_ftl_simple_id_tbl,
	.probe    = mtdx_ftl_simple_probe,
	.remove   = mtdx_ftl_simple_remove,
	.suspend  = mtdx_ftl_simple_suspend,
	.resume   = mtdx_ftl_simple_resume
};

static int __init mtdx_ftl_simple_init(void)
{
	return mtdx_register_driver(&mtdx_ftl_simple_driver);
}

static void __exit mtdx_ftl_simple_exit(void)
{
	mtdx_unregister_driver(&mtdx_ftl_simple_driver);
}

module_init(mtdx_ftl_simple_init);
module_exit(mtdx_ftl_simple_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("MTDX simple FTL");
MODULE_DEVICE_TABLE(mtdx, mtdx_ftl_simple_id_tbl);
