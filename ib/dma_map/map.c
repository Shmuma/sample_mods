#include <linux/kernel.h>
#include <linux/module.h>

#include <rdma/ib_verbs.h>


static void map_add_device (struct ib_device *dev)
{
	char *buf;
	u64 key;
	size_t len = 2048;
	struct ib_pd *pd;
	struct ib_mr *mr;
 
	printk (KERN_INFO "IB add device called. Name = %s\n", dev->name);
	printk (KERN_INFO "dev = %p, dma_ops = %p, map_single = %p\n", dev, dev->dma_ops, dev->dma_ops->map_single);

	return;

	pd = ib_alloc_pd (dev);
	mr = ib_get_dma_mr (pd, IB_ACCESS_LOCAL_WRITE);

	buf = kzalloc (len, GFP_KERNEL);

	if (!buf) {
		printk (KERN_INFO "Memory allocation failed");
		return;
	}

	printk (KERN_INFO "Memory allocated\n");

	key = ib_dma_map_single (dev, buf, len, DMA_TO_DEVICE);

	printk (KERN_INFO "Map done\n", key);
}


static void map_remove_device (struct ib_device *dev)
{
	printk (KERN_INFO "IB remove device called. Name = %s\n", dev->name);
}




static struct ib_client client = {
	.name = "verbs_test",
	.add  = map_add_device,
	.remove = map_remove_device,
};


static int __init map_init (void)
{
	if (ib_register_client (&client)) {
		printk (KERN_WARNING "IB client registration failed. Is IB modules loaded?\n");
		return -ENODEV;
	}
	return 0;
}


static void __exit map_exit (void)
{
}


module_init (map_init);
module_exit (map_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION("Verbs DMA mapping");

