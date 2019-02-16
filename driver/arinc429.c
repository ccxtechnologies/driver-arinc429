/*
 * Copyright (C), 2019 CCX Technologies
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <net/sock.h>
#include <linux/init.h>

#include "arinc429.h"

MODULE_DESCRIPTION("ARINC-429 Socket Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Charles Eidsness <charles@ccxtechnologies.com>");
MODULE_VERSION("1.0.0");

MODULE_ALIAS_NETPROTO(PF_ARINC429);

/* ====== skbuff Private Data ===== */

struct arinc429_skb_priv {
	int ifindex;
	union arinc429_word word[0];
};

/* ====== Socket Lists ====== */

/* We need to track the lost of open sockets per device so that
 * we know where to send ingress packets */

struct dev_socket_info {
	struct hlist_node node;
	struct sock *sk;
	void (*ingress_func)(struct sk_buff*, struct sock *);
};

struct dev_socket_list {
	struct hlist_head head;
	int remove_on_zero_entries;
	int entries;
};

static DEFINE_SPINLOCK(dev_socket_lock);
static struct kmem_cache *dev_socket_cache __read_mostly;

static int dev_register_socket(struct net_device *dev,
			 void (*ingress_func)(struct sk_buff *, struct sock *),
			 struct sock *sk)
{
	struct dev_socket_list *sk_list;
	struct dev_socket_info *sk_info;

	pr_debug("arinc429: Registering socket with %s\n", dev->name);

	if (dev && dev->type != ARPHRD_ARINC429) {
		pr_err("arinc429: %s is not a valid device.\n", dev->name);
		return -ENODEV;
	}

	if (!dev->ml_priv) {
		pr_err("arinc429: %s has no registerd socket list.\n",
		       dev->name);
		return -ENODEV;
	}

	sk_info = kmem_cache_alloc(dev_socket_cache, GFP_KERNEL);
	if (!sk_info) {
		pr_info("Failed to allocate socket info\n");
		return -ENOMEM;
	}

	sk_info->sk = sk;
	sk_info->ingress_func = ingress_func;

	spin_lock(&dev_socket_lock);

	sk_list = (struct dev_socket_list *)dev->ml_priv;

	hlist_for_each_entry_rcu(sk_info, &sk_list->head, node) {
		if (sk_info->ingress_func == ingress_func
		    && sk_info->sk == sk) {
			pr_info("Socket already attached to %s\n", dev->name);
			kmem_cache_free(dev_socket_cache, sk_info);
			spin_unlock(&dev_socket_lock);
			return 0;
		}
	}

	hlist_add_head_rcu(&sk_info->node, &sk_list->head);
	sk_list->entries++;

	spin_unlock(&dev_socket_lock);

	return 0;
}

static void dev_unregister_socket(struct net_device *dev,
			 void (*ingress_func)(struct sk_buff *, struct sock *),
			 struct sock *sk)
{
	struct dev_socket_list *sk_list;
	struct dev_socket_info *sk_info;

	pr_debug("arinc429: Unregistering socket with %s\n", dev->name);

	if (dev && dev->type != ARPHRD_ARINC429) {
		pr_err("arinc429: %s is not a valid device.\n", dev->name);
		return;
	}

	if (!dev->ml_priv) {
		pr_err("arinc429: %s has no registerd socket list.\n",
		       dev->name);
		return;
	}

	spin_lock(&dev_socket_lock);

	sk_list = (struct dev_socket_list *)dev->ml_priv;

	hlist_for_each_entry_rcu(sk_info, &sk_list->head, node) {
		if ((sk_info->ingress_func == ingress_func)
		    && (sk_info->sk == sk)) {
			break;
		}
	}

	if (!sk_info) {
		pr_err("arinc429: failed to find socket in device %s.\n",
		       dev->name);
		spin_unlock(&dev_socket_lock);
		return;
	}

	hlist_del_rcu(&sk_info->node);
	sk_list->entries--;

	if (sk_list->remove_on_zero_entries && (sk_list->entries <= 0)) {
		pr_debug("arinc429: Removing socket list from %s\n", dev->name);
		kfree(sk_list);
		dev->ml_priv = NULL;
	}

	spin_unlock(&dev_socket_lock);

	kmem_cache_free(dev_socket_cache, sk_info);

}

static void dev_remove_socket_list(struct net_device *dev)
{
	struct dev_socket_list *sk_list;

	pr_debug("arinc429: Removing socket list from %s\n",dev->name);

	spin_lock(&dev_socket_lock);

	sk_list = (struct dev_socket_list *)dev->ml_priv;
	if (sk_list) {
		sk_list->remove_on_zero_entries = 1;
		if (!sk_list->entries)
			kfree(sk_list);
		dev->ml_priv = NULL;
	} else {
		pr_err("arinc429: receive list not found for device %s\n",
		       dev->name);
	}

	spin_unlock(&dev_socket_lock);
}

static int dev_add_socket_list(struct net_device *dev)
{
	struct dev_socket_list *sk_list;

	pr_debug("arinc429: Adding socket list to %s\n",dev->name);

	if (dev->ml_priv) {
		pr_err("arinc429: Device %s already has a socket list.\n",
		       dev->name);
		return -EINVAL;
	}

	sk_list = kzalloc(sizeof(*sk_list), GFP_KERNEL);
	if (!sk_list) {
		pr_err("arinc429: Failed to allocate socket list.\n");
		return -ENOMEM;
	}

	dev->ml_priv = sk_list;

	return 0;
}

/* ====== Raw Protocol ===== */

struct proto_raw_sock {
	struct sock sk;
	int ifindex;
	int bound;
};

static void raw_ingress(struct sk_buff *skb, struct sock *sk)
{
	pr_info("Raw ingress\n");
}

static int proto_raw_bind(struct socket *sock, struct sockaddr *saddr, int len)
{
	DECLARE_SOCKADDR(struct sockaddr_arinc429 *, addr, saddr);
	struct sock *sk = sock->sk;
	struct proto_raw_sock *psk = (struct proto_raw_sock*)sk;
	struct net_device *dev;
	int err;

	pr_debug("arinc429: Binding ARINC-429 Raw Socket\n");

	if (len != sizeof(*addr)) {
		pr_err("arinc429-raw: Address length should"
		       " be %ld not %d.\n", sizeof(*addr), len);
		return -EINVAL;
	}

	if (!addr->arinc429_ifindex) {
		pr_err("arinc429-raw: Must specify ifindex in Address.\n");
		return -EINVAL;
	}

	lock_sock(sk);

	if (psk->bound && (addr->arinc429_ifindex == psk->ifindex)) {
		pr_debug("arinc429-raw: Socket already bound to %d.\n",
			 psk->ifindex);
		release_sock(sk);
		return 0;
	}

	dev = dev_get_by_index(sock_net(sk), addr->arinc429_ifindex);

	if (!dev) {
		pr_err("arinc429-raw: Can't find device %d.\n",
		       addr->arinc429_ifindex);
		release_sock(sk);
		return -ENODEV;
	}

	if (dev->type != ARPHRD_ARINC429) {
		pr_err("arinc429-raw: Device %d isn't an ARINC-429 Device.\n",
		       addr->arinc429_ifindex);
		dev_put(dev);
		release_sock(sk);
		return -ENODEV;
	}

	err = dev_register_socket(dev, raw_ingress, sk);
	if (err) {
		pr_err("Failed to register socket with device %s: %d\n",
		       dev->name, err);
		dev_put(dev);
		release_sock(sk);
		return -ENODEV;
	}

	psk->ifindex = dev->ifindex;
	psk->bound = 1;

	release_sock(sk);

	if (!(dev->flags & IFF_UP)) {
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD)) {
			sk->sk_error_report(sk);
		}
	}

	dev_put(dev);

	return 0;
}

static int proto_raw_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct proto_raw_sock *psk = (struct proto_raw_sock*)sk;
	struct net_device *dev = NULL;

	if (!sk) {
		return 0;
	}

	pr_debug("arinc429-raw: Releasing ARINC-429 Raw Socket\n");

	dev = dev_get_by_index(sock_net(sk), psk->ifindex);
	if (dev) {
		dev_unregister_socket(dev, raw_ingress, sk);
	} else {
		pr_warning("arinc429-raw: No device registered with socket\n");
	}

	lock_sock(sk);

	psk->ifindex = 0;
	psk->bound   = 0;

	sock_orphan(sk);
	sock->sk = NULL;


	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int proto_raw_ioctl(struct socket *sock, unsigned int cmd,
			   unsigned long arg)
{
	struct sock *sk = sock->sk;

	switch (cmd) {
	case SIOCGSTAMP:
		return sock_get_timestamp(sk, (struct timeval __user *)arg);

	default:
		return -ENOIOCTLCMD;
	}
}

static int proto_raw_getname(struct socket *sock, struct sockaddr *saddr,
			     int *len, int peer)
{
	DECLARE_SOCKADDR(struct sockaddr_arinc429 *, addr, saddr);
	struct sock *sk = sock->sk;
	struct proto_raw_sock *psk = (struct proto_raw_sock*)sk;

	if (peer)
		return -EOPNOTSUPP;

	memset(addr, 0, sizeof(*addr));
	addr->arinc429_family  = AF_ARINC429;
	addr->arinc429_ifindex = psk->ifindex;

	*len = sizeof(*addr);

	return 0;
}

static int proto_raw_sendmsg(struct socket *sock, struct msghdr *msg,
			     size_t size)
{
	struct sock *sk = sock->sk;
	struct proto_raw_sock *psk = (struct proto_raw_sock*)sk;
	struct sk_buff *skb;
	struct net_device *dev;
	int ifindex;
	int err;

	pr_debug("arinc429-raw: Sending a Raw message\n");

	if (unlikely(size % ARINC429_WORD_SIZE)) {
		pr_warn("arinc429-raw: Must be multiple of word size: %ld\n",
			ARINC429_WORD_SIZE);
		return -EINVAL;
	}

	/* Get the interface index from the message, otherwise from the socket */
	if (msg->msg_name) {
		DECLARE_SOCKADDR(struct sockaddr_arinc429 *, addr,
				 msg->msg_name);

		if (msg->msg_namelen < sizeof(*addr))
			return -EINVAL;

		if (addr->arinc429_family != AF_ARINC429)
			return -EINVAL;

		ifindex = addr->arinc429_ifindex;
		pr_debug("arinc429-raw: ifindex %d from message.\n", ifindex);

	} else {
		ifindex = psk->ifindex;
		pr_debug("arinc429-raw: ifindex %d from socket.\n", ifindex);
	}

	/* Make sure the device is valid */
	dev = dev_get_by_index(sock_net(sk), ifindex);
	if (!dev) {
		pr_err("arinc429-raw: Can't find device %d.\n", ifindex);
		return -ENXIO;
	}

	if (unlikely(dev->type != ARPHRD_ARINC429)) {
		pr_err("arinc429-raw: Device %d isn't an ARINC-429 Device.\n",
		       ifindex);
		dev_put(dev);
		return -ENODEV;
	}

	if (unlikely(size > dev->mtu)) {
		pr_err("arinc429-raw: Doesn't fit in MTU of %d bytes.\n",
		       dev->mtu);
		dev_put(dev);
		return -EMSGSIZE;
	}

	if (unlikely(!(dev->flags & IFF_UP))) {
		pr_err("arinc429-raw: Device isn't up\n");
		dev_put(dev);
		return -ENETDOWN;
	}

	/* Allocate and configure the skbuffer */
	skb = sock_alloc_send_skb(sk, size + sizeof(struct arinc429_skb_priv),
				  msg->msg_flags & MSG_DONTWAIT, &err);

	if (!skb) {
		pr_err("arinc429-raw: Unable to allocate skbuff: %d.\n", err);
		dev_put(dev);
		return err;
	}

	skb_reserve(skb, sizeof(struct arinc429_skb_priv));
	((struct arinc429_skb_priv *)(skb->head))->ifindex = dev->ifindex;

	err = memcpy_from_msg(skb_put(skb, size), msg, size);
	if (err < 0) {
		pr_err("arinc429-raw: Unable to memcpy from mesg: %d.\n", err);
		kfree_skb(skb);
		dev_put(dev);
		return err;
	}

	sock_tx_timestamp(sk, sk->sk_tsflags, &skb_shinfo(skb)->tx_flags);

	skb->dev = dev;
	skb->sk  = sk;
	skb->priority = sk->sk_priority;
	skb->protocol = htons(ETH_P_ARINC429);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_HOST;

	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);

	/* send to netdevice */
	err = dev_queue_xmit(skb);
	if (err > 0) {
		err = net_xmit_errno(err);
	}

	if (err) {
		pr_err("arinc429-raw: Send to netdevice failed: %d\n", err);
		dev_put(dev);
		return err;
	}

	dev_put(dev);
	return size;
}

static int proto_raw_recvmsg(struct socket *sock,
			     struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int err = 0;
	int noblock;

	pr_debug("arinc429-raw: Receiving a Raw message\n");

	noblock = flags & MSG_DONTWAIT;
	flags &= ~MSG_DONTWAIT;

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb) {
		pr_debug("arinc429-raw: No data in receive message\n");
		return err;
	}

	if (size < skb->len) {
		msg->msg_flags |= MSG_TRUNC;
	} else {
		size = skb->len;
	}

	err = memcpy_to_msg(msg, skb->data, size);
	if (err < 0) {
		pr_err("arinc429-raw: Failed to copy message data.\n");
		skb_free_datagram(sk, skb);
		return err;
	}

	sock_recv_ts_and_drops(msg, sk, skb);

	if (msg->msg_name) {
		__sockaddr_check_size(sizeof(struct sockaddr_arinc429));
		msg->msg_namelen = sizeof(struct sockaddr_arinc429);
		memcpy(msg->msg_name, skb->cb, msg->msg_namelen);
	}

	skb_free_datagram(sk, skb);

	return size;
}

static const struct proto_ops proto_raw_ops = {
	.owner		= THIS_MODULE,
	.family		= PF_ARINC429,

	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= sock_no_setsockopt,
	.getsockopt	= sock_no_getsockopt,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage,

	.poll		= datagram_poll,

	.bind		= proto_raw_bind,
	.release	= proto_raw_release,
	.getname	= proto_raw_getname,
	.sendmsg	= proto_raw_sendmsg,
	.recvmsg	= proto_raw_recvmsg,
	.ioctl		= proto_raw_ioctl,
};

static struct proto proto_raw = {
	.name		= "ARINC429_RAW",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct proto_raw_sock),
};

/* ====== Socket Creator ====== */

static void arinc429_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->sk_receive_queue);
}

static int arinc429_sock_create(struct net *net, struct socket *sock,
				int protocol, int kern)
{
	struct sock *sk;
	static const struct proto_ops* popts;
	static struct proto* p;
	int err;

	pr_debug("arinc429: Creating new ARINC429 socket.\n");

	sock->state = SS_UNCONNECTED;

	if (!net_eq(net, &init_net)) {
		pr_err("arinc429: Device not in namespace\n");
		return -EAFNOSUPPORT;
	}

	switch (protocol) {
	case ARINC429_PROTO_RAW:
		pr_debug("arinc429: Configurtion Raw Protocol.\n");

		popts = &proto_raw_ops;
		p = &proto_raw;

		break;

	default:
		pr_err("arinc429: Invalid protocol %d\n", protocol);
		return -EPROTONOSUPPORT;
	}

	sock->ops = popts;

	sk = sk_alloc(net, PF_ARINC429, GFP_KERNEL, p, kern);
	if (!sk) {
		pr_err("arinc429: Failed to allocate socket.\n");
		return -ENOMEM;
	}

	sock_init_data(sock, sk);
	sk->sk_destruct = arinc429_sock_destruct;

	if (sk->sk_prot->init) {
		err = sk->sk_prot->init(sk);
		if (err) {
			pr_err("arinc429: Failed to init socket protocol: %d\n",
			       err);
			sock_orphan(sk);
			sock_put(sk);
			return err;
		}
	}

	return 0;
}


static const struct net_proto_family arinc429_net_proto_family = {
	.family	= PF_ARINC429,
	.create	= arinc429_sock_create,
	.owner	= THIS_MODULE,
};

/* ====== NetDev Notifier ====== */

static int arinc429_netdev_notifier(struct notifier_block *nb,
				    unsigned long msg, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	if (dev->type != ARPHRD_ARINC429)
		return NOTIFY_DONE;

	switch (msg) {
	case NETDEV_REGISTER:
		pr_info("arinc429: Registering new ARINC-429 Device.\n");
		dev_add_socket_list(dev);
		break;

	case NETDEV_UNREGISTER:
		pr_info("arinc429: Unregistering ARINC-429 Device.\n");
		dev_remove_socket_list(dev);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block arinc429_notifier_block __read_mostly = {
	.notifier_call = arinc429_netdev_notifier,
};

/* ====== Ingress Packet Processing ====== */

static int arinc429_packet_ingress(struct sk_buff *skb, struct net_device *dev,
				   struct packet_type *pt,
				   struct net_device *orig_dev)
{
	int err;

	pr_debug("arinc429: Ingress packet.\n");

	if (unlikely(!net_eq(dev_net(dev), &init_net))) {
		pr_err("Device not in namespace\n");
		kfree_skb(skb);
		return NET_RX_DROP;
	}

	err = WARN_ONCE(dev->type != ARPHRD_ARINC429 ||
			skb->len % ARINC429_WORD_SIZE,
			"arinc429: dropped non conform ARINC429 skbuf:"
			" dev type %d, len %d\n", dev->type, skb->len);
	if (err) {
		pr_err("Device not in namespace: %d\n", err);
		kfree_skb(skb);
		return NET_RX_DROP;
	}

	pr_debug("arinc429: Locking RCU.\n");
	rcu_read_lock();

	pr_debug("arinc429: Unlocking RCU.\n");
	rcu_read_unlock();

	pr_debug("arinc429: Consuming skb.\n");
	consume_skb(skb);

	return NET_RX_SUCCESS;
}

static struct packet_type arinc429_packet_type __read_mostly = {
	.type	= cpu_to_be16(ETH_P_ARINC429),
	.func	= arinc429_packet_ingress,
};

/* ====== Module Init/Exit ====== */

static __init int arinc429_init(void)
{
	int rc;

	pr_info("arinc429: Initialising ARINC-429 Socket Driver\n");

	dev_socket_cache = kmem_cache_create("arinc429_dev_socket",
					     sizeof(struct dev_socket_info),
					     0, 0, NULL);
	if (!dev_socket_cache) {
		pr_err("arinc429: Failed to allocate device socket cache.\n");
		return -ENOMEM;
	}

	rc = proto_register(&proto_raw, ARINC429_PROTO_RAW);
	if (rc) {
		pr_err("arinc429: Failed to register Raw Protocol: %d\n", rc);
		kmem_cache_destroy(dev_socket_cache);
		return rc;
	}

	rc = sock_register(&arinc429_net_proto_family);
	if (rc) {
		pr_err("arinc429: Failed to register Socket Type: %d\n", rc);
		proto_unregister(&proto_raw);
		kmem_cache_destroy(dev_socket_cache);
		return rc;
	}

	rc = register_netdevice_notifier(&arinc429_notifier_block);
	if (rc) {
		pr_err("arinc429: Failed to register with NetDev: %d\n", rc);
		sock_unregister(PF_ARINC429);
		proto_unregister(&proto_raw);
		kmem_cache_destroy(dev_socket_cache);
		return rc;
	}

	dev_add_pack(&arinc429_packet_type);

	return 0;
}

static __exit void arinc429_exit(void)
{
	int err;

	dev_remove_pack(&arinc429_packet_type);

	err = unregister_netdevice_notifier(&arinc429_notifier_block);
	if (err) {
		pr_err("arinc429: Failed to unregister with NetDev: %d\n", err);
	}

	sock_unregister(PF_ARINC429);
	proto_unregister(&proto_raw);
	kmem_cache_destroy(dev_socket_cache);

	pr_info("arinc429: Exited ARINC-429 Socket Driver\n");
}

module_init(arinc429_init);
module_exit(arinc429_exit);
