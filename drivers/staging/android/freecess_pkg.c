#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/file.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#include <linux/freecess.h>

#define MAX_REC_UID 64
static atomic_t uid_rec[MAX_REC_UID];
static DEFINE_SPINLOCK(uid_rec_lock);
extern void binders_in_transcation(int uid);

/*
 * uid_rec[] is the watch table: a slot holds the UID the daemon asked to
 * watch, or 0 when the slot is free.  All access is serialised by
 * uid_rec_lock, and every slot update uses atomic_cmpxchg() so a slot is
 * never claimed twice and a concurrent delete can never be undone by a
 * stale read-modify-write.
 */
static void freecess_add_uid(uid_t uid)
{
	int i;

	spin_lock_bh(&uid_rec_lock);

	/* Never keep a UID twice. */
	for (i = 0; i < MAX_REC_UID; i++) {
		if (atomic_read(&uid_rec[i]) == uid)
			goto out;
	}

	/* Claim the first free slot; cmpxchg() makes the claim atomic. */
	for (i = 0; i < MAX_REC_UID; i++) {
		if (atomic_cmpxchg(&uid_rec[i], 0, uid) == 0)
			goto out;
	}

	pr_err("%s: uid %d not added, table full\n", __func__, uid);
out:
	spin_unlock_bh(&uid_rec_lock);
}

static void freecess_del_uid(uid_t uid)
{
	int i;

	spin_lock_bh(&uid_rec_lock);

	for (i = 0; i < MAX_REC_UID; i++) {
		if (atomic_read(&uid_rec[i]) == uid) {
			/* Clear the slot only while it still holds @uid. */
			atomic_cmpxchg(&uid_rec[i], uid, 0);
			break;
		}
	}

	spin_unlock_bh(&uid_rec_lock);
}

static void freecess_clear_all(void)
{
	int i;

	spin_lock_bh(&uid_rec_lock);

	for (i = 0; i < MAX_REC_UID; i++) {
		uid_t uid = atomic_read(&uid_rec[i]);

		if (uid)
			atomic_cmpxchg(&uid_rec[i], uid, 0);
	}

	spin_unlock_bh(&uid_rec_lock);
}

static int find_and_clear_uid(uid_t uid)
{
	int found = 0;
	int i;

	spin_lock_bh(&uid_rec_lock);

	for (i = 0; i < MAX_REC_UID; i++) {
		uid_t inner_uid = atomic_read(&uid_rec[i]);

		if (unlikely(inner_uid == uid)) {
			/* Only the caller that wins the clear may report. */
			if (atomic_cmpxchg(&uid_rec[i], uid, 0) == uid)
				found = 1;
			break;
		}
	}

	spin_unlock_bh(&uid_rec_lock);

	return found;
}

static void kfreecess_pkg_hook(void *data, unsigned int len)
{
	struct kfreecess_msg_data *payload = data;

	switch (payload->pkg_info.cmd) {
	case ADD_UID:
		freecess_add_uid(payload->pkg_info.uid);
		break;
	case DEL_UID:
		freecess_del_uid(payload->pkg_info.uid);
		break;
	case CLEAR_ALL_UID:
		freecess_clear_all();
		break;
	default:
		break;
	}
}

static void kfreecess_cfb_hook(void *data, unsigned int len)
{
	struct kfreecess_msg_data *payload = data;
	int uid = payload->target_uid;

	pr_debug("cfb_target: uid = %d\n", uid);
	binders_in_transcation(uid);
}

static uid_t __sock_i_uid(struct sock *sk)
{
	/*
	 * This runs in NET_RX softirq.  sk->sk_socket can be NULLed by
	 * sock_orphan() in tcp_close() on another CPU; dereferencing it
	 * (and SOCK_INODE()) is a TOCTOU.  sk_uid is stable on the sock
	 * and does not depend on the socket inode.
	 */
	if (!sk)
		return 0;
	return from_kuid_munged(&init_user_ns, sk->sk_uid);
}

static unsigned int freecess_ip4_in(void *priv,
				    struct sk_buff *skb,
				    const struct nf_hook_state *state)
{
	struct iphdr *iph;
	struct sock *sk;
	uid_t uid;
	int found;

	/* A truncated or malformed packet has no full IP header to read. */
	if (!pskb_may_pull(skb, sizeof(struct iphdr)))
		return NF_ACCEPT;

	iph = ip_hdr(skb);

	/* Only the first fragment carries the transport header. */
	if (iph->frag_off & htons(IP_OFFSET))
		return NF_ACCEPT;

	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	sk = skb_to_full_sk(skb);
	if (!sk || !sk_fullsock(sk))
		return NF_ACCEPT;

	uid = __sock_i_uid(sk);
	if (uid < UID_MIN_VALUE)
		return NF_ACCEPT;

	found = find_and_clear_uid(uid);
	if (!found)
		return NF_ACCEPT;

	if (pkg_report((int)uid))
		pr_err("%s: up report failed!\n", __func__);

	return NF_ACCEPT;
}

static unsigned int freecess_ip6_in(void *priv,
				    struct sk_buff *skb,
				    const struct nf_hook_state *state)
{
	struct sock *sk;
	unsigned int thoff = 0;
	unsigned short frag_off = 0;
	int protohdr;
	uid_t uid;
	int found;

	/* A truncated or malformed packet has no full IPv6 header to read. */
	if (!pskb_may_pull(skb, sizeof(struct ipv6hdr)))
		return NF_ACCEPT;

	protohdr = ipv6_find_hdr(skb, &thoff, -1, &frag_off, NULL);
	if (protohdr != IPPROTO_TCP)
		return NF_ACCEPT;

	/* Only the first fragment carries the transport header. */
	if (frag_off)
		return NF_ACCEPT;

	sk = skb_to_full_sk(skb);
	if (!sk || !sk_fullsock(sk))
		return NF_ACCEPT;

	uid = __sock_i_uid(sk);
	if (uid < UID_MIN_VALUE)
		return NF_ACCEPT;

	found = find_and_clear_uid(uid);
	if (!found)
		return NF_ACCEPT;

	if (pkg_report((int)uid))
		pr_err("%s: up report failed!\n", __func__);

	return NF_ACCEPT;
}

static inline unsigned int freecess_ip4_out(void *priv,
					    struct sk_buff *skb,
					    const struct nf_hook_state *state)
{
	return NF_ACCEPT;
}

static inline unsigned int freecess_ip6_out(void *priv,
					    struct sk_buff *skb,
					    const struct nf_hook_state *state)
{
	return NF_ACCEPT;
}

static struct nf_hook_ops freecess_nf_ops[] = {
	{
		.hook		= freecess_ip4_in,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= NF_IP_PRI_SELINUX_LAST + 1,
	},
	{
		.hook		= freecess_ip6_in,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= NF_IP6_PRI_SELINUX_LAST + 1,
	},
	{
		.hook		= freecess_ip4_out,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= NF_IP_PRI_SELINUX_LAST + 1,
	},
	{
		.hook		= freecess_ip6_out,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= NF_IP6_PRI_SELINUX_LAST + 1,
	},
};

static int __init kfreecess_pkg_init(void)
{
	struct net *net;
	struct net *failed_net = NULL;
	int ret = 0;
	int i;

	for (i = 0; i < MAX_REC_UID; i++)
		atomic_set(&uid_rec[i], 0);

	rtnl_lock();
	for_each_net(net) {
		ret = nf_register_net_hooks(net, freecess_nf_ops,
					    ARRAY_SIZE(freecess_nf_ops));
		if (ret < 0) {
			failed_net = net;
			break;
		}
	}
	rtnl_unlock();

	if (failed_net) {
		pr_err("%s: nf_register_net_hooks failed: %d\n", __func__, ret);

		/*
		 * Only the namespaces iterated before the failing one were
		 * registered successfully; unregister just those. Calling
		 * nf_unregister_net_hooks() on the failing and later
		 * namespaces would warn ("hook not found") for hooks that
		 * were never registered.
		 */
		rtnl_lock();
		for_each_net(net) {
			if (net == failed_net)
				break;
			nf_unregister_net_hooks(net, freecess_nf_ops,
						ARRAY_SIZE(freecess_nf_ops));
		}
		rtnl_unlock();

		return ret;
	}

	ret = register_kfreecess_hook(MOD_PKG, kfreecess_pkg_hook);
	if (ret) {
		pr_err("%s: register MOD_PKG hook failed: %d\n", __func__, ret);
		goto err_unreg_hooks;
	}

	ret = register_kfreecess_hook(MOD_CFB, kfreecess_cfb_hook);
	if (ret) {
		pr_err("%s: register MOD_CFB hook failed: %d\n", __func__, ret);
		unregister_kfreecess_hook(MOD_PKG);
		goto err_unreg_hooks;
	}

	pr_debug("freecess pkg hooks registered\n");

	return 0;

err_unreg_hooks:
	rtnl_lock();
	for_each_net(net)
		nf_unregister_net_hooks(net, freecess_nf_ops,
					ARRAY_SIZE(freecess_nf_ops));
	rtnl_unlock();

	return ret;
}

static void __exit kfreecess_pkg_exit(void)
{
	struct net *net;

	unregister_kfreecess_hook(MOD_PKG);
	unregister_kfreecess_hook(MOD_CFB);

	rtnl_lock();
	for_each_net(net)
		nf_unregister_net_hooks(net, freecess_nf_ops,
					ARRAY_SIZE(freecess_nf_ops));
	rtnl_unlock();
}

module_init(kfreecess_pkg_init);
module_exit(kfreecess_pkg_exit);

MODULE_LICENSE("GPL");
