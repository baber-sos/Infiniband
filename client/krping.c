// Client

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include </usr/src/ofa_kernel/default/include/rdma/ib_verbs.h>
#include </usr/src/ofa_kernel/default/include/rdma/rdma_cm.h>

#include "getopt.h"

#define PFX "krping: "

static int debug = 0;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("Steve Wise");
MODULE_DESCRIPTION("RDMA ping server");
MODULE_LICENSE("Dual BSD/GPL");

enum mem_type {
	DMA = 1,
	FASTREG = 2,
	MW = 3,
	MR = 4
};

static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"mem_mode", OPT_STRING, 'm'},
	{"server_inv", OPT_NOPARAM, 'I'},
 	{"wlat", OPT_NOPARAM, 'l'},
 	{"rlat", OPT_NOPARAM, 'L'},
 	{"bw", OPT_NOPARAM, 'B'},
 	{"duplex", OPT_NOPARAM, 'd'},
 	{"txdepth", OPT_INT, 'T'},
 	{"poll", OPT_NOPARAM, 'P'},
 	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
 	{"read_inv", OPT_NOPARAM, 'R'},
 	{"fr", OPT_NOPARAM, 'f'},
	{NULL, 0, 0}
};

struct krping_stats {
	unsigned long long send_bytes;
	unsigned long long send_msgs;
	unsigned long long recv_bytes;
	unsigned long long recv_msgs;
	unsigned long long write_bytes;
	unsigned long long write_msgs;
	unsigned long long read_bytes;
	unsigned long long read_msgs;
};

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 37)
static DECLARE_MUTEX(krping_mutex);
#else
static DEFINE_SEMAPHORE(krping_mutex);
#endif

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;

/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr6=2001:db8:0:f101::1,validate > /proc/krping
 *
 * krping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct krping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 128*1024
#define RPING_SQ_DEPTH 64

/*
 * Control block struct.
 */
struct krping_cb {
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	enum mem_type mem;
	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_send_wr fastreg_wr;
	struct ib_send_wr invalidate_wr;
	struct ib_mr *fastreg_mr;
	int server_invalidate;
	int read_inv;
	u8 key;

	struct ib_mw *mw;
	struct ib_mw_bind bind_attr;

	struct ib_recv_wr rq_wr;	/* recv work request record */
	struct ib_sge recv_sgl;		/* recv single SGE */
	struct krping_rdma_info recv_buf;/* malloc'd buffer */
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct krping_rdma_info send_buf;/* single send buf */
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	struct ib_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	u64  start_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(start_mapping)
	struct ib_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int validate;			/* validate ping data */
	int wlat;			/* run wlat test */
	int rlat;			/* run rlat test */
	int bw;				/* run bw test */
	int duplex;			/* run bw full duplex test */
	int poll;			/* poll or block for rlat test */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;		/* use 0 for lkey */
	int frtest;			/* fastreg test */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;	
};

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	printk(KERN_INFO "Entered krping_cma_event_handler\n");
	int ret;
	struct krping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	printk(KERN_INFO "LEFT krping_cma_event_handler\n");
	return 0;
}

static int client_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	printk(KERN_INFO "Entered client_recv\n");
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;
	printk(KERN_INFO "Left client_recv\n");
	return 0;
}

static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	printk(KERN_INFO "Entered krping_cq_event_handler\n");
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
	if (cb->frtest) {
		printk(KERN_ERR PFX "cq completion event in frtest!\n");
		return;
	}
	if (!cb->wlat && !cb->rlat && !cb->bw)
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n\n");
			printk("send completion: %d\n\n", cb->send_buf.buf);
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			printk("rdma write completion\n\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n\n");
			printk("rdma write completion: %s\n\n", cb->rdma_buf);
			cb->stats.read_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			printk("recv completion: %d\n\n",cb->recv_buf.buf);
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			ret = cb->server;
			if (!ret)  ret = client_recv(cb, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	printk(KERN_INFO "Left krping_cq_event_handler\n");
	return;
error:
	printk(KERN_INFO "Error::::krping_cq_event_handler\n");
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}


static void krping_setup_wr(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_setup_wr\n");
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	if (cb->local_dma_lkey)
		cb->recv_sgl.lkey = cb->qp->device->local_dma_lkey;
	else if (cb->mem == DMA)
		cb->recv_sgl.lkey = cb->dma_mr->lkey;
	else
		cb->recv_sgl.lkey = cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	if (cb->local_dma_lkey)
		cb->send_sgl.lkey = cb->qp->device->local_dma_lkey;
	else if (cb->mem == DMA)
		cb->send_sgl.lkey = cb->dma_mr->lkey;
	else
		cb->send_sgl.lkey = cb->send_mr->lkey;

	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	if (cb->server || cb->wlat || cb->rlat || cb->bw) {
		cb->rdma_sgl.addr = cb->rdma_dma_addr;
		if (cb->mem == MR)
			cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
		cb->rdma_sq_wr.send_flags = IB_SEND_SIGNALED;
		cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
		cb->rdma_sq_wr.num_sge = 1;
	}

	printk(KERN_INFO "Left krping_setup_wr\n");
}

//5th -- done
static int krping_setup_buffers(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_setup_buffers\n");
	int ret;
	struct ib_phys_buf buf;
	u64 iovbase;
	printk("krping_setup_buffers setup 1\n\n");
	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device, 
				   &cb->recv_buf, 
				   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	printk("krping_setup_buffers setup 2\n\n");
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device, 
					   &cb->send_buf, sizeof(cb->send_buf),
					   DMA_BIDIRECTIONAL);
	printk("krping_setup_buffers setup 3\n\n");
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);
	printk("krping_setup_buffers setup 4\n\n");
	if (cb->mem == DMA) {
		printk("krping_setup_buffers setup 5\n\n");
		cb->dma_mr = ib_get_dma_mr(cb->pd, IB_ACCESS_LOCAL_WRITE|
					   IB_ACCESS_REMOTE_READ|
				           IB_ACCESS_REMOTE_WRITE);
		printk("krping_setup_buffers setup 6\n\n");
		if (IS_ERR(cb->dma_mr)) {
			printk("krping_setup_buffers setup 6 Error\n\n");
			DEBUG_LOG(PFX "reg_dmamr failed\n");
			ret = PTR_ERR(cb->dma_mr);
			goto bail;
		}
	}

	cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL);
	printk("krping_setup_buffers setup 7\n\n");
	if (!cb->rdma_buf) {
		printk("krping_setup_buffers setup 8\n\n");
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}

	printk("krping_setup_buffers setup 9\n\n");
	cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device, 
			       cb->rdma_buf, cb->size, 
			       DMA_BIDIRECTIONAL);
	printk("krping_setup_buffers setup 10\n\n");
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	printk("krping_setup_buffers setup 11\n\n");

	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {
		printk("krping_setup_buffers setup 12\n\n");
		cb->start_buf = kmalloc(cb->size, GFP_KERNEL);
		if (!cb->start_buf) {
			printk("krping_setup_buffers setup 13\n\n");
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto bail;
		}

		cb->start_dma_addr = dma_map_single(cb->pd->device->dma_device, 
						   cb->start_buf, cb->size, 
						   DMA_BIDIRECTIONAL);
		pci_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);

	}

	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	printk(KERN_INFO "Left krping_setup_buffers\n");
	return 0;
bail:
	printk(KERN_INFO "Entered  Bail:::krping_setup_buffers\n");
	if (cb->fastreg_mr && !IS_ERR(cb->fastreg_mr))
		ib_dereg_mr(cb->fastreg_mr);
	if (cb->mw && !IS_ERR(cb->mw))
		ib_dealloc_mw(cb->mw);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->recv_mr && !IS_ERR(cb->recv_mr))
		ib_dereg_mr(cb->recv_mr);
	if (cb->send_mr && !IS_ERR(cb->send_mr))
		ib_dereg_mr(cb->send_mr);
	if (cb->rdma_buf)
		kfree(cb->rdma_buf);
	if (cb->start_buf)
		kfree(cb->start_buf);
	printk(KERN_INFO "Left Bail:::krping_setup_buffers\n");
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_free_buffers\n");
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->send_mr)
		ib_dereg_mr(cb->send_mr);
	if (cb->recv_mr)
		ib_dereg_mr(cb->recv_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->fastreg_mr)
		ib_dereg_mr(cb->fastreg_mr);
	if (cb->mw)
		ib_dealloc_mw(cb->mw);

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, rdma_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
	kfree(cb->rdma_buf);
	if (cb->start_buf) {
		dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, start_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
	printk(KERN_INFO "Left krping_free_buffers\n");
}

//4th -- done
static int krping_create_qp(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_create_qp\n");
	struct ib_qp_init_attr init_attr;
	int ret;

	printk(KERN_INFO "krping_create_qp: checkpoint0\n");
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	printk(KERN_INFO "krping_create_qp: checkpoint1\n");
	if (cb->server) {
		printk(KERN_INFO "krping_create_qp: checkpoint2a. inside if(cb->server)\n");
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		printk(KERN_INFO "krping_create_qp: checkpoint2b. created qp\n");
		if (!ret)
			printk(KERN_INFO "krping_create_qp: checkpoint2c. inside if(!ret)\n");
			cb->qp = cb->child_cm_id->qp;
	} else {
		printk(KERN_INFO "krping_create_qp: checkpoint3a. inside else of if(cb->server)\n");
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		printk(KERN_INFO "krping_create_qp: checkpoint3b. created qp\n");
		if (!ret)
			printk(KERN_INFO "krping_create_qp: checkpoint3c. inside else of if(cb->server)\n");
			cb->qp = cb->cm_id->qp;
	}
	printk(KERN_INFO "Leaving krping_create_qp\n");
	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_free_qp\n");
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
	printk(KERN_INFO "LEFT krping_free_qp\n");
}

// 3rd -- done
static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	printk(KERN_INFO "Entered krping_setup_qp\n");
	int ret;
	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);
	printk(KERN_INFO "Entered krping_setup_qp first\n");
	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
			      cb, cb->txdepth * 2, 0);
	printk(KERN_INFO "Entered krping_setup_qp second\n");
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);
	printk("created cq %p\n", cb->cq);
	printk("device pointer %p\n", cb->cq->device);
	printk(KERN_INFO "created cq krping_setup_qp\n");
	if (!cb->wlat && !cb->rlat && !cb->bw && !cb->frtest) {

		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);       // Locked in here (Latest)
		if (ret) {
			printk(KERN_ERR PFX "ib_create_cq failed\n");
			goto err2;
		}
	}
	printk(KERN_INFO "IF_CONDITION#End krping_setup_qp\n");
	// Huzaifa 
	if (cb->cq == 0) {
		printk("CQ is NULL krping_setup_qp\n");
	}
	// Huzaifa  end	
	printk(KERN_INFO "Entered krping_setup_qp third\n");
	ret = krping_create_qp(cb);
	printk(KERN_INFO "Entered krping_setup_qp fourth\n");


	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	printk(KERN_INFO "LEFT krping_setup_qp\n");
	return 0;
err2:
printk(KERN_INFO "ERR2 krping_setup_qp\n");
	ib_destroy_cq(cb->cq);
err1:
printk(KERN_INFO "ERR1 krping_setup_qp\n");
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * FASTREG mode: invalidate and rebind via fastreg wr.
 * MW mode: rebind the MW.
 * other modes: just return the mr rkey.
 */
static u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv)
{
	printk(KERN_INFO "Entered krping_rdma_rkey\n");
	u32 rkey = 0xffffffff;
	rkey = cb->dma_mr->rkey;
	printk(KERN_INFO "Left krping_rdma_rkey\n");
	return rkey;
}

// 8th -- done
static void krping_format_send(struct krping_cb *cb, u64 buf)
{
	printk(KERN_INFO "Entered krping_format_send\n");
	struct krping_rdma_info *info = &cb->send_buf;
	u32 rkey;

	/*
	 * Client side will do fastreg or mw bind before
	 * advertising the rdma buffer.  Server side
	 * sends have no data.
	 */
	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {
		rkey = krping_rdma_rkey(cb, buf, !cb->server_invalidate);
		info->buf = htonll(buf);
		info->rkey = htonl(rkey);
		info->size = htonl(cb->size);
		DEBUG_LOG("RDMA addr %llx rkey %x len %d\n",
			  (unsigned long long)buf, rkey, cb->size);
		printk("RDMA addr %llx rkey %x len %d\n",
			  (unsigned long long)buf, rkey, cb->size);
	}
	printk(KERN_INFO "Left krping_format_send\n");
}

// 2 -- done
static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *cb)
{
	printk(KERN_INFO "Entered fill_sockaddr\n");
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	} else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
	}
	printk(KERN_INFO "Left fill_sockaddr\n");
}

// 7th -- done
static void krping_test_client(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_test_client\n");
	int ping, start, cc, i, ret;
	struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		krping_format_send(cb, cb->start_dma_addr);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "krping_format_send failed\n");
			break;
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}

		krping_format_send(cb, cb->rdma_dma_addr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}

		if (cb->validate)
			if (memcmp(cb->start_buf, cb->rdma_buf, cb->size)) {
				printk(KERN_ERR PFX "data mismatch!\n");
				break;
			}

		if (cb->verbose)
			printk(KERN_INFO PFX "ping data: %s\n", cb->rdma_buf);
		printk(KERN_INFO "Left krping_test_client\n");
#ifdef SLOW_KRPING
		wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
#endif
	}
}

//6th -- done
static int krping_connect_client(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_connect_client\n");
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	printk(KERN_INFO "Left krping_connect_client\n");
	return 0;
}

// 1st -- done
static int krping_bind_client(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_bind_client\n");
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	printk(KERN_INFO "Left krping_bind_client\n");
	return 0;
}

// start
static void krping_run_client(struct krping_cb *cb)
{
	printk(KERN_INFO "Entered krping_run_client\n");
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_client(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	krping_test_client(cb);
	rdma_disconnect(cb->cm_id);
	printk(KERN_INFO "Left krping_run_client\n");
err2:
	printk(KERN_INFO "ERR2 krping_run_client\n");
	krping_free_buffers(cb);
err1:
	printk(KERN_INFO "ERR1 krping_run_client\n");
	krping_free_qp(cb);
}

int krping_doit(char *cmd)
{
	printk(KERN_INFO "Entered krping_doit\n");
	struct krping_cb *cb;
	int op;
	int ret = 0;
	char *optarg;
	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	down(&krping_mutex);
	list_add_tail(&cb->list, &krping_cbs);
	up(&krping_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	cb->txdepth = RPING_SQ_DEPTH;
	cb->mem = DMA;
	init_waitqueue_head(&cb->sem);

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg,
			      &optint)) != 0) {
		// Mobeen
		printk("Op has the value: %c\n", op);
		// Mobeen end
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			in4_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET;
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'A':
			cb->addr_str = optarg;
			in6_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET6;
			DEBUG_LOG("ipv6addr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'P':
			cb->poll = 1;
			DEBUG_LOG("server\n");
			break;
		case 's':
			cb->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = optint;
			if ((cb->size < 1) ||
			    (cb->size > RPING_BUFSIZE)) {
				printk(KERN_ERR PFX "Invalid size %d "
				       "(valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int)optint);
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				printk(KERN_ERR PFX "Invalid count %d\n",
					cb->count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'l':
			cb->wlat++;
			break;
		case 'L':
			cb->rlat++;
			break;
		case 'B':
			cb->bw++;
			break;
		case 'd':
			cb->duplex++;
			break;
		case 'm':
			if (!strncmp(optarg, "dma", 3))
				cb->mem = DMA;
			else if (!strncmp(optarg, "fastreg", 7))
				cb->mem = FASTREG;
			else if (!strncmp(optarg, "mw", 2))
				cb->mem = MW;
			else if (!strncmp(optarg, "mr", 2))
				cb->mem = MR;
			else {
				printk(KERN_ERR PFX "unknown mem mode %s.  "
					"Must be dma, fastreg, mw, or mr\n",
					optarg);
				ret = -EINVAL;
				break;
			}
			break;
		case 'I':
			cb->server_invalidate = 1;
			break;
		case 'T':
			cb->txdepth = optint;
			DEBUG_LOG("txdepth %d\n", (int) cb->txdepth);
			break;
		case 'Z':
			cb->local_dma_lkey = 1;
			DEBUG_LOG("using local dma lkey\n");
			break;
		case 'R':
			cb->read_inv = 1;
			DEBUG_LOG("using read-with-inv\n");
			break;
		case 'f':
			cb->frtest = 1;
			DEBUG_LOG("fast-reg test!\n");
			break;
		default:
			printk(KERN_ERR PFX "unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
	if (ret)
		goto out;


	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->server && cb->frtest) {
		printk(KERN_ERR PFX "must be client to run frtest\n");
		ret = -EINVAL;
		goto out;
	}

	if ((cb->frtest + cb->bw + cb->rlat + cb->wlat) > 1) {
		printk(KERN_ERR PFX "Pick only one test: fr, bw, rlat, wlat\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->server_invalidate && cb->mem != FASTREG) {
		printk(KERN_ERR PFX "server_invalidate only valid with fastreg mem_mode\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->read_inv && cb->mem != FASTREG) {
		printk(KERN_ERR PFX "read_inv only valid with fastreg mem_mode\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->mem != MR && (cb->wlat || cb->rlat || cb->bw)) {
		printk(KERN_ERR PFX "wlat, rlat, and bw tests only support mem_mode MR\n");
		ret = -EINVAL;
		goto out;
	}

	 cb->cm_id = rdma_create_id(krping_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	// // Sohaib --
	//cb->cm_id = rdma_create_id(krping_cma_event_handler, cb, RDMA_PS_UD, IB_QPT_UD);
	// // RC stands for Reliable Connected
	// // UC stands for Unreliable Connected
	// // UD stands for Unreliable Datagram
	// // -- Sohaib
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	if (!cb->server)
		krping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
	printk(KERN_INFO "Left krping_doit\n");
out:
	printk(KERN_INFO "out krping_doit\n");
	down(&krping_mutex);
	list_del(&cb->list);
	up(&krping_mutex);
	kfree(cb);
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(char *page, char **start, off_t off, int count,
			    int *eof, void *data)
{
	printk(KERN_INFO "Entered krping_read_proc\n");
	struct krping_cb *cb;
	int cc = 0;
	char *cp = page;
	int num = 1;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	*cp = 0;
	down(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			cc = sprintf(cp,
			     "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
			     num++, cb->pd->device->name, cb->stats.send_bytes,
			     cb->stats.send_msgs, cb->stats.recv_bytes,
			     cb->stats.recv_msgs, cb->stats.write_bytes,
			     cb->stats.write_msgs,
			     cb->stats.read_bytes,
			     cb->stats.read_msgs);
		} else {
			cc = sprintf(cp, "%d listen\n", num++);
		}
		cp += cc;
	}
	up(&krping_mutex);
	*eof = 1;
	module_put(THIS_MODULE);
	printk(KERN_INFO "Left krping_read_proc\n");
	return strlen(page);
}

/*
 * Write proc is used to start a ping client or server.
 */
static int krping_write_proc(struct file *file, const char *buffer,
			     unsigned long count, void *data)
{
	printk(KERN_INFO "Entered krping_write_proc\n");
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	printk(KERN_INFO "Left krping_write_proc\n");
	if (rc)
		return rc;
	else
		return (int) count;
}

struct file_operations krping_ops = {
	.owner = THIS_MODULE,
	.read = krping_read_proc,
	.write = krping_write_proc
};

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);
}

module_init(krping_init);
module_exit(krping_exit);
