#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/blkdev.h>


static char *device = NULL;
static struct page *page = NULL;


module_param (device, charp, 0444);
MODULE_PARM_DESC (device, "Block device to test BIO completion for correctness.");

static void end_io_handler (struct bio *bio, int val);
static void perform_bio (struct block_device *dev, struct page *page);


int __init bio_md_init (void)
{
	int err = 0;
	struct block_device *bd_disk;

	if (!device) {
		printk (KERN_WARNING "md-bio: You must secify 'device' module parameter.\n");
		err = -EINVAL;
		goto err_dev;
	}

	/* Discover device */
	bd_disk = lookup_bdev (device);
	if (IS_ERR (bd_disk)) {
		err = PTR_ERR (bd_disk);
		printk (KERN_WARNING "md-bio: disk %s not found, error %d\n", device, err);
		goto err_dev;
	}

	printk (KERN_INFO "bd_disk = %p\n", bd_disk->bd_disk);
	printk (KERN_INFO "bd_dev  = %x\n", bd_disk->bd_dev);
	printk (KERN_INFO "bd_contains = %p\n", bd_disk->bd_contains);
	printk (KERN_INFO "bd_part = %p\n", bd_disk->bd_part);
	printk (KERN_INFO "bd_inode = %p\n", bd_disk->bd_inode);

	if (!bd_disk->bd_disk) {
		printk (KERN_WARNING "bd_disk field is empty!\n");
		err = -EINVAL;
		goto err_mem;
	}

	/* allocate page for IO */
	page = alloc_page (GFP_KERNEL | __GFP_ZERO);
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

	printk (KERN_INFO "before alloc\n");

	bio = bio_alloc (GFP_KERNEL, 1);
	if (!bio) {
		printk (KERN_WARNING "md-bio: bio_alloc failed\n");
		return;
	}

	bio->bi_sector = 0;
	bio->bi_size = PAGE_SIZE;
	bio->bi_bdev = dev;
	bio->bi_io_vec[0].bv_page = page;
	bio->bi_io_vec[0].bv_len = PAGE_SIZE;
	bio->bi_io_vec[0].bv_offset = 0;
	bio->bi_vcnt = 1;
	bio->bi_idx = 0;
	bio->bi_rw = READ;
	bio->bi_end_io = end_io_handler;

	printk (KERN_INFO "before make req\n");
	generic_make_request (bio);
}


static
void end_io_handler (struct bio *bio, int err)
{
	printk (KERN_INFO "md-bio: end_io_handler called. Err = %d, bi_size = %d\n", err, bio->bi_size);
	if (bio->bi_size)
		BUG ();
}



module_init (bio_md_init);
module_exit (bio_md_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Module for BIO MD bug catching");
