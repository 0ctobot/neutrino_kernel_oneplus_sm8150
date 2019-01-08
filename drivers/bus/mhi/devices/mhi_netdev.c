/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/msm_rmnet.h>
#include <linux/if_arp.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/ipc_logging.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/of_device.h>
#include <linux/rtnetlink.h>
#include <linux/mhi.h>

#define MHI_NETDEV_DRIVER_NAME "mhi_netdev"
#define WATCHDOG_TIMEOUT (30 * HZ)
#define IPC_LOG_PAGES (100)

#ifdef CONFIG_MHI_DEBUG

#define IPC_LOG_LVL (MHI_MSG_LVL_VERBOSE)

#define MHI_ASSERT(cond, msg) do { \
	if (cond) \
		panic(msg); \
} while (0)

#define MSG_VERB(fmt, ...) do { \
	if (mhi_netdev->msg_lvl <= MHI_MSG_LVL_VERBOSE) \
		pr_err("[D][%s] " fmt, __func__, ##__VA_ARGS__);\
	if (mhi_netdev->ipc_log && (mhi_netdev->ipc_log_lvl <= \
				    MHI_MSG_LVL_VERBOSE)) \
		ipc_log_string(mhi_netdev->ipc_log, "[D][%s] " fmt, \
			       __func__, ##__VA_ARGS__); \
} while (0)

#else

#define IPC_LOG_LVL (MHI_MSG_LVL_ERROR)

#define MHI_ASSERT(cond, msg) do { \
	if (cond) { \
		MSG_ERR(msg); \
		WARN_ON(cond); \
	} \
} while (0)

#define MSG_VERB(fmt, ...)

#endif

#define MSG_LOG(fmt, ...) do { \
	if (mhi_netdev->msg_lvl <= MHI_MSG_LVL_INFO) \
		pr_err("[I][%s] " fmt, __func__, ##__VA_ARGS__);\
	if (mhi_netdev->ipc_log && (mhi_netdev->ipc_log_lvl <= \
				    MHI_MSG_LVL_INFO)) \
		ipc_log_string(mhi_netdev->ipc_log, "[I][%s] " fmt, \
			       __func__, ##__VA_ARGS__); \
} while (0)

#define MSG_ERR(fmt, ...) do { \
	if (mhi_netdev->msg_lvl <= MHI_MSG_LVL_ERROR) \
		pr_err("[E][%s] " fmt, __func__, ##__VA_ARGS__); \
	if (mhi_netdev->ipc_log && (mhi_netdev->ipc_log_lvl <= \
				    MHI_MSG_LVL_ERROR)) \
		ipc_log_string(mhi_netdev->ipc_log, "[E][%s] " fmt, \
			       __func__, ##__VA_ARGS__); \
} while (0)

struct mhi_netdev {
	int alias;
	struct mhi_device *mhi_dev;
	int wake;

	u32 mru;
	const char *interface_name;
	struct napi_struct napi;
	struct net_device *ndev;

	struct dentry *dentry;
	enum MHI_DEBUG_LEVEL msg_lvl;
	enum MHI_DEBUG_LEVEL ipc_log_lvl;
	void *ipc_log;
};

struct mhi_netdev_priv {
	struct mhi_netdev *mhi_netdev;
};

static struct mhi_driver mhi_netdev_driver;
static void mhi_netdev_create_debugfs(struct mhi_netdev *mhi_netdev);

static __be16 mhi_netdev_ip_type_trans(struct sk_buff *skb)
{
	__be16 protocol = 0;

	/* determine L3 protocol */
	switch (skb->data[0] & 0xf0) {
	case 0x40:
		protocol = htons(ETH_P_IP);
		break;
	case 0x60:
		protocol = htons(ETH_P_IPV6);
		break;
	default:
		/* default is QMAP */
		protocol = htons(ETH_P_MAP);
		break;
	}
	return protocol;
}

static int mhi_netdev_alloc_skb(struct mhi_netdev *mhi_netdev, gfp_t gfp_t)
{
	u32 cur_mru = mhi_netdev->mru;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;
	int ret;
	struct sk_buff *skb;
	int no_tre = mhi_get_no_free_descriptors(mhi_dev, DMA_FROM_DEVICE);
	int i;

	for (i = 0; i < no_tre; i++) {
		skb = alloc_skb(cur_mru, gfp_t);
		if (!skb)
			return -ENOMEM;

		skb->dev = mhi_netdev->ndev;

		ret = mhi_queue_transfer(mhi_dev, DMA_FROM_DEVICE, skb, cur_mru,
					 MHI_EOT);

		if (ret) {
			MSG_ERR("Failed to queue skb, ret:%d\n", ret);
			ret = -EIO;
			goto error_queue;
		}

	}

	return 0;

error_queue:
	dev_kfree_skb_any(skb);

	return ret;
}

static int mhi_netdev_poll(struct napi_struct *napi, int budget)
{
	struct net_device *dev = napi->dev;
	struct mhi_netdev_priv *mhi_netdev_priv = netdev_priv(dev);
	struct mhi_netdev *mhi_netdev = mhi_netdev_priv->mhi_netdev;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;
	int rx_work = 0;

	MSG_VERB("Entered\n");

	rx_work = mhi_poll(mhi_dev, budget);
	if (rx_work < 0) {
		MSG_ERR("Error polling ret:%d\n", rx_work);
		rx_work = 0;
		napi_complete(napi);
		goto exit_poll;
	}

	/* queue new buffers */
	mhi_netdev_alloc_skb(mhi_netdev, GFP_ATOMIC);

	/* complete work if # of packet processed less than allocated budget */
	if (rx_work < budget)
		napi_complete(napi);

exit_poll:

	MSG_VERB("polled %d pkts\n", rx_work);

	return rx_work;
}

static int mhi_netdev_open(struct net_device *dev)
{
	struct mhi_netdev_priv *mhi_netdev_priv = netdev_priv(dev);
	struct mhi_netdev *mhi_netdev = mhi_netdev_priv->mhi_netdev;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;

	MSG_LOG("Opened net dev interface\n");

	/* tx queue may not necessarily be stopped already
	 * so stop the queue if tx path is not enabled
	 */
	if (!mhi_dev->ul_chan)
		netif_stop_queue(dev);
	else
		netif_start_queue(dev);

	return 0;

}

static int mhi_netdev_change_mtu(struct net_device *dev, int new_mtu)
{
	struct mhi_netdev_priv *mhi_netdev_priv = netdev_priv(dev);
	struct mhi_netdev *mhi_netdev = mhi_netdev_priv->mhi_netdev;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;

	if (new_mtu < 0 || mhi_dev->mtu < new_mtu)
		return -EINVAL;

	dev->mtu = new_mtu;
	return 0;
}

static int mhi_netdev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mhi_netdev_priv *mhi_netdev_priv = netdev_priv(dev);
	struct mhi_netdev *mhi_netdev = mhi_netdev_priv->mhi_netdev;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;
	int res = 0;

	MSG_VERB("Entered\n");

	res = mhi_queue_transfer(mhi_dev, DMA_TO_DEVICE, skb, skb->len,
				 MHI_EOT);
	if (res) {
		MSG_VERB("Failed to queue with reason:%d\n", res);
		netif_stop_queue(dev);
		res = NETDEV_TX_BUSY;
	}

	MSG_VERB("Exited\n");

	return res;
}

static int mhi_netdev_ioctl_extended(struct net_device *dev, struct ifreq *ifr)
{
	struct rmnet_ioctl_extended_s ext_cmd;
	int rc = 0;
	struct mhi_netdev_priv *mhi_netdev_priv = netdev_priv(dev);
	struct mhi_netdev *mhi_netdev = mhi_netdev_priv->mhi_netdev;
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;

	rc = copy_from_user(&ext_cmd, ifr->ifr_ifru.ifru_data,
			    sizeof(struct rmnet_ioctl_extended_s));
	if (rc)
		return rc;

	switch (ext_cmd.extended_ioctl) {
	case RMNET_IOCTL_GET_SUPPORTED_FEATURES:
		ext_cmd.u.data = 0;
		break;
	case RMNET_IOCTL_GET_DRIVER_NAME:
		strlcpy(ext_cmd.u.if_name, mhi_netdev->interface_name,
			sizeof(ext_cmd.u.if_name));
		break;
	case RMNET_IOCTL_SET_SLEEP_STATE:
		if (ext_cmd.u.data && mhi_netdev->wake) {
			/* Request to enable LPM */
			MSG_VERB("Enable MHI LPM");
			mhi_netdev->wake--;
			mhi_device_put(mhi_dev);
		} else if (!ext_cmd.u.data && !mhi_netdev->wake) {
			/* Request to disable LPM */
			MSG_VERB("Disable MHI LPM");
			mhi_netdev->wake++;
			mhi_device_get(mhi_dev);
		}
		break;
	default:
		rc = -EINVAL;
		break;
	}

	rc = copy_to_user(ifr->ifr_ifru.ifru_data, &ext_cmd,
			  sizeof(struct rmnet_ioctl_extended_s));
	return rc;
}

static int mhi_netdev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	int rc = 0;
	struct rmnet_ioctl_data_s ioctl_data;

	switch (cmd) {
	case RMNET_IOCTL_SET_LLP_IP: /* set RAWIP protocol */
		break;
	case RMNET_IOCTL_GET_LLP: /* get link protocol state */
		ioctl_data.u.operation_mode = RMNET_MODE_LLP_IP;
		if (copy_to_user(ifr->ifr_ifru.ifru_data, &ioctl_data,
		    sizeof(struct rmnet_ioctl_data_s)))
			rc = -EFAULT;
		break;
	case RMNET_IOCTL_GET_OPMODE: /* get operation mode */
		ioctl_data.u.operation_mode = RMNET_MODE_LLP_IP;
		if (copy_to_user(ifr->ifr_ifru.ifru_data, &ioctl_data,
		    sizeof(struct rmnet_ioctl_data_s)))
			rc = -EFAULT;
		break;
	case RMNET_IOCTL_SET_QOS_ENABLE:
		rc = -EINVAL;
		break;
	case RMNET_IOCTL_SET_QOS_DISABLE:
		rc = 0;
		break;
	case RMNET_IOCTL_OPEN:
	case RMNET_IOCTL_CLOSE:
		/* we just ignore them and return success */
		rc = 0;
		break;
	case RMNET_IOCTL_EXTENDED:
		rc = mhi_netdev_ioctl_extended(dev, ifr);
		break;
	default:
		/* don't fail any IOCTL right now */
		rc = 0;
		break;
	}

	return rc;
}

static const struct net_device_ops mhi_netdev_ops_ip = {
	.ndo_open = mhi_netdev_open,
	.ndo_start_xmit = mhi_netdev_xmit,
	.ndo_do_ioctl = mhi_netdev_ioctl,
	.ndo_change_mtu = mhi_netdev_change_mtu,
	.ndo_set_mac_address = 0,
	.ndo_validate_addr = 0,
};

static void mhi_netdev_setup(struct net_device *dev)
{
	dev->netdev_ops = &mhi_netdev_ops_ip;
	ether_setup(dev);

	/* set this after calling ether_setup */
	dev->header_ops = 0;  /* No header */
	dev->type = ARPHRD_RAWIP;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
	dev->watchdog_timeo = WATCHDOG_TIMEOUT;
}

/* enable mhi_netdev netdev, call only after grabbing mhi_netdev.mutex */
static int mhi_netdev_enable_iface(struct mhi_netdev *mhi_netdev)
{
	int ret = 0;
	char ifalias[IFALIASZ];
	char ifname[IFNAMSIZ];
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;
	int no_tre;

	MSG_LOG("Prepare the channels for transfer\n");

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret) {
		MSG_ERR("Failed to start TX chan ret %d\n", ret);
		goto mhi_failed_to_start;
	}

	/* first time enabling the node */
	if (!mhi_netdev->ndev) {
		struct mhi_netdev_priv *mhi_netdev_priv;

		snprintf(ifalias, sizeof(ifalias), "%s_%04x_%02u.%02u.%02u_%u",
			 mhi_netdev->interface_name, mhi_dev->dev_id,
			 mhi_dev->domain, mhi_dev->bus, mhi_dev->slot,
			 mhi_netdev->alias);

		snprintf(ifname, sizeof(ifname), "%s%%d",
			 mhi_netdev->interface_name);

		rtnl_lock();
		mhi_netdev->ndev = alloc_netdev(sizeof(*mhi_netdev_priv),
					ifname, NET_NAME_PREDICTABLE,
					mhi_netdev_setup);

		if (!mhi_netdev->ndev) {
			ret = -ENOMEM;
			rtnl_unlock();
			goto net_dev_alloc_fail;
		}

		mhi_netdev->ndev->mtu = mhi_dev->mtu;
		SET_NETDEV_DEV(mhi_netdev->ndev, &mhi_dev->dev);
		dev_set_alias(mhi_netdev->ndev, ifalias, strlen(ifalias));
		mhi_netdev_priv = netdev_priv(mhi_netdev->ndev);
		mhi_netdev_priv->mhi_netdev = mhi_netdev;
		rtnl_unlock();

		netif_napi_add(mhi_netdev->ndev, &mhi_netdev->napi,
			       mhi_netdev_poll, NAPI_POLL_WEIGHT);
		ret = register_netdev(mhi_netdev->ndev);
		if (ret) {
			MSG_ERR("Network device registration failed\n");
			goto net_dev_reg_fail;
		}
	}

	/* queue buffer for rx path */
	no_tre = mhi_get_no_free_descriptors(mhi_dev, DMA_FROM_DEVICE);
	mhi_netdev_alloc_skb(mhi_netdev, GFP_KERNEL);

	napi_enable(&mhi_netdev->napi);

	MSG_LOG("Exited.\n");

	return 0;

net_dev_reg_fail:
	netif_napi_del(&mhi_netdev->napi);
	free_netdev(mhi_netdev->ndev);
	mhi_netdev->ndev = NULL;

net_dev_alloc_fail:
	mhi_unprepare_from_transfer(mhi_dev);

mhi_failed_to_start:
	MSG_ERR("Exited ret %d.\n", ret);

	return ret;
}

static void mhi_netdev_xfer_ul_cb(struct mhi_device *mhi_dev,
				  struct mhi_result *mhi_result)
{
	struct mhi_netdev *mhi_netdev = mhi_device_get_devdata(mhi_dev);
	struct sk_buff *skb = mhi_result->buf_addr;
	struct net_device *ndev = mhi_netdev->ndev;

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb(skb);

	if (netif_queue_stopped(ndev))
		netif_wake_queue(ndev);
}

static void mhi_netdev_xfer_dl_cb(struct mhi_device *mhi_dev,
				  struct mhi_result *mhi_result)
{
	struct mhi_netdev *mhi_netdev = mhi_device_get_devdata(mhi_dev);
	struct sk_buff *skb = mhi_result->buf_addr;
	struct net_device *dev = mhi_netdev->ndev;

	if (mhi_result->transaction_status == -ENOTCONN) {
		dev_kfree_skb(skb);
		return;
	}

	skb_put(skb, mhi_result->bytes_xferd);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += mhi_result->bytes_xferd;
	skb->protocol = mhi_netdev_ip_type_trans(skb);
	netif_receive_skb(skb);
}

static void mhi_netdev_status_cb(struct mhi_device *mhi_dev, enum MHI_CB mhi_cb)
{
	struct mhi_netdev *mhi_netdev = mhi_device_get_devdata(mhi_dev);

	if (mhi_cb != MHI_CB_PENDING_DATA)
		return;

	if (napi_schedule_prep(&mhi_netdev->napi)) {
		__napi_schedule(&mhi_netdev->napi);
		return;
	}

}

#ifdef CONFIG_DEBUG_FS

struct dentry *dentry;

static void mhi_netdev_create_debugfs(struct mhi_netdev *mhi_netdev)
{
	char node_name[32];
	struct mhi_device *mhi_dev = mhi_netdev->mhi_dev;

	/* Both tx & rx client handle contain same device info */
	snprintf(node_name, sizeof(node_name), "%s_%04x_%02u.%02u.%02u_%u",
		 mhi_netdev->interface_name, mhi_dev->dev_id, mhi_dev->domain,
		 mhi_dev->bus, mhi_dev->slot, mhi_netdev->alias);

	if (IS_ERR_OR_NULL(dentry))
		return;

	mhi_netdev->dentry = debugfs_create_dir(node_name, dentry);
	if (IS_ERR_OR_NULL(mhi_netdev->dentry))
		return;
}

static void mhi_netdev_create_debugfs_dir(void)
{
	dentry = debugfs_create_dir(MHI_NETDEV_DRIVER_NAME, 0);
}

#else

static void mhi_netdev_create_debugfs(struct mhi_netdev_private *mhi_netdev)
{
}

static void mhi_netdev_create_debugfs_dir(void)
{
}

#endif

static void mhi_netdev_remove(struct mhi_device *mhi_dev)
{
	struct mhi_netdev *mhi_netdev = mhi_device_get_devdata(mhi_dev);

	MSG_LOG("Remove notification received\n");

	netif_stop_queue(mhi_netdev->ndev);
	napi_disable(&mhi_netdev->napi);
	unregister_netdev(mhi_netdev->ndev);
	netif_napi_del(&mhi_netdev->napi);
	free_netdev(mhi_netdev->ndev);

	if (!IS_ERR_OR_NULL(mhi_netdev->dentry))
		debugfs_remove_recursive(mhi_netdev->dentry);
}

static int mhi_netdev_probe(struct mhi_device *mhi_dev,
			    const struct mhi_device_id *id)
{
	int ret;
	struct mhi_netdev *mhi_netdev;
	struct device_node *of_node = mhi_dev->dev.of_node;
	char node_name[32];

	if (!of_node)
		return -ENODEV;

	mhi_netdev = devm_kzalloc(&mhi_dev->dev, sizeof(*mhi_netdev),
				  GFP_KERNEL);
	if (!mhi_netdev)
		return -ENOMEM;

	mhi_netdev->alias = of_alias_get_id(of_node, "mhi_netdev");
	if (mhi_netdev->alias < 0)
		return -ENODEV;

	mhi_netdev->mhi_dev = mhi_dev;
	mhi_device_set_devdata(mhi_dev, mhi_netdev);

	ret = of_property_read_u32(of_node, "mhi,mru", &mhi_netdev->mru);
	if (ret)
		return -ENODEV;

	ret = of_property_read_string(of_node, "mhi,interface-name",
				      &mhi_netdev->interface_name);
	if (ret)
		mhi_netdev->interface_name = mhi_netdev_driver.driver.name;

	/* create ipc log buffer */
	snprintf(node_name, sizeof(node_name), "%s_%04x_%02u.%02u.%02u_%u",
		 mhi_netdev->interface_name, mhi_dev->dev_id, mhi_dev->domain,
		 mhi_dev->bus, mhi_dev->slot, mhi_netdev->alias);
	mhi_netdev->ipc_log = ipc_log_context_create(IPC_LOG_PAGES, node_name,
						     0);
	mhi_netdev->msg_lvl = MHI_MSG_LVL_ERROR;
	mhi_netdev->ipc_log_lvl = IPC_LOG_LVL;

	/* setup network interface */
	ret = mhi_netdev_enable_iface(mhi_netdev);
	if (ret)
		return ret;

	mhi_netdev_create_debugfs(mhi_netdev);

	return 0;
}

static const struct mhi_device_id mhi_netdev_match_table[] = {
	{ .chan = "IP_HW0" },
	{ .chan = "IP_HW_ADPL" },
	{},
};

static struct mhi_driver mhi_netdev_driver = {
	.id_table = mhi_netdev_match_table,
	.probe = mhi_netdev_probe,
	.remove = mhi_netdev_remove,
	.ul_xfer_cb = mhi_netdev_xfer_ul_cb,
	.dl_xfer_cb = mhi_netdev_xfer_dl_cb,
	.status_cb = mhi_netdev_status_cb,
	.driver = {
		.name = "mhi_netdev",
		.owner = THIS_MODULE,
	}
};

static int __init mhi_netdev_init(void)
{
	mhi_netdev_create_debugfs_dir();

	return mhi_driver_register(&mhi_netdev_driver);
}
module_init(mhi_netdev_init);

MODULE_DESCRIPTION("MHI NETDEV Network Interface");
MODULE_LICENSE("GPL v2");
