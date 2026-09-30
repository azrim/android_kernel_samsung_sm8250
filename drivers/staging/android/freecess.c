/*
 * Copyright (c) Samsung Technologies Co., Ltd. 2001-2017. All rights reserved.
 *
 * File name: freecess.c
 * Description: Use to thaw process from state 'D'
 * Author: chao.gu@samsung.com
 * Version: 0.2
 * Date:  2017/07/17
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/freecess.h>
#include <linux/freezer.h>
#include <net/sock.h>
#include <linux/hrtimer.h>
#include <linux/proc_fs.h>

#define RET_OK		0
#define RET_ERR		1

/* uid 1000 is AID_SYSTEM, the only uid allowed to talk to this driver. */
#define SYSTEM_UID	1000

/*
 * Shared state.
 *
 * bind_port[], freecess_fw_version and mod_recv_handler[] are written by
 * recv_handler(), which runs in the netlink receive path (softirq), and read
 * from the signal/binder paths (task context). They are all single words, so
 * they are kept consistent with the atomic helpers or READ_ONCE()/WRITE_ONCE()
 * rather than with a lock: mod_sendmsg() allocates with GFP_ATOMIC and is
 * called from atomic context, where a sleeping lock would be illegal.
 */
static struct sock *kfreecess_mod_sock;
static atomic_t bind_port[MOD_END];
static atomic_t kfreecess_init_suc;
static atomic_t last_kill_pid = ATOMIC_INIT(-1);

int freecess_fw_version;    /* record freecess framework version */

struct priv_data {
	int target_uid;
	int flag;		    /* MOD_SIG, MOD_BINDER */
	int code;		    /* RPC code */
	char rpcname[INTERFACETOKEN_BUFF_SIZE];	/* interface token */
	pkg_info_t pkg_info;	/* MOD_PKG */
};

static freecess_hook mod_recv_handler[MOD_END];

static int check_msg_type(int type)
{
	return type > MSG_NOOP && type < MSG_TYPE_END;
}

static int check_mod_type(int mod)
{
	return mod > MOD_NOOP && mod < MOD_END;
}

int thread_group_is_frozen(struct task_struct *task)
{
	struct task_struct *leader = task->group_leader;

	return freezing(leader) || frozen(leader);
}

/* Dumped on every loopback handshake, so keep it at debug level. */
static void dump_kfreecess_msg(const struct kfreecess_msg_data *msg)
{
	pr_debug("-----kfreecess msg dump-----\n");
	if (!msg) {
		pr_debug("msg is NULL\n");
		return;
	}

	pr_debug("type: %d\n", msg->type);
	pr_debug("mode: %d\n", msg->mod);
	pr_debug("src_portid: %d\n", msg->src_portid);
	pr_debug("dest_portid: %d\n", msg->dst_portid);
	pr_debug("kernel version: %d\n", FREECESS_KERNEL_VERSION);
	pr_debug("fw version: %d\n", FREECESS_PEER_VERSION(msg->version));
	pr_debug("target_uid: %d\n", msg->target_uid);
}

int mod_sendmsg(int type, int mod, struct priv_data *data)
{
	struct kfreecess_msg_data *payload;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int dst_portid;
	int ret;

	if (!atomic_read(&kfreecess_init_suc))
		return RET_ERR;

	if (!check_msg_type(type)) {
		pr_err("%s: msg type is invalid! %d\n", __func__, type);
		return RET_ERR;
	}

	if (!check_mod_type(mod)) {
		pr_err("%s: mod type is invalid! %d\n", __func__, mod);
		return RET_ERR;
	}

	skb = nlmsg_new(sizeof(struct kfreecess_msg_data), GFP_ATOMIC);
	if (!skb) {
		pr_err("%s: alloc_skb failed! %d\n", __func__, mod);
		return RET_ERR;
	}

	nlh = nlmsg_put(skb, 0, 0, 0, sizeof(struct kfreecess_msg_data), 0);
	if (!nlh) {
		kfree_skb(skb);
		return RET_ERR;
	}

	/* The daemon registers its portid via LOOPBACK; 0 means "not bound". */
	dst_portid = atomic_read(&bind_port[mod]);

	payload = nlmsg_data(nlh);
	payload->type = type;
	payload->mod = mod;
	payload->src_portid = KERNEL_ID_NETLINK;
	payload->dst_portid = dst_portid;
	payload->version = FREECESS_PACK_VERSION(FREECESS_KERNEL_VERSION);

	if (data) {
		payload->target_uid = data->target_uid;

		/*
		 * Keep the field packing wire-compatible with userspace: each
		 * module fills only the subset of fields it defines, and the
		 * rest keep the value nlmsg_new() gave them.
		 *   MOD_BINDER: code, rpcname, pkg_info.cmd, flag
		 *   MOD_SIG:    flag
		 *   MOD_PKG:    pkg_info (cmd + uid)
		 *   MOD_CFB:    flag
		 */
		switch (mod) {
		case MOD_BINDER:
			payload->code = data->code;
			memcpy(payload->rpcname, data->rpcname,
			       sizeof(data->rpcname));
			payload->pkg_info.cmd = data->pkg_info.cmd;
			payload->flag = data->flag;
			break;
		case MOD_PKG:
			memcpy(&payload->pkg_info, &data->pkg_info,
			       sizeof(pkg_info_t));
			break;
		default:	/* MOD_SIG, MOD_CFB */
			payload->flag = data->flag;
			break;
		}
	}

	/*
	 * nlmsg_unicast() takes ownership of @skb and frees it on both the
	 * success and the failure path, so do not free it here.
	 */
	ret = nlmsg_unicast(kfreecess_mod_sock, skb, dst_portid);
	if (ret < 0) {
		pr_err("%s: nlmsg_unicast failed! errno %d\n", __func__, ret);
		return RET_ERR;
	}

	return RET_OK;
}

int sig_report(struct task_struct *p, bool report_pid)
{
	struct priv_data data;
	int target_pid = task_tgid_nr(p);

	memset(&data, 0, sizeof(struct priv_data));
	data.target_uid = task_uid(p).val;
	if (report_pid && !p->group_leader->ptrace)
		data.flag = target_pid;

	/*
	 * Report at most once per pid: the first frozen thread that gets a
	 * signal wakes userspace, later signals for the same pid are dropped.
	 */
	if (!thread_group_is_frozen(p) ||
	    target_pid == atomic_read(&last_kill_pid))
		return RET_OK;

	atomic_set(&last_kill_pid, target_pid);
	return mod_sendmsg(MSG_TO_USER, MOD_SIG, &data);
}

int binder_report(struct task_struct *p, int code, const char *str, int flag)
{
	struct priv_data data;

	memset(&data, 0, sizeof(struct priv_data));
	data.target_uid = -1;
	data.flag = flag;
	data.code = code;
	strlcpy(data.rpcname, str, INTERFACETOKEN_BUFF_SIZE);
	if (p) {
		data.target_uid = task_uid(p).val;
		data.pkg_info.cmd = p->pid;	/* binder carries pid here */
	}

	return mod_sendmsg(MSG_TO_USER, MOD_BINDER, &data);
}

int pkg_report(int target_uid)
{
	struct priv_data data;

	memset(&data, 0, sizeof(struct priv_data));
	data.target_uid = target_uid;
	data.pkg_info.uid = (uid_t)target_uid;

	return mod_sendmsg(MSG_TO_USER, MOD_PKG, &data);
}

int cfb_report(int target_uid, const char *reason)
{
	struct priv_data data;

	pr_info("cfb_report: uid %d frozen, reason: %s\n", target_uid, reason);
	memset(&data, 0, sizeof(struct priv_data));
	data.target_uid = target_uid;

	return mod_sendmsg(MSG_TO_USER, MOD_CFB, &data);
}

static void recv_handler(struct sk_buff *skb)
{
	struct kfreecess_msg_data *payload;
	struct nlmsghdr *nlh;
	unsigned int msglen;
	freecess_hook hook;
	int mod;
	int type;
	uid_t uid;

	if (!skb) {
		pr_err("%s: skb is NULL!\n", __func__);
		return;
	}

	uid = NETLINK_CREDS(skb)->uid.val;
	/* Only the system user may talk to the kernel side of Freecess. */
	if (uid != SYSTEM_UID) {
		pr_err_ratelimited("freecess: uid %u permission denied\n", uid);
		return;
	}

	/*
	 * Reject anything that is not a whole netlink header plus a whole
	 * kfreecess_msg_data before touching the payload.
	 */
	if (skb->len < NLMSG_SPACE(0) + sizeof(struct kfreecess_msg_data)) {
		pr_err("freecess: skb too short %u < %zu\n", skb->len,
		       NLMSG_SPACE(0) + sizeof(struct kfreecess_msg_data));
		return;
	}

	nlh = nlmsg_hdr(skb);
	msglen = NLMSG_PAYLOAD(nlh, 0);
	if (msglen < sizeof(struct kfreecess_msg_data)) {
		pr_err("freecess: payload too short %u < %zu\n", msglen,
		       sizeof(struct kfreecess_msg_data));
		return;
	}

	payload = nlmsg_data(nlh);

	/*
	 * Every field below is controlled by userspace. Validate it first and
	 * act only on the validated values, never on a field that was not
	 * checked.
	 */
	if (payload->src_portid < 0) {
		pr_err("freecess: invalid src_portid %d\n",
		       payload->src_portid);
		return;
	}

	if (payload->dst_portid != KERNEL_ID_NETLINK) {
		pr_err("freecess: dst_portid %d is not kernel\n",
		       payload->dst_portid);
		return;
	}

	mod = payload->mod;
	if (!check_mod_type(mod)) {
		pr_err("freecess: invalid mod %d\n", mod);
		return;
	}

	type = payload->type;
	if (!check_msg_type(type)) {
		pr_err("freecess: invalid type %d\n", type);
		return;
	}

	switch (type) {
	case LOOPBACK_MSG:
		/*
		 * Handshake: the daemon publishes its portid and framework
		 * version. This is the only writer of bind_port[].
		 */
		atomic_set(&bind_port[mod], payload->src_portid);
		WRITE_ONCE(freecess_fw_version,
			   FREECESS_PEER_VERSION(payload->version));
		dump_kfreecess_msg(payload);
		mod_sendmsg(LOOPBACK_MSG, mod, NULL);
		break;
	case MSG_TO_KERN:
		hook = READ_ONCE(mod_recv_handler[mod]);
		if (hook)
			hook(payload, sizeof(struct kfreecess_msg_data));
		break;
	default:
		/* MSG_TO_USER is kernel->user only; nothing to dispatch. */
		pr_debug("freecess: unexpected type %d from user\n", type);
		break;
	}
}

int register_kfreecess_hook(int mod, freecess_hook hook)
{
	if (!check_mod_type(mod)) {
		pr_err("%s: mod type is invalid! %d\n", __func__, mod);
		return RET_ERR;
	}

	if (hook)
		WRITE_ONCE(mod_recv_handler[mod], hook);

	return RET_OK;
}

int unregister_kfreecess_hook(int mod)
{
	if (!check_mod_type(mod)) {
		pr_err("%s: mod type is invalid! %d\n", __func__, mod);
		return RET_ERR;
	}

	WRITE_ONCE(mod_recv_handler[mod], NULL);

	return RET_OK;
}

static int __init kfreecess_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = recv_handler,
	};
	int i;

	kfreecess_mod_sock = netlink_kernel_create(&init_net,
						   NETLINK_KFREECESS, &cfg);
	if (!kfreecess_mod_sock) {
		pr_err("%s: create kfreecess_mod_sock socket error!\n",
		       __func__);
		return RET_ERR;
	}

	for (i = MOD_NOOP + 1; i < MOD_END; i++)
		atomic_set(&bind_port[i], 0);

	/* Publish readiness only once all state is initialised. */
	atomic_set(&kfreecess_init_suc, 1);

	return RET_OK;
}

static void __exit kfreecess_exit(void)
{
	if (kfreecess_mod_sock)
		netlink_kernel_release(kfreecess_mod_sock);
}

module_init(kfreecess_init);
module_exit(kfreecess_exit);

MODULE_LICENSE("GPL");
