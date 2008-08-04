#include "../mtdx_common.h"
#include <linux/module.h>
#include <stdio.h>

struct mtdx_driver *test_driver;

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

}
