#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/in.h>
#include <linux/workqueue.h>

#include <net/sock.h>

#define PORT 12345


static struct socket *sock;


static int make_server_socket (void);

static void accept_work (struct work_struct *);
static int send_hello_msg (struct socket *sock);
static int recv_hello_msg (struct socket *sock);

static DECLARE_WORK (sock_accept, accept_work);



static int __init s_init (void)
{
	int ret;

	printk (KERN_INFO "Socket test module\n");

	ret = make_server_socket ();
	if (ret) {
		printk (KERN_INFO "Server socket creation failed: %d\n", ret);
		return ret;
	}

	return 0;
}


static void __exit s_exit (void)
{
	printk (KERN_INFO "Socket test module unload\n");
	if (sock)
		sock_release (sock);
}


static int make_server_socket (void)
{
	int ret;
	struct sockaddr_in sin;

	ret = sock_create (AF_INET, SOCK_STREAM, 0, &sock);
	if (ret)
		goto err;

	sin.sin_family = AF_INET;
	sin.sin_port = htons (PORT);
	sin.sin_addr.s_addr = 0;

	ret = kernel_bind (sock, (struct sockaddr*)&sin, sizeof (sin));
	if (ret)
		goto err;

	ret = kernel_listen (sock, 1024);
	if (ret)
		goto err;

	schedule_work (&sock_accept);

	return 0;
err:
	if (sock)
		sock_release (sock);
	return ret;
}


static void accept_work (struct work_struct *dummy)
{
	struct socket *c_sock = NULL;
	int ret;
	struct sockaddr_in sin;
	int len;

	ret = kernel_accept (sock, &c_sock, 0);

	if (ret) {
		printk (KERN_INFO "kernel_accept failed: %d\n", ret);
		goto out;
	}

	ret = kernel_getpeername (c_sock, (struct sockaddr*)&sin, &len);

	if (ret) {
		printk (KERN_INFO "getpeername failed: %d\n", ret);
		goto out;
	}

	printk (KERN_INFO "Accepted connection from 0x%x\n", sin.sin_addr.s_addr);

	ret = send_hello_msg (c_sock);
	if (ret) {
		printk (KERN_INFO "Message send failed: %d\n", ret);
		goto out;
	}

	ret = recv_hello_msg (c_sock);
	if (ret) {
		printk (KERN_INFO "Message recv failed: %d\n", ret);
		goto out;
	}

out:
	if (c_sock)
		sock_release (c_sock);

	return;
}


static int send_hello_msg (struct socket *sock)
{
	int ret = 0, len;
	struct msghdr hdr;
	struct iovec iov;
	char* msg = "Hello, World!\n";

	len = strlen (msg);

	iov.iov_base = msg;
	iov.iov_len = len;

	memset (&hdr, 0, sizeof (hdr));
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	while (iov.iov_len) {
		ret = sock_sendmsg (sock, &hdr, iov.iov_len);

		if (ret <= 0)
			break;

		iov.iov_base += ret;
		iov.iov_len -= ret;
		ret = iov.iov_len;
	}

	return ret;
}


static int recv_hello_msg (struct socket *sock)
{
	struct msghdr hdr;
	struct iovec iov;
	char buf[16+1];
	int ret;

	memset (&hdr, 0, sizeof (hdr));
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	iov.iov_base = &buf;
	iov.iov_len = sizeof (buf)-1;

	ret = sock_recvmsg (sock, &hdr, iov.iov_len, 0);

	if (ret > 0) {
		buf[ret] = 0;
		printk (KERN_INFO "Got: %s\n", buf);
	}

	return ret;
}



module_init (s_init);
module_exit (s_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION("Kernel socket test module");


/* 0xC0A86402 == 3232261122 */
