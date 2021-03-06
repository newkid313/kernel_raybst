/* /linux/drivers/misc/modem_if/modem_io_device.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* #define DEBUG */
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/if_ether.h>
#include <linux/etherdevice.h>
#include <linux/ratelimit.h>

#include <linux/platform_data/modem.h>
#include "modem_prj.h"

#define HDLC_START	0x7F
#define HDLC_END	0x7E
#define SIZE_OF_HDLC_START	1
#define SIZE_OF_HDLC_END	1
#define MAX_RXDATA_SIZE		(4096 - 512)

static const char hdlc_start[1] = { HDLC_START };
static const char hdlc_end[1] = { HDLC_END };

struct fmt_hdr {
	u16 len;
	u8 control;
} __packed;

struct raw_hdr {
	u32 len;
	u8 channel;
	u8 control;
} __packed;

struct rfs_hdr {
	u32 len;
	u8 cmd;
	u8 id;
} __packed;

static const char const *modem_state_name[] = {
	[STATE_OFFLINE]		= "OFFLINE",
	[STATE_CRASH_EXIT]	= "CRASH_EXIT",
	[STATE_BOOTING]		= "BOOTING",
	[STATE_ONLINE]		= "ONLINE",
	[STATE_LOADER_DONE]	= "LOADER_DONE",
};

static int rx_iodev_skb(struct io_device *iod);

static int get_header_size(struct io_device *iod)
{
	switch (iod->format) {
	case IPC_FMT:
		return sizeof(struct fmt_hdr);

	case IPC_RAW:
	case IPC_MULTI_RAW:
		return sizeof(struct raw_hdr);

	case IPC_RFS:
		return sizeof(struct rfs_hdr);

	case IPC_BOOT:
		/* minimum size for transaction align */
		return 4;

	case IPC_RAMDUMP:
	default:
		return 0;
	}
}

static int get_hdlc_size(struct io_device *iod, char *buf)
{
	struct fmt_hdr *fmt_header;
	struct raw_hdr *raw_header;
	struct rfs_hdr *rfs_header;

	pr_debug("[IOD] <%s> buf : %02x %02x %02x (%d)\n",
		__func__, *buf, *(buf + 1), *(buf + 2), __LINE__);

	switch (iod->format) {
	case IPC_FMT:
		fmt_header = (struct fmt_hdr *)buf;
		return fmt_header->len;
	case IPC_RAW:
	case IPC_MULTI_RAW:
		raw_header = (struct raw_hdr *)buf;
		return raw_header->len;
	case IPC_RFS:
		rfs_header = (struct rfs_hdr *)buf;
		return rfs_header->len;
	default:
		break;
	}
	return 0;
}

static void *get_header(struct io_device *iod, size_t count,
			char *frame_header_buf)
{
	struct fmt_hdr *fmt_h;
	struct raw_hdr *raw_h;
	struct rfs_hdr *rfs_h;

	switch (iod->format) {
	case IPC_FMT:
		fmt_h = (struct fmt_hdr *)frame_header_buf;

		fmt_h->len = count + sizeof(struct fmt_hdr);
		fmt_h->control = 0;

		return (void *)frame_header_buf;

	case IPC_RAW:
	case IPC_MULTI_RAW:
		raw_h = (struct raw_hdr *)frame_header_buf;

		raw_h->len = count + sizeof(struct raw_hdr);
		raw_h->channel = iod->id & 0x1F;
		raw_h->control = 0;

		return (void *)frame_header_buf;

	case IPC_RFS:
		rfs_h = (struct rfs_hdr *)frame_header_buf;

		rfs_h->len = count + sizeof(struct raw_hdr);
		rfs_h->id = iod->id;

		return (void *)frame_header_buf;

	default:
		return 0;
	}
}

static inline int rx_hdlc_head_start_check(char *buf)
{
	/* check hdlc head and return size of start byte */
	return (buf[0] == HDLC_START) ? SIZE_OF_HDLC_START : -EBADMSG;
}

static inline int rx_hdlc_tail_check(char *buf)
{
	/* check hdlc tail and return size of tail byte */
	return (buf[0] == HDLC_END) ? SIZE_OF_HDLC_END : -EBADMSG;
}

/* remove hdlc header and store IPC header */
static int rx_hdlc_head_check(struct io_device *iod, char *buf, unsigned rest)
{
	struct header_data *hdr = &iod->h_data;
	int head_size = get_header_size(iod);
	int done_len = 0;
	int len = 0;

	/* first frame, remove start header 7F */
	if (!hdr->start) {
		len = rx_hdlc_head_start_check(buf);
		if (len < 0) {
			pr_err("[IOD] <%s> Wrong HDLC start: 0x%x(%s)\n",
				__func__, *buf, iod->name);
			return len; /*Wrong hdlc start*/
		}

		pr_debug("[IOD] <%s> check len : %d, rest : %d (%d)\n",
			__func__, len, rest, __LINE__);

		/* set the start flag of current packet */
		hdr->start = HDLC_START;
		hdr->len = 0;

		buf += len;
		done_len += len;
		rest -= len; /* rest, call by value */
	}

	pr_debug("[IOD] <%s> check len : %d, rest : %d (%d)\n",
		__func__, len, rest, __LINE__);

	/* store the IPC header to iod priv */
	if (hdr->len < head_size) {
		len = min(rest, head_size - hdr->len);
		memcpy(hdr->hdr + hdr->len, buf, len);
		hdr->len += len;
		done_len += len;
	}

	pr_debug("[IOD] <%s> check done_len : %d, rest : %d (%d)\n",
		__func__, done_len, rest, __LINE__);
	return done_len;
}

/* alloc skb and copy dat to skb */
static int rx_hdlc_data_check(struct io_device *iod, char *buf, unsigned rest)
{
	struct header_data *hdr = &iod->h_data;
	struct sk_buff *skb = iod->skb_recv;
	int head_size = get_header_size(iod);
	int data_size = get_hdlc_size(iod, hdr->hdr) - head_size;
	int alloc_size = min(data_size, MAX_RXDATA_SIZE);
	int len;
	int done_len = 0;
	int rest_len = data_size - hdr->flag_len;

	/* first payload data - alloc skb */
	if (!skb) {
		switch (iod->format) {
		case IPC_RFS:
			alloc_size =
				min(data_size + head_size, MAX_RXDATA_SIZE);
			skb = alloc_skb(alloc_size, GFP_ATOMIC);
			if (unlikely(!skb))
				return -ENOMEM;
			/* copy the RFS haeder to skb->data */
			memcpy(skb_put(skb, head_size), hdr->hdr, head_size);
			break;

		case IPC_MULTI_RAW:
			if (data_size > MAX_RXDATA_SIZE) {
				pr_err("[IOD] <%s> %s: packet size too large (%d)\n",
					__func__, iod->name, data_size);
				return -EINVAL;
			}

			if (iod->use_handover)
				skb = alloc_skb(alloc_size +
					sizeof(struct ethhdr), GFP_ATOMIC);
			else
				skb = alloc_skb(alloc_size, GFP_ATOMIC);
			if (unlikely(!skb))
				return -ENOMEM;

			if (iod->use_handover)
				skb_reserve(skb, sizeof(struct ethhdr));
			break;

		default:
			skb = alloc_skb(alloc_size, GFP_ATOMIC);
			if (unlikely(!skb))
				return -ENOMEM;
			break;
		}
		iod->skb_recv = skb;
	}

	while (rest > 0) {
		len = min(rest,  alloc_size - skb->len);
		len = min(len, rest_len);
		memcpy(skb_put(skb, len), buf, len);
		buf += len;
		done_len += len;
		hdr->flag_len += len;
		rest -= len;
		rest_len -= len;

		if (!rest_len || !rest)
			break;

		rx_iodev_skb(iod);
		iod->skb_recv =  NULL;

		alloc_size = min(rest_len, MAX_RXDATA_SIZE);
		skb = alloc_skb(alloc_size, GFP_ATOMIC);
		if (unlikely(!skb))
			return -ENOMEM;
		iod->skb_recv = skb;
	}

	return done_len;
}

static int rx_iodev_skb_raw(struct io_device *iod)
{
	int err;
	struct sk_buff *skb = iod->skb_recv;
	struct net_device *ndev;
	struct iphdr *ip_header;
	struct ethhdr *ehdr;
	const char source[ETH_ALEN] = SOURCE_MAC_ADDR;

	switch (iod->io_typ) {
	case IODEV_MISC:
		skb_queue_tail(&iod->sk_rx_q, iod->skb_recv);
		wake_up(&iod->wq);
		return 0;

	case IODEV_NET:
		ndev = iod->ndev;
		if (!ndev)
			return NET_RX_DROP;

		skb->dev = ndev;
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += skb->len;

		/* check the version of IP */
		ip_header = (struct iphdr *)skb->data;
		if (ip_header->version == IP6VERSION)
			skb->protocol = htons(ETH_P_IPV6);
		else
			skb->protocol = htons(ETH_P_IP);

		if (iod->use_handover) {
			ehdr = (void *)skb_push(skb, sizeof(struct ethhdr));
			memcpy(ehdr->h_dest, ndev->dev_addr, ETH_ALEN);
			memcpy(ehdr->h_source, source, ETH_ALEN);
			ehdr->h_proto = skb->protocol;
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			skb_reset_mac_header(skb);

			skb_pull(skb, sizeof(struct ethhdr));
		}

		err = netif_rx_ni(skb);
		if (err != NET_RX_SUCCESS)
			dev_err(&ndev->dev, "rx error: %d\n", err);
		return err;

	default:
		pr_err("[IOD] <%s> wrong io_type : %d\n",
			__func__, iod->io_typ);
		return -EINVAL;
	}
}

static void rx_iodev_work(struct work_struct *work)
{
	int ret;
	struct sk_buff *skb;
	struct io_device *real_iod;
	struct io_device *iod = container_of(work, struct io_device,
				rx_work.work);

	skb = skb_dequeue(&iod->sk_rx_q);
	while (skb) {
		real_iod = *((struct io_device **)skb->cb);
		real_iod->skb_recv = skb;

		ret = rx_iodev_skb_raw(real_iod);
		if (ret == NET_RX_DROP) {
			pr_err("[IOD] <%s> queue delayed work!\n", __func__);
			skb_queue_head(&iod->sk_rx_q, skb);
			schedule_delayed_work(&iod->rx_work,
				msecs_to_jiffies(20));
			break;
		} else if (ret < 0)
			dev_kfree_skb_any(skb);

		skb = skb_dequeue(&iod->sk_rx_q);
	}
}

static int rx_multipdp(struct io_device *iod)
{
	u8 ch;
	struct raw_hdr *raw_header = (struct raw_hdr *)&iod->h_data.hdr;
	struct io_raw_devices *io_raw_devs =
				(struct io_raw_devices *)iod->private_data;
	struct io_device *real_iod;

	ch = raw_header->channel;
	real_iod = io_raw_devs->raw_devices[ch];
	if (!real_iod) {
		pr_err("[IOD] <%s> wrong channel %d\n", __func__, ch);
		return -EINVAL;
	}

	*((struct io_device **)iod->skb_recv->cb) = real_iod;
	skb_queue_tail(&iod->sk_rx_q, iod->skb_recv);
	pr_debug("[IOD] <%s> sk_rx_qlen:%d\n", __func__, iod->sk_rx_q.qlen);

	schedule_delayed_work(&iod->rx_work, 0);
	return 0;
}

/* de-mux function draft */
static int rx_iodev_skb(struct io_device *iod)
{
	switch (iod->format) {
	case IPC_MULTI_RAW:
		return rx_multipdp(iod);

	case IPC_FMT:
	case IPC_RFS:
	default:
		skb_queue_tail(&iod->sk_rx_q, iod->skb_recv);
		wake_up(&iod->wq);
		pr_debug("[IOD] <%s> wake up fmt,rfs skb\n", __func__);
		return 0;
	}
}

static int rx_hdlc_packet(struct io_device *iod, const char *data,
		unsigned recv_size)
{
	unsigned rest = recv_size;
	char *buf = (char *)data;
	int err = 0;
	int len;

	if (rest <= 0)
		goto exit;

	pr_debug("[IOD] <%s> RX_SIZE=%d\n", __func__, rest);
	if (iod->h_data.flag_len)
		goto data_check;

next_frame:
	err = len = rx_hdlc_head_check(iod, buf, rest);
	if (err < 0)
		goto exit; /* buf++; rest--; goto next_frame; */
	pr_debug("[IOD] <%s> check len=%d, rest=%d (%d)\n",
		__func__, len, rest, __LINE__);

	buf += len;
	rest -= len;
	if (rest <= 0)
		goto exit;

data_check:
	err = len = rx_hdlc_data_check(iod, buf, rest);
	if (err < 0)
		goto exit;
	pr_debug("[IOD] <%s> check len=%d, rest=%d (%d)\n",
		__func__, len, rest, __LINE__);

	buf += len;
	rest -= len;

	if (!rest && iod->h_data.flag_len)
		return 0;
	else if (rest <= 0)
		goto exit;

	err = len = rx_hdlc_tail_check(buf);
	if (err < 0) {
		pr_err("[IOD] <%s> Wrong HDLC end: 0x%x(%s)\n",
			__func__, *buf, iod->name);
		goto exit;
	}
	pr_debug("[IOD] <%s> check len=%d, rest=%d (%d)\n",
		__func__, len, rest, __LINE__);

	buf += len;
	rest -= len;

	err = rx_iodev_skb(iod);
	if (err < 0)
		goto exit;

	/* initialize header & skb */
	iod->skb_recv = NULL;
	memset(&iod->h_data, 0x00, sizeof(struct header_data));

	if (rest)
		goto next_frame;

exit:
	if (err < 0) {
		/* clear headers */
		memset(&iod->h_data, 0x00, sizeof(struct header_data));

		if (iod->skb_recv) {
			dev_kfree_skb_any(iod->skb_recv);
			iod->skb_recv = NULL;
		}
	}

	return err;
}

/* called from link device when a packet arrives for this io device */
static int io_dev_recv_data_from_link_dev(struct io_device *iod,
			const char *data, unsigned int len)
{
	struct sk_buff *skb;
	int err;

	switch (iod->format) {
	case IPC_FMT:
	case IPC_RAW:
	case IPC_RFS:
	case IPC_MULTI_RAW:
		err = rx_hdlc_packet(iod, data, len);
		if (err < 0)
			pr_err("[IOD] <%s> fail process hdlc fram\n", __func__);
		return err;

	case IPC_CMD:
		/* TODO- handle flow control command from CP */
		return 0;

	case IPC_BOOT:
	case IPC_RAMDUMP:
		/* save packet to sk_buff */
		skb = alloc_skb(len, GFP_ATOMIC);
		if (!skb) {
			pr_err("[IOD] <%s> fail alloc skb (%d)\n",
				__func__, __LINE__);
			return -ENOMEM;
		}

		pr_debug("[IOD] <%s> boot len : %d\n", __func__, len);

		memcpy(skb_put(skb, len), data, len);
		skb_queue_tail(&iod->sk_rx_q, skb);
		pr_debug("[IOD] <%s> skb len : %d\n", __func__, skb->len);

		wake_up(&iod->wq);
		return len;

	default:
		return -EINVAL;
	}
}

/* inform the IO device that the modem is now online or offline or
 * crashing or whatever...
 */
static void io_dev_modem_state_changed(struct io_device *iod,
			enum modem_state state)
{
	iod->mc->phone_state = state;
	pr_info("[IOD] <%s> %s state changed: %s\n",
		__func__, iod->name, modem_state_name[state]);

	if ((state == STATE_CRASH_EXIT)
		|| (state == STATE_NV_REBUILDING))
		wake_up(&iod->wq);
}

static int misc_open(struct inode *inode, struct file *filp)
{
	struct io_device *iod = to_io_device(filp->private_data);
	filp->private_data = (void *)iod;

	pr_info("[IOD] <%s> Open node : %s\n", __func__, iod->name);

	if (iod->link->init_comm)
		return iod->link->init_comm(iod->link, iod);
	return 0;
}

static int misc_release(struct inode *inode, struct file *filp)
{
	struct io_device *iod = (struct io_device *)filp->private_data;

	pr_info("[IOD] <%s> Release node : %s\n", __func__, iod->name);

	if (iod->link->terminate_comm)
		iod->link->terminate_comm(iod->link, iod);

	skb_queue_purge(&iod->sk_rx_q);
	return 0;
}

static unsigned int misc_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct io_device *iod = (struct io_device *)filp->private_data;

	poll_wait(filp, &iod->wq, wait);

	if ((!skb_queue_empty(&iod->sk_rx_q))
				&& (iod->mc->phone_state != STATE_OFFLINE))
		return POLLIN | POLLRDNORM;
	else if ((iod->mc->phone_state == STATE_CRASH_EXIT) ||
			(iod->mc->phone_state == STATE_NV_REBUILDING))
		return POLLHUP;
	else
		return 0;
}

static long misc_ioctl(struct file *filp, unsigned int cmd, unsigned long _arg)
{
	int i;
	int p_state;
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct io_raw_devices *io_raw_devs;

	pr_info("[IOD] <%s> cmd 0x%x from %s\n", __func__, cmd, iod->name);

	switch (cmd) {
	case IOCTL_MODEM_ON:
		pr_info("[IOD] IOCTL_MODEM_ON\n");
		return iod->mc->ops.modem_on(iod->mc);

	case IOCTL_MODEM_OFF:
		pr_info("[IOD] IOCTL_MODEM_OFF\n");
		return iod->mc->ops.modem_off(iod->mc);

	case IOCTL_MODEM_RESET:
		pr_info("[IOD] IOCTL_MODEM_RESET\n");
		return iod->mc->ops.modem_reset(iod->mc);

	case IOCTL_MODEM_FORCE_CRASH_EXIT:
		pr_info("[IOD] IOCTL_MODEM_FORCE_CRASH_EXIT\n");
		return iod->mc->ops.modem_force_crash_exit(iod->mc);

	case IOCTL_MODEM_DUMP_RESET:
		pr_info("[IOD] IOCTL_MODEM_FORCE_CRASH_EXIT\n");
		return iod->mc->ops.modem_dump_reset(iod->mc);

	case IOCTL_MODEM_BOOT_ON:
		pr_info("[IOD] IOCTL_MODEM_BOOT_ON\n");
		return iod->mc->ops.modem_boot_on(iod->mc);

	case IOCTL_MODEM_BOOT_OFF:
		pr_info("[IOD] IOCTL_MODEM_BOOT_OFF\n");
		return iod->mc->ops.modem_boot_off(iod->mc);

	/* TODO - will remove this command after ril updated */
	case IOCTL_MODEM_START:
		pr_info("[IOD] IOCTL_MODEM_START\n");
		return 0;

	case IOCTL_MODEM_STATUS:
		pr_info("[IOD] IOCTL_MODEM_STATUS\n");

		p_state = iod->mc->phone_state;
		if (p_state == STATE_NV_REBUILDING) {
			pr_info("[IOD] <%s> send nv rebuild state : %d\n",
				__func__, p_state);
			iod->mc->phone_state = STATE_ONLINE;
		}
		return p_state;

	case IOCTL_MODEM_DUMP_START:
		pr_info("[IOD] IOCTL_MODEM_DUMP_START\n");
		return iod->link->dump_start(iod->link, iod);

	case IOCTL_MODEM_DUMP_UPDATE:
		pr_info("[IOD] IOCTL_MODEM_DUMP_UPDATE\n");
		return iod->link->dump_update(iod->link, iod, _arg);

	case IOCTL_MODEM_GOTA_START:
		pr_info("[IOD] IOCTL_MODEM_GOTA_START\n");
		return iod->link->gota_start(iod->link, iod);

	case IOCTL_MODEM_FW_UPDATE:
		pr_info("[IOD] IOCTL_MODEM_FW_UPDATE\n");
		return iod->link->modem_update(iod->link, iod, _arg);

	case IOCTL_MODEM_PROTOCOL_SUSPEND:
		pr_info("[IOD] IOCTL_MODEM_PROTOCOL_SUSPEND\n");

		if (iod->format != IPC_MULTI_RAW)
			return -EINVAL;
		io_raw_devs = (struct io_raw_devices *)iod->private_data;

		for (i = 0; i < MAX_RAW_DEVS; i++) {
			if (io_raw_devs->raw_devices[i] &&
			(io_raw_devs->raw_devices[i]->io_typ == IODEV_NET)) {
				netif_stop_queue(
					io_raw_devs->raw_devices[i]->ndev);
				pr_info("[IOD] <%s> netif stopped : %s\n",
					__func__,
					io_raw_devs->raw_devices[i]->name);
			}
		}
		return 0;

	case IOCTL_MODEM_PROTOCOL_RESUME:
		pr_info("[IOD] IOCTL_MODEM_PROTOCOL_RESUME\n");

		if (iod->format != IPC_MULTI_RAW)
			return -EINVAL;
		io_raw_devs = (struct io_raw_devices *)iod->private_data;

		for (i = 0; i < MAX_RAW_DEVS; i++) {
			if (io_raw_devs->raw_devices[i] &&
			(io_raw_devs->raw_devices[i]->io_typ == IODEV_NET)) {
				netif_wake_queue(
					io_raw_devs->raw_devices[i]->ndev);
				pr_info("[IOD] <%s> netif woke : %s\n",
					__func__,
					io_raw_devs->raw_devices[i]->name);
			}
		}
		return 0;

	default:
		return -EINVAL;
	}
}

static ssize_t misc_write(struct file *filp, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct io_device *iod = (struct io_device *)filp->private_data;
	int frame_len = 0;
	char frame_header_buf[sizeof(struct raw_hdr)];
	struct sk_buff *skb;

	/* TODO - check here flow control for only raw data */

	if (iod->format == IPC_BOOT || iod->format == IPC_RAMDUMP)
		frame_len = count + get_header_size(iod);
	else
		frame_len = count + SIZE_OF_HDLC_START + get_header_size(iod)
					+ SIZE_OF_HDLC_END;

	skb = alloc_skb(frame_len, GFP_KERNEL);
	if (!skb) {
		pr_err("[IOD] <%s> fail alloc skb\n", __func__);
		return -ENOMEM;
	}

	switch (iod->format) {
	case IPC_BOOT:
	case IPC_RAMDUMP:
		if (copy_from_user(skb_put(skb, count), buf, count) != 0) {
			dev_kfree_skb_any(skb);
			return -EFAULT;
		}
		break;

	case IPC_RFS:
		memcpy(skb_put(skb, SIZE_OF_HDLC_START), hdlc_start,
				SIZE_OF_HDLC_START);
		if (copy_from_user(skb_put(skb, count), buf, count) != 0) {
			dev_kfree_skb_any(skb);
			return -EFAULT;
		}
		memcpy(skb_put(skb, SIZE_OF_HDLC_END), hdlc_end,
					SIZE_OF_HDLC_END);
		break;

	default:
		memcpy(skb_put(skb, SIZE_OF_HDLC_START), hdlc_start,
				SIZE_OF_HDLC_START);
		memcpy(skb_put(skb, get_header_size(iod)),
			get_header(iod, count, frame_header_buf),
			get_header_size(iod));
		if (copy_from_user(skb_put(skb, count), buf, count) != 0) {
			dev_kfree_skb_any(skb);
			return -EFAULT;
		}
		memcpy(skb_put(skb, SIZE_OF_HDLC_END), hdlc_end,
					SIZE_OF_HDLC_END);
		break;
	}

	/* send data with sk_buff, link device will put sk_buff
	 * into the specific sk_buff_q and run work-q to send data
	 */
	return iod->link->send(iod->link, iod, skb);
}

static ssize_t misc_read(struct file *filp, char *buf, size_t count,
			loff_t *f_pos)
{
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct sk_buff *skb;
	int pktsize = 0;

	skb = skb_dequeue(&iod->sk_rx_q);
	if (!skb) {
		printk_ratelimited(KERN_ERR "[IOD] no data from sk_rx_q, "
			"modem_state : %s(%s)\n",
			modem_state_name[iod->mc->phone_state], iod->name);
		return 0;
	}

	if (skb->len > count) {
		pr_err("[IOD] <%s> skb len is too big = %d,%d!(%d)\n",
			__func__, count, skb->len, __LINE__);
		dev_kfree_skb_any(skb);
		return -EIO;
	}
	pr_debug("[IOD] <%s> skb len : %d\n", __func__, skb->len);

	pktsize = skb->len;
	if (copy_to_user(buf, skb->data, pktsize) != 0)
		return -EIO;
	dev_kfree_skb_any(skb);

	pr_debug("[IOD] <%s> copy to user : %d\n", __func__, pktsize);
	return pktsize;
}

static const struct file_operations misc_io_fops = {
	.owner = THIS_MODULE,
	.open = misc_open,
	.release = misc_release,
	.poll = misc_poll,
	.unlocked_ioctl = misc_ioctl,
	.write = misc_write,
	.read = misc_read,
};

static int vnet_open(struct net_device *ndev)
{
	pr_info("[IOD] <%s> Open node : %s\n", __func__, ndev->name);
	netif_start_queue(ndev);
	return 0;
}

static int vnet_stop(struct net_device *ndev)
{
	pr_info("[IOD] <%s> Release node : %s\n", __func__, ndev->name);
	netif_stop_queue(ndev);
	return 0;
}

static int vnet_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	int ret;
	struct raw_hdr hd;
	struct sk_buff *skb_new;
	struct vnet *vnet = netdev_priv(ndev);
	struct io_device *iod = vnet->iod;

	pr_debug("[IOD] <%s> xmit data from %s (%d)\n",
		__func__, ndev->name, skb->len);

	/* To use Network bridge */
	if (iod->use_handover) {
		if (iod->id >= PSD_DATA_CHID_BEGIN &&
			iod->id <= PSD_DATA_CHID_END)
			skb_pull(skb, sizeof(struct ethhdr));
	}

	hd.len = skb->len + sizeof(hd);
	hd.control = 0;
	hd.channel = iod->id & 0x1F;

	skb_new = skb_copy_expand(skb, sizeof(hd) + sizeof(hdlc_start),
				sizeof(hdlc_end), GFP_ATOMIC);
	if (!skb_new) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	memcpy(skb_push(skb_new, sizeof(hd)), &hd, sizeof(hd));
	memcpy(skb_push(skb_new, sizeof(hdlc_start)), hdlc_start,
				sizeof(hdlc_start));
	memcpy(skb_put(skb_new, sizeof(hdlc_end)), hdlc_end, sizeof(hdlc_end));

	ret = iod->link->send(iod->link, iod, skb_new);
	if (ret < 0) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_BUSY;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static struct net_device_ops vnet_ops = {
	.ndo_open = vnet_open,
	.ndo_stop = vnet_stop,
	.ndo_start_xmit = vnet_xmit,
};

static void vnet_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &vnet_ops;
	ndev->type = ARPHRD_PPP;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	ndev->addr_len = 0;
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 1000;
	ndev->mtu = ETH_DATA_LEN;
	ndev->watchdog_timeo = 5 * HZ;
}

static void vnet_setup_ether(struct net_device *ndev)
{
	ndev->netdev_ops = &vnet_ops;
	ndev->type = ARPHRD_ETHER;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST | IFF_SLAVE;
	ndev->addr_len = ETH_ALEN;
	random_ether_addr(ndev->dev_addr);
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 1000;
	ndev->mtu = ETH_DATA_LEN;
	ndev->watchdog_timeo = 5 * HZ;
}

int init_io_device(struct io_device *iod)
{
	int ret = 0;
	struct vnet *vnet;

	/* get modem state from modem control device */
	iod->modem_state_changed = io_dev_modem_state_changed;
	/* get data from link device */
	iod->recv = io_dev_recv_data_from_link_dev;

	INIT_LIST_HEAD(&iod->list);

	/* register misc or net drv */
	switch (iod->io_typ) {
	case IODEV_MISC:
		init_waitqueue_head(&iod->wq);
		skb_queue_head_init(&iod->sk_rx_q);
		INIT_DELAYED_WORK(&iod->rx_work, rx_iodev_work);

		iod->miscdev.minor = MISC_DYNAMIC_MINOR;
		iod->miscdev.name = iod->name;
		iod->miscdev.fops = &misc_io_fops;

		ret = misc_register(&iod->miscdev);
		if (ret)
			pr_err("[IOD] <%s> failed to register misc io device : %s\n",
				__func__, iod->name);

		break;

	case IODEV_NET:
		if (iod->use_handover)
			iod->ndev = alloc_netdev(0, iod->name,
						vnet_setup_ether);
		else
			iod->ndev = alloc_netdev(0, iod->name, vnet_setup);

		if (!iod->ndev) {
			pr_err("[IOD] <%s> failed to alloc netdev\n", __func__);
			return -ENOMEM;
		}

		ret = register_netdev(iod->ndev);
		if (ret) {
			pr_err("[IOD] <%s> failed to register netdev [%s][%d]\n",
				__func__, iod->name, ret);
			free_netdev(iod->ndev);
		}

		pr_info("[IOD] <%s> (iod:0x%p)\n", __func__, iod);
		vnet = netdev_priv(iod->ndev);
		pr_info("[IOD] <%s> (vnet:0x%p)\n", __func__, vnet);
		vnet->iod = iod;

		break;

	case IODEV_DUMMY:
		skb_queue_head_init(&iod->sk_rx_q);
		INIT_DELAYED_WORK(&iod->rx_work, rx_iodev_work);

		iod->miscdev.minor = MISC_DYNAMIC_MINOR;
		iod->miscdev.name = iod->name;
		iod->miscdev.fops = &misc_io_fops;

		ret = misc_register(&iod->miscdev);
		if (ret)
			pr_err("[IOD] <%s> failed to register misc io device : %s\n",
				__func__, iod->name);

		break;

	default:
		pr_err("[IOD] <%s> wrong io_type : %d\n",
			__func__, iod->io_typ);
		return -EINVAL;
	}

	pr_info("[IOD] <%s> %s(%d) : init_io_device() done : %d\n",
		__func__, iod->name, iod->io_typ, ret);
	return ret;
}

