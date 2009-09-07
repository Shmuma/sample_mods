#include <linux/kernel.h>
#include <linux/module.h>


int __init bio_init (void)
{
	return 0;
}


void __exit bio_exit (void)
{
}


module_init (bio_init);
module_exit (bio_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Module for BIO MD bug catching");
