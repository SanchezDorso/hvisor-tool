#ifndef _HVISOR_VIRTIO_CONSOLE_H
#define _HVISOR_VIRTIO_CONSOLE_H
#include <linux/virtio_console.h>
#include "event_monitor.h"
#include "virtio.h"

#define CONSOLE_SUPPORTED_FEATURES ((1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_CONSOLE_F_SIZE))
#define CONSOLE_MAX_QUEUES 2
#define VIRTQUEUE_CONSOLE_MAX_SIZE 64
#define CONSOLE_QUEUE_RX 0
#define CONSOLE_QUEUE_TX 1

typedef struct virtio_console_config ConsoleConfig;
typedef struct virtio_console_dev {
    ConsoleConfig config;
    int master_fd;
    int rx_ready;
    struct hvisor_event *event;
} ConsoleDev;

ConsoleDev *init_console_dev();
int virtio_console_init(VirtIODevice *vdev);
int virtio_console_rxq_notify_handler(VirtIODevice *vdev, VirtQueue *vq);
int virtio_console_txq_notify_handler(VirtIODevice *vdev, VirtQueue *vq);
void virtio_console_close(VirtIODevice *vdev);
#endif