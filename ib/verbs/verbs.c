/* 0xC0A80101 == 3232235777 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/in.h>
#include <linux/igmp.h>
#include <linux/workqueue.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_cm.h>
#include <rdma/ib_sa.h>

#include <net/sock.h>


#define NEXTJIFF(secs)	(jiffies + (secs) * HZ)

#define CQ_SIZE		255
#define SEND_INTERVAL   1
#define PORT		12347


static void accept_work (struct work_struct *);


static DECLARE_WORK (sock_accept, accept_work);


/* module params */
static unsigned int server_addr;
module_param (server_addr, uint, 0644);
MODULE_PARM_DESC (server_addr, "server address. If not specified, wait for connection");


/* module state */
static struct ib_sa_client verbs_sa_client;
static int have_path;
static int have_remote_info;

static struct ib_sa_path_rec path;

static struct ib_device *ib_dev;
static struct ib_device_attr dev_attr;
static struct ib_port_attr port_attr;

static struct ib_pd *pd;
static struct ib_mr *mr;
static struct ib_cq *send_cq;
static struct ib_cq *recv_cq;
static struct ib_qp *qp;
u16 pkey;
static struct ib_ah *ah;


static size_t buf_size = 1024;
static char *send_buf;
static char *recv_buf;

static u64 send_key;
static u64 recv_key;


/* network socket (server-side) */
static struct socket *sock;


/* IB information required for data exchange in UD mode */
struct ib_side_info {
	union ib_gid	gid;
	u32		qp_num;
	u16		lid;
	u32		qkey;
};


static struct ib_side_info local_info, remote_info;


/* prototypes */
static int init_qp (struct ib_qp *qp);
static int path_rec_lookup_start (void);
static void verbs_path_rec_completion (int status, struct ib_sa_path_rec *resp, void *context);

static void timer_func (unsigned long);

static DEFINE_TIMER (verbs_timer, timer_func, 0, 0);

static int send_remote_info (struct socket *sock, struct ib_side_info *info);
static int recv_remote_info (struct socket *sock, struct ib_side_info *info);

static int exchange_info (unsigned int addr);

static void verbs_post_recv_req (void);


static void verbs_comp_handler_recv (struct ib_cq *cq, void *context)
{
	printk (KERN_INFO "verbs_comp_handler_recv called\n");
}


static void verbs_qp_event(struct ib_event *event, void *context)
{
	printk(KERN_ERR "QP event %d\n", event->event);
}


static void verbs_add_device (struct ib_device *dev)
{
	int ret;
	struct ib_qp_init_attr attrs;

	if (ib_dev)
		return;

	/* durty hack for ib_dma_map_single not to segfault */
	dev->dma_ops = NULL;
	ib_dev = dev;

	printk (KERN_INFO "IB add device called. Name = %s\n", dev->name);

	ret = ib_query_device (dev, &dev_attr);
	if (ret) {
		printk (KERN_INFO "ib_quer_device failed: %d\n", ret);
		return;
	}

	printk (KERN_INFO "IB device caps: max_qp %d, max_mcast_grp: %d, max_pkeys: %d\n",
		dev_attr.max_qp, dev_attr.max_mcast_grp, (int)dev_attr.max_pkeys);

	/* We'll work with first port. It's a sample module, anyway. Who is that moron which decided
	 * to count ports from one? */
	ret = ib_query_port (dev, 1, &port_attr);
	if (ret) {
		printk (KERN_INFO "ib_query_port failed: %d\n", ret);
		return;
	}

	printk (KERN_INFO "Port info: lid: %u, sm_lid: %u, max_msg_size: %u\n",
		(unsigned)port_attr.lid, (unsigned)port_attr.sm_lid, port_attr.max_msg_sz);

	pd = ib_alloc_pd (dev);
	if (IS_ERR (pd)) {
		ret = PTR_ERR (pd);
		printk (KERN_INFO "pd allocation failed: %d\n", ret);
		return;
	}

	printk (KERN_INFO "PD allocated\n");

	mr = ib_get_dma_mr (pd, IB_ACCESS_LOCAL_WRITE);
	if (IS_ERR (mr)) {
		ret = PTR_ERR (mr);
		printk (KERN_INFO "get_dma_mr failed: %d\n", ret);
		return;
	}

	send_cq = ib_create_cq (dev, NULL, NULL, NULL, 1, 1);
	if (IS_ERR (send_cq)) {
		ret = PTR_ERR (send_cq);
		printk (KERN_INFO "ib_create_cq failed: %d\n", ret);
		return;
	}

	recv_cq = ib_create_cq (dev, verbs_comp_handler_recv, NULL, NULL, 1, 1);
	if (IS_ERR (recv_cq)) {
		ret = PTR_ERR (recv_cq);
		printk (KERN_INFO "ib_create_cq failed: %d\n", ret);
		return;
	}

	ib_req_notify_cq (recv_cq, IB_CQ_NEXT_COMP);
	printk (KERN_INFO "CQs allocated\n");

	ib_query_pkey (dev, 1, 0, &pkey);

	/* allocate memory */
	send_buf = kmalloc (buf_size + 40, GFP_KERNEL);
	recv_buf = kmalloc (buf_size + 40, GFP_KERNEL);

	if (!send_buf || !recv_buf) {
		printk (KERN_INFO "Memory allocation error\n");
		return;
	}

	printk (KERN_INFO "Trying to register regions\n");
	if (ib_dev->dma_ops)
		printk (KERN_INFO "DMA ops are defined\n");

	memset (send_buf, 0, buf_size+40);
	memset (send_buf, 0, buf_size+40);

	send_key = ib_dma_map_single (ib_dev, send_buf, buf_size, DMA_FROM_DEVICE);
	printk (KERN_INFO "send_key obtained %llx\n", send_key);
	recv_key = ib_dma_map_single (ib_dev, recv_buf, buf_size, DMA_TO_DEVICE);
	printk (KERN_INFO "recv_key obtained %llx\n", recv_key);

	if (ib_dma_mapping_error (ib_dev, send_key)) {
		printk (KERN_INFO "Error mapping send buffer\n");
		return;
	}

	if (ib_dma_mapping_error (ib_dev, recv_key)) {
		printk (KERN_INFO "Error mapping recv buffer\n");
		return;
	}

	memset (&attrs, 0, sizeof (attrs));
	attrs.qp_type = IB_QPT_UD;
	attrs.sq_sig_type = IB_SIGNAL_ALL_WR;
	attrs.event_handler = verbs_qp_event;
	attrs.cap.max_send_wr = CQ_SIZE;
	attrs.cap.max_recv_wr = CQ_SIZE;
	attrs.cap.max_send_sge = 1;
	attrs.cap.max_recv_sge = 1;
	attrs.send_cq = send_cq;
	attrs.recv_cq = recv_cq;

	qp = ib_create_qp (pd, &attrs);
	if (IS_ERR (qp)) {
		ret = PTR_ERR (qp);
		printk (KERN_INFO "qp allocation failed: %d\n", ret);
		return;
	}

	printk (KERN_INFO "Create QP with num %x\n", qp->qp_num);

	if (init_qp (qp)) {
		printk (KERN_INFO "Failed to initialize QP\n");
		return;
	}

	ret = ib_query_gid (ib_dev, 1, 0, &local_info.gid);
	if (ret) {
		printk (KERN_INFO "query_gid failed %d\n", ret);
		return;
	}

	local_info.qp_num = qp->qp_num;
	local_info.lid = port_attr.lid;

	/* now we are ready to send our QP number and other stuff to other party */
	if (!server_addr) {
		schedule_work (&sock_accept);
		flush_scheduled_work ();
	}
	else
		exchange_info (server_addr);

	if (!have_remote_info) {
		printk (KERN_INFO "Have no remote info, give up\n");
		return;
	}

	ret = path_rec_lookup_start ();
	if (ret) {
		printk (KERN_INFO "path_rec lookup start failed: %d\n", ret);
		return;
	}

	/* post receive request */
	verbs_post_recv_req ();

	mod_timer (&verbs_timer, NEXTJIFF(1));
}


static void verbs_remove_device (struct ib_device *dev)
{
	printk (KERN_INFO "IB remove device called. Name = %s\n", dev->name);

	if (ah)
		ib_destroy_ah (ah);
	if (qp)
		ib_destroy_qp (qp);
	if (send_cq)
		ib_destroy_cq (send_cq);
	if (recv_cq)
		ib_destroy_cq (recv_cq);
	if (mr)
		ib_dereg_mr (mr);
	if (pd)
		ib_dealloc_pd (pd);
}


static int init_qp (struct ib_qp *qp)
{
	struct ib_qp_attr qp_attr;
	int ret, attr_mask;
	struct ib_qp_init_attr init_attr;

	memset (&qp_attr, 0, sizeof (qp_attr));

	qp_attr.qp_state = IB_QPS_INIT;
	qp_attr.pkey_index = 0;
	qp_attr.port_num = 1;
	qp_attr.qkey = 0;
	attr_mask = IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_PORT | IB_QP_QKEY;
	ret = ib_modify_qp (qp, &qp_attr, attr_mask);
	if (ret) {
		printk (KERN_INFO "failed to modify QP to init, ret = %d\n", ret);
		return 1;
	}

	qp_attr.qp_state = IB_QPS_RTR;
	/* Can't set this in a INIT->RTR transition */
	attr_mask &= ~IB_QP_PORT;
	ret = ib_modify_qp(qp, &qp_attr, attr_mask);
	if (ret) {
		printk (KERN_INFO "failed to modify QP to RTR, ret = %d\n", ret);
		return 1;
	}

	qp_attr.qp_state = IB_QPS_RTS;
	qp_attr.sq_psn = 0;
	attr_mask |= IB_QP_SQ_PSN;
	attr_mask &= ~IB_QP_PKEY_INDEX;
	ret = ib_modify_qp(qp, &qp_attr, attr_mask);
	if (ret) {
		printk (KERN_INFO "failed to modify QP to RTS, ret = %d\n", ret);
		return 1;
	}

	ret = ib_query_qp (qp, &qp_attr, IB_QP_QKEY, &init_attr);
	if (ret) {
		printk (KERN_INFO "failed to query QP: %d\n", ret);
		return 1;
	}

	local_info.qkey = qp_attr.qkey;

	return 0;
}


static void timer_func (unsigned long dummy)
{
	struct ib_send_wr wr, *bad_wr;
	struct ib_sge sge;
	int ret;
	struct ib_wc wc;
	static int id = 1;

	if (!have_path)
		return;

	if (!have_remote_info)
		return;

	printk (KERN_INFO "verbs_timer: sending datagram to LID = %u, qpn = %x\n", remote_info.lid, remote_info.qp_num);

	memset (&wr, 0, sizeof (wr));
	wr.wr_id = id++;
	wr.wr.ud.ah = ah;
	wr.wr.ud.port_num = 1;
	wr.wr.ud.remote_qkey = remote_info.qkey;
	wr.wr.ud.remote_qpn = remote_info.qp_num;
	wr.opcode = IB_WR_SEND;
	wr.sg_list = &sge;
	wr.send_flags = 0;
	wr.num_sge = 1;

	/* sge */
	sge.addr = send_key;
	sge.length = buf_size;
	sge.lkey = mr->lkey;

	ret = ib_post_send (qp, &wr, &bad_wr);

	if (ret)
		printk (KERN_INFO "post_send failed: %d\n", ret);
	else
		printk (KERN_INFO "post_send succeeded\n");

	ret = ib_req_notify_cq (recv_cq, IB_CQ_NEXT_COMP);
	printk (KERN_INFO "notify_cq return %d for recv_cq\n", ret);

/* 	ret = ib_req_notify_cq (send_cq, IB_CQ_NEXT_COMP); */
/* 	printk (KERN_INFO "notify_cq return %d for send_cq\n", ret); */

	ret = ib_poll_cq (recv_cq, 1, &wc);
	printk (KERN_INFO "poll_cq returned %d for recv_cq\n", ret);

	if (ret) {
		printk (KERN_INFO "ID: %llu, status: %d, opcode: %d, len: %u\n",
			wc.wr_id, (int)wc.status, (int)wc.opcode, wc.byte_len);
		verbs_post_recv_req ();
	}

	ret = ib_poll_cq (send_cq, 1, &wc);
	printk (KERN_INFO "poll_cq returned %d for send_cq\n", ret);

	mod_timer (&verbs_timer, NEXTJIFF(SEND_INTERVAL));
}


static int path_rec_lookup_start (void)
{
	int qid;
	struct ib_sa_path_rec rec;
	struct ib_sa_query *sa_query;

	rec.dlid = cpu_to_be16 (remote_info.lid);
	rec.slid = cpu_to_be16 (port_attr.lid);
	rec.pkey = cpu_to_be16 (pkey);
	rec.numb_path = 1;

	qid = ib_sa_path_rec_get (&verbs_sa_client, ib_dev, 1, &rec, IB_SA_PATH_REC_DLID | IB_SA_PATH_REC_SLID | IB_SA_PATH_REC_PKEY | IB_SA_PATH_REC_NUMB_PATH,
				  10000, GFP_KERNEL, verbs_path_rec_completion, NULL, &sa_query);

	if (qid < 0) {
		printk (KERN_INFO "path_rec_get failed: %d\n", qid);
		sa_query = NULL;
	}

	return 0;
}


static void verbs_path_rec_completion (int status, struct ib_sa_path_rec *resp, void *context)
{
	struct ib_ah_attr av;
	int ret;

	printk (KERN_INFO "path_rec_completion called. Status = %d\n", status);
	if (!status) {
		if (!ib_init_ah_from_path (ib_dev, 1, resp, &av)) {
			printk (KERN_INFO "ah: flags = %d, dlid = %d, port = %d\n", (int)av.ah_flags, (int)av.dlid, (int)av.port_num);
			ah = ib_create_ah (pd, &av);
			if (IS_ERR (ah)) {
				ret = PTR_ERR (ah);
				printk (KERN_INFO "ib_create_ah failed: %d\n", ret);
				return;
			}
			path = *resp;
			have_path = 1;
		}
	}
}


static void accept_work (struct work_struct *dummy)
{
	struct socket *c_sock = NULL;
	int ret, len;
	struct sockaddr_in sin;

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

	ret = send_remote_info (c_sock, &local_info);
	if (ret)
		goto out;

	ret = recv_remote_info (c_sock, &remote_info);
	if (ret)
		goto out;

	have_remote_info = 1;
	printk (KERN_INFO "Got information about remote side.\n");
	printk (KERN_INFO "QPN: 0x%x, QKey: %u, LID: 0x%x, GID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		remote_info.qp_num, remote_info.qkey, (int)remote_info.lid,
		remote_info.gid.raw[0], remote_info.gid.raw[1], remote_info.gid.raw[2], remote_info.gid.raw[3],
		remote_info.gid.raw[4], remote_info.gid.raw[5], remote_info.gid.raw[6], remote_info.gid.raw[7],
		remote_info.gid.raw[8], remote_info.gid.raw[9], remote_info.gid.raw[10], remote_info.gid.raw[10],
		remote_info.gid.raw[12], remote_info.gid.raw[13], remote_info.gid.raw[14], remote_info.gid.raw[15]);
out:
	if (c_sock)
		sock_release (c_sock);
	if (sock)
		sock_release (sock);

	c_sock = sock = NULL;
}


static int send_remote_info (struct socket *sock, struct ib_side_info *info)
{
	struct msghdr hdr;
	struct kvec iov;
	int ret;

	printk (KERN_INFO "send_remote_info\n");
	iov.iov_base = info;
	iov.iov_len = sizeof (*info);

	memset (&hdr, 0, sizeof (hdr));

	while (iov.iov_len) {
		ret = kernel_sendmsg (sock, &hdr, &iov, 1, iov.iov_len);
		if (ret < 0) {
			printk (KERN_INFO "sock_sendmsg error: %d\n", ret);
			return ret;
		}
		if (!ret)
			break;
		iov.iov_base += ret;
		iov.iov_len -= ret;
	}

	return 0;
}


static int recv_remote_info (struct socket *sock, struct ib_side_info *info)
{
	struct msghdr hdr;
	struct kvec iov;
	int ret;

	printk (KERN_INFO "recv_remote_info\n");

	/* receive remote info */
	memset (&hdr, 0, sizeof (hdr));

	iov.iov_base = info;
	iov.iov_len = sizeof (*info);

	while (iov.iov_len) {
		ret = kernel_recvmsg (sock, &hdr, &iov, 1, iov.iov_len, 0);
		if (ret < 0) {
			printk (KERN_INFO "sock_recvmsg failed: %d\n", ret);
			return ret;
		}
		if (!ret)
			break;
		iov.iov_base += ret;
		iov.iov_len -= ret;
	}

	return 0;
}


static int exchange_info (unsigned int addr)
{
	struct socket *sock = NULL;
	struct sockaddr_in sin;
	int ret;

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

	ret = recv_remote_info (sock, &remote_info);
	if (ret)
		goto err;
	ret = send_remote_info (sock, &local_info);
	if (ret)
		goto err;

	have_remote_info = 1;
	printk (KERN_INFO "Got information about remote side.\n");
	printk (KERN_INFO "QPN: 0x%x, QKey: %u, LID: 0x%x, GID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		remote_info.qp_num, remote_info.qkey, remote_info.lid,
		remote_info.gid.raw[0], remote_info.gid.raw[1], remote_info.gid.raw[2], remote_info.gid.raw[3],
		remote_info.gid.raw[4], remote_info.gid.raw[5], remote_info.gid.raw[6], remote_info.gid.raw[7],
		remote_info.gid.raw[8], remote_info.gid.raw[9], remote_info.gid.raw[10], remote_info.gid.raw[10],
		remote_info.gid.raw[12], remote_info.gid.raw[13], remote_info.gid.raw[14], remote_info.gid.raw[15]);

err:
	if (sock)
		sock_release (sock);
	return ret;
}


static void verbs_post_recv_req (void)
{
	struct ib_recv_wr wr, *bad_wr;
	struct ib_sge sge;
	int ret;
	static int id = 1;

	memset (&wr, 0, sizeof (wr));
	wr.wr_id = id++;
	wr.num_sge = 1;
	wr.sg_list = &sge;

	sge.addr = recv_key;
	sge.length = buf_size + 40;
	sge.lkey = mr->lkey;

	ret = ib_post_recv (qp, &wr, &bad_wr);

	if (ret)
		printk (KERN_INFO "post_recv failed: %d\n", ret);
}




static struct ib_client client = {
	.name = "verbs_test",
	.add  = verbs_add_device,
	.remove = verbs_remove_device,
};


static int make_server_socket (void)
{
	int res;
	struct sockaddr_in sin;

	res = sock_create (AF_INET, SOCK_STREAM, 0, &sock);

	if (res < 0)
		goto err;

	sin.sin_family = AF_INET;
	sin.sin_port = htons (PORT);
	sin.sin_addr.s_addr = 0;

	res = kernel_bind (sock, (struct sockaddr*)&sin, sizeof (sin));

	if (res)
		goto err;

	res = kernel_listen (sock, 256);

	if (res)
		goto err;

	return 0;

err:
	if (sock)
		sock_release (sock);
	return res;
}


static int __init verbs_init (void)
{
	int res = 0;

	printk (KERN_INFO "Verbs test module\n");

	if (!server_addr)
		res = make_server_socket ();

	if (res) {
		printk (KERN_INFO "Socket creation failed: %d\n", res);
		return -EINVAL;
	}

	/* register Subnet Administrator client` */
	ib_sa_register_client(&verbs_sa_client);

	if (ib_register_client (&client)) {
		printk (KERN_WARNING "IB client registration failed. Is IB modules loaded?\n");
		return -ENODEV;
	}

	return 0;
}


static void __exit verbs_exit (void)
{
	if (sock)
		sock_release (sock);
	del_timer (&verbs_timer);
	ib_unregister_client (&client);
	ib_sa_unregister_client(&verbs_sa_client);
}


module_init (verbs_init);
module_exit (verbs_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Lapan <max.lapan@gmail.com>");
MODULE_DESCRIPTION("Verbs test module");
