/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	$Id: tcp_ipv4.c,v 1.240 2002/02/01 22:01:04 davem Exp $
 *
 *		IPv4 specific functions
 *
 *
 *		code split from:
 *		linux/ipv4/tcp.c
 *		linux/ipv4/tcp_input.c
 *		linux/ipv4/tcp_output.c
 *
 *		See tcp.c for author information
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

/*
 * Changes:
 *		David S. Miller	:	New socket lookup architecture.
 *					This code is dedicated to John Dyson.
 *		David S. Miller :	Change semantics of established hash,
 *					half is devoted to TIME_WAIT sockets
 *					and the rest go in the other half.
 *		Andi Kleen :		Add support for syncookies and fixed
 *					some bugs: ip options weren't passed to
 *					the TCP layer, missed a check for an
 *					ACK bit.
 *		Andi Kleen :		Implemented fast path mtu discovery.
 *	     				Fixed many serious bugs in the
 *					request_sock handling and moved
 *					most of it into the af independent code.
 *					Added tail drop and some other bugfixes.
 *					Added new listen semantics.
 *		Mike McLagan	:	Routing by source
 *	Juan Jose Ciarlante:		ip_dynaddr bits
 *		Andi Kleen:		various fixes.
 *	Vitaly E. Lavrov	:	Transparent proxy revived after year
 *					coma.
 *	Andi Kleen		:	Fix new listen.
 *	Andi Kleen		:	Fix accept error reporting.
 *	YOSHIFUJI Hideaki @USAGI and:	Support IPV6_V6ONLY socket option, which
 *	Alexey Kuznetsov		allow both IPv4 and IPv6 sockets to bind
 *					a single port at the same time.
 */

#include <linux/config.h>

#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/cache.h>
#include <linux/jhash.h>
#include <linux/init.h>
#include <linux/times.h>

#include <net/icmp.h>
#include <net/inet_hashtables.h>
#include <net/tcp.h>
#include <net/transp_v6.h>
#include <net/ipv6.h>
#include <net/inet_common.h>
#include <net/timewait_sock.h>
#include <net/xfrm.h>

#include <linux/inet.h>
#include <linux/ipv6.h>
#include <linux/stddef.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

int sysctl_tcp_tw_reuse;
int sysctl_tcp_low_latency;

/* Check TCP sequence numbers in ICMP packets. */
#define ICMP_MIN_LENGTH 8

/* Socket used for sending RSTs */
static struct socket *tcp_socket;

void tcp_v4_send_check(struct sock *sk, int len, struct sk_buff *skb);

struct inet_hashinfo __cacheline_aligned tcp_hashinfo = {
	.lhash_lock	= RW_LOCK_UNLOCKED,
	.lhash_users	= ATOMIC_INIT(0),
	.lhash_wait	= __WAIT_QUEUE_HEAD_INITIALIZER(tcp_hashinfo.lhash_wait),
};

static int tcp_v4_get_port(struct sock *sk, unsigned short snum)
{
	return inet_csk_get_port(&tcp_hashinfo, sk, snum,
				 inet_csk_bind_conflict);
}

static void tcp_v4_hash(struct sock *sk)
{
	inet_hash(&tcp_hashinfo, sk);
}

void tcp_unhash(struct sock *sk)
{
	inet_unhash(&tcp_hashinfo, sk);
}

static inline __u32 tcp_v4_init_sequence(struct sock *sk, struct sk_buff *skb)
{
	return secure_tcp_sequence_number(skb->nh.iph->daddr,
					  skb->nh.iph->saddr,
					  skb->h.th->dest,
					  skb->h.th->source);
}

int tcp_twsk_unique(struct sock *sk, struct sock *sktw, void *twp)
{
	const struct tcp_timewait_sock *tcptw = tcp_twsk(sktw);
	struct tcp_sock *tp = tcp_sk(sk);

	/* With PAWS, it is safe from the viewpoint
	   of data integrity. Even without PAWS it is safe provided sequence
	   spaces do not overlap i.e. at data rates <= 80Mbit/sec.

	   Actually, the idea is close to VJ's one, only timestamp cache is
	   held not per host, but per port pair and TW bucket is used as state
	   holder.

	   If TW bucket has been already destroyed we fall back to VJ's scheme
	   and use initial timestamp retrieved from peer table.
	 */
	if (tcptw->tw_ts_recent_stamp &&
	    (twp == NULL || (sysctl_tcp_tw_reuse &&
			     xtime.tv_sec - tcptw->tw_ts_recent_stamp > 1))) {
		tp->write_seq = tcptw->tw_snd_nxt + 65535 + 2;
		if (tp->write_seq == 0)
			tp->write_seq = 1;
		tp->rx_opt.ts_recent	   = tcptw->tw_ts_recent;
		tp->rx_opt.ts_recent_stamp = tcptw->tw_ts_recent_stamp;
		sock_hold(sktw);
		return 1;
	}

	return 0;
}

EXPORT_SYMBOL_GPL(tcp_twsk_unique);

/* This will initiate an outgoing connection. */
int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
	struct inet_sock *inet = inet_sk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct sockaddr_in *usin = (struct sockaddr_in *)uaddr;
	struct rtable *rt;
	u32 daddr, nexthop;
	int tmp;
	int err;

	if (addr_len < sizeof(struct sockaddr_in))
		return -EINVAL;

	if (usin->sin_family != AF_INET)
		return -EAFNOSUPPORT;

	nexthop = daddr = usin->sin_addr.s_addr;
	if (inet->opt && inet->opt->srr) {
		if (!daddr)
			return -EINVAL;
		nexthop = inet->opt->faddr;
	}

	tmp = ip_route_connect(&rt, nexthop, inet->saddr,
			       RT_CONN_FLAGS(sk), sk->sk_bound_dev_if,
			       IPPROTO_TCP,
			       inet->sport, usin->sin_port, sk);
	if (tmp < 0)
		return tmp;

	if (rt->rt_flags & (RTCF_MULTICAST | RTCF_BROADCAST)) {
		ip_rt_put(rt);
		return -ENETUNREACH;
	}

	if (!inet->opt || !inet->opt->srr)
		daddr = rt->rt_dst;

	if (!inet->saddr)
		inet->saddr = rt->rt_src;
	inet->rcv_saddr = inet->saddr;

	if (tp->rx_opt.ts_recent_stamp && inet->daddr != daddr) {
		/* Reset inherited state */
		tp->rx_opt.ts_recent	   = 0;
		tp->rx_opt.ts_recent_stamp = 0;
		tp->write_seq		   = 0;
	}

	if (tcp_death_row.sysctl_tw_recycle &&
	    !tp->rx_opt.ts_recent_stamp && rt->rt_dst == daddr) {
		struct inet_peer *peer = rt_get_peer(rt);

		/* VJ's idea. We save last timestamp seen from
		 * the destination in peer table, when entering state TIME-WAIT
		 * and initialize rx_opt.ts_recent from it, when trying new connection.
		 */

		if (peer && peer->tcp_ts_stamp + TCP_PAWS_MSL >= xtime.tv_sec) {
			tp->rx_opt.ts_recent_stamp = peer->tcp_ts_stamp;
			tp->rx_opt.ts_recent = peer->tcp_ts;
		}
	}

	inet->dport = usin->sin_port;
	inet->daddr = daddr;

	inet_csk(sk)->icsk_ext_hdr_len = 0;
	if (inet->opt)
		inet_csk(sk)->icsk_ext_hdr_len = inet->opt->optlen;

	tp->rx_opt.mss_clamp = 536;

	/* Socket identity is still unknown (sport may be zero).
	 * However we set state to SYN-SENT and not releasing socket
	 * lock select source port, enter ourselves into the hash tables and
	 * complete initialization after this.
	 */
	tcp_set_state(sk, TCP_SYN_SENT);
	err = inet_hash_connect(&tcp_death_row, sk);
	if (err)
		goto failure;

	err = ip_route_newports(&rt, inet->sport, inet->dport, sk);
	if (err)
		goto failure;

	/* OK, now commit destination to socket.  */
	sk_setup_caps(sk, &rt->u.dst);

	if (!tp->write_seq)
		tp->write_seq = secure_tcp_sequence_number(inet->saddr,
							   inet->daddr,
							   inet->sport,
							   usin->sin_port);

	inet->id = tp->write_seq ^ jiffies;

	err = tcp_connect(sk);
	rt = NULL;
	if (err)
		goto failure;

	return 0;

failure:
	/* This unhashes the socket and releases the local port, if necessary. */
	tcp_set_state(sk, TCP_CLOSE);
	ip_rt_put(rt);
	sk->sk_route_caps = 0;
	inet->dport = 0;
	return err;
}

/*
 * This routine does path mtu discovery as defined in RFC1191.
 */
static void do_pmtu_discovery(struct sock *sk, struct iphdr *iph, u32 mtu)
{
	struct dst_entry *dst;
	struct inet_sock *inet = inet_sk(sk);

	/* We are not interested in TCP_LISTEN and open_requests (SYN-ACKs
	 * send out by Linux are always <576bytes so they should go through
	 * unfragmented).
	 */
	if (sk->sk_state == TCP_LISTEN)
		return;

	/* We don't check in the destentry if pmtu discovery is forbidden
	 * on this route. We just assume that no packet_to_big packets
	 * are send back when pmtu discovery is not active.
     	 * There is a small race when the user changes this flag in the
	 * route, but I think that's acceptable.
	 */
	if ((dst = __sk_dst_check(sk, 0)) == NULL)
		return;

	dst->ops->update_pmtu(dst, mtu);

	/* Something is about to be wrong... Remember soft error
	 * for the case, if this connection will not able to recover.
	 */
	if (mtu < dst_mtu(dst) && ip_dont_fragment(sk, dst))
		sk->sk_err_soft = EMSGSIZE;

	mtu = dst_mtu(dst);

	if (inet->pmtudisc != IP_PMTUDISC_DONT &&
	    inet_csk(sk)->icsk_pmtu_cookie > mtu) {
		tcp_sync_mss(sk, mtu);

		/* Resend the TCP packet because it's
		 * clear that the old packet has been
		 * dropped. This is the new "fast" path mtu
		 * discovery.
		 */
		tcp_simple_retransmit(sk);
	} /* else let the usual retransmit timer handle it */
}

/*
 * This routine is called by the ICMP module when it gets some
 * sort of error condition.  If err < 0 then the socket should
 * be closed and the error returned to the user.  If err > 0
 * it's just the icmp type << 8 | icmp code.  After adjustment
 * header points to the first 8 bytes of the tcp header.  We need
 * to find the appropriate port.
 *
 * The locking strategy used here is very "optimistic". When
 * someone else accesses the socket the ICMP is just dropped
 * and for some paths there is no check at all.
 * A more general error queue to queue errors for later handling
 * is probably better.
 *
 */

void tcp_v4_err(struct sk_buff *skb, u32 info)
{
	struct iphdr *iph = (struct iphdr *)skb->data;
	struct tcphdr *th = (struct tcphdr *)(skb->data + (iph->ihl << 2));
	struct tcp_sock *tp;
	struct inet_sock *inet;
	int type = skb->h.icmph->type;
	int code = skb->h.icmph->code;
	struct sock *sk;
	__u32 seq;
	int err;

	if (skb->len < (iph->ihl << 2) + 8) {
		ICMP_INC_STATS_BH(ICMP_MIB_INERRORS);
		return;
	}

	sk = inet_lookup(&tcp_hashinfo, iph->daddr, th->dest, iph->saddr,
			 th->source, inet_iif(skb));
	if (!sk) {
		ICMP_INC_STATS_BH(ICMP_MIB_INERRORS);
		return;
	}
	if (sk->sk_state == TCP_TIME_WAIT) {
		inet_twsk_put((struct inet_timewait_sock *)sk);
		return;
	}

	bh_lock_sock(sk);
	/* If too many ICMPs get dropped on busy
	 * servers this needs to be solved differently.
	 */
	if (sock_owned_by_user(sk))
		NET_INC_STATS_BH(LINUX_MIB_LOCKDROPPEDICMPS);

	if (sk->sk_state == TCP_CLOSE)
		goto out;

	tp = tcp_sk(sk);
	seq = ntohl(th->seq);
	if (sk->sk_state != TCP_LISTEN &&
	    !between(seq, tp->snd_una, tp->snd_nxt)) {
		NET_INC_STATS(LINUX_MIB_OUTOFWINDOWICMPS);
		goto out;
	}

	switch (type) {
	case ICMP_SOURCE_QUENCH:
		/* Just silently ignore these. */
		goto out;
	case ICMP_PARAMETERPROB:
		err = EPROTO;
		break;
	case ICMP_DEST_UNREACH:
		if (code > NR_ICMP_UNREACH)
			goto out;

		if (code == ICMP_FRAG_NEEDED) { /* PMTU discovery (RFC1191) */
			if (!sock_owned_by_user(sk))
				do_pmtu_discovery(sk, iph, info);
			goto out;
		}

		err = icmp_err_convert[code].errno;
		break;
	case ICMP_TIME_EXCEEDED:
		err = EHOSTUNREACH;
		break;
	default:
		goto out;
	}

	switch (sk->sk_state) {
		struct request_sock *req, **prev;
	case TCP_LISTEN:
		if (sock_owned_by_user(sk))
			goto out;

		req = inet_csk_search_req(sk, &prev, th->dest,
					  iph->daddr, iph->saddr);
		if (!req)
			goto out;

		/* ICMPs are not backlogged, hence we cannot get
		   an established socket here.
		 */
		BUG_TRAP(!req->sk);

		if (seq != tcp_rsk(req)->snt_isn) {
			NET_INC_STATS_BH(LINUX_MIB_OUTOFWINDOWICMPS);
			goto out;
		}

		/*
		 * Still in SYN_RECV, just remove it silently.
		 * There is no good way to pass the error to the newly
		 * created socket, and POSIX does not want network
		 * errors returned from accept().
		 */
		inet_csk_reqsk_queue_drop(sk, req, prev);
		goto out;

	case TCP_SYN_SENT:
	case TCP_SYN_RECV:  /* Cannot happen.
			       It can f.e. if SYNs crossed.
			     */
		if (!sock_owned_by_user(sk)) {
			TCP_INC_STATS_BH(TCP_MIB_ATTEMPTFAILS);
			sk->sk_err = err;

			sk->sk_error_report(sk);

			tcp_done(sk);
		} else {
			sk->sk_err_soft = err;
		}
		goto out;
	}

	/* If we've already connected we will keep trying
	 * until we time out, or the user gives up.
	 *
	 * rfc1122 4.2.3.9 allows to consider as hard errors
	 * only PROTO_UNREACH and PORT_UNREACH (well, FRAG_FAILED too,
	 * but it is obsoleted by pmtu discovery).
	 *
	 * Note, that in modern internet, where routing is unreliable
	 * and in each dark corner broken firewalls sit, sending random
	 * errors ordered by their masters even this two messages finally lose
	 * their original sense (even Linux sends invalid PORT_UNREACHs)
	 *
	 * Now we are in compliance with RFCs.
	 *							--ANK (980905)
	 */

	inet = inet_sk(sk);
	if (!sock_owned_by_user(sk) && inet->recverr) {
		sk->sk_err = err;
		sk->sk_error_report(sk);
	} else	{ /* Only an error on timeout */
		sk->sk_err_soft = err;
	}

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}

/* This routine computes an IPv4 TCP checksum. */
void tcp_v4_send_check(struct sock *sk, int len, struct sk_buff *skb)
{
	struct inet_sock *inet = inet_sk(sk);
	struct tcphdr *th = skb->h.th;

	if (skb->ip_summed == CHECKSUM_HW) {
		th->check = ~tcp_v4_check(th, len, inet->saddr, inet->daddr, 0);
		skb->csum = offsetof(struct tcphdr, check);
	} else {
		th->check = tcp_v4_check(th, len, inet->saddr, inet->daddr,
					 csum_partial((char *)th,
						      th->doff << 2,
						      skb->csum));
	}
}

/*
 *	This routine will send an RST to the other tcp.
 *
 *	Someone asks: why I NEVER use socket parameters (TOS, TTL etc.)
 *		      for reset.
 *	Answer: if a packet caused RST, it is not for a socket
 *		existing in our system, if it is matched to a socket,
 *		it is just duplicate segment or bug in other side's TCP.
 *		So that we build reply only basing on parameters
 *		arrived with segment.
 *	Exception: precedence violation. We do not implement it in any case.
 */

static void tcp_v4_send_reset(struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th;
	struct tcphdr rth;
	struct ip_reply_arg arg;

	/* Never send a reset in response to a reset. */
	if (th->rst)
		return;

	if (((struct rtable *)skb->dst)->rt_type != RTN_LOCAL)
		return;

	/* Swap the send and the receive. */
	memset(&rth, 0, sizeof(struct tcphdr));
	rth.dest   = th->source;
	rth.source = th->dest;
	rth.doff   = sizeof(struct tcphdr) / 4;
	rth.rst    = 1;

	if (th->ack) {
		rth.seq = th->ack_seq;
	} else {
		rth.ack = 1;
		rth.ack_seq = htonl(ntohl(th->seq) + th->syn + th->fin +
				    skb->len - (th->doff << 2));
	}

	memset(&arg, 0, sizeof arg);
	arg.iov[0].iov_base = (unsigned char *)&rth;
	arg.iov[0].iov_len  = sizeof rth;
	arg.csum = csum_tcpudp_nofold(skb->nh.iph->daddr,
				      skb->nh.iph->saddr, /*XXX*/
				      sizeof(struct tcphdr), IPPROTO_TCP, 0);
	arg.csumoffset = offsetof(struct tcphdr, check) / 2;

	ip_send_reply(tcp_socket->sk, skb, &arg, sizeof rth);

	TCP_INC_STATS_BH(TCP_MIB_OUTSEGS);
	TCP_INC_STATS_BH(TCP_MIB_OUTRSTS);
}

/* The code following below sending ACKs in SYN-RECV and TIME-WAIT states
   outside socket context is ugly, certainly. What can I do?
 */

static void tcp_v4_send_ack(struct sk_buff *skb, u32 seq, u32 ack,
			    u32 win, u32 ts)
{
	struct tcphdr *th = skb->h.th;
	struct {
		struct tcphdr th;
		u32 tsopt[3];
	} rep;
	struct ip_reply_arg arg;

	memset(&rep.th, 0, sizeof(struct tcphdr));
	memset(&arg, 0, sizeof arg);

	arg.iov[0].iov_base = (unsigned char *)&rep;
	arg.iov[0].iov_len  = sizeof(rep.th);
	if (ts) {
		rep.tsopt[0] = htonl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16) |
				     (TCPOPT_TIMESTAMP << 8) |
				     TCPOLEN_TIMESTAMP);
		rep.tsopt[1] = htonl(tcp_time_stamp);
		rep.tsopt[2] = htonl(ts);
		arg.iov[0].iov_len = sizeof(rep);
	}

	/* Swap the send and the receive. */
	rep.th.dest    = th->source;
	rep.th.source  = th->dest;
	rep.th.doff    = arg.iov[0].iov_len / 4;
	rep.th.seq     = htonl(seq);
	rep.th.ack_seq = htonl(ack);
	rep.th.ack     = 1;
	rep.th.window  = htons(win);

	arg.csum = csum_tcpudp_nofold(skb->nh.iph->daddr,
				      skb->nh.iph->saddr, /*XXX*/
				      arg.iov[0].iov_len, IPPROTO_TCP, 0);
	arg.csumoffset = offsetof(struct tcphdr, check) / 2;

	ip_send_reply(tcp_socket->sk, skb, &arg, arg.iov[0].iov_len);

	TCP_INC_STATS_BH(TCP_MIB_OUTSEGS);
}

static void tcp_v4_timewait_ack(struct sock *sk, struct sk_buff *skb)
{
	struct inet_timewait_sock *tw = inet_twsk(sk);
	const struct tcp_timewait_sock *tcptw = tcp_twsk(sk);

	tcp_v4_send_ack(skb, tcptw->tw_snd_nxt, tcptw->tw_rcv_nxt,
			tcptw->tw_rcv_wnd >> tw->tw_rcv_wscale, tcptw->tw_ts_recent);

	inet_twsk_put(tw);
}

static void tcp_v4_reqsk_send_ack(struct sk_buff *skb, struct request_sock *req)
{
	tcp_v4_send_ack(skb, tcp_rsk(req)->snt_isn + 1, tcp_rsk(req)->rcv_isn + 1, req->rcv_wnd,
			req->ts_recent);
}

/*
 *	Send a SYN-ACK after having received an ACK.
 *	This still operates on a request_sock only, not on a big
 *	socket.
 */
static int tcp_v4_send_synack(struct sock *sk, struct request_sock *req,
			      struct dst_entry *dst)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	int err = -1;
	struct sk_buff * skb;

	/* First, grab a route. */
	if (!dst && (dst = inet_csk_route_req(sk, req)) == NULL)
		goto out;

	skb = tcp_make_synack(sk, dst, req);

	if (skb) {
		struct tcphdr *th = skb->h.th;

		th->check = tcp_v4_check(th, skb->len,
					 ireq->loc_addr,
					 ireq->rmt_addr,
					 csum_partial((char *)th, skb->len,
						      skb->csum));

		err = ip_build_and_send_pkt(skb, sk, ireq->loc_addr,
					    ireq->rmt_addr,
					    ireq->opt);
		if (err == NET_XMIT_CN)
			err = 0;
	}

out:
	dst_release(dst);
	return err;
}

/*
 *	IPv4 request_sock destructor.
 */
static void tcp_v4_reqsk_destructor(struct request_sock *req)
{
	kfree(inet_rsk(req)->opt);
}

#ifdef CONFIG_SYN_COOKIES
static void syn_flood_warning(struct sk_buff *skb)
{
	static unsigned long warntime;

	if (time_after(jiffies, (warntime + HZ * 60))) {
		warntime = jiffies;
		printk(KERN_INFO
		       "possible SYN flooding on port %d. Sending cookies.\n",
		       ntohs(skb->h.th->dest));
	}
}
#endif

/*
 * Save and compile IPv4 options into the request_sock if needed.
 */
static struct ip_options *tcp_v4_save_options(struct sock *sk,
					      struct sk_buff *skb)
{
	struct ip_options *opt = &(IPCB(skb)->opt);
	struct ip_options *dopt = NULL;

	if (opt && opt->optlen) {
		int opt_size = optlength(opt);
		dopt = kmalloc(opt_size, GFP_ATOMIC);
		if (dopt) {
			if (ip_options_echo(dopt, skb)) {
				kfree(dopt);
				dopt = NULL;
			}
		}
	}
	return dopt;
}

struct request_sock_ops tcp_request_sock_ops = {
	.family		=	PF_INET,
	.obj_size	=	sizeof(struct tcp_request_sock),
	.rtx_syn_ack	=	tcp_v4_send_synack,
	.send_ack	=	tcp_v4_reqsk_send_ack,
	.destructor	=	tcp_v4_reqsk_destructor,
	.send_reset	=	tcp_v4_send_reset,
};

static struct timewait_sock_ops tcp_timewait_sock_ops = {
	.twsk_obj_size	= sizeof(struct tcp_timewait_sock),
	.twsk_unique	= tcp_twsk_unique,
};

int tcp_v4_conn_request(struct sock *sk, struct sk_buff *skb)
{
	struct inet_request_sock *ireq;
	struct tcp_options_received tmp_opt;
	struct request_sock *req;
	__u32 saddr = skb->nh.iph->saddr;
	__u32 daddr = skb->nh.iph->daddr;
	__u32 isn = TCP_SKB_CB(skb)->when;
	struct dst_entry *dst = NULL;
#ifdef CONFIG_SYN_COOKIES
	int want_cookie = 0;
#else
#define want_cookie 0 /* Argh, why doesn't gcc optimize this :( */
#endif

	/* Never answer to SYNs send to broadcast or multicast */
	if (((struct rtable *)skb->dst)->rt_flags &
	    (RTCF_BROADCAST | RTCF_MULTICAST))
		goto drop;

	/* TW buckets are converted to open requests without
	 * limitations, they conserve resources and peer is
	 * evidently real one.
	 */
	if (inet_csk_reqsk_queue_is_full(sk) && !isn) {
#ifdef CONFIG_SYN_COOKIES
		if (sysctl_tcp_syncookies) {
			want_cookie = 1;
		} else
#endif
		goto drop;
	}

	/* Accept backlog is full. If we have already queued enough
	 * of warm entries in syn queue, drop request. It is better than
	 * clogging syn queue with openreqs with exponentially increasing
	 * timeout.
	 */
	if (sk_acceptq_is_full(sk) && inet_csk_reqsk_queue_young(sk) > 1)
		goto drop;

	req = reqsk_alloc(&tcp_request_sock_ops);
	if (!req)
		goto drop;

	tcp_clear_options(&tmp_opt);
	tmp_opt.mss_clamp = 536;
	tmp_opt.user_mss  = tcp_sk(sk)->rx_opt.user_mss;

	tcp_parse_options(skb, &tmp_opt, 0);

	if (want_cookie) {
		tcp_clear_options(&tmp_opt);
		tmp_opt.saw_tstamp = 0;
	}

	if (tmp_opt.saw_tstamp && !tmp_opt.rcv_tsval) {
		/* Some OSes (unknown ones, but I see them on web server, which
		 * contains information interesting only for windows'
		 * users) do not send their stamp in SYN. It is easy case.
		 * We simply do not advertise TS support.
		 */
		tmp_opt.saw_tstamp = 0;
		tmp_opt.tstamp_ok  = 0;
	}
	tmp_opt.tstamp_ok = tmp_opt.saw_tstamp;

	tcp_openreq_init(req, &tmp_opt, skb);

	ireq = inet_rsk(req);
	ireq->loc_addr = daddr;
	ireq->rmt_addr = saddr;
	ireq->opt = tcp_v4_save_options(sk, skb);
	if (!want_cookie)
		TCP_ECN_create_request(req, skb->h.th);

	if (want_cookie) {
#ifdef CONFIG_SYN_COOKIES
		syn_flood_warning(skb);
#endif
		isn = cookie_v4_init_sequence(sk, skb, &req->mss);
	} else if (!isn) {
		struct inet_peer *peer = NULL;

		/* VJ's idea. We save last timestamp seen
		 * from the destination in peer table, when entering
		 * state TIME-WAIT, and check against it before
		 * accepting new connection request.
		 *
		 * If "isn" is not zero, this request hit alive
		 * timewait bucket, so that all the necessary checks
		 * are made in the function processing timewait state.
		 */
		if (tmp_opt.saw_tstamp &&
		    tcp_death_row.sysctl_tw_recycle &&
		    (dst = inet_csk_route_req(sk, req)) != NULL &&
		    (peer = rt_get_peer((struct rtable *)dst)) != NULL &&
		    peer->v4daddr == saddr) {
			if (xtime.tv_sec < peer->tcp_ts_stamp + TCP_PAWS_MSL &&
			    (s32)(peer->tcp_ts - req->ts_recent) >
							TCP_PAWS_WINDOW) {
				NET_INC_STATS_BH(LINUX_MIB_PAWSPASSIVEREJECTED);
				dst_release(dst);
				goto drop_and_free;
			}
		}
		/* Kill the following clause, if you dislike this way. */
		else if (!sysctl_tcp_syncookies &&
			 (sysctl_max_syn_backlog - inet_csk_reqsk_queue_len(sk) <
			  (sysctl_max_syn_backlog >> 2)) &&
			 (!peer || !peer->tcp_ts_stamp) &&
			 (!dst || !dst_metric(dst, RTAX_RTT))) {
			/* Without syncookies last quarter of
			 * backlog is filled with destinations,
			 * proven to be alive.
			 * It means that we continue to communicate
			 * to destinations, already remembered
			 * to the moment of synflood.
			 */
			LIMIT_NETDEBUG(KERN_DEBUG "TCP: drop open "
				       "request from %u.%u.%u.%u/%u\n",
				       NIPQUAD(saddr),
				       ntohs(skb->h.th->source));
			dst_release(dst);
			goto drop_and_free;
		}

		isn = tcp_v4_init_sequence(sk, skb);
	}
	tcp_rsk(req)->snt_isn = isn;

	if (tcp_v4_send_synack(sk, req, dst))
		goto drop_and_free;

	if (want_cookie) {
	   	reqsk_free(req);
	} else {
		inet_csk_reqsk_queue_hash_add(sk, req, TCP_TIMEOUT_INIT);
	}
	return 0;

drop_and_free:
	reqsk_free(req);
drop:
	TCP_INC_STATS_BH(TCP_MIB_ATTEMPTFAILS);
	return 0;
}


/*
 * The three way handshake has completed - we got a valid synack -
 * now create the new socket.
 */
struct sock *tcp_v4_syn_recv_sock(struct sock *sk, struct sk_buff *skb,
				  struct request_sock *req,
				  struct dst_entry *dst)
{
	struct inet_request_sock *ireq;
	struct inet_sock *newinet;
	struct tcp_sock *newtp;
	struct sock *newsk;

	if (sk_acceptq_is_full(sk))
		goto exit_overflow;

	if (!dst && (dst = inet_csk_route_req(sk, req)) == NULL)
		goto exit;

	newsk = tcp_create_openreq_child(sk, req, skb);
	if (!newsk)
		goto exit;

	sk_setup_caps(newsk, dst);

	newtp		      = tcp_sk(newsk);
	newinet		      = inet_sk(newsk);
	ireq		      = inet_rsk(req);
	newinet->daddr	      = ireq->rmt_addr;
	newinet->rcv_saddr    = ireq->loc_addr;
	newinet->saddr	      = ireq->loc_addr;
	newinet->opt	      = ireq->opt;
	ireq->opt	      = NULL;
	newinet->mc_index     = inet_iif(skb);
	newinet->mc_ttl	      = skb->nh.iph->ttl;
	inet_csk(newsk)->icsk_ext_hdr_len = 0;
	if (newinet->opt)
		inet_csk(newsk)->icsk_ext_hdr_len = newinet->opt->optlen;
	newinet->id = newtp->write_seq ^ jiffies;

	tcp_sync_mss(newsk, dst_mtu(dst));
	newtp->advmss = dst_metric(dst, RTAX_ADVMSS);
	tcp_initialize_rcv_mss(newsk);

	__inet_hash(&tcp_hashinfo, newsk, 0);
	__inet_inherit_port(&tcp_hashinfo, sk, newsk);

	return newsk;

exit_overflow:
	NET_INC_STATS_BH(LINUX_MIB_LISTENOVERFLOWS);
exit:
	NET_INC_STATS_BH(LINUX_MIB_LISTENDROPS);
	dst_release(dst);
	return NULL;
}

static struct sock *tcp_v4_hnd_req(struct sock *sk, struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th;
	struct iphdr *iph = skb->nh.iph;
	struct sock *nsk;
	struct request_sock **prev;
	/* Find possible connection requests. */
	struct request_sock *req = inet_csk_search_req(sk, &prev, th->source,
						       iph->saddr, iph->daddr);
	if (req)
		return tcp_check_req(sk, skb, req, prev);

	nsk = __inet_lookup_established(&tcp_hashinfo, skb->nh.iph->saddr,
					th->source, skb->nh.iph->daddr,
					ntohs(th->dest), inet_iif(skb));

	if (nsk) {
		if (nsk->sk_state != TCP_TIME_WAIT) {
			bh_lock_sock(nsk);
			return nsk;
		}
		inet_twsk_put((struct inet_timewait_sock *)nsk);
		return NULL;
	}

#ifdef CONFIG_SYN_COOKIES
	if (!th->rst && !th->syn && th->ack)
		sk = cookie_v4_check(sk, skb, &(IPCB(skb)->opt));
#endif
	return sk;
}

static int tcp_v4_checksum_init(struct sk_buff *skb)
{
	if (skb->ip_summed == CHECKSUM_HW) {
		if (!tcp_v4_check(skb->h.th, skb->len, skb->nh.iph->saddr,
				  skb->nh.iph->daddr, skb->csum)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			return 0;
		}
	}

	skb->csum = csum_tcpudp_nofold(skb->nh.iph->saddr, skb->nh.iph->daddr,
				       skb->len, IPPROTO_TCP, 0);

	if (skb->len <= 76) {
		return __skb_checksum_complete(skb);
	}
	return 0;
}


/* The socket must have it's spinlock held when we get
 * here.
 *
 * We have a potential double-lock case here, so even when
 * doing backlog processing we use the BH locking scheme.
 * This is because we cannot sleep with the original spinlock
 * held.
 */
int tcp_v4_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	if (sk->sk_state == TCP_ESTABLISHED) { /* Fast path */
		TCP_CHECK_TIMER(sk);
		if (tcp_rcv_established(sk, skb, skb->h.th, skb->len))
			goto reset;
		TCP_CHECK_TIMER(sk);
		return 0;
	}

	if (skb->len < (skb->h.th->doff << 2) || tcp_checksum_complete(skb))
		goto csum_err;

	if (sk->sk_state == TCP_LISTEN) {
		struct sock *nsk = tcp_v4_hnd_req(sk, skb);
		if (!nsk)
			goto discard;

		if (nsk != sk) {
			if (tcp_child_process(sk, nsk, skb))
				goto reset;
			return 0;
		}
	}

	TCP_CHECK_TIMER(sk);
	if (tcp_rcv_state_process(sk, skb, skb->h.th, skb->len))
		goto reset;
	TCP_CHECK_TIMER(sk);
	return 0;

reset:
	tcp_v4_send_reset(skb);
discard:
	kfree_skb(skb);
	/* Be careful here. If this function gets more complicated and
	 * gcc suffers from register pressure on the x86, sk (in %ebx)
	 * might be destroyed here. This current version compiles correctly,
	 * but you have been warned.
	 */
	return 0;

csum_err:
	TCP_INC_STATS_BH(TCP_MIB_INERRS);
	goto discard;
}

/*
 *	From tcp_input.c
 */

int tcp_v4_rcv(struct sk_buff *skb)
{
	struct tcphdr *th;
	struct sock *sk;
	int ret;

	if (skb->pkt_type != PACKET_HOST)
		goto discard_it;

	/* Count it even if it's bad */
	TCP_INC_STATS_BH(TCP_MIB_INSEGS);

	if (!pskb_may_pull(skb, sizeof(struct tcphdr)))
		goto discard_it;

	th = skb->h.th;

	if (th->doff < sizeof(struct tcphdr) / 4)
		goto bad_packet;
	if (!pskb_may_pull(skb, th->doff * 4))
		goto discard_it;

	/* An explanation is required here, I think.
	 * Packet length and doff are validated by header prediction,
	 * provided case of th->doff==0 is eliminated.
	 * So, we defer the checks. */
	if ((skb->ip_summed != CHECKSUM_UNNECESSARY &&
	     tcp_v4_checksum_init(skb)))
		goto bad_packet;

	th = skb->h.th;
	TCP_SKB_CB(skb)->seq = ntohl(th->seq);
	TCP_SKB_CB(skb)->end_seq = (TCP_SKB_CB(skb)->seq + th->syn + th->fin +
				    skb->len - th->doff * 4);
	TCP_SKB_CB(skb)->ack_seq = ntohl(th->ack_seq);
	TCP_SKB_CB(skb)->when	 = 0;
	TCP_SKB_CB(skb)->flags	 = skb->nh.iph->tos;
	TCP_SKB_CB(skb)->sacked	 = 0;

	sk = __inet_lookup(&tcp_hashinfo, skb->nh.iph->saddr, th->source,
			   skb->nh.iph->daddr, ntohs(th->dest),
			   inet_iif(skb));

	if (!sk)
		goto no_tcp_socket;

process:
	if (sk->sk_state == TCP_TIME_WAIT)
		goto do_time_wait;

	if (!xfrm4_policy_check(sk, XFRM_POLICY_IN, skb))
		goto discard_and_relse;
	nf_reset(skb);

	if (sk_filter(sk, skb, 0))
		goto discard_and_relse;

	skb->dev = NULL;

	bh_lock_sock(sk);
	ret = 0;
	if (!sock_owned_by_user(sk)) {
		if (!tcp_prequeue(sk, skb))
			ret = tcp_v4_do_rcv(sk, skb);
	} else
		sk_add_backlog(sk, skb);
	bh_unlock_sock(sk);

	sock_put(sk);

	return ret;

no_tcp_socket:
	if (!xfrm4_policy_check(NULL, XFRM_POLICY_IN, skb))
		goto discard_it;

	if (skb->len < (th->doff << 2) || tcp_checksum_complete(skb)) {
bad_packet:
		TCP_INC_STATS_BH(TCP_MIB_INERRS);
	} else {
		tcp_v4_send_reset(skb);
	}

discard_it:
	/* Discard frame. */
	kfree_skb(skb);
  	return 0;

discard_and_relse:
	sock_put(sk);
	goto discard_it;

do_time_wait:
	if (!xfrm4_policy_check(NULL, XFRM_POLICY_IN, skb)) {
		inet_twsk_put((struct inet_timewait_sock *) sk);
		goto discard_it;
	}

	if (skb->len < (th->doff << 2) || tcp_checksum_complete(skb)) {
		TCP_INC_STATS_BH(TCP_MIB_INERRS);
		inet_twsk_put((struct inet_timewait_sock *) sk);
		goto discard_it;
	}
	switch (tcp_timewait_state_process((struct inet_timewait_sock *)sk,
					   skb, th)) {
	case TCP_TW_SYN: {
		struct sock *sk2 = inet_lookup_listener(&tcp_hashinfo,
							skb->nh.iph->daddr,
							ntohs(th->dest),
							inet_iif(skb));
		if (sk2) {
			inet_twsk_deschedule((struct inet_timewait_sock *)sk,
					     &tcp_death_row);
			inet_twsk_put((struct inet_timewait_sock *)sk);
			sk = sk2;
			goto process;
		}
		/* Fall through to ACK */
	}
	case TCP_TW_ACK:
		tcp_v4_timewait_ack(sk, skb);
		break;
	case TCP_TW_RST:
		goto no_tcp_socket;
	case TCP_TW_SUCCESS:;
	}
	goto discard_it;
}

/* VJ's idea. Save last timestamp seen from this destination
 * and hold it at least for normal timewait interval to use for duplicate
 * segment detection in subsequent connections, before they enter synchronized
 * state.
 */

int tcp_v4_remember_stamp(struct sock *sk)
{
	struct inet_sock *inet = inet_sk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct rtable *rt = (struct rtable *)__sk_dst_get(sk);
	struct inet_peer *peer = NULL;
	int release_it = 0;

	if (!rt || rt->rt_dst != inet->daddr) {
		peer = inet_getpeer(inet->daddr, 1);
		release_it = 1;
	} else {
		if (!rt->peer)
			rt_bind_peer(rt, 1);
		peer = rt->peer;
	}

	if (peer) {
		if ((s32)(peer->tcp_ts - tp->rx_opt.ts_recent) <= 0 ||
		    (peer->tcp_ts_stamp + TCP_PAWS_MSL < xtime.tv_sec &&
		     peer->tcp_ts_stamp <= tp->rx_opt.ts_recent_stamp)) {
			peer->tcp_ts_stamp = tp->rx_opt.ts_recent_stamp;
			peer->tcp_ts = tp->rx_opt.ts_recent;
		}
		if (release_it)
			inet_putpeer(peer);
		return 1;
	}

	return 0;
}

int tcp_v4_tw_remember_stamp(struct inet_timewait_sock *tw)
{
	struct inet_peer *peer = inet_getpeer(tw->tw_daddr, 1);

	if (peer) {
		const struct tcp_timewait_sock *tcptw = tcp_twsk((struct sock *)tw);

		if ((s32)(peer->tcp_ts - tcptw->tw_ts_recent) <= 0 ||
		    (peer->tcp_ts_stamp + TCP_PAWS_MSL < xtime.tv_sec &&
		     peer->tcp_ts_stamp <= tcptw->tw_ts_recent_stamp)) {
			peer->tcp_ts_stamp = tcptw->tw_ts_recent_stamp;
			peer->tcp_ts	   = tcptw->tw_ts_recent;
		}
		inet_putpeer(peer);
		return 1;
	}

	return 0;
}

struct inet_connection_sock_af_ops ipv4_specific = {
	.queue_xmit	=	ip_queue_xmit,
	.send_check	=	tcp_v4_send_check,
	.rebuild_header	=	inet_sk_rebuild_header,
	.conn_request	=	tcp_v4_conn_request,
	.syn_recv_sock	=	tcp_v4_syn_recv_sock,
	.remember_stamp	=	tcp_v4_remember_stamp,
	.net_header_len	=	sizeof(struct iphdr),
	.setsockopt	=	ip_setsockopt,
	.getsockopt	=	ip_getsockopt,
	.addr2sockaddr	=	inet_csk_addr2sockaddr,
	.sockaddr_len	=	sizeof(struct sockaddr_in),
};

/* NOTE: A lot of things set to zero explicitly by call to
 *       sk_alloc() so need not be done here.
 */
static int tcp_v4_init_sock(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	skb_queue_head_init(&tp->out_of_order_queue);
	tcp_init_xmit_timers(sk);
	tcp_prequeue_init(tp);

	icsk->icsk_rto = TCP_TIMEOUT_INIT;
	tp->mdev = TCP_TIMEOUT_INIT;

	/* So many TCP implementations out there (incorrectly) count the
	 * initial SYN frame in their delayed-ACK and congestion control
	 * algorithms that we must have the following bandaid to talk
	 * efficiently to them.  -DaveM
	 */
	tp->snd_cwnd = 2;

	/* See draft-stevens-tcpca-spec-01 for discussion of the
	 * initialization of these values.
	 */
	tp->snd_ssthresh = 0x7fffffff;	/* Infinity */
	tp->snd_cwnd_clamp = ~0;
	tp->mss_cache = 536;

	tp->reordering = sysctl_tcp_reordering;
	icsk->icsk_ca_ops = &tcp_init_congestion_ops;

	sk->sk_state = TCP_CLOSE;

	sk->sk_write_space = sk_stream_write_space;
	sock_set_flag(sk, SOCK_USE_WRITE_QUEUE);

	icsk->icsk_af_ops = &ipv4_specific;
	icsk->icsk_sync_mss = tcp_sync_mss;

	sk->sk_sndbuf = sysctl_tcp_wmem[1];
	sk->sk_rcvbuf = sysctl_tcp_rmem[1];

	atomic_inc(&tcp_sockets_allocated);

	return 0;
}

int tcp_v4_destroy_sock(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tcp_clear_xmit_timers(sk);

	tcp_cleanup_congestion_control(sk);

	/* Cleanup up the write buffer. */
  	sk_stream_writequeue_purge(sk);

	/* Cleans up our, hopefully empty, out_of_order_queue. */
  	__skb_queue_purge(&tp->out_of_order_queue);

	/* Clean prequeue, it must be empty really */
	__skb_queue_purge(&tp->ucopy.prequeue);

	/* Clean up a referenced TCP bind bucket. */
	if (inet_csk(sk)->icsk_bind_hash)
		inet_put_port(&tcp_hashinfo, sk);

	/*
	 * If sendmsg cached page exists, toss it.
	 */
	if (sk->sk_sndmsg_page) {
		__free_page(sk->sk_sndmsg_page);
		sk->sk_sndmsg_page = NULL;
	}

	atomic_dec(&tcp_sockets_allocated);

	return 0;
}

EXPORT_SYMBOL(tcp_v4_destroy_sock);

#ifdef CONFIG_PROC_FS
/* Proc filesystem TCP sock list dumping. */

static inline struct inet_timewait_sock *tw_head(struct hlist_head *head)
{
	return hlist_empty(head) ? NULL :
		list_entry(head->first, struct inet_timewait_sock, tw_node);
}

static inline struct inet_timewait_sock *tw_next(struct inet_timewait_sock *tw)
{
	return tw->tw_node.next ?
		hlist_entry(tw->tw_node.next, typeof(*tw), tw_node) : NULL;
}

static void *listening_get_next(struct seq_file *seq, void *cur)
{
	struct inet_connection_sock *icsk;
	struct hlist_node *node;
	struct sock *sk = cur;
	struct tcp_iter_state* st = seq->private;

	if (!sk) {
		st->bucket = 0;
		sk = sk_head(&tcp_hashinfo.listening_hash[0]);
		goto get_sk;
	}

	++st->num;

	if (st->state == TCP_SEQ_STATE_OPENREQ) {
		struct request_sock *req = cur;

	       	icsk = inet_csk(st->syn_wait_sk);
		req = req->dl_next;
		while (1) {
			while (req) {
				if (req->rsk_ops->family == st->family) {
					cur = req;
					goto out;
				}
				req = req->dl_next;
			}
			if (++st->sbucket >= TCP_SYNQ_HSIZE)
				break;
get_req:
			req = icsk->icsk_accept_queue.listen_opt->syn_table[st->sbucket];
		}
		sk	  = sk_next(st->syn_wait_sk);
		st->state = TCP_SEQ_STATE_LISTENING;
		read_unlock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
	} else {
	       	icsk = inet_csk(sk);
		read_lock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
		if (reqsk_queue_len(&icsk->icsk_accept_queue))
			goto start_req;
		read_unlock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
		sk = sk_next(sk);
	}
get_sk:
	sk_for_each_from(sk, node) {
		if (sk->sk_family == st->family) {
			cur = sk;
			goto out;
		}
	       	icsk = inet_csk(sk);
		read_lock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
		if (reqsk_queue_len(&icsk->icsk_accept_queue)) {
start_req:
			st->uid		= sock_i_uid(sk);
			st->syn_wait_sk = sk;
			st->state	= TCP_SEQ_STATE_OPENREQ;
			st->sbucket	= 0;
			goto get_req;
		}
		read_unlock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
	}
	if (++st->bucket < INET_LHTABLE_SIZE) {
		sk = sk_head(&tcp_hashinfo.listening_hash[st->bucket]);
		goto get_sk;
	}
	cur = NULL;
out:
	return cur;
}

static void *listening_get_idx(struct seq_file *seq, loff_t *pos)
{
	void *rc = listening_get_next(seq, NULL);

	while (rc && *pos) {
		rc = listening_get_next(seq, rc);
		--*pos;
	}
	return rc;
}

static void *established_get_first(struct seq_file *seq)
{
	struct tcp_iter_state* st = seq->private;
	void *rc = NULL;

	for (st->bucket = 0; st->bucket < tcp_hashinfo.ehash_size; ++st->bucket) {
		struct sock *sk;
		struct hlist_node *node;
		struct inet_timewait_sock *tw;

		/* We can reschedule _before_ having picked the target: */
		cond_resched_softirq();

		read_lock(&tcp_hashinfo.ehash[st->bucket].lock);
		sk_for_each(sk, node, &tcp_hashinfo.ehash[st->bucket].chain) {
			if (sk->sk_family != st->family) {
				continue;
			}
			rc = sk;
			goto out;
		}
		st->state = TCP_SEQ_STATE_TIME_WAIT;
		inet_twsk_for_each(tw, node,
				   &tcp_hashinfo.ehash[st->bucket + tcp_hashinfo.ehash_size].chain) {
			if (tw->tw_family != st->family) {
				continue;
			}
			rc = tw;
			goto out;
		}
		read_unlock(&tcp_hashinfo.ehash[st->bucket].lock);
		st->state = TCP_SEQ_STATE_ESTABLISHED;
	}
out:
	return rc;
}

static void *established_get_next(struct seq_file *seq, void *cur)
{
	struct sock *sk = cur;
	struct inet_timewait_sock *tw;
	struct hlist_node *node;
	struct tcp_iter_state* st = seq->private;

	++st->num;

	if (st->state == TCP_SEQ_STATE_TIME_WAIT) {
		tw = cur;
		tw = tw_next(tw);
get_tw:
		while (tw && tw->tw_family != st->family) {
			tw = tw_next(tw);
		}
		if (tw) {
			cur = tw;
			goto out;
		}
		read_unlock(&tcp_hashinfo.ehash[st->bucket].lock);
		st->state = TCP_SEQ_STATE_ESTABLISHED;

		/* We can reschedule between buckets: */
		cond_resched_softirq();

		if (++st->bucket < tcp_hashinfo.ehash_size) {
			read_lock(&tcp_hashinfo.ehash[st->bucket].lock);
			sk = sk_head(&tcp_hashinfo.ehash[st->bucket].chain);
		} else {
			cur = NULL;
			goto out;
		}
	} else
		sk = sk_next(sk);

	sk_for_each_from(sk, node) {
		if (sk->sk_family == st->family)
			goto found;
	}

	st->state = TCP_SEQ_STATE_TIME_WAIT;
	tw = tw_head(&tcp_hashinfo.ehash[st->bucket + tcp_hashinfo.ehash_size].chain);
	goto get_tw;
found:
	cur = sk;
out:
	return cur;
}

static void *established_get_idx(struct seq_file *seq, loff_t pos)
{
	void *rc = established_get_first(seq);

	while (rc && pos) {
		rc = established_get_next(seq, rc);
		--pos;
	}		
	return rc;
}

static void *tcp_get_idx(struct seq_file *seq, loff_t pos)
{
	void *rc;
	struct tcp_iter_state* st = seq->private;

	inet_listen_lock(&tcp_hashinfo);
	st->state = TCP_SEQ_STATE_LISTENING;
	rc	  = listening_get_idx(seq, &pos);

	if (!rc) {
		inet_listen_unlock(&tcp_hashinfo);
		local_bh_disable();
		st->state = TCP_SEQ_STATE_ESTABLISHED;
		rc	  = established_get_idx(seq, pos);
	}

	return rc;
}

static void *tcp_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct tcp_iter_state* st = seq->private;
	st->state = TCP_SEQ_STATE_LISTENING;
	st->num = 0;
	return *pos ? tcp_get_idx(seq, *pos - 1) : SEQ_START_TOKEN;
}

static void *tcp_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	void *rc = NULL;
	struct tcp_iter_state* st;

	if (v == SEQ_START_TOKEN) {
		rc = tcp_get_idx(seq, 0);
		goto out;
	}
	st = seq->private;

	switch (st->state) {
	case TCP_SEQ_STATE_OPENREQ:
	case TCP_SEQ_STATE_LISTENING:
		rc = listening_get_next(seq, v);
		if (!rc) {
			inet_listen_unlock(&tcp_hashinfo);
			local_bh_disable();
			st->state = TCP_SEQ_STATE_ESTABLISHED;
			rc	  = established_get_first(seq);
		}
		break;
	case TCP_SEQ_STATE_ESTABLISHED:
	case TCP_SEQ_STATE_TIME_WAIT:
		rc = established_get_next(seq, v);
		break;
	}
out:
	++*pos;
	return rc;
}

static void tcp_seq_stop(struct seq_file *seq, void *v)
{
	struct tcp_iter_state* st = seq->private;

	switch (st->state) {
	case TCP_SEQ_STATE_OPENREQ:
		if (v) {
			struct inet_connection_sock *icsk = inet_csk(st->syn_wait_sk);
			read_unlock_bh(&icsk->icsk_accept_queue.syn_wait_lock);
		}
	case TCP_SEQ_STATE_LISTENING:
		if (v != SEQ_START_TOKEN)
			inet_listen_unlock(&tcp_hashinfo);
		break;
	case TCP_SEQ_STATE_TIME_WAIT:
	case TCP_SEQ_STATE_ESTABLISHED:
		if (v)
			read_unlock(&tcp_hashinfo.ehash[st->bucket].lock);
		local_bh_enable();
		break;
	}
}

static int tcp_seq_open(struct inode *inode, struct file *file)
{
	struct tcp_seq_afinfo *afinfo = PDE(inode)->data;
	struct seq_file *seq;
	struct tcp_iter_state *s;
	int rc;

	if (unlikely(afinfo == NULL))
		return -EINVAL;

	s = kmalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	memset(s, 0, sizeof(*s));
	s->family		= afinfo->family;
	s->seq_ops.start	= tcp_seq_start;
	s->seq_ops.next		= tcp_seq_next;
	s->seq_ops.show		= afinfo->seq_show;
	s->seq_ops.stop		= tcp_seq_stop;

	rc = seq_open(file, &s->seq_ops);
	if (rc)
		goto out_kfree;
	seq	     = file->private_data;
	seq->private = s;
out:
	return rc;
out_kfree:
	kfree(s);
	goto out;
}

int tcp_proc_register(struct tcp_seq_afinfo *afinfo)
{
	int rc = 0;
	struct proc_dir_entry *p;

	if (!afinfo)
		return -EINVAL;
	afinfo->seq_fops->owner		= afinfo->owner;
	afinfo->seq_fops->open		= tcp_seq_open;
	afinfo->seq_fops->read		= seq_read;
	afinfo->seq_fops->llseek	= seq_lseek;
	afinfo->seq_fops->release	= seq_release_private;
	
	p = proc_net_fops_create(afinfo->name, S_IRUGO, afinfo->seq_fops);
	if (p)
		p->data = afinfo;
	else
		rc = -ENOMEM;
	return rc;
}

void tcp_proc_unregister(struct tcp_seq_afinfo *afinfo)
{
	if (!afinfo)
		return;
	proc_net_remove(afinfo->name);
	memset(afinfo->seq_fops, 0, sizeof(*afinfo->seq_fops)); 
}

static void get_openreq4(struct sock *sk, struct request_sock *req,
			 char *tmpbuf, int i, int uid)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	int ttd = req->expires - jiffies;

	sprintf(tmpbuf, "%4d: %08X:%04X %08X:%04X"
		" %02X %08X:%08X %02X:%08lX %08X %5d %8d %u %d %p",
		i,
		ireq->loc_addr,
		ntohs(inet_sk(sk)->sport),
		ireq->rmt_addr,
		ntohs(ireq->rmt_port),
		TCP_SYN_RECV,
		0, 0, /* could print option size, but that is af dependent. */
		1,    /* timers active (only the expire timer) */
		jiffies_to_clock_t(ttd),
		req->retrans,
		uid,
		0,  /* non standard timer */
		0, /* open_requests have no inode */
		atomic_read(&sk->sk_refcnt),
		req);
}

static void get_tcp4_sock(struct sock *sp, char *tmpbuf, int i)
{
	int timer_active;
	unsigned long timer_expires;
	struct tcp_sock *tp = tcp_sk(sp);
	const struct inet_connection_sock *icsk = inet_csk(sp);
	struct inet_sock *inet = inet_sk(sp);
	unsigned int dest = inet->daddr;
	unsigned int src = inet->rcv_saddr;
	__u16 destp = ntohs(inet->dport);
	__u16 srcp = ntohs(inet->sport);

	if (icsk->icsk_pending == ICSK_TIME_RETRANS) {
		timer_active	= 1;
		timer_expires	= icsk->icsk_timeout;
	} else if (icsk->icsk_pending == ICSK_TIME_PROBE0) {
		timer_active	= 4;
		timer_expires	= icsk->icsk_timeout;
	} else if (timer_pending(&sp->sk_timer)) {
		timer_active	= 2;
		timer_expires	= sp->sk_timer.expires;
	} else {
		timer_active	= 0;
		timer_expires = jiffies;
	}

	sprintf(tmpbuf, "%4d: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08lX "
			"%08X %5d %8d %lu %d %p %u %u %u %u %d",
		i, src, srcp, dest, destp, sp->sk_state,
		tp->write_seq - tp->snd_una, tp->rcv_nxt - tp->copied_seq,
		timer_active,
		jiffies_to_clock_t(timer_expires - jiffies),
		icsk->icsk_retransmits,
		sock_i_uid(sp),
		icsk->icsk_probes_out,
		sock_i_ino(sp),
		atomic_read(&sp->sk_refcnt), sp,
		icsk->icsk_rto,
		icsk->icsk_ack.ato,
		(icsk->icsk_ack.quick << 1) | icsk->icsk_ack.pingpong,
		tp->snd_cwnd,
		tp->snd_ssthresh >= 0xFFFF ? -1 : tp->snd_ssthresh);
}

static void get_timewait4_sock(struct inet_timewait_sock *tw, char *tmpbuf, int i)
{
	unsigned int dest, src;
	__u16 destp, srcp;
	int ttd = tw->tw_ttd - jiffies;

	if (ttd < 0)
		ttd = 0;

	dest  = tw->tw_daddr;
	src   = tw->tw_rcv_saddr;
	destp = ntohs(tw->tw_dport);
	srcp  = ntohs(tw->tw_sport);

	sprintf(tmpbuf, "%4d: %08X:%04X %08X:%04X"
		" %02X %08X:%08X %02X:%08lX %08X %5d %8d %d %d %p",
		i, src, srcp, dest, destp, tw->tw_substate, 0, 0,
		3, jiffies_to_clock_t(ttd), 0, 0, 0, 0,
		atomic_read(&tw->tw_refcnt), tw);
}

#define TMPSZ 150

static int tcp4_seq_show(struct seq_file *seq, void *v)
{
	struct tcp_iter_state* st;
	char tmpbuf[TMPSZ + 1];

	if (v == SEQ_START_TOKEN) {
		seq_printf(seq, "%-*s\n", TMPSZ - 1,
			   "  sl  local_address rem_address   st tx_queue "
			   "rx_queue tr tm->when retrnsmt   uid  timeout "
			   "inode");
		goto out;
	}
	st = seq->private;

	switch (st->state) {
	case TCP_SEQ_STATE_LISTENING:
	case TCP_SEQ_STATE_ESTABLISHED:
		get_tcp4_sock(v, tmpbuf, st->num);
		break;
	case TCP_SEQ_STATE_OPENREQ:
		get_openreq4(st->syn_wait_sk, v, tmpbuf, st->num, st->uid);
		break;
	case TCP_SEQ_STATE_TIME_WAIT:
		get_timewait4_sock(v, tmpbuf, st->num);
		break;
	}
	seq_printf(seq, "%-*s\n", TMPSZ - 1, tmpbuf);
out:
	return 0;
}

static struct file_operations tcp4_seq_fops;
static struct tcp_seq_afinfo tcp4_seq_afinfo = {
	.owner		= THIS_MODULE,
	.name		= "tcp",
	.family		= AF_INET,
	.seq_show	= tcp4_seq_show,
	.seq_fops	= &tcp4_seq_fops,
};

int __init tcp4_proc_init(void)
{
	return tcp_proc_register(&tcp4_seq_afinfo);
}

void tcp4_proc_exit(void)
{
	tcp_proc_unregister(&tcp4_seq_afinfo);
}
#endif /* CONFIG_PROC_FS */

struct proto tcp_prot = {
	.name			= "TCP",
	.owner			= THIS_MODULE,
	.close			= tcp_close,
	.connect		= tcp_v4_connect,
	.disconnect		= tcp_disconnect,
	.accept			= inet_csk_accept,
	.ioctl			= tcp_ioctl,
	.init			= tcp_v4_init_sock,
	.destroy		= tcp_v4_destroy_sock,
	.shutdown		= tcp_shutdown,
	.setsockopt		= tcp_setsockopt,
	.getsockopt		= tcp_getsockopt,
	.sendmsg		= tcp_sendmsg,
	.recvmsg		= tcp_recvmsg,
	.backlog_rcv		= tcp_v4_do_rcv,
	.hash			= tcp_v4_hash,
	.unhash			= tcp_unhash,
	.get_port		= tcp_v4_get_port,
	.enter_memory_pressure	= tcp_enter_memory_pressure,
	.sockets_allocated	= &tcp_sockets_allocated,
	.orphan_count		= &tcp_orphan_count,
	.memory_allocated	= &tcp_memory_allocated,
	.memory_pressure	= &tcp_memory_pressure,
	.sysctl_mem		= sysctl_tcp_mem,
	.sysctl_wmem		= sysctl_tcp_wmem,
	.sysctl_rmem		= sysctl_tcp_rmem,
	.max_header		= MAX_TCP_HEADER,
	.obj_size		= sizeof(struct tcp_sock),
	.twsk_prot		= &tcp_timewait_sock_ops,
	.rsk_prot		= &tcp_request_sock_ops,
};



void __init tcp_v4_init(struct net_proto_family *ops)
{
	int err = sock_create_kern(PF_INET, SOCK_RAW, IPPROTO_TCP, &tcp_socket);
	if (err < 0)
		panic("Failed to create the TCP control socket.\n");
	tcp_socket->sk->sk_allocation   = GFP_ATOMIC;
	inet_sk(tcp_socket->sk)->uc_ttl = -1;

	/* Unhash it so that IP input processing does not even
	 * see it, we do not wish this socket to see incoming
	 * packets.
	 */
	tcp_socket->sk->sk_prot->unhash(tcp_socket->sk);
}

EXPORT_SYMBOL(ipv4_specific);
EXPORT_SYMBOL(inet_bind_bucket_create);
EXPORT_SYMBOL(tcp_hashinfo);
EXPORT_SYMBOL(tcp_prot);
EXPORT_SYMBOL(tcp_unhash);
EXPORT_SYMBOL(tcp_v4_conn_request);
EXPORT_SYMBOL(tcp_v4_connect);
EXPORT_SYMBOL(tcp_v4_do_rcv);
EXPORT_SYMBOL(tcp_v4_remember_stamp);
EXPORT_SYMBOL(tcp_v4_send_check);
EXPORT_SYMBOL(tcp_v4_syn_recv_sock);

#ifdef CONFIG_PROC_FS
EXPORT_SYMBOL(tcp_proc_register);
EXPORT_SYMBOL(tcp_proc_unregister);
#endif
EXPORT_SYMBOL(sysctl_local_port_range);
EXPORT_SYMBOL(sysctl_tcp_low_latency);
EXPORT_SYMBOL(sysctl_tcp_tw_reuse);

