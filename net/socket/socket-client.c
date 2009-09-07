#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/in.h>
#include <linux/workqueue.h>

#include <net/sock.h>

#define PORT 12345

static int do_connect (void);


static unsigned int server_addr;
module_param (server_addr, uint, 0644);
MODULE_PARM_DESC (server_addr, "server address. If specified, start both client and server.");



static int __init sc_init (void)
{
	int ret;

	printk (KERN_INFO "Socket test module: client side\n");
	printk (KERN_INFO "server_addr = %x\n", server_addr);

	ret = do_connect ();
	if (ret) {
		printk (KERN_INFO "do_connect failed: %d\n", ret);
		return ret;
	}

	return 0;
}


static void __exit sc_exit (void)
{
	printk (KERN_INFO "Socket client test module unload\n");
}


static int do_connect (void)
{
	struct socket *sock;
	struct sockaddr_in sin;
	int ret;

	printk (KERN_INFO "connect_work thread started\n");

	ret = sock_create (AF_INET, SOCK_STREAM, 0, &sock);
	if (ret) {
		printk (KERN_INFO "Sock create failed: %d\n", ret);
		goto err;
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons (PORT);
	sin.sin_addr.s_addr = htonl (server_addr);

	printk (KERN_INFO "Trying to connect to 0x%x\n", server_addr);

	ret = kernel_connect (sock, (struct sockaddr*)&sin, sizeof (sin), 0);
	if (ret) {
		printk (KERN_INFO "Connect failed: %d\n", ret);
		goto err;
	}

	printk (KERN_INFO "Client thread connected to 0x%x\n", server_addr);
	return 0;
err:
	if (sock)
		sock_release (sock);
	return ret;
}


module_init (sc_init);
module_exit (sc_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION("Kernel socket test module, client side");
