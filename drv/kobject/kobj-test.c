#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kobject.h>

static int obj_counter = 0;
static struct test_object *obj = NULL;


struct test_object {
	struct kobject kobj;
	int value;
};


static ssize_t test_attr_show (struct kobject *kobj,
				 struct attribute *attr,
				 char *buf)
{
	return 0;
}


static ssize_t test_attr_store (struct kobject *kobj,
				struct attribute *attr,
				const char *buf, size_t len)
{
	return 0;
}



static struct sysfs_ops test_sysfs_ops = {
	.show = test_attr_show,
	.store = test_attr_store,
};


static struct kobj_type test_ktype = {
	.sysfs_ops = &test_sysfs_ops,
};


static int __init kobj_test_init (void)
{
	obj = kmalloc (sizeof (*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->value = 0x123;
	kobject_init (&obj->kobj, &test_ktype);
	kobject_set_name (&obj->kobj, "test_object%d", obj_counter++);

	return 0;
}


static void __exit kobj_test_exit (void)
{
	if (!obj)
		return;
		
	kobject_del (&obj->kobj);
	kfree (obj);
}


module_init (kobj_test_init);
module_exit (kobj_test_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION ("Sample module for kobject model");
