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
#include <linux/socket.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include </usr/src/ofa_kernel/default/include/rdma/ib_verbs.h>
#include </usr/src/ofa_kernel/default/include/rdma/rdma_cm.h>

#include "getopt.h"

#define PFX "mega-VM: "

static int debug = 1;
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
struct ib_context {
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	enum mem_type mem;
	struct ib_mr *dma_mr;

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
	int txdepth;			/* SQ depth */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	//struct list_head list;	
};


static int krping_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret;
	struct ib_context *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id, (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			DEBUG_LOG(KERN_ERR PFX "rdma_resolve_route error %d\n", ret);
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
		DEBUG_LOG(KERN_ERR PFX "cma event %d, error %d\n", event->event, event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		DEBUG_LOG(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		DEBUG_LOG(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		DEBUG_LOG(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

//Done
static int server_recv(struct ib_context *cb, struct ib_wc *wc)
{
	DEBUG_LOG("Entering server_recv \n\n");
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		DEBUG_LOG(KERN_ERR PFX "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	DEBUG_LOG("Received rkey %x addr %llx len %d from peer\n", cb->remote_rkey, (unsigned long long)cb->remote_addr, cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;
	DEBUG_LOG("Exiting server_recv \n\n");
	return 0;
}

//Done
static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	DEBUG_LOG("Entering krping_cq_event_handler\n\n");
	struct ib_context *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		DEBUG_LOG(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				DEBUG_LOG(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n\n");
			DEBUG_LOG("send completion: %d\n\n", cb->send_buf.buf);
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			DEBUG_LOG("rdma write completion\n\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n\n");
			DEBUG_LOG("rdma write completion: %s\n\n", cb->rdma_buf);
			cb->stats.read_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			DEBUG_LOG("recv completion: %d\n\n",cb->recv_buf.buf);
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			ret = cb->server;
			if (ret)  ret = server_recv(cb, &wc);
			if (ret) {
				DEBUG_LOG(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				DEBUG_LOG(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			DEBUG_LOG(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	DEBUG_LOG("Exiting krping_cq_event_handler\n\n");
	return;
error:
	DEBUG_LOG("Error krping_cq_event_handler\n\n");
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

//Done
static int krping_accept(struct ib_context *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		DEBUG_LOG(KERN_ERR PFX "wait for CONNECTED state %d\n", 
			cb->state);
		return -1;
	}
	return 0;
}

//Done
static void krping_setup_wr(struct ib_context *cb)
{
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->dma_mr->lkey;

	cb->rq_wr.sg_list = &cb->recv_sgl;

	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->dma_mr->lkey;
	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	if (cb->server) {
		cb->rdma_sgl.addr = cb->rdma_dma_addr;
		cb->rdma_sq_wr.send_flags = IB_SEND_SIGNALED;
		cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
		cb->rdma_sq_wr.num_sge = 1;
	}

}

//Done
static int krping_setup_buffers(struct ib_context *cb)
{
	int ret;

	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device, &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);

	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device, &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

		cb->dma_mr = ib_get_dma_mr(cb->pd, IB_ACCESS_LOCAL_WRITE|IB_ACCESS_REMOTE_READ|IB_ACCESS_REMOTE_WRITE);
		if (IS_ERR(cb->dma_mr)) {
			DEBUG_LOG(PFX "reg_dmamr failed\n");
			ret = PTR_ERR(cb->dma_mr);
			goto bail;
		}
	DEBUG_LOG("Point 3 successful\n\n");


	cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL);
	if (!cb->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}

	cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device, cb->rdma_buf, cb->size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);

	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	DEBUG_LOG("Point 4 successful\n\n");

	return 0;
bail:
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
	return ret;
}

//Done
static void krping_free_buffers(struct ib_context *cb)
{
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

	dma_unmap_single(cb->pd->device->dma_device, pci_unmap_addr(cb, recv_mapping), sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);

	dma_unmap_single(cb->pd->device->dma_device, pci_unmap_addr(cb, send_mapping), sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

	dma_unmap_single(cb->pd->device->dma_device, pci_unmap_addr(cb, rdma_mapping), cb->size, DMA_BIDIRECTIONAL);
	kfree(cb->rdma_buf);

	if (cb->start_buf) {
		dma_unmap_single(cb->pd->device->dma_device, pci_unmap_addr(cb, start_mapping), cb->size, DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
}

//Done
static int krping_create_qp(struct ib_context *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;
	DEBUG_LOG(KERN_INFO "Inside create qp\n");
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	DEBUG_LOG(KERN_INFO "Initialised variables for create qp\n");
	ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
	if (!ret)
		cb->qp = cb->child_cm_id->qp;
	DEBUG_LOG(KERN_INFO "Create qp successfully\n");
	
	return ret;
}

//Done
static void krping_free_qp(struct ib_context *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

// Done
static int krping_setup_qp(struct ib_context *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	DEBUG_LOG(KERN_INFO "Entered krping_setup_qp\n\n");
	if (cm_id != 0){
		cb->pd = ib_alloc_pd(cm_id->device);
		if (IS_ERR(cb->pd)) {
			DEBUG_LOG(KERN_ERR PFX "ib_alloc_pd failed\n");
			return PTR_ERR(cb->pd);
		}
	} else
	DEBUG_LOG("created pd %p\n", cb->pd);
	DEBUG_LOG(KERN_INFO "created pd %p\n", cb->pd);
	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL, cb, cb->txdepth * 2, 0);
	if (IS_ERR(cb->cq)) {
		DEBUG_LOG(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);
	DEBUG_LOG(KERN_INFO "created cq %p\n", cb->cq);

	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "ib_create_cq failed\n");
		goto err2;
	}
	

	ret = krping_create_qp(cb);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	DEBUG_LOG(KERN_INFO "created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

//Done
static u32 krping_rdma_rkey(struct ib_context *cb, u64 buf, int post_inv)
{
	u32 rkey = 0xffffffff;
	rkey = cb->dma_mr->rkey;
	return rkey;
}

//Done
static void krping_test_server(struct ib_context *cb)
{
	struct ib_send_wr *bad_wr;
	int ret;

	while (1) {
		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		if (cb->state != RDMA_READ_ADV) {
			DEBUG_LOG(KERN_ERR PFX "wait for RDMA_READ_ADV state %d\n", cb->state);
			break;
		}

		DEBUG_LOG("server received sink adv\n");

		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = cb->remote_len;
		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 1);
		cb->rdma_sq_wr.next = NULL;

		/* Issue RDMA Read. */
	
		cb->rdma_sq_wr.opcode = IB_WR_RDMA_READ;

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			DEBUG_LOG(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		cb->rdma_sq_wr.next = NULL;

		DEBUG_LOG("server posted rdma read req \n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_READ_COMPLETE);
		if (cb->state != RDMA_READ_COMPLETE) {
			DEBUG_LOG(KERN_ERR PFX 
			       "wait for RDMA_READ_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server received read complete\n");

		/* Display data in recv buf */
		if (cb->verbose)
			DEBUG_LOG(KERN_INFO PFX "server ping data: %s\n", cb->rdma_buf);

	
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			DEBUG_LOG(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");

		/* Wait for client's RDMA STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			DEBUG_LOG(KERN_ERR PFX "wait for RDMA_WRITE_ADV state %d\n", cb->state);
			break;
		}
		DEBUG_LOG("server received sink adv\n");
		/* RDMA Write echo data */
		cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = strlen(cb->rdma_buf) + 1;

		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 0);
			
		DEBUG_LOG("rdma write from lkey %x laddr %llx len %d\n", cb->rdma_sq_wr.sg_list->lkey,
			  (unsigned long long)cb->rdma_sq_wr.sg_list->addr, cb->rdma_sq_wr.sg_list->length);

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			DEBUG_LOG(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			DEBUG_LOG(KERN_ERR PFX "wait for RDMA_WRITE_COMPLETE state %d\n", cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		cb->state = CONNECTED;

		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			DEBUG_LOG(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");
	}
}


static void fill_sockaddr(struct sockaddr_storage *sin, struct ib_context *cb)
{
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
}

// Done
static int krping_bind_server(struct ib_context *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}
	DEBUG_LOG("Point 1 successful\n\n");
	// Huzaifa
	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		DEBUG_LOG(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n", cb->state);
		return -1;
	}

	return 0;
}

static void megaVM_StartServer(struct ib_context *context)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_server(context);
	if (ret)
		return;
	ret = krping_setup_qp(context, context->child_cm_id);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(context);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	ret = ib_post_recv(context->qp, &context->rq_wr, &bad_wr);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}
	ret = krping_accept(context);
	if (ret) {
		DEBUG_LOG(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	krping_test_server(context);
	rdma_disconnect(context->child_cm_id);
err2:
	krping_free_buffers(context);
err1:
	krping_free_qp(context);
err0:
	rdma_destroy_id(context->child_cm_id);
}

int initialisation(void)
{
	struct ib_context *context;
	char *optarg;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;


	// Initialization
	context->state = IDLE;
	context->size = 64; // In our case this should be 4096 ==> 4 kB
	context->txdepth = RPING_SQ_DEPTH;
	context->mem = DMA;
	context->server = 1;
	context->port = htons(9999);
	optarg = "12.12.12.1";
	DEBUG_LOG("My address %s\n", optarg);
	in4_pton(optarg, -1, context->addr, -1, NULL);
	context->addr_type = AF_INET;
	init_waitqueue_head(&context->sem);

	// ID assigned by the opensm (Open subnet Manager)
	context->cm_id = rdma_create_id(krping_cma_event_handler, context, RDMA_PS_TCP, IB_QPT_RC);
	
	if (context->cm_id == NULL) {
		DEBUG_LOG(KERN_ERR PFX "rdma_create_id error \n");
		goto out;
	} else
		DEBUG_LOG("created cm_id %p\n", context->cm_id);
	
	megaVM_StartServer(context);

	// Need to change its position when integrated into megVM
	rdma_destroy_id(context->cm_id);
out:
	kfree(context);
	return 0;
}

static int __init megaVM_init(void)
{
	DEBUG_LOG("MegaVM_server init\n");
	initialisation();
}

static void __exit megaVM_exit(void)
{
	DEBUG_LOG("MegaVM_server exit\n");
}

module_init(megaVM_init);
module_exit(megaVM_exit);
