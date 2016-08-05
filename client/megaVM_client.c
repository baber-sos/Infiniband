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


#define PFX "megaVM: "

static int debug = 0;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("MegaVM");
MODULE_DESCRIPTION("megaVM_client");
MODULE_LICENSE("Dual BSD/GPL");

enum mem_type {
	DMA = 1,
	FASTREG = 2,
	MW = 3,
	MR = 4
};


struct megaVM_stats {
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


/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/megaVM  
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/megaVM  
 * /bin/echo client,port=9999,addr6=2001:db8:0:f101::1,validate > /proc/megaVM
 *
 * megaVM "ping/pong" loop:
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

struct megaVM_rdma_info {
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
	struct megaVM_rdma_info recv_buf;/* malloc'd buffer */
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct megaVM_rdma_info send_buf;/* single send buf */
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	struct ib_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
	struct ib_mr *rdma_mr;

	char *start_buf;		/* rdma read src */
	u64  start_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(start_mapping)
	struct ib_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct megaVM_stats stats;

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
};

static int megaVM_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret;
	struct ib_context *context = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == context->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		context->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", ret);
			wake_up_interruptible(&context->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		context->state = ROUTE_RESOLVED;
		wake_up_interruptible(&context->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		context->state = CONNECT_REQUEST;
		context->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", context->child_cm_id);
		wake_up_interruptible(&context->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!context->server) {
			context->state = CONNECTED;
		}
		wake_up_interruptible(&context->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		context->state = ERROR;
		wake_up_interruptible(&context->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		context->state = ERROR;
		wake_up_interruptible(&context->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&context->sem);
		break;
	}
	return 0;
}

static int client_recv(struct ib_context *context, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(context->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	if (context->state == RDMA_READ_ADV)
		context->state = RDMA_WRITE_ADV;
	else
		context->state = RDMA_WRITE_COMPLETE;
	return 0;
}

static void megaVM_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ib_context *context = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(context->cq != cq);
	if (context->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}

	ib_req_notify_cq(context->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(context->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with " "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n\n");
			context->stats.send_bytes += context->send_sgl.length;
			context->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			context->stats.write_bytes += context->rdma_sq_wr.sg_list->length;
			context->stats.write_msgs++;
			context->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&context->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n\n");
			context->stats.read_bytes += context->rdma_sq_wr.sg_list->length;
			context->stats.read_msgs++;
			context->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&context->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			context->stats.recv_bytes += sizeof(context->recv_buf);
			context->stats.recv_msgs++;
			ret = context->server;
			if (!ret)  ret = client_recv(context, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(context->qp, &context->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&context->sem);
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

	return;
error:
	context->state = ERROR;
	wake_up_interruptible(&context->sem);
}


static void megaVM_setup_wr(struct ib_context *context)
{

	context->recv_sgl.addr = context->recv_dma_addr;
	context->recv_sgl.length = sizeof context->recv_buf;
	
	context->recv_sgl.lkey = context->dma_mr->lkey;
	context->rq_wr.sg_list = &context->recv_sgl;
	context->rq_wr.num_sge = 1;

	context->send_sgl.addr = context->send_dma_addr;
	context->send_sgl.length = sizeof context->send_buf;
	context->send_sgl.lkey = context->dma_mr->lkey;

	context->sq_wr.opcode = IB_WR_SEND;
	context->sq_wr.send_flags = IB_SEND_SIGNALED;
	context->sq_wr.sg_list = &context->send_sgl;
	context->sq_wr.num_sge = 1;

}

//5th -- done
static int megaVM_setup_buffers(struct ib_context *context)
{
	int ret;
	DEBUG_LOG(PFX "megaVM_setup_buffers called on context %p\n", context);

	context->recv_dma_addr = dma_map_single(context->pd->device->dma_device, &context->recv_buf, sizeof(context->recv_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(context, recv_mapping, context->recv_dma_addr);

	context->send_dma_addr = dma_map_single(context->pd->device->dma_device, &context->send_buf, sizeof(context->send_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(context, send_mapping, context->send_dma_addr);
	
	if (context->mem == DMA) {
		context->dma_mr = ib_get_dma_mr(context->pd, IB_ACCESS_LOCAL_WRITE|IB_ACCESS_REMOTE_READ|IB_ACCESS_REMOTE_WRITE);
		
		if (context->dma_mr == NULL) {
			DEBUG_LOG(PFX "reg_dmamr failed\n");
			ret = -1;
			goto bail;
		}
	}

	context->rdma_buf = kmalloc(context->size, GFP_KERNEL);
	if (!context->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -1;
		goto bail;
	}

	context->rdma_dma_addr = dma_map_single(context->pd->device->dma_device, context->rdma_buf, context->size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(context, rdma_mapping, context->rdma_dma_addr);

	context->start_buf = kmalloc(context->size, GFP_KERNEL);
	if (!context->start_buf) {
		DEBUG_LOG(PFX "start_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}

	context->start_dma_addr = dma_map_single(context->pd->device->dma_device, context->start_buf, context->size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(context, start_mapping, context->start_dma_addr);

	megaVM_setup_wr(context);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:

	if (context->rdma_mr && !IS_ERR(context->rdma_mr))
		ib_dereg_mr(context->rdma_mr);
	if (context->dma_mr && !IS_ERR(context->dma_mr))
		ib_dereg_mr(context->dma_mr);
	if (context->recv_mr && !IS_ERR(context->recv_mr))
		ib_dereg_mr(context->recv_mr);
	if (context->send_mr && !IS_ERR(context->send_mr))
		ib_dereg_mr(context->send_mr);
	if (context->rdma_buf)
		kfree(context->rdma_buf);
	if (context->start_buf)
		kfree(context->start_buf);
	return ret;
}

static void megaVM_free_buffers(struct ib_context *context)
{
	DEBUG_LOG("megaVM_free_buffers called on context %p\n", context);
	
	if (context->dma_mr)
		ib_dereg_mr(context->dma_mr);
	if (context->send_mr)
		ib_dereg_mr(context->send_mr);
	if (context->recv_mr)
		ib_dereg_mr(context->recv_mr);
	if (context->rdma_mr)
		ib_dereg_mr(context->rdma_mr);
	if (context->start_mr)
		ib_dereg_mr(context->start_mr);

	dma_unmap_single(context->pd->device->dma_device, pci_unmap_addr(context, recv_mapping), sizeof(context->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(context->pd->device->dma_device,
			 
	pci_unmap_addr(context, send_mapping),sizeof(context->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(context->pd->device->dma_device, pci_unmap_addr(context, rdma_mapping), context->size, DMA_BIDIRECTIONAL);
	kfree(context->rdma_buf);

	if (context->start_buf) {
		dma_unmap_single(context->pd->device->dma_device, pci_unmap_addr(context, start_mapping), context->size, DMA_BIDIRECTIONAL);
		kfree(context->start_buf);
	}
}

//4th -- done
static int megaVM_create_qp(struct ib_context *context)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = context->txdepth;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = context->cq;
	init_attr.recv_cq = context->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;


	ret = rdma_create_qp(context->cm_id, context->pd, &init_attr);
	if (!ret)
		context->qp = context->cm_id->qp;
		return ret;
}

static void megaVM_free_qp(struct ib_context *context)
{
	ib_destroy_qp(context->qp);
	ib_destroy_cq(context->cq);
	ib_dealloc_pd(context->pd);
}

// 3rd -- done
static int megaVM_setup_qp(struct ib_context *context, struct rdma_cm_id *cm_id)
{
	int ret;
	context->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(context->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(context->pd);
	}
	DEBUG_LOG("created pd %p\n", context->pd);
	context->cq = ib_create_cq(cm_id->device, megaVM_cq_event_handler, NULL,
			      context, context->txdepth * 2, 0);
	if (IS_ERR(context->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(context->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", context->cq);

	ret = ib_req_notify_cq(context->cq, IB_CQ_NEXT_COMP); 
	if (ret) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		goto err2;
	}

	if (context->cq == NULL) {
		printk("CQ is NULL megaVM_setup_qp\n");
	}
	ret = megaVM_create_qp(context);


	if (ret) {
		printk(KERN_ERR PFX "megaVM_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", context->qp);
	return 0;
err2:
	ib_destroy_cq(context->cq);
err1:
	ib_dealloc_pd(context->pd);
	return ret;
}

// 8th -- done
static void megaVM_format_send(struct ib_context *context, u64 buf)
{
	struct megaVM_rdma_info *info = &context->send_buf;
	u32 rkey;

	rkey = context->dma_mr->rkey;
	info->buf = htonll(buf);
	info->rkey = htonl(rkey);
	info->size = htonl(context->size);
	DEBUG_LOG("RDMA addr %llx rkey %x len %d\n", (unsigned long long)buf, rkey, context->size);

	
}

// 2 -- done
static void fill_sockaddr(struct sockaddr_storage *sin, struct ib_context *context)
{
	memset(sin, 0, sizeof(*sin));

	if (context->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, context->addr, 4);
		sin4->sin_port = context->port;
	}
}

// 7th -- done
// static void megaVM_test_client(struct ib_context *context)
// {
// 	int ping, start, cc, i, ret;
// 	struct ib_send_wr *bad_wr;
// 	unsigned char c;

// 	start = 65;
// 	for (ping = 0; !context->count || ping < context->count; ping++) {
// 		context->state = RDMA_READ_ADV;

// 		/* Put some ascii text in the buffer. */
// 		cc = sprintf(context->start_buf, "rdma-ping-%d: ", ping);
// 		for (i = cc, c = start; i < context->size; i++) {
// 			context->start_buf[i] = c;
// 			c++;
// 			if (c > 122)
// 				c = 65;
// 		}
// 		start++;
// 		if (start > 122)
// 			start = 65;
// 		context->start_buf[context->size - 1] = 0;

// 		megaVM_format_send(context, context->start_dma_addr);
// 		if (context->state == ERROR) {
// 			printk(KERN_ERR PFX "megaVM_format_send failed\n");
// 			break;
// 		}
// 		ret = ib_post_send(context->qp, &context->sq_wr, &bad_wr);
// 		if (ret) {
// 			printk(KERN_ERR PFX "post send error %d\n", ret);
// 			break;
// 		}

// 		/* Wait for server to ACK */
// 		wait_event_interruptible(context->sem, context->state >= RDMA_WRITE_ADV);
// 		if (context->state != RDMA_WRITE_ADV) {
// 			printk(KERN_ERR PFX 
// 			       "wait for RDMA_WRITE_ADV state %d\n",
// 			       context->state);
// 			break;
// 		}

// 		megaVM_format_send(context, context->rdma_dma_addr);
// 		ret = ib_post_send(context->qp, &context->sq_wr, &bad_wr);
// 		if (ret) {
// 			printk(KERN_ERR PFX "post send error %d\n", ret);
// 			break;
// 		}

// 		/* Wait for the server to say the RDMA Write is complete. */
// 		wait_event_interruptible(context->sem, 
// 					 context->state >= RDMA_WRITE_COMPLETE);
// 		printk("Data comparison\ncontext->start_buf: %s\ncontext->rdma_buf: %s\n",context->start_buf,context->rdma_buf);
// 		if (context->state != RDMA_WRITE_COMPLETE) {
// 			printk(KERN_ERR PFX 
// 			       "wait for RDMA_WRITE_COMPLETE state %d\n",
// 			       context->state);
// 			break;
// 		}

// 		if (context->validate)
// 			if (memcmp(context->start_buf, context->rdma_buf, context->size)) {
// 				printk(KERN_ERR PFX "data mismatch!\n");
// 				break;
// 			}


// 		if (context->verbose)
// 			printk(KERN_INFO PFX "ping data: %s\n", context->rdma_buf);
// 		#ifdef SLOW_megaVM
// 			wait_event_interruptible_timeout(context->sem, context->state == ERROR, HZ);
// 		#endif
// 	}
// }

//**************** EXPERIMENT ****************************
static void megaVM_test_client(struct ib_context *context)
{
	context->state = RDMA_READ_ADV;	
	int ping, start, cc, i, ret;
	struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
		cc = sprintf(context->start_buf, "rdma-ping-1: ");
	for (i = cc, c = start; i < context->size; i++) {
		context->start_buf[i] = c;
		c++;
		if (c > 122)
			c = 65;
	}
	start++;
	if (start > 122)
		start = 65;
	context->start_buf[context->size - 1] = 0;

	megaVM_format_send(context, context->start_dma_addr);
	if (context->state == ERROR) {
		printk(KERN_ERR PFX "megaVM_format_send failed\n");
	}
	
	ret = ib_post_send(context->qp, &context->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
	}
	wait_event_interruptible(context->sem, context->state >= RDMA_WRITE_ADV);

}
// ******************* END *******************************

//6th -- done
static int megaVM_connect_client(struct ib_context *context)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(context->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(context->sem, context->state >= CONNECTED);
	if (context->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", context->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

// 1st -- done
static int megaVM_bind_client(struct ib_context *context)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, context);

	ret = rdma_resolve_addr(context->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(context->sem, context->state >= ROUTE_RESOLVED);
	if (context->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX "addr/route resolution did not resolve: state %d\n", context->state);
		return -EINTR;
	}
	return 0;
}

// start
static void megaVM_run_client(struct ib_context *context)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = megaVM_bind_client(context);
	if (ret)
		return;

	ret = megaVM_setup_qp(context, context->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = megaVM_setup_buffers(context);
	if (ret) {
		printk(KERN_ERR PFX "megaVM_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(context->qp, &context->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = megaVM_connect_client(context);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	megaVM_test_client(context);
	rdma_disconnect(context->cm_id);
err2:
	megaVM_free_buffers(context);
err1:
	megaVM_free_qp(context);
}

int megaVM_doit(void)
{
	struct ib_context *context;
	int ret = 0;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -1;


	context->state = IDLE;
	context->size = 64;
	context->txdepth = RPING_SQ_DEPTH;
	context->mem = DMA;
	init_waitqueue_head(&context->sem);
	context->server = 0;
	context->addr_str = "12.12.12.1";
	in4_pton(context->addr_str, -1, context->addr, -1, NULL);
	context->addr_type = AF_INET;
	context->port = htons(9999);
	context->count = 1;


	context->cm_id = rdma_create_id(megaVM_cma_event_handler, context, RDMA_PS_TCP, IB_QPT_RC);

	if (context->cm_id == NULL) {
		ret = -1;
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", context->cm_id);

	megaVM_run_client(context);

	DEBUG_LOG("destroy cm_id %p\n", context->cm_id);

	rdma_destroy_id(context->cm_id);
out:
	kfree(context);
	return ret;
}


static int __init megaVM_init(void)
{
	DEBUG_LOG("megaVM_init\n");
	megaVM_doit();
	return 0;
}

static void __exit megaVM_exit(void)
{
	DEBUG_LOG("megaVM_exit\n");
}

module_init(megaVM_init);
module_exit(megaVM_exit);
