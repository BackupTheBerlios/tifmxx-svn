#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/blkdev.h>


static struct block_device_operations ti_bd_op = {
	.owner = THIS_MODULE,
	.open = tifm_open,
	.close = tifm_close,
	.ioctl = tifm_ioctl,
	.revalidate_disk = tifm_revalidate,
	.media_changed = tifm_media_changed
};

static const pci_device_id tiarray_pci_device_id[] = {
	{0x104c, 0xac8f, -1, -1, 0, 0, 0},
	{0x104c, 0x8033, -1, -1, 0, 0, 0},
	{}
};
 

static struct pci_driver ti7x20_pci_driver = {
	.name =     "tifm",
	.id_table = tiarray_pci_device_id,,
	.probe =    tifm_drv_init,
	.remove =   tifm_drv_exit,
	.suspend =  ti7x20_suspend,
	.resume =   ti7x20_resume
};

static int __init
ti7x20_init_module()
{
	return pci_register_driver(ti7x20_pci_driver);
}

static void __exit
ti7x20_cleanup_module()
{
	pci_unregister_driver(ti7x20_pci_driver);
}

module_param(major_num, int, 0);

MODULE_DESCRIPTION("TIFM INFO | TI PCI7x20/7x21 Flash Media Driver 1.5");
MODULE_VERSION("1.5");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, tiarray_pci_device_id);
MODULE_AUTHOR("r/e by Alex Dubov");

module_init(ti7x20_init_module);
module_exit(ti7x20_cleanup_module);