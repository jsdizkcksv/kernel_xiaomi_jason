/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2009 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

/*
 * lowcomms.c
 *
 * This is the "low-level" comms layer.
 *
 * It is responsible for sending/receiving messages
 * from other nodes in the cluster.
 *
 * Cluster nodes are referred to by their nodeids. nodeids are
 * simply 32 bit numbers to the locking module - if they need to
 * be expanded for the cluster infrastructure then that is its
 * responsibility. It is this layer's
 * responsibility to resolve these into IP address or
 * whatever it needs for inter-node communication.
 *
 * The comms level is two kernel threads that deal mainly with
 * the receiving of messages from other nodes and passing them
 * up to the mid-level comms layer (which understands the
 * message format) for execution by the locking core, and
 * a send thread which does all the setting up of connections
 * to remote nodes and the sending of data. Threads are not allowed
 * to send their own data because it may cause them to wait in times
 * of high load. Also, this way, the sending thread can collect together
 * messages bound for one node and send them in one block.
 *
 * lowcomms will choose to use either TCP or SCTP as its transport layer
 * depending on the configuration variable 'protocol'. This should be set
 * to 0 (default) for TCP or 1 for SCTP. It should be configured using a
 * cluster-wide mechanism as it must be the same on all nodes of the cluster
 * for the DLM to function.
 *
 */

#include <asm/ioctls.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/mutex.h>
#include <linux/sctp.h>
#include <linux/slab.h>
#include <net/sctp/sctp.h>
#include <net/ipv6.h>

#include "dlm_internal.h"
#include "lowcomms.h"
#include "midcomms.h"
#include "config.h"

#define NEEDED_RMEM (4*1024*1024)
#define CONN_HASH_SIZE 32

/* Number of messages to send before rescheduling */
#define MAX_SEND_MSG_COUNT 25

struct cbuf {
	unsigned int base;
	unsigned int len;
	unsigned int mask;
};

static void cbuf_add(struct cbuf *cb, int n)
{
	cb->len += n;
}

static int cbuf_data(struct cbuf *cb)
{
	return ((cb->base + cb->len) & cb->mask);
}

static void cbuf_init(struct cbuf *cb, int size)
{
	cb->base = cb->len = 0;
	cb->mask = size-1;
}

static void cbuf_eat(struct cbuf *cb, int n)
{
	cb->len  -= n;
	cb->base += n;
	cb->base &= cb->mask;
}

static bool cbuf_empty(struct cbuf *cb)
{
	return cb->len == 0;
}

struct connection {
	struct socket *sock;	/* NULL if not connected */
	uint32_t nodeid;	/* So we know who we are in the list */
	struct mutex sock_mutex;
	unsigned long flags;
#define CF_READ_PENDING 1
#define CF_WRITE_PENDING 2
#define CF_CONNECT_PENDING 3
#define CF_INIT_PENDING 4
#define CF_IS_OTHERCON 5
#define CF_CLOSE 6
#define CF_APP_LIMITED 7
	struct list_head writequeue;  /* List of outgoing writequeue_entries */
	spinlock_t writequeue_lock;
	int (*rx_action) (struct connection *);	/* What to do when active */
	void (*connect_action) (struct connection *);	/* What to do to connect */
	struct page *rx_page;
	struct cbuf cb;
	int retries;
#define MAX_CONNECT_RETRIES 3
	struct hlist_node list;
	struct connection *othercon;
	struct work_struct rwork; /* Receive workqueue */
	struct work_struct swork; /* Send workqueue */
	void (*orig_error_report)(struct sock *sk);
};
#define sock2con(x) ((struct connection *)(x)->sk_user_data)

/* An entry waiting to be sent */
struct writequeue_entry {
	struct list_head list;
	struct page *page;
	int offset;
	int len;
	int end;
	int users;
	struct connection *con;
};

struct dlm_node_addr {
	struct list_head list;
	int nodeid;
	int addr_count;
	int curr_addr_index;
	struct sockaddr_storage *addr[DLM_MAX_ADDR_COUNT];
};

static LIST_HEAD(dlm_node_addrs);
static DEFINE_SPINLOCK(dlm_node_addrs_spin);

static struct sockaddr_storage *dlm_local_addr[DLM_MAX_ADDR_COUNT];
static int dlm_local_count;
static int dlm_allow_conn;

/* Work queues */
static struct workqueue_struct *recv_workqueue;
static struct workqueue_struct *send_workqueue;

static struct hlist_head connection_hash[CONN_HASH_SIZE];
static DEFINE_MUTEX(connections_lock);
static struct kmem_cache *con_cache;

static void process_recv_sockets(struct work_struct *work);
static void process_send_sockets(struct work_struct *work);


/* This is deliberately very simple because most clusters have simple
   sequential nodeids, so we should be able to go straight to a connection
   struct in the array */
static inline int nodeid_hash(int nodeid)
{
	return nodeid & (CONN_HASH_SIZE-1);
}

static struct connection *__find_con(int nodeid)
{
	int r;
	struct connection *con;

	r = nodeid_hash(nodeid);

	hlist_for_each_entry(con, &connection_hash[r], list) {
		if (con->nodeid == nodeid)
			return con;
	}
	return NULL;
}

/*
 * If 'allocation' is zero then we don't attempt to create a new
 * connection structure for this node.
 */
static struct connection *__nodeid2con(int nodeid, gfp_t alloc)
{
	struct connection *con = NULL;
	int r;

	con = __find_con(nodeid);
	if (con || !alloc)
		return con;

	con = kmem_cache_zalloc(con_cache, alloc);
	if (!con)
		return NULL;

	r = nodeid_hash(nodeid);
	hlist_add_head(&con->list, &connection_hash[r]);

	con->nodeid = nodeid;
	mutex_init(&con->sock_mutex);
	INIT_LIST_HEAD(&con->writequeue);
	spin_lock_init(&con->writequeue_lock);
	INIT_WORK(&con->swork, process_send_sockets);
	INIT_WORK(&con->rwork, process_recv_sockets);

	/* Setup action pointers for child sockets */
	if (con->nodeid) {
		struct connection *zerocon = __find_con(0);

		con->connect_action = zerocon->connect_action;
		if (!con->rx_action)
			con->rx_action = zerocon->rx_action;
	}

	return con;
}

/* Loop round all connections */
static void foreach_conn(void (*conn_func)(struct connection *c))
{
	int i;
	struct hlist_node *n;
	struct connection *con;

	for (i = 0; i < CONN_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(con, n, &connection_hash[i], list)
			conn_func(con);
	}
}

static struct connection *nodeid2con(int nodeid, gfp_t allocation)
{
	struct connection *con;

	mutex_lock(&connections_lock);
	con = __nodeid2con(nodeid, allocation);
	mutex_unlock(&connections_lock);

	return con;
}

static struct dlm_node_addr *find_node_addr(int nodeid)
{
	struct dlm_node_addr *na;

	list_for_each_entry(na, &dlm_node_addrs, list) {
		if (na->nodeid == nodeid)
			return na;
	}
	return NULL;
}

static int addr_compare(struct sockaddr_storage *x, struct sockaddr_storage *y)
{
	switch (x->ss_family) {
	case AF_INET: {
		struct sockaddr_in *sinx = (struct sockaddr_in *)x;
		struct sockaddr_in *siny = (struct sockaddr_in *)y;
		if (sinx->sin_addr.s_addr != siny->sin_addr.s_addr)
			return 0;
		if (sinx->sin_port != siny->sin_port)
			return 0;
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sinx = (struct sockaddr_in6 *)x;
		struct sockaddr_in6 *siny = (struct sockaddr_in6 *)y;
		if (!ipv6_addr_equal(&sinx->sin6_addr, &siny->sin6_addr))
			return 0;
		if (sinx->sin6_port != siny->sin6_port)
			return 0;
		break;
	}
	default:
		return 0;
	}
	return 1;
}

static int nodeid_to_addr(int nodeid, struct sockaddr_storage *sas_out,
			  struct sockaddr *sa_out, bool try_new_addr)
{
	struct sockaddr_storage sas;
	struct dlm_node_addr *na;

	if (!dlm_local_count)
		return -1;

	spin_lock(&dlm_node_addrs_spin);
	na = find_node_addr(nodeid);
	if (na && na->addr_count) {
		memcpy(&sas, na->addr[na->curr_addr_index],
		       sizeof(struct sockaddr_storage));

		if (try_new_addr) {
			na->curr_addr_index++;
			if (na->curr_addr_index == na->addr_count)
				na->curr_addr_index = 0;
		}
	}
	spin_unlock(&dlm_node_addrs_spin);

	if (!na)
		return -EEXIST;

	if (!na->addr_count)
		return -ENOENT;

	if (sas_out)
		memcpy(sas_out, &sas, sizeof(struct sockaddr_storage));

	if (!sa_out)
		return 0;

	if (dlm_local_addr[0]->ss_family == AF_INET) {
		struct sockaddr_in *in4  = (struct sockaddr_in *) &sas;
		struct sockaddr_in *ret4 = (struct sockaddr_in *) sa_out;
		ret4->sin_addr.s_addr = in4->sin_addr.s_addr;
	} else {
		struct sockaddr_in6 *in6  = (struct sockaddr_in6 *) &sas;
		struct sockaddr_in6 *ret6 = (struct sockaddr_in6 *) sa_out;
		ret6->sin6_addr = in6->sin6_addr;
	}

	return 0;
}

static int addr_to_nodeid(struct sockaddr_storage *addr, int *nodeid)
{
	struct dlm_node_addr *na;
	int rv = -EEXIST;
	int addr_i;

	spin_lock(&dlm_node_addrs_spin);
	list_for_each_entry(na, &dlm_node_addrs, list) {
		if (!na->addr_count)
			continue;

		for (addr_i = 0; addr_i < na->addr_count; addr_i++) {
			if (addr_compare(na->addr[addr_i], addr)) {
				*nodeid = na->nodeid;
				rv = 0;
				goto unlock;
			}
		}
	}
unlock:
	spin_unlock(&dlm_node_addrs_spin);
	return rv;
}

int dlm_lowcomms_addr(int nodeid, struct sockaddr_storage *addr, int len)
{
	struct sockaddr_storage *new_addr;
	struct dlm_node_addr *new_node, *na;

	new_node = kzalloc(sizeof(struct dlm_node_addr), GFP_NOFS);
	if (!new_node)
		return -ENOMEM;

	new_addr = kzalloc(sizeof(struct sockaddr_storage), GFP_NOFS);
	if (!new_addr) {
		kfree(new_node);
		return -ENOMEM;
	}

	memcpy(new_addr, addr, len);

	spin_lock(&dlm_node_addrs_spin);
	na = find_node_addr(nodeid);
	if (!na) {
		new_node->nodeid = nodeid;
		new_node->addr[0] = new_addr;
		new_node->addr_count = 1;
		list_add(&new_node->list, &dlm_node_addrs);
		spin_unlock(&dlm_node_addrs_spin);
		return 0;
	}

	if (na->addr_count >= DLM_MAX_ADDR_COUNT) {
		spin_unlock(&dlm_node_addrs_spin);
		kfree(new_addr);
		kfree(new_node);
		return -ENOSPC;
	}

	na->addr[na->addr_count++] = new_addr;
	spin_unlock(&dlm_node_addrs_spin);
	kfree(new_node);
	return 0;
}

/* Data available on socket or listen socket received a connect */
static void lowcomms_data_ready(struct sock *sk)
{
	struct connection *con = sock2con(sk);
	if (con && !test_and_set_bit(CF_READ_PENDING, &con->flags))
		queue_work(recv_workqueue, &con->rwork);
}

static void lowcomms_write_space(struct sock *sk)
{
	struct connection *con = sock2con(sk);

	if (!con)
		return;

	clear_bit(SOCK_NOSPACE, &con->sock->flags);

	if (test_and_clear_bit(CF_APP_LIMITED, &con->flags)) {
		con->sock->sk->sk_write_pending--;
		clear_bit(SOCKWQ_ASYNC_NOSPACE, &con->sock->flags);
	}

	if (!test_and_set_bit(CF_WRITE_PENDING, &con->flags))
		queue_work(send_workqueue, &con->swork);
}

static inline void lowcomms_connect_sock(struct connection *con)
{
	if (test_bit(CF_CLOSE, &con->flags))
		return;
	if (!test_and_set_bit(CF_CONNECT_PENDING, &con->flags))
		queue_work(send_workqueue, &con->swork);
}

static void lowcomms_state_change(struct sock *sk)
{
	/* SCTP layer is not calling sk_data_ready when the connection
	 * is done, so we catch the signal through here. Also, it
	 * doesn't switch socket state when entering shutdown, so we
	 * skip the write in that case.
	 */
	if (sk->sk_shutdown) {
		if (sk->sk_shutdown == RCV_SHUTDOWN)
			lowcomms_data_ready(sk);
	} else if (sk->sk_state == TCP_ESTABLISHED) {
		lowcomms_write_space(sk);
	}
}

int dlm_lowcomms_connect_node(int nodeid)
{
	struct connection *con;

	if (nodeid == dlm_our_nodeid())
		return 0;

	con = nodeid2con(nodeid, GFP_NOFS);
	if (!con)
		return -ENOMEM;
	lowcomms_connect_sock(con);
	return 0;
}

static void lowcomms_error_report(struct sock *sk)
{
	struct connection *con = sock2con(sk);
	struct sockaddr_storage saddr;

	if (nodeid_to_addr(con->nodeid, &saddr, NULL, false)) {
		printk_ratelimited(KERN_ERR "dlm: node %d: socket error "
				   "sending to node %d, port %d, "
				   "sk_err=%d/%d\n", dlm_our_nodeid(),
				   con->nodeid, dlm_config.ci_tcp_port,
				   sk->sk_err, sk->sk_err_soft);
		return;
	} else if (saddr.ss_family == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)&saddr;

		printk_ratelimited(KERN_ERR "dlm: node %d: socket error "
				   "sending to node %d at %pI4, port %d, "
				   "sk_err=%d/%d\n", dlm_our_nodeid(),
				   con->nodeid, &sin4->sin_addr.s_addr,
				   dlm_config.ci_tcp_port, sk->sk_err,
				   sk->sk_err_soft);
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&saddr;

		printk_ratelimited(KERN_ERR "dlm: node %d: socket error "
				   "sending to node %d at %u.%u.%u.%u, "
				   "port %d, sk_err=%d/%d\n", dlm_our_nodeid(),
				   con->nodeid, sin6->sin6_addr.s6_addr32[0],
				   sin6->sin6_addr.s6_addr32[1],
				   sin6->sin6_addr.s6_addr32[2],
				   sin6->sin6_addr.s6_addr32[3],
				   dlm_config.ci_tcp_port, sk->sk_err,
				   sk->sk_err_soft);
	}
	con->orig_error_report(sk);
}

/* Make a socket active */
static void add_sock(struct socket *sock, struct connection *con)
{
	con->sock = sock;

	/* Install a data_ready callback */
	con->sock->sk->sk_data_ready = lowcomms_data_ready;
	con->sock->sk->sk_write_space = lowcomms_write_space;
	con->sock->sk->sk_state_change = lowcomms_state_change;
	con->sock->sk->sk_user_data = con;
	con->sock->sk->sk_allocation = GFP_NOFS;
	con->orig_error_report = con->sock->sk->sk_error_report;
	con->sock->sk->sk_error_report = lowcomms_error_report;
}

/* Add the port number to an IPv6 or 4 sockaddr and return the address
   length */
static void make_sockaddr(struct sockaddr_storage *saddr, uint16_t port,
			  int *addr_len)
{
	saddr->ss_family =  dlm_local_addr[0]->ss_family;
	if (saddr->ss_family == AF_INET) {
		struct sockaddr_in *in4_addr = (struct sockaddr_in *)saddr;
		in4_addr->sin_port = cpu_to_be16(port);
		*addr_len = sizeof(struct sockaddr_in);
		memset(&in4_addr->sin_zero, 0, sizeof(in4_addr->sin_zero));
	} else {
		struct sockaddr_in6 *in6_addr = (struct sockaddr_in6 *)saddr;
		in6_addr->sin6_port = cpu_to_be16(port);
		*addr_len = sizeof(struct sockaddr_in6);
	}
	memset((char *)saddr + *addr_len, 0, sizeof(struct sockaddr_storage) - *addr_len);
}

/* Close a remote connection and tidy up */
static void close_connection(struct connection *con, bool and_other,
			     bool tx, bool rx)
{
	clear_bit(CF_CONNECT_PENDING, &con->flags);
	clear_bit(CF_WRITE_PENDING, &con->flags);
	if (tx && cancel_work_sync(&con->swork))
		log_print("canceled swork for node %d", con->nodeid);
	if (rx && cancel_work_sync(&con->rwork))
		log_print("canceled rwork for node %d", con->nodeid);

	mutex_lock(&con->sock_mutex);
	if (con->sock) {
		sock_release(con->sock);
		con->sock = NULL;
	}
	if (con->othercon && and_other) {
		/* Will only re-enter once. */
		close_connection(con->othercon, false, tx, rx);
	}
	if (con->rx_page) {
		__free_page(con->rx_page);
		con->rx_page = NULL;
	}

	con->retries = 0;
	mutex_unlock(&con->sock_mutex);
}

/* Data received from remote end */
static int receive_from_sock(struct connection *con)
{
	int ret = 0;
	struct msghdr msg = {};
	struct kvec iov[2];
	unsigned len;
	int r;
	int call_again_soon = 0;
	int nvec;

	mutex_lock(&con->sock_mutex);

	if (con->sock == NULL) {
		ret = -EAGAIN;
		goto out_close;
	}
	if (con->nodeid == 0) {
		ret = -EINVAL;
		goto out_close;
	}

	if (con->rx_page == NULL) {
		/*
		 * This doesn't need to be atomic, but I think it should
		 * improve performance if it is.
		 */
		con->rx_page = alloc_page(GFP_ATOMIC);
		if (con->rx_page == NULL)
			goto out_resched;
		cbuf_init(&con->cb, PAGE_CACHE_SIZE);
	}

	/*
	 * iov[0] is the bit of the circular buffer between the current end
	 * point (cb.base + cb.len) and the end of the buffer.
	 */
	iov[0].iov_len = con->cb.base - cbuf_data(&con->cb);
	iov[0].iov_base = page_address(con->rx_page) + cbuf_data(&con->cb);
	iov[1].iov_len = 0;
	nvec = 1;

	/*
	 * iov[1] is the bit of the circular buffer between the start of the
	 * buffer and the start of the currently used section (cb.base)
	 */
	if (cbuf_data(&con->cb) >= con->cb.base) {
		iov[0].iov_len = PAGE_CACHE_SIZE - cbuf_data(&con->cb);
		iov[1].iov_len = con->cb.base;
		iov[1].iov_base = page_address(con->rx_page);
		nvec = 2;
	}
	len = iov[0].iov_len + iov[1].iov_len;

	r = ret = kernel_recvmsg(con->sock, &msg, iov, nvec, len,
			       MSG_DONTWAIT | MSG_NOSIGNAL);
	if (ret <= 0)
		goto out_close;
	else if (ret == len)
		call_again_soon = 1;

	cbuf_add(&con->cb, ret);
	ret = dlm_process_incoming_buffer(con->nodeid,
					  page_address(con->rx_page),
					  con->cb.base, con->cb.len,
					  PAGE_CACHE_SIZE);
	if (ret == -EBADMSG) {
		log_print("lowcomms: addr=%p, base=%u, len=%u, read=%d",
			  page_address(con->rx_page), con->cb.base,
			  con->cb.len, r);
	}
	if (ret < 0)
		goto out_close;
	cbuf_eat(&con->cb, ret);

	if (cbuf_empty(&con->cb) && !call_again_soon) {
		__free_page(con->rx_page);
		con->rx_page = NULL;
	}

	if (call_again_soon)
		goto out_resched;
	mutex_unlock(&con->sock_mutex);
	return 0;

out_resched:
	if (!test_and_set_bit(CF_READ_PENDING, &con->flags))
		queue_work(recv_workqueue, &con->rwork);
	mutex_unlock(&con->sock_mutex);
	return -EAGAIN;

out_close:
	mutex_unlock(&con->sock_mutex);
	if (ret != -EAGAIN) {
		close_connection(con, false, true, false);
		/* Reconnect when there is something to send */
	}
	/* Don't return success if we really got EOF */
	if (ret == 0)
		ret = -EAGAIN;

	return ret;
}

/* Listening socket is busy, accept a connection */
static int tcp_accept_from_sock(struct connection *con)
{
	int result;
	struct sockaddr_storage peeraddr;
	struct socket *newsock;
	int len;
	int nodeid;
	struct connection *newcon;
	struct connection *addcon;

	mutex_lock(&connections_lock);
	if (!dlm_allow_conn) {
		mutex_unlock(&connections_lock);
		return -1;
	}
	mutex_unlock(&connections_lock);

	memset(&peeraddr, 0, sizeof(peeraddr));
	result = sock_create_kern(&init_net, dlm_local_addr[0]->ss_family,
				  SOCK_STREAM, IPPROTO_TCP, &newsock);
	if (result < 0)
		return -ENOMEM;

	mutex_lock_nested(&con->sock_mutex, 0);

	result = -ENOTCONN;
	if (con->sock == NULL)
		goto accept_err;

	newsock->type = con->sock->type;
	newsock->ops = con->sock->ops;

	result = con->sock->ops->accept(con->sock, newsock, O_NONBLOCK, true);
	if (result < 0)
		goto accept_err;

	/* Get the connected socket's peer */
	memset(&peeraddr, 0, sizeof(peeraddr));
	if (newsock->ops->getname(newsock, (struct sockaddr *)&peeraddr,
				  &len, 2)) {
		result = -ECONNABORTED;
		goto accept_err;
	}

	/* Get the new node's NODEID */
	make_sockaddr(&peeraddr, 0, &len);
	if (addr_to_nodeid(&peeraddr, &nodeid)) {
		unsigned char *b=(unsigned char *)&peeraddr;
		log_print("connect from non cluster node");
		print_hex_dump_bytes("ss: ", DUMP_PREFIX_NONE, 
				     b, sizeof(struct sockaddr_storage));
		sock_release(newsock);
		mutex_unlock(&con->sock_mutex);
		return -1;
	}

	log_print("got connection from %d", nodeid);

	/*  Check to see if we already have a connection to this node. This
	 *  could happen if the two nodes initiate a connection at roughly
	 *  the same time and the connections cross on the wire.
	 *  In this case we store the incoming one in "othercon"
	 */
	newcon = nodeid2con(nodeid, GFP_NOFS);
	if (!newcon) {
		result = -ENOMEM;
		goto accept_err;
	}
	mutex_lock_nested(&newcon->sock_mutex, 1);
	if (newcon->sock) {
		struct connection *othercon = newcon->othercon;

		if (!othercon) {
			othercon = kmem_cache_zalloc(con_cache, GFP_NOFS);
			if (!othercon) {
				log_print("failed to allocate incoming socket");
				mutex_unlock(&newcon->sock_mutex);
				result = -ENOMEM;
				goto accept_err;
			}
			othercon->nodeid = nodeid;
			othercon->rx_action = receive_from_sock;
			mutex_init(&othercon->sock_mutex);
			INIT_WORK(&othercon->swork, process_send_sockets);
			INIT_WORK(&othercon->rwork, process_recv_sockets);
			set_bit(CF_IS_OTHERCON, &othercon->flags);
		}
		if (!othercon->sock) {
			newcon->othercon = othercon;
			othercon->sock = newsock;
			newsock->sk->sk_user_data = othercon;
			add_sock(newsock, othercon);
			addcon = othercon;
		}
		else {
			printk("Extra connection from node %d attempted\n", nodeid);
			result = -EAGAIN;
			mutex_unlock(&newcon->sock_mutex);
			goto accept_err;
		}
	}
	else {
		newsock->sk->sk_user_data = newcon;
		newcon->rx_action = receive_from_sock;
		add_sock(newsock, newcon);
		addcon = newcon;
	}

	mutex_unlock(&newcon->sock_mutex);

	/*
	 * Add it to the active queue in case we got data
	 * between processing the accept adding the socket
	 * to the read_sockets list
	 */
	if (!test_and_set_bit(CF_READ_PENDING, &addcon->flags))
		queue_work(recv_workqueue, &addcon->rwork);
	mutex_unlock(&con->sock_mutex);

	return 0;

accept_err:
	mutex_unlock(&con->sock_mutex);
	sock_release(newsock);

	if (result != -EAGAIN)
		log_print("error accepting connection from node: %d", result);
	return result;
}

static int sctp_accept_from_sock(struct connection *con)
{
	/* Check that the new node is in the lockspace */
	struct sctp_prim prim;
	int nodeid;
	int prim_len, ret;
	int addr_len;
	struct connection *newcon;
	struct connection *addcon;
	struct socket *newsock;

	mutex_lock(&connections_lock);
	if (!dlm_allow_conn) {
		mutex_unlock(&connections_lock);
		return -1;
	}
	mutex_unlock(&connections_lock);

	mutex_lock_nested(&con->sock_mutex, 0);

	ret = kernel_accept(con->sock, &newsock, O_NONBLOCK);
	if (ret < 0)
		goto accept_err;

	memset(&prim, 0, sizeof(struct sctp_prim));
	prim_len = sizeof(struct sctp_prim);

	ret = kernel_getsockopt(newsock, IPPROTO_SCTP, SCTP_PRIMARY_ADDR,
				(char *)&prim, &prim_len);
	if (ret < 0) {
		log_print("getsockopt/sctp_primary_addr failed: %d", ret);
		goto accept_err;
	}

	make_sockaddr(&prim.ssp_addr, 0, &addr_len);
	if (addr_to_nodeid(&prim.ssp_addr, &nodeid)) {
		unsigned char *b = (unsigned char *)&prim.ssp_addr;

		log_print("reject connect from unknown addr");
		print_hex_dump_bytes("ss: ", DUMP_PREFIX_NONE,
				     b, sizeof(struct sockaddr_storage));
		goto accept_err;
	}

	newcon = nodeid2con(nodeid, GFP_NOFS);
	if (!newcon) {
		ret = -ENOMEM;
		goto accept_err;
	}

	mutex_lock_nested(&newcon->sock_mutex, 1);

	if (newcon->sock) {
		struct connection *othercon = newcon->othercon;

		if (!othercon) {
			othercon = kmem_cache_zalloc(con_cache, GFP_NOFS);
			if (!othercon) {
				log_print("failed to allocate incoming socket");
				mutex_unlock(&newcon->sock_mutex);
				ret = -ENOMEM;
				goto accept_err;
			}
			othercon->nodeid = nodeid;
			othercon->rx_action = receive_from_sock;
			mutex_init(&othercon->sock_mutex);
			INIT_WORK(&othercon->swork, process_send_sockets);
			INIT_WORK(&othercon->rwork, process_recv_sockets);
			set_bit(CF_IS_OTHERCON, &othercon->flags);
		}
		if (!othercon->sock) {
			newcon->othercon = othercon;
			othercon->sock = newsock;
			newsock->sk->sk_user_data = othercon;
			add_sock(newsock, othercon);
			addcon = othercon;
		} else {
			printk("Extra connection from node %d attempted\n", nodeid);
			ret = -EAGAIN;
			mutex_unlock(&newcon->sock_mutex);
			goto accept_err;
		}
	} else {
		newsock->sk->sk_user_data = newcon;
		newcon->rx_action = receive_from_sock;
		add_sock(newsock, newcon);
		addcon = newcon;
	}

	log_print("connected to %d", nodeid);

	mutex_unlock(&newcon->sock_mutex);

	/*
	 * Add it to the active queue in case we got data
	 * between processing the accept adding the socket
	 * to the read_sockets list
	 */
	if (!test_and_set_bit(CF_READ_PENDING, &addcon->flags))
		queue_work(recv_workqueue, &addcon->rwork);
	mutex_unlock(&con->sock_mutex);

	return 0;

accept_err:
	mutex_unlock(&con->sock_mutex);
	if (newsock)
		sock_release(newsock);
	if (ret != -EAGAIN)
		log_print("error accepting connection from node: %d", ret);

	return ret;
}

static void free_entry(struct writequeue_entry *e)
{
	__free_page(e->page);
	kfree(e);
}

/*
 * writequeue_entry_complete - try to delete and free write queue entry
 * @e: write queue entry to try to delete
 * @completed: bytes completed
 *
 * writequeue_lock must be held.
 */
static void writequeue_entry_complete(struct writequeue_entry *e, int completed)
{
	e->offset += completed;
	e->len -= completed;

	if (e->len == 0 && e->users == 0) {
		list_del(&e->list);
		free_entry(e);
	}
}

/*
 * sctp_bind_addrs - bind a SCTP socket to all our addresses
 */
static int sctp_bind_addrs(struct connection *con, uint16_t port)
{
	struct sockaddr_storage localaddr;
	int i, addr_len, result = 0;

	for (i = 0; i < dlm_local_count; i++) {
		memcpy(&localaddr, dlm_local_addr[i], sizeof(localaddr));
		make_sockaddr(&localaddr, port, &addr_len);

		if (!i)
			result = kernel_bind(con->sock,
					     (struct sockaddr *)&localaddr,
					     addr_len);
		else
			result = kernel_setsockopt(con->sock, SOL_SCTP,
						   SCTP_SOCKOPT_BINDX_ADD,
						   (char *)&localaddr, addr_len);

		if (result < 0) {
			log_print("Can't bind to %d addr number %d, %d.\n",
				  port, i + 1, result);
			break;
		}
	}
	return result;
}

/* Initiate an SCTP association.
   This is a special case of send_to_sock() in that we don't yet have a
   peeled-off socket for this association, so we use the listening socket
   and add the primary IP address of the remote node.
 */
static void sctp_connect_to_sock(struct connection *con)
{
	struct sockaddr_storage daddr;
	int one = 1;
	int result;
	int addr_len;
	struct socket *sock;

	if (con->nodeid == 0) {
		log_print("attempt to connect sock 0 foiled");
		return;
	}

	mutex_lock(&con->sock_mutex);

	/* Some odd races can cause double-connects, ignore them */
	if (con->retries++ > MAX_CONNECT_RETRIES)
		goto out;

	if (con->sock) {
		log_print("node %d already connected.", con->nodeid);
		goto out;
	}

	memset(&daddr, 0, sizeof(daddr));
	result = nodeid_to_addr(con->nodeid, &daddr, NULL, true);
	if (result < 0) {
		log_print("no address for nodeid %d", con->nodeid);
		goto out;
	}

	/* Create a socket to communicate with */
	result = sock_create_kern(&init_net, dlm_local_addr[0]->ss_family,
				  SOCK_STREAM, IPPROTO_SCTP, &sock);
	if (result < 0)
		goto socket_err;

	sock->sk->sk_user_data = con;
	con->rx_action = receive_from_sock;
	con->connect_action = sctp_connect_to_sock;
	add_sock(sock, con);

	/* Bind to all addresses. */
	if (sctp_bind_addrs(con, 0))
		goto bind_err;

	make_sockaddr(&daddr, dlm_config.ci_tcp_port, &addr_len);

	log_print("connecting to %d", con->nodeid);

	/* Turn off Nagle's algorithm */
	kernel_setsockopt(sock, SOL_TCP, TCP_NODELAY, (char *)&one,
			  sizeof(one));

	result = sock->ops->connect(sock, (struct sockaddr *)&daddr, addr_len,
				   O_NONBLOCK);
	if (result == -EINPROGRESS)
		result = 0;
	if (result == 0)
		goto out;


bind_err:
	con->sock = NULL;
	sock_release(sock);

socket_err:
	/*
	 * Some errors are fatal and this list might need adjusting. For other
	 * errors we try again until the max number of retries is reached.
	 */
	if (result != -EHOSTUNREACH &&
	    result != -ENETUNREACH &&
	    result != -ENETDOWN &&
	    result != -EINVAL &&
	    result != -EPROTONOSUPPORT) {
		log_print("connect %d try %d error %d", con->nodeid,
			  con->retries, result);
		mutex_unlock(&con->sock_mutex);
		msleep(1000);
		clear_bit(CF_CONNECT_PENDING, &con->flags);
		lowcomms_connect_sock(con);
		return;
	}

out:
	mutex_unlock(&con->sock_mutex);
	set_bit(CF_WRITE_PENDING, &con->flags);
}

/* Connect a new socket to its peer */
static void tcp_connect_to_sock(struct connection *con)
{
	struct sockaddr_storage saddr, src_addr;
	int addr_len;
	struct socket *sock = NULL;
	int one = 1;
	int result;

	if (con->nodeid == 0) {
		log_print("attempt to connect sock 0 foiled");
		return;
	}

	mutex_lock(&con->sock_mutex);
	if (con->retries++ > MAX_CONNECT_RETRIES)
		goto out;

	/* Some odd races can cause double-connects, ignore them */
	if (con->sock)
		goto out;

	/* Create a socket to communicate with */
	result = sock_create_kern(&init_net, dlm_local_addr[0]->ss_family,
				  SOCK_STREAM, IPPROTO_TCP, &sock);
	if (result < 0)
		goto out_err;

	memset(&saddr, 0, sizeof(saddr));
	result = nodeid_to_addr(con->nodeid, &saddr, NULL, false);
	if (result < 0) {
		log_print("no address for nodeid %d", con->nodeid);
		goto out_err;
	}

	sock->sk->sk_user_data = con;
	con->rx_action = receive_from_sock;
	con->connect_action = tcp_connect_to_sock;
	add_sock(sock, con);

	/* Bind to our cluster-known address connecting to avoid
	   routing problems */
	memcpy(&src_addr, dlm_local_addr[0], sizeof(src_addr));
	make_sockaddr(&src_addr, 0, &addr_len);
	result = sock->ops->bind(sock, (struct sockaddr *) &src_addr,
				 addr_len);
	if (result < 0) {
		log_print("could not bind for connect: %d", result);
		/* This *may* not indicate a critical error */
	}

	make_sockaddr(&saddr, dlm_config.ci_tcp_port, &addr_len);

	log_print("connecting to %d", con->nodeid);

	/* Turn off Nagle's algorithm */
	kernel_setsockopt(sock, SOL_TCP, TCP_NODELAY, (char *)&one,
			  sizeof(one));

	result = sock->ops->connect(sock, (struct sockaddr *)&saddr, addr_len,
				   O_NONBLOCK);
	if (result == -EINPROGRESS)
		result = 0;
	if (result == 0)
		goto out;

out_err:
	if (con->sock) {
		sock_release(con->sock);
		con->sock = NULL;
	} else if (sock) {
		sock_release(sock);
	}
	/*
	 * Some errors are fatal and this list might need adjusting. For other
	 * errors we try again until the max number of retries is reached.
	 */
	if (result != -EHOSTUNREACH &&
	    result != -ENETUNREACH &&
	    result != -ENETDOWN && 
	    result != -EINVAL &&
	    result != -EPROTONOSUPPORT) {
		log_print("connect %d try %d error %d", con->nodeid,
			  con->retries, result);
		mutex_unlock(&con->sock_mutex);
		msleep(1000);
		clear_bit(CF_CONNECT_PENDING, &con->flags);
		lowcomms_connect_sock(con);
		return;
	}
out:
	mutex_unlock(&con->sock_mutex);
	set_bit(CF_WRITE_PENDING, &con->flags);
	return;
}

static struct socket *tcp_create_listen_sock(struct connection *con,
					     struct sockaddr_storage *saddr)
{
	struct socket *sock = NULL;
	int result = 0;
	int one = 1;
	int addr_len;

	if (dlm_local_addr[0]->ss_family == AF_INET)
		addr_len = sizeof(struct sockaddr_in);
	else
		addr_len = sizeof(struct sockaddr_in6);

	/* Create a socket to communicate with */
	result = sock_create_kern(&init_net, dlm_local_addr[0]->ss_family,
				  SOCK_STREAM, IPPROTO_TCP, &sock);
	if (result < 0) {
		log_print("Can't create listening comms socket");
		goto create_out;
	}

	/* Turn off Nagle's algorithm */
	kernel_setsockopt(sock, SOL_TCP, TCP_NODELAY, (char *)&one,
			  sizeof(one));

	result = kernel_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				   (char *)&one, sizeof(one));

	if (result < 0) {
		log_print("Failed to set SO_REUSEADDR on socket: %d", result);
	}
	con->rx_action = tcp_accept_from_sock;
	con->connect_action = tcp_connect_to_sock;

	/* Bind to our port */
	make_sockaddr(saddr, dlm_config.ci_tcp_port, &addr_len);
	result = sock->ops->bind(sock, (struct sockaddr *) saddr, addr_len);
	if (result < 0) {
		log_print("Can't bind to port %d", dlm_config.ci_tcp_port);
		sock_release(sock);
		sock = NULL;
		con->sock = NULL;
		goto create_out;
	}
	result = kernel_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
				 (char *)&one, sizeof(one));
	if (result < 0) {
		log_print("Set keepalive failed: %d", result);
	}

	result = sock->ops->listen(sock, 5);
	if (result < 0) {
		log_print("Can't listen on port %d", dlm_config.ci_tcp_port);
		sock_release(sock);
		sock = NULL;
		goto create_out;
	}

create_out:
	return sock;
}

/* Get local addresses */
static void init_local(void)
{
	struct sockaddr_storage sas, *addr;
	int i;

	dlm_local_count = 0;
	for (i = 0; i < DLM_MAX_ADDR_COUNT; i++) {
		if (dlm_our_addr(&sas, i))
			break;

		addr = kmalloc(sizeof(*addr), GFP_NOFS);
		if (!addr)
			break;
		memcpy(addr, &sas, sizeof(*addr));
		dlm_local_addr[dlm_local_count++] = addr;
	}
}

/* Initialise SCTP socket and bind to all interfaces */
static int sctp_listen_for_all(void)
{
	struct socket *sock = NULL;
	int result = -EINVAL;
	struct connection *con = nodeid2con(0, GFP_NOFS);
	int bufsize = NEEDED_RMEM;
	int one = 1;

	if (!con)
		return -ENOMEM;

	log_print("Using SCTP for communications");

	result = sock_create_kern(&init_net, dlm_local_addr[0]->ss_family,
				  SOCK_STREAM, IPPROTO_SCTP, &sock);
	if (result < 0) {
		log_print("Can't create comms socket, check SCTP is loaded");
		goto out;
	}

	result = kernel_setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE,
				 (char *)&bufsize, sizeof(bufsize));
	if (result)
		log_print("Error increasing buffer space on socket %d", result);

	result = kernel_setsockopt(sock, SOL_SCTP, SCTP_NODELAY, (char *)&one,
				   sizeof(one));
	if (result < 0)
		log_print("Could not set SCTP NODELAY error %d\n", result);

	/* Init con struct */
	sock->sk->sk_user_data = con;
	con->sock = sock;
	con->sock->sk->sk_data_ready = lowcomms_data_ready;
	con->rx_action = sctp_accept_from_sock;
	con->connect_action = sctp_connect_to_sock;

	/* Bind to all addresses. */
	if (sctp_bind_addrs(con, dlm_config.ci_tcp_port))
		goto create_delsock;

	result = sock->ops->listen(sock, 5);
	if (result < 0) {
		log_print("Can't set socket listening");
		goto create_delsock;
	}

	return 0;

create_delsock:
	sock_release(sock);
	con->sock = NULL;
out:
	return result;
}

static int tcp_listen_for_all(void)
{
	struct socket *sock = NULL;
	struct connection *con = nodeid2con(0, GFP_NOFS);
	int result = -EINVAL;

	if (!con)
		return -ENOMEM;

	/* We don't support multi-homed hosts */
	if (dlm_local_addr[1] != NULL) {
		log_print("TCP protocol can't handle multi-homed hosts, "
			  "try SCTP");
		return -EINVAL;
	}

	log_print("Using TCP for communications");

	sock = tcp_create_listen_sock(con, dlm_local_addr[0]);
	if (sock) {
		add_sock(sock, con);
		result = 0;
	}
	else {
		result = -EADDRINUSE;
	}

	return result;
}



static struct writequeue_entry *new_writequeue_entry(struct connection *con,
						     gfp_t allocation)
{
	struct writequeue_entry *entry;

	entry = kmalloc(sizeof(struct writequeue_entry), allocation);
	if (!entry)
		return NULL;

	entry->page = alloc_page(allocation);
	if (!entry->page) {
		kfree(entry);
		return NULL;
	}

	entry->offset = 0;
	entry->len = 0;
	entry->end = 0;
	entry->users = 0;
	entry->con = con;

	return entry;
}

void *dlm_lowcomms_get_buffer(int nodeid, int len, gfp_t allocation, char **ppc)
{
	struct connection *con;
	struct writequeue_entry *e;
	int offset = 0;

	con = nodeid2con(nodeid, allocation);
	if (!con)
		return NULL;

	spin_lock(&con->writequeue_lock);
	e = list_entry(con->writequeue.prev, struct writequeue_entry, list);
	if ((&e->list == &con->writequeue) ||
	    (PAGE_CACHE_SIZE - e->end < len)) {
		e = NULL;
	} else {
		offset = e->end;
		e->end += len;
		e->users++;
	}
	spin_unlock(&con->writequeue_lock);

	if (e) {
	got_one:
		*ppc = page_address(e->page) + offset;
		return e;
	}

	e = new_writequeue_entry(con, allocation);
	if (e) {
		spin_lock(&con->writequeue_lock);
		offset = e->end;
		e->end += len;
		e->users++;
		list_add_tail(&e->list, &con->writequeue);
		spin_unlock(&con->writequeue_lock);
		goto got_one;
	}
	return NULL;
}

void dlm_lowcomms_commit_buffer(void *mh)
{
	struct writequeue_entry *e = (struct writequeue_entry *)mh;
	struct connection *con = e->con;
	int users;

	spin_lock(&con->writequeue_lock);
	users = --e->users;
	if (users)
		goto out;
	e->len = e->end - e->offset;
	spin_unlock(&con->writequeue_lock);

	if (!test_and_set_bit(CF_WRITE_PENDING, &con->flags)) {
		queue_work(send_workqueue, &con->swork);
	}
	return;

out:
	spin_unlock(&con->writequeue_lock);
	return;
}

/* Send a message */
static void send_to_sock(struct connection *con)
{
	int ret = 0;
	const int msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
	struct writequeue_entry *e;
	int len, offset;
	int count = 0;

	mutex_lock(&con->sock_mutex);
	if (con->sock == NULL)
		goto out_connect;

	spin_lock(&con->writequeue_lock);
	for (;;) {
		e = list_entry(con->writequeue.next, struct writequeue_entry,
			       list);
		if ((struct list_head *) e == &con->writequeue)
			break;

		len = e->len;
		offset = e->offset;
		BUG_ON(len == 0 && e->users == 0);
		spin_unlock(&con->writequeue_lock);

		ret = 0;
		if (len) {
			ret = kernel_sendpage(con->sock, e->page, offset, len,
					      msg_flags);
			if (ret == -EAGAIN || ret == 0) {
				if (ret == -EAGAIN &&
				    test_bit(SOCKWQ_ASYNC_NOSPACE, &con->sock->flags) &&
				    !test_and_set_bit(CF_APP_LIMITED, &con->flags)) {
					/* Notify TCP that we're limited by the
					 * application window size.
					 */
					set_bit(SOCK_NOSPACE, &con->sock->flags);
					con->sock->sk->sk_write_pending++;
				}
				cond_resched();
				goto out;
			} else if (ret < 0)
				goto send_error;
		}

		/* Don't starve people filling buffers */
		if (++count >= MAX_SEND_MSG_COUNT) {
			cond_resched();
			count = 0;
		}

		spin_lock(&con->writequeue_lock);
		writequeue_entry_complete(e, ret);
	}
	spin_unlock(&con->writequeue_lock);
out:
	mutex_unlock(&con->sock_mutex);
	return;

send_error:
	mutex_unlock(&con->sock_mutex);
	close_connection(con, false, false, true);
	lowcomms_connect_sock(con);
	return;

out_connect:
	mutex_unlock(&con->sock_mutex);
	lowcomms_connect_sock(con);
}

static void clean_one_writequeue(struct connection *con)
{
	struct writequeue_entry *e, *safe;

	spin_lock(&con->writequeue_lock);
	list_for_each_entry_safe(e, safe, &con->writequeue, list) {
		list_del(&e->list);
		free_entry(e);
	}
	spin_unlock(&con->writequeue_lock);
}

/* Called from recovery when it knows that a node has
   left the cluster */
int dlm_lowcomms_close(int nodeid)
{
	struct connection *con;
	struct dlm_node_addr *na;

	log_print("closing connection to node %d", nodeid);
	con = nodeid2con(nodeid, 0);
	if (con) {
		set_bit(CF_CLOSE, &con->flags);
		close_connection(con, true, true, true);
		clean_one_writequeue(con);
	}

	spin_lock(&dlm_node_addrs_spin);
	na = find_node_addr(nodeid);
	if (na) {
		list_del(&na->list);
		while (na->addr_count--)
			kfree(na->addr[na->addr_count]);
		kfree(na);
	}
	spin_unlock(&dlm_node_addrs_spin);

	return 0;
}

/* Receive workqueue function */
static void process_recv_sockets(struct work_struct *work)
{
	struct connection *con = container_of(work, struct connection, rwork);
	int err;

	clear_bit(CF_READ_PENDING, &con->flags);
	do {
		err = con->rx_action(con);
	} while (!err);
}

/* Send workqueue function */
static void process_send_sockets(struct work_struct *work)
{
	struct connection *con = container_of(work, struct connection, swork);

	if (test_and_clear_bit(CF_CONNECT_PENDING, &con->flags))
		con->connect_action(con);
	if (test_and_clear_bit(CF_WRITE_PENDING, &con->flags))
		send_to_sock(con);
}


/* Discard all entries on the write queues */
static void clean_writequeues(void)
{
	foreach_conn(clean_one_writequeue);
}

static void work_stop(void)
{
	destroy_workqueue(recv_workqueue);
	destroy_workqueue(send_workqueue);
}

static int work_start(void)
{
	recv_workqueue = alloc_workqueue("dlm_recv",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!recv_workqueue) {
		log_print("can't start dlm_recv");
		return -ENOMEM;
	}

	send_workqueue = alloc_workqueue("dlm_send",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!send_workqueue) {
		log_print("can't start dlm_send");
		destroy_workqueue(recv_workqueue);
		return -ENOMEM;
	}

	return 0;
}

static void stop_conn(struct connection *con)
{
	con->flags |= 0x0F;
	if (con->sock && con->sock->sk)
		con->sock->sk->sk_user_data = NULL;
}

static void free_conn(struct connection *con)
{
	close_connection(con, true, true, true);
	if (con->othercon)
		kmem_cache_free(con_cache, con->othercon);
	hlist_del(&con->list);
	kmem_cache_free(con_cache, con);
}

void dlm_lowcomms_stop(void)
{
	/* Set all the flags to prevent any
	   socket activity.
	*/
	mutex_lock(&connections_lock);
	dlm_allow_conn = 0;
	foreach_conn(stop_conn);
	clean_writequeues();
	foreach_conn(free_conn);
	mutex_unlock(&connections_lock);

	work_stop();

	kmem_cache_destroy(con_cache);
}

int dlm_lowcomms_start(void)
{
	int error = -EINVAL;
	struct connection *con;
	int i;

	for (i = 0; i < CONN_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&connection_hash[i]);

	init_local();
	if (!dlm_local_count) {
		error = -ENOTCONN;
		log_print("no local IP address has been set");
		goto fail;
	}

	error = -ENOMEM;
	con_cache = kmem_cache_create("dlm_conn", sizeof(struct connection),
				      __alignof__(struct connection), 0,
				      NULL);
	if (!con_cache)
		goto fail;

	error = work_start();
	if (error)
		goto fail_destroy;

	dlm_allow_conn = 1;

	/* Start listening */
	if (dlm_config.ci_protocol == 0)
		error = tcp_listen_for_all();
	else
		error = sctp_listen_for_all();
	if (error)
		goto fail_unlisten;

	return 0;

fail_unlisten:
	dlm_allow_conn = 0;
	con = nodeid2con(0,0);
	if (con) {
		close_connection(con, false, true, true);
		kmem_cache_free(con_cache, con);
	}
fail_destroy:
	kmem_cache_destroy(con_cache);
fail:
	return error;
}

void dlm_lowcomms_exit(void)
{
	struct dlm_node_addr *na, *safe;

	spin_lock(&dlm_node_addrs_spin);
	list_for_each_entry_safe(na, safe, &dlm_node_addrs, list) {
		list_del(&na->list);
		while (na->addr_count--)
			kfree(na->addr[na->addr_count]);
		kfree(na);
	}
	spin_unlock(&dlm_node_addrs_spin);
}
