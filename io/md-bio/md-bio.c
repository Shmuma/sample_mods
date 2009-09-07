#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/blkdev.h>


static char *device = NULL;
static struct page *page = NULL;


module_param (device, charp, 0444);
MODULE_PARM_DESC (device, "Block device to test BIO completion for correctness. WARNING! Module will perform write to first sector of this device!");

static void end_io_handler (struct bio *bio, int val);
static void perform_bio (struct block_device *dev, struct page *page);


int __init bio_md_init (void)
{
	int err = 0;
	dev_t dev;
	struct block_device *bd_disk;

	if (!device) {
		printk (KERN_WARNING "md-bio: You must secify 'device' module parameter. Be carefull, I'll write to this device!\n");
		err = -EINVAL;
		goto err_dev;
	}

	/* Discover device */
	dev = blk_lookup_devt (device, 0);
	if (!dev) {
		printk (KERN_WARNING "md-bio: device %s not found\n", device);
		err = -ENODEV;
		goto err_dev;
	}

	bd_disk = bdget (dev);
	if (!bd_disk) {
		printk (KERN_WARNING "md-bio: disk %s is not a block device\n", device);
		err = -EINVAL;
		goto err_dev;
	}

	/* allocate page for IO */
	page = alloc_page (GFP_KERNEL);
	if (!page) {
		printk (KERN_WARNING "md-bio: cannot allocate page for IO\n");
		err = -ENOMEM;
		goto err_mem;
	}

	/* now we can issue IO request */
	perform_bio (bd_disk, page);

err_mem:
	bdput (bd_disk);
err_dev:
	return err;
}


void __exit bio_md_exit (void)
{
	if (page)
		__free_pages (page, 0);
}



static
void perform_bio (struct block_device *dev, struct page *page)
{
	struct bio *bio;
	struct bio_vec vec;

	bio = bio_alloc (GFP_KERNEL, 1);
	if (!bio) {
		printk (KERN_WARNING "md-bio: bio_alloc failed\n");
		return;
	}

	vec.bv_page = page;
	vec.bv_len = PAGE_SIZE;
	vec.bv_offset = 0;

	bio->bi_sector = 0;
	bio->bi_size = PAGE_SIZE;
	bio->bi_bdev = dev;
	bio->bi_io_vec = &vec;
	bio->bi_vcnt = 1;
	bio->bi_rw = WRITE;
	bio->bi_end_io = end_io_handler;

	generic_make_request (bio);
}


static
void end_io_handler (struct bio *bio, int val)
{
	printk (KERN_INFO "md-bio: end_io_handler called\n");
}



module_init (bio_md_init);
module_exit (bio_md_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Module for BIO MD bug catching");
