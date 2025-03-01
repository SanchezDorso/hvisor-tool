#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtio.h"
#include "hvisor.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "virtio_console.h"
#include "log.h"
#include <sys/mman.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>                                                                                           
#include <limits.h>
#include <sys/stat.h>
/// hvisor kernel module fd
int ko_fd;
volatile struct virtio_bridge *virtio_bridge;

pthread_mutex_t RES_MUTEX = PTHREAD_MUTEX_INITIALIZER;
VirtIODevice *vdevs[MAX_DEVS];
int vdevs_num;

void *virt_addr;
void *phys_addr;

#define WAIT_TIME 1000 // 1ms

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_error("fcntl(F_GETFL) failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("fcntl(F_SETFL) failed");
        return -1;
    }
    return 0;
}

inline int is_queue_full(unsigned int front, unsigned int rear, unsigned int size)
{
    if (((rear + 1) & (size - 1)) == front) {
        return 1;
    } else {
        return 0;
    }
}

inline int is_queue_empty(unsigned int front, unsigned int rear)
{
    return rear == front;
}

/// Write barrier to make sure all write operations are finished before this operation
static inline void write_barrier(void) {
    #ifdef ARM64
        asm volatile ("dmb ishst":: : "memory");
    #endif
    #ifdef RISCV64
        asm volatile ("fence w,w"::: "memory");
    #endif
}

static inline void read_barrier(void) {
    #ifdef ARM64
        asm volatile ("dmb ishld":: : "memory");
    #endif
    #ifdef RISCV64
        asm volatile ("fence r,r"::: "memory");
    #endif
}

static inline void rw_barrier(void) {
    #ifdef ARM64
        asm volatile ("dmb ish":: : "memory");
    #endif
    #ifdef RISCV64
        asm volatile ("fence rw,rw"::: "memory");
    #endif
}

// create a virtio device.
static VirtIODevice *create_virtio_device(VirtioDeviceType dev_type, uint32_t zone_id, 
						uint64_t base_addr, uint64_t len, uint32_t irq_id, void* arg)
{
	log_info("create virtio device type %d, zone id %d, base addr %lx, len %lx, irq id %d", 
				dev_type, zone_id, base_addr, len, irq_id);
    VirtIODevice *vdev = NULL;
    int is_err;
	vdev = calloc(1, sizeof(VirtIODevice));
	init_mmio_regs(&vdev->regs, dev_type);
	vdev->base_addr = base_addr;
	vdev->len = len;
	vdev->zone_id = zone_id;
	vdev->irq_id = irq_id;
	vdev->type = dev_type;
    switch (dev_type)
    {
    case VirtioTBlock: 
        vdev->regs.dev_feature = BLK_SUPPORTED_FEATURES;
        vdev->dev = init_blk_dev(vdev);
        init_virtio_queue(vdev, dev_type);
        is_err = virtio_blk_init(vdev, (const char*)arg);
        break;
    case VirtioTNet:
        vdev->regs.dev_feature = NET_SUPPORTED_FEATURES;
        uint8_t mac[] = {0x00, 0x16, 0x3E, 0x10, 0x10, 0x10};
        vdev->dev = init_net_dev(mac);
        init_virtio_queue(vdev, dev_type);
        is_err = virtio_net_init(vdev, (char *)arg);
        break;
    case VirtioTConsole:
        vdev->regs.dev_feature = CONSOLE_SUPPORTED_FEATURES;
        vdev->dev = init_console_dev();
        init_virtio_queue(vdev, dev_type);
        is_err = virtio_console_init(vdev);
        break;
	default:
		log_error("unsupported virtio device type\n");
		goto err;
    }
    if (is_err) goto err;
    log_info("create virtio device %d success", dev_type);
    vdevs[vdevs_num++] = vdev;
    return vdev;

err:
	free(vdev);
	return NULL;
}

void init_virtio_queue(VirtIODevice *vdev, VirtioDeviceType type)
{
    VirtQueue *vq = NULL;
    switch (type)
    {
    case VirtioTBlock:
        vdev->vqs_len = 1;
        vq = malloc(sizeof(VirtQueue));
        virtqueue_reset(vq, 0);
        vq->queue_num_max = VIRTQUEUE_BLK_MAX_SIZE;
        vq->notify_handler = virtio_blk_notify_handler;
        vq->dev = vdev;
        vdev->vqs = vq;
        break;
    case VirtioTNet:
        vdev->vqs_len = NET_MAX_QUEUES;
        vq = malloc(sizeof(VirtQueue) * NET_MAX_QUEUES);
        for (int i = 0; i < NET_MAX_QUEUES; ++i) {
            virtqueue_reset(vq, i);
            vq[i].queue_num_max = VIRTQUEUE_NET_MAX_SIZE;
            vq[i].dev = vdev;
        }
        vq[NET_QUEUE_RX].notify_handler = virtio_net_rxq_notify_handler;
        vq[NET_QUEUE_TX].notify_handler = virtio_net_txq_notify_handler;
        vdev->vqs = vq;
        break;
    case VirtioTConsole:
        vdev->vqs_len = CONSOLE_MAX_QUEUES;
        vq = malloc(sizeof(VirtQueue) * CONSOLE_MAX_QUEUES);
        for (int i = 0; i < CONSOLE_MAX_QUEUES; ++i) {
            virtqueue_reset(vq, i);
            vq[i].queue_num_max = VIRTQUEUE_CONSOLE_MAX_SIZE;
            vq[i].dev = vdev;
        }
        vq[CONSOLE_QUEUE_RX].notify_handler = virtio_console_rxq_notify_handler;
        vq[CONSOLE_QUEUE_TX].notify_handler = virtio_console_txq_notify_handler;
        vdev->vqs = vq;
        break;
    default:
        break;
    }
}

void init_mmio_regs(VirtMmioRegs *regs, VirtioDeviceType type)
{
    regs->device_id = type;
    regs->queue_sel = 0;
}

void virtio_dev_reset(VirtIODevice *vdev)
{
    // When driver read first 4 encoded messages, it will reset dev.
    log_trace("virtio dev reset");
    vdev->regs.status = 0;
    vdev->regs.interrupt_status = 0;
    vdev->regs.interrupt_count = 0;
    int idx = vdev->regs.queue_sel;
    vdev->vqs[idx].ready = 0;
    for(uint32_t i=0; i<vdev->vqs_len; i++) {
        virtqueue_reset(&vdev->vqs[i], i);
    }
    vdev->activated = false;
}

void virtqueue_reset(VirtQueue *vq, int idx)
{
    // reserve these fields
    void *addr = vq->notify_handler;
    VirtIODevice *dev = vq->dev;
    uint32_t queue_num_max = vq->queue_num_max;
    memset(vq, 0, sizeof(VirtQueue));
    vq->vq_idx = idx;
    vq->notify_handler = addr;
    vq->dev = dev;
    vq->queue_num_max = queue_num_max;
	pthread_mutex_init(&vq->used_ring_lock, NULL);
}

// check if virtqueue has new requests
bool virtqueue_is_empty(VirtQueue *vq)
{
    if(vq->avail_ring == NULL) {
        log_error("virtqueue's avail ring is invalid");
        return true;
    }
	// read_barrier();
	log_debug("vq->last_avail_idx is %d, vq->avail_ring->idx is %d", vq->last_avail_idx, vq->avail_ring->idx);
    if (vq->last_avail_idx == vq->avail_ring->idx)
        return true;
    else
        return false;
}

bool desc_is_writable(volatile VirtqDesc *desc_table, uint16_t idx)
{
    if (desc_table[idx].flags & VRING_DESC_F_WRITE)
        return true;
    return false;
}

void* get_virt_addr(void *addr)
{
    return virt_addr - phys_addr + addr;
}

// get non root linux's ipa
void* get_phys_addr(void *addr)
{
    return addr - virt_addr + phys_addr;
}

// When virtio device is processing virtqueue, driver adding an elem to virtqueue is no need to notify device.
void virtqueue_disable_notify(VirtQueue *vq) {
	if (vq->event_idx_enabled) {
		VQ_AVAIL_EVENT(vq) = vq->last_avail_idx - 1;
	} else {
    	vq->used_ring->flags |= (uint16_t)VRING_USED_F_NO_NOTIFY;
	}
	write_barrier();
}

void virtqueue_enable_notify(VirtQueue *vq) {
	if (vq->event_idx_enabled) {
		VQ_AVAIL_EVENT(vq) = vq->avail_ring->idx;
	} else {
   		vq->used_ring->flags &= !(uint16_t)VRING_USED_F_NO_NOTIFY;
	} 
	write_barrier();
}

void virtqueue_set_desc_table(VirtQueue *vq)
{
    log_trace("desc table ipa is %#x", vq->desc_table_addr);
    vq->desc_table = (VirtqDesc *)(virt_addr + vq->desc_table_addr - phys_addr);
}

void virtqueue_set_avail(VirtQueue *vq)
{
    log_trace("avail ring ipa is %#x", vq->avail_addr);
    vq->avail_ring = (VirtqAvail *)(virt_addr + vq->avail_addr - phys_addr);
}

void virtqueue_set_used(VirtQueue *vq)
{
    log_trace("used ring ipa is %#x", vq->used_addr);
    vq->used_ring = (VirtqUsed *)(virt_addr + vq->used_addr - phys_addr);
}

// record one descriptor to iov.
static inline int descriptor2iov(int i, volatile VirtqDesc *vd,
           struct iovec *iov, uint16_t *flags) {
    void *host_addr;
    host_addr = get_virt_addr((void *)vd->addr);
    iov[i].iov_base = host_addr;
    iov[i].iov_len = vd->len;
    // log_debug("vd->addr ipa is %x, iov_base is %x, iov_len is %d", vd->addr, host_addr, vd->len);
    if (flags != NULL)
        flags[i] = vd->flags;
    return 0;
}

/// record one descriptor list to iov
/// \param desc_idx the first descriptor's idx in descriptor list.
/// \param iov the iov to record
/// \param flags each descriptor's flags
/// \param append_len the number of iovs to append
/// \return the len of iovs
int process_descriptor_chain(VirtQueue *vq, uint16_t *desc_idx,
                struct iovec **iov, uint16_t **flags, int append_len)
{
    uint16_t next, idx;
    volatile VirtqDesc *vdesc, *ind_table, *ind_desc;
	int chain_len = 0, i, table_len;
    idx = vq->last_avail_idx;
    if(idx == vq->avail_ring->idx)
        return 0;
    vq->last_avail_idx++;
    *desc_idx = next = vq->avail_ring->ring[idx & (vq->num - 1)];
	// record desc chain' len to chain_len
	for (i=0; i<(int)vq->num; i++, next = vdesc->next) {
        vdesc = &vq->desc_table[next];
		// TODO: vdesc->len may be not chain_len, virtio specification doesn't say it.
		if (vdesc->flags & VRING_DESC_F_INDIRECT) {
			chain_len += vdesc->len / 16;
			i--;
		}
		if ((vdesc->flags & VRING_DESC_F_NEXT) == 0)
            break;
	}

	chain_len += i + 1, next = *desc_idx;
	
	*iov = malloc(sizeof(struct iovec) * ( chain_len + append_len));
	if (flags != NULL)
		*flags = malloc(sizeof(uint16_t) * ( chain_len + append_len));

	for (i=0; i<chain_len; i++, next = vdesc->next) {
		vdesc = &vq->desc_table[next];
		if (vdesc->flags & VRING_DESC_F_INDIRECT) {
			ind_table = (VirtqDesc *)(get_virt_addr((void *)vdesc->addr));
			table_len = vdesc->len / 16;
			log_debug("table_len is %d", table_len);
			next = 0;
			for (;;) {
				log_debug("next is %d", next);
				ind_desc = &ind_table[next];
				descriptor2iov(i, ind_desc, *iov, flags == NULL ? NULL : *flags);
				table_len--;
				i++;
				if ((ind_desc->flags & VRING_DESC_F_NEXT) == 0)
            		break;
				next = ind_desc->next;
			}
			if (table_len != 0) {
				log_error("invalid indirect descriptor chain");
				break;
			}
		} else {
			descriptor2iov(i, vdesc, *iov, flags == NULL ? NULL : *flags);
		}
	}
    return chain_len;
}

void update_used_ring(VirtQueue *vq, uint16_t idx, uint32_t iolen)
{
    volatile VirtqUsed *used_ring;
    volatile VirtqUsedElem *elem;
    uint16_t used_idx, mask;
	// There is no need to worry about if used_ring is full, because used_ring's len is equal to descriptor table's. 
    write_barrier();
	// pthread_mutex_lock(&vq->used_ring_lock);
    used_ring = vq->used_ring;
    used_idx = used_ring->idx;
    mask = vq->num - 1;
    elem = &used_ring->ring[used_idx++ & mask];
    elem->id = idx;
    elem->len = iolen;
    used_ring->idx = used_idx;
	write_barrier();
	// pthread_mutex_unlock(&vq->used_ring_lock);
    log_debug("update used ring: used_idx is %d, elem->idx is %d, vq->num is %d", used_idx, idx, vq->num);
}

static uint64_t virtio_mmio_read(VirtIODevice *vdev, uint64_t offset, unsigned size)
{
    log_debug("virtio mmio read at %#x", offset);
    if (!vdev) {
        switch (offset) {
        case VIRTIO_MMIO_MAGIC_VALUE:
            return VIRT_MAGIC;
        case VIRTIO_MMIO_VERSION:
            return VIRT_VERSION;
        case VIRTIO_MMIO_VENDOR_ID:
            return VIRT_VENDOR;
        default:
            return 0;
        }
    }

    if (offset >= VIRTIO_MMIO_CONFIG) {
        offset -= VIRTIO_MMIO_CONFIG;
        // the first member of vdev->dev must be config.
        return *(uint64_t *)(vdev->dev + offset);
    }

    if (size != 4) {
        log_error("virtio-mmio-read: wrong size access to register!");
        return 0;
    }

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRT_MAGIC;
    case VIRTIO_MMIO_VERSION:
        return VIRT_VERSION;
    case VIRTIO_MMIO_DEVICE_ID:
        return vdev->regs.device_id;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRT_VENDOR;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (vdev->regs.dev_feature_sel) {
            return vdev->regs.dev_feature >> 32;
        } else {
            return vdev->regs.dev_feature;
        }
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return vdev->vqs[vdev->regs.queue_sel].queue_num_max;
    case VIRTIO_MMIO_QUEUE_READY:
        return vdev->vqs[vdev->regs.queue_sel].ready;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
		if (vdev->regs.interrupt_status == 0) {
			log_error("virtio-mmio-read: interrupt status is 0, type is %d", vdev->type);
		}
        return vdev->regs.interrupt_status;
    case VIRTIO_MMIO_STATUS:
        return vdev->regs.status;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        return vdev->regs.generation;
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
    case VIRTIO_MMIO_DRIVER_FEATURES:
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
    case VIRTIO_MMIO_QUEUE_SEL:
    case VIRTIO_MMIO_QUEUE_NUM:
    case VIRTIO_MMIO_QUEUE_NOTIFY:
    case VIRTIO_MMIO_INTERRUPT_ACK:
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
    case VIRTIO_MMIO_QUEUE_USED_LOW:
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        log_error("read of write-only register");
        return 0;
    default:
        log_error("bad register offset %#x", offset);
        return 0;
    }
    return 0;
}

static void virtio_mmio_write(VirtIODevice *vdev, uint64_t offset, uint64_t value, unsigned size)
{
    log_debug("virtio mmio write at %#x, value is %#x\n", offset, value);
    VirtMmioRegs *regs = &vdev->regs;
    VirtQueue *vqs = vdev->vqs;
    if (!vdev) {
        return;
    }

    if (offset >= VIRTIO_MMIO_CONFIG) {
        offset -= VIRTIO_MMIO_CONFIG;
        log_error("virtio_mmio_write: can't write config space");
        return;
    }
    if (size != 4) {
        log_error("virtio_mmio_write: wrong size access to register!");
        return;
    }

    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        if (value) {
            regs->dev_feature_sel = 1;
        } else {
            regs->dev_feature_sel = 0;
        }
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (regs->drv_feature_sel) {
            regs->drv_feature |= value << 32;
        } else {
            regs->drv_feature |= value;
        }
		if (regs->drv_feature & (1ULL << VIRTIO_RING_F_EVENT_IDX)) {
			int len = vdev->vqs_len;
			for (int i=0; i<len; i++) 
				vqs[i].event_idx_enabled = 1;
		}
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        if (value) {
            regs->drv_feature_sel = 1;
        } else {
            regs->drv_feature_sel = 0;
        }
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        if (value < vdev->vqs_len) {
            regs->queue_sel = value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        vqs[regs->queue_sel].num = value;
        log_trace("virtqueue num is %d", value);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        vqs[regs->queue_sel].ready = value;
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        log_debug("queue notify begin");
        if (value < vdev->vqs_len) {
            log_trace("queue notify ready, handler addr is %#x", vqs[value].notify_handler);
            vqs[value].notify_handler(vdev, &vqs[value]);
        }
        log_debug("queue notify end");
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        if (value == regs->interrupt_status && regs->interrupt_count > 0) {
            regs->interrupt_count --;
            break;
        } else if (value != regs->interrupt_status) {
            log_error("interrupt_status is not equal to ack, type is %d", vdev->type);
        }
        regs->interrupt_status &= !value;
        break;
    case VIRTIO_MMIO_STATUS:
        regs->status = value;
        if (regs->status == 0) {
            virtio_dev_reset(vdev);
        }
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        vqs[regs->queue_sel].desc_table_addr |= value & UINT32_MAX;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        vqs[regs->queue_sel].desc_table_addr |= value << 32;
        virtqueue_set_desc_table(&vqs[regs->queue_sel]);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        vqs[regs->queue_sel].avail_addr |= value & UINT32_MAX;
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        vqs[regs->queue_sel].avail_addr |= value << 32;
        virtqueue_set_avail(&vqs[regs->queue_sel]);
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        vqs[regs->queue_sel].used_addr |= value & UINT32_MAX;
        break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        vqs[regs->queue_sel].used_addr |= value << 32;
        virtqueue_set_used(&vqs[regs->queue_sel]);
        break;
    case VIRTIO_MMIO_MAGIC_VALUE:
    case VIRTIO_MMIO_VERSION:
    case VIRTIO_MMIO_DEVICE_ID:
    case VIRTIO_MMIO_VENDOR_ID:
    case VIRTIO_MMIO_DEVICE_FEATURES:
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
    case VIRTIO_MMIO_INTERRUPT_STATUS:
    case VIRTIO_MMIO_CONFIG_GENERATION:
        log_error("%s: write to read-only register 0#x", __func__, offset);
        break;

    default:
        log_error("%s: bad register offset 0#x", __func__, offset);
    }
}

static inline bool in_range(uint64_t value, uint64_t lower, uint64_t len)
{
    return ((value >= lower) && (value < (lower + len)));
}

// Inject irq_id to target zone. It will add to res list, and notify hypervisor through ioctl.
void virtio_inject_irq(VirtQueue *vq)
{
	uint16_t last_used_idx, idx, event_idx;
	last_used_idx = vq->last_used_idx;
	vq->last_used_idx = idx = vq->used_ring->idx;
	// read_barrier();
	if (idx == last_used_idx) {
		log_debug("idx equals last_used_idx");
		return ;
	}
    if (!vq->event_idx_enabled && (vq->avail_ring->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
		log_debug("no interrupt");
		return ;
	}
	if (vq->event_idx_enabled) {
		event_idx = VQ_USED_EVENT(vq);
		log_debug("idx is %d, event_idx is %d, last_used_idx is %d", idx, event_idx, last_used_idx);
		if(!vring_need_event(event_idx, idx, last_used_idx)) {
			return;
		}
	}
    volatile struct device_res *res;
    while (is_queue_full(virtio_bridge->res_front, virtio_bridge->res_rear, MAX_REQ));
    pthread_mutex_lock(&RES_MUTEX);
    unsigned int res_rear = virtio_bridge->res_rear;
    res = &virtio_bridge->res_list[res_rear];
    res->irq_id = vq->dev->irq_id;
    res->target_zone = vq->dev->zone_id;
    write_barrier();
    virtio_bridge->res_rear = (res_rear + 1) & (MAX_REQ - 1);
    write_barrier();
	vq->dev->regs.interrupt_status = VIRTIO_MMIO_INT_VRING;
    vq->dev->regs.interrupt_count ++;
    pthread_mutex_unlock(&RES_MUTEX);
	log_debug("inject irq to device %d, vq is %d", vq->dev->type, vq->vq_idx);
    ioctl(ko_fd, HVISOR_FINISH_REQ);
}

static void virtio_finish_cfg_req(uint32_t target_cpu, uint64_t value) {
    virtio_bridge->cfg_values[target_cpu] = value;
    write_barrier();
    virtio_bridge->cfg_flags[target_cpu]++;
    write_barrier();
}

static int virtio_handle_req(volatile struct device_req *req)
{
    int i;
    uint64_t value = 0;
    for (i = 0; i < vdevs_num; ++i) {
        if ((req->src_zone == vdevs[i]->zone_id) && in_range(req->address, vdevs[i]->base_addr, vdevs[i]->len))
            break;
    }
    if (i == vdevs_num) {
        log_error("no matched virtio dev");
        return -1;
    }
    VirtIODevice *vdev = vdevs[i];
    if (vdev->type == VirtioTNet)
        log_debug("vdev type is net");
    else if (vdev->type == VirtioTBlock)
        log_debug("vdev type is blk");
    else if (vdev->type == VirtioTConsole)
        log_debug("vdev type is con");
    uint64_t offs = req->address - vdev->base_addr;
    if (req->is_write) {
        virtio_mmio_write(vdev, offs, req->value, req->size);
    } else {
        value = virtio_mmio_read(vdev, offs, req->size);
        log_debug("read value is 0x%x\n", value);
    }
    if (!req->need_interrupt) {
        // If a request is a control not a data request
        virtio_finish_cfg_req(req->src_cpu, value);
    } 
    log_trace("src_zone is %d, src_cpu is %lld", req->src_zone, req->src_cpu);
    return 0;
}

static void virtio_close() {
	log_info("virtio devices will be closed");
	destroy_event_monitor();
	for(int i=0; i<vdevs_num; i++)
        vdevs[i]->virtio_close(vdevs[i]);
	close(ko_fd);
	munmap((void *)virtio_bridge, MMAP_SIZE);
	munmap((void *)virt_addr, NON_ROOT_PHYS_SIZE);
	mutithread_log_exit();
	log_warn("virtio daemon exit successfully");
}

void handle_virtio_requests()
{
	int sig;
	sigset_t wait_set;
	struct timespec timeout;
    unsigned int req_front = virtio_bridge->req_front;
    volatile struct device_req *req;
	timeout.tv_sec = 0;
	timeout.tv_nsec = WAIT_TIME;
	sigemptyset(&wait_set);
	sigaddset(&wait_set, SIGHVI);
	sigaddset(&wait_set, SIGTERM);
	virtio_bridge->need_wakeup = 1;
	
	int signal_count = 0, proc_count = 0;
	unsigned long long count = 0;
	for (;;) {
		log_warn("signal_count is %d, proc_count is %d", signal_count, proc_count);
		sigwait(&wait_set, &sig);
		sig = SIGHVI;
		signal_count++;
		if (sig == SIGTERM) {
			virtio_close();
			break;
		} else if (sig != SIGHVI) {
			log_error("unknown signal %d", sig);
			continue;
		}
		while(1) {
			// read_barrier();
			if (!is_queue_empty(req_front, virtio_bridge->req_rear)) {
				count = 0;
				proc_count++;
				req = &virtio_bridge->req_list[req_front];
				virtio_bridge->need_wakeup = 0;
				virtio_handle_req(req);
				req_front = (req_front + 1) & (MAX_REQ - 1);
				virtio_bridge->req_front = req_front;
				write_barrier();
			} 
			else {
				count++;
				if (count < 10000000) 
					continue;
				count = 0;
				virtio_bridge->need_wakeup = 1;
				write_barrier();
				nanosleep(&timeout, NULL);
				read_barrier();
				if(is_queue_empty(req_front, virtio_bridge->req_rear)) {
					break;
				} 		
			}
		}
	}
}

int virtio_init()
{
    // The higher log level is , faster virtio-blk will be.
    int err;
	int log_level = LOG_WARN;

	sigset_t block_mask;
	sigfillset(&block_mask);
	pthread_sigmask(SIG_BLOCK, &block_mask, NULL);

	multithread_log_init();
    log_set_level(log_level);
    FILE *log_file = fopen("log.txt", "w+");
    log_add_fp(log_file, LOG_WARN);
    log_info("hvisor init");
    ko_fd = open("/dev/hvisor", O_RDWR);
    if (ko_fd < 0) {
        log_error("open hvisor failed");
        exit(1);
    }
    // ioctl for init virtio
    err = ioctl(ko_fd, HVISOR_INIT_VIRTIO);
    if (err) {
        log_error("ioctl failed, err code is %d", err);
        close(ko_fd);
        exit(1);
    }

    // mmap: create shared memory
    virtio_bridge = (struct virtio_bridge *) mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ko_fd, 0);
    if (virtio_bridge == (void *)-1) {
        log_error("mmap failed");
        goto unmap;
    }

	// mmap: map non root linux physical memory to virtual memory
    phys_addr = (void *)NON_ROOT_PHYS_START;
    virt_addr = mmap(NULL, NON_ROOT_PHYS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED , ko_fd, (off_t) phys_addr);
    log_info("mmap virt addr is %#x", virt_addr);

    initialize_event_monitor();
    log_info("hvisor init okay!");
	return 0;
unmap:
    munmap((void *)virtio_bridge, MMAP_SIZE);
    return -1;
}

static int create_virtio_device_from_cmd(char *cmd) {
	log_info("cmd is %s", cmd);
	VirtioDeviceType dev_type = VirtioTNone;
	uint64_t base_addr = 0, len = 0;
	uint32_t zone_id = 0, irq_id = 0;
	char *opt, *now, *arg = NULL;

	opt = strdup(cmd);
	now = strtok(opt, ",");

	if (strcmp(now, "blk") == 0) {
		dev_type = VirtioTBlock;
	} else if (strcmp(now, "net") == 0) {
		dev_type = VirtioTNet;
	} else if (strcmp(now, "console") == 0) {
        dev_type = VirtioTConsole;
    } else {
		log_error("unknown device type %s", now);
		return -1;
	}

	while ((now = strtok(NULL, "=")) != NULL) {
		if (strcmp(now, "addr") == 0) {
			now = strtok(NULL, ",");
			base_addr = strtoul(now, NULL, 16);
		} else if (strcmp(now, "len") == 0) {
			now = strtok(NULL, ",");
			len = strtoul(now, NULL, 16);
		} else if (strcmp(now, "irq") == 0) {
			now = strtok(NULL, ",");
			irq_id = strtoul(now, NULL, 10);
		} else if (strcmp(now, "zone_id") == 0) {
			now = strtok(NULL, ",");
			zone_id = strtoul(now, NULL, 10);
		} else if (strcmp(now, "img") == 0) {
			if (dev_type != VirtioTBlock) {
				log_error("image path only for block device");
				return -1;
			}
			arg = strtok(NULL, ",");
		} else if (strcmp(now, "tap") == 0) {
			if (dev_type != VirtioTNet) {
				log_error("tap only for net device");
				return -1;
			}
			arg = strtok(NULL, ",");
		} else {
			log_error("unknown option %s", now);
			return -1;
		}
	}
	free(opt);

	if (base_addr == 0 || len == 0 || irq_id == 0 || zone_id == 0) {
		log_error("missing arguments");
		return -1;
	}
	create_virtio_device(dev_type, zone_id, base_addr, len, irq_id, arg);
	return 0;
}

int virtio_start(int argc, char *argv[]) {
	static struct option long_options[] = {
		{"device", required_argument, 0, 'd'},	
		{0, 0, 0, 0},
	};
	char *optstring = "d:";
	int opt, err = 0;
	virtio_init();
	while ( (opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
		switch (opt) {
			case 'd':
				err = create_virtio_device_from_cmd(optarg);
				if (err) {
					log_error("create virtio device failed");
					goto err_out;
				}
				break;
			default:
				log_error("unknown option %c", opt);
				goto err_out;
		}
	}
	for (int i=0; i<vdevs_num; i++) {
		virtio_bridge->mmio_addrs[i] = vdevs[i]->base_addr;	
	}
	write_barrier();
	virtio_bridge->mmio_avail = 1;
	write_barrier();
    handle_virtio_requests();
	return 0;
err_out:
	virtio_close();
	return err;
}

