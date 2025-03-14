/*
 *	DCCP over IPv6
 *	Linux INET6 implementation 
 *
 *	Based on net/dccp6/ipv6.c
 *
 *	Arnaldo Carvalho de Melo <acme@ghostprotocols.net>
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/xfrm.h>

#include <net/addrconf.h>
#include <net/inet_common.h>
#include <net/inet_hashtables.h>
#include <net/inet_sock.h>
#include <net/inet6_connection_sock.h>
#include <net/inet6_hashtables.h>
#include <net/ip6_route.h>
#include <net/ipv6.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/ip6_checksum.h>
#include <net/xfrm.h>

#include "dccp.h"
#include "ipv6.h"

static void dccp_v6_ctl_send_reset(struct sk_buff *skb);
static void dccp_v6_reqsk_send_ack(struct sk_buff *skb,
				   struct request_sock *req);
static void dccp_v6_send_check(struct sock *sk, int len, struct sk_buff *skb);

static int dccp_v6_do_rcv(struct sock *sk, struct sk_buff *skb);

static struct inet_connection_sock_af_ops dccp_ipv6_mapped;
static struct inet_connection_sock_af_ops dccp_ipv6_af_ops;

static int dccp_v6_get_port(struct sock *sk, unsigned short snum)
{
	return inet_csk_get_port(&dccp_hashinfo, sk, snum,
				 inet6_csk_bind_conflict);
}

static void dccp_v6_hash(struct sock *sk)
{
	if (sk->sk_state != DCCP_CLOSED) {
		if (inet_csk(sk)->icsk_af_ops == &dccp_ipv6_mapped) {
			dccp_prot.hash(sk);
			return;
		}
		local_bh_disable();
		__inet6_hash(&dccp_hashinfo, sk);
		local_bh_enable();
	}
}

static inline u16 dccp_v6_check(struct dccp_hdr *dh, int len,
				struct in6_addr *saddr, 
				struct in6_addr *daddr, 
				unsigned long base)
{
	return csum_ipv6_magic(saddr, daddr, len, IPPROTO_DCCP, base);
}

static __u32 dccp_v6_init_sequence(struct sock *sk, struct sk_buff *skb)
{
	const struct dccp_hdr *dh = dccp_hdr(skb);

	if (skb->protocol == htons(ETH_P_IPV6))
		return secure_tcpv6_sequence_number(skb->nh.ipv6h->daddr.s6_addr32,
						    skb->nh.ipv6h->saddr.s6_addr32,
						    dh->dccph_dport,
						    dh->dccph_sport);
	else
		return secure_dccp_sequence_number(skb->nh.iph->daddr,
						   skb->nh.iph->saddr,
						   dh->dccph_dport,
						   dh->dccph_sport);
}

static int dccp_v6_connect(struct sock *sk, struct sockaddr *uaddr, 
			   int addr_len)
{
	struct sockaddr_in6 *usin = (struct sockaddr_in6 *) uaddr;
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_sock *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct dccp_sock *dp = dccp_sk(sk);
	struct in6_addr *saddr = NULL, *final_p = NULL, final;
	struct flowi fl;
	struct dst_entry *dst;
	int addr_type;
	int err;

	dp->dccps_role = DCCP_ROLE_CLIENT;

	if (addr_len < SIN6_LEN_RFC2133) 
		return -EINVAL;

	if (usin->sin6_family != AF_INET6) 
		return -EAFNOSUPPORT;

	memset(&fl, 0, sizeof(fl));

	if (np->sndflow) {
		fl.fl6_flowlabel = usin->sin6_flowinfo & IPV6_FLOWINFO_MASK;
		IP6_ECN_flow_init(fl.fl6_flowlabel);
		if (fl.fl6_flowlabel & IPV6_FLOWLABEL_MASK) {
			struct ip6_flowlabel *flowlabel;
			flowlabel = fl6_sock_lookup(sk, fl.fl6_flowlabel);
			if (flowlabel == NULL)
				return -EINVAL;
			ipv6_addr_copy(&usin->sin6_addr, &flowlabel->dst);
			fl6_sock_release(flowlabel);
		}
	}

	/*
  	 *	connect() to INADDR_ANY means loopback (BSD'ism).
  	 */
  	
  	if (ipv6_addr_any(&usin->sin6_addr))
		usin->sin6_addr.s6_addr[15] = 0x1; 

	addr_type = ipv6_addr_type(&usin->sin6_addr);

	if(addr_type & IPV6_ADDR_MULTICAST)
		return -ENETUNREACH;

	if (addr_type & IPV6_ADDR_LINKLOCAL) {
		if (addr_len >= sizeof(struct sockaddr_in6) &&
		    usin->sin6_scope_id) {
			/* If interface is set while binding, indices
			 * must coincide.
			 */
			if (sk->sk_bound_dev_if &&
			    sk->sk_bound_dev_if != usin->sin6_scope_id)
				return -EINVAL;

			sk->sk_bound_dev_if = usin->sin6_scope_id;
		}

		/* Connect to link-local address requires an interface */
		if (!sk->sk_bound_dev_if)
			return -EINVAL;
	}

	ipv6_addr_copy(&np->daddr, &usin->sin6_addr);
	np->flow_label = fl.fl6_flowlabel;

	/*
	 *	DCCP over IPv4
	 */

	if (addr_type == IPV6_ADDR_MAPPED) {
		u32 exthdrlen = icsk->icsk_ext_hdr_len;
		struct sockaddr_in sin;

		SOCK_DEBUG(sk, "connect: ipv4 mapped\n");

		if (__ipv6_only_sock(sk))
			return -ENETUNREACH;

		sin.sin_family = AF_INET;
		sin.sin_port = usin->sin6_port;
		sin.sin_addr.s_addr = usin->sin6_addr.s6_addr32[3];

		icsk->icsk_af_ops = &dccp_ipv6_mapped;
		sk->sk_backlog_rcv = dccp_v4_do_rcv;

		err = dccp_v4_connect(sk, (struct sockaddr *)&sin, sizeof(sin));

		if (err) {
			icsk->icsk_ext_hdr_len = exthdrlen;
			icsk->icsk_af_ops = &dccp_ipv6_af_ops;
			sk->sk_backlog_rcv = dccp_v6_do_rcv;
			goto failure;
		} else {
			ipv6_addr_set(&np->saddr, 0, 0, htonl(0x0000FFFF),
				      inet->saddr);
			ipv6_addr_set(&np->rcv_saddr, 0, 0, htonl(0x0000FFFF),
				      inet->rcv_saddr);
		}

		return err;
	}

	if (!ipv6_addr_any(&np->rcv_saddr))
		saddr = &np->rcv_saddr;

	fl.proto = IPPROTO_DCCP;
	ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
	ipv6_addr_copy(&fl.fl6_src, saddr ? saddr : &np->saddr);
	fl.oif = sk->sk_bound_dev_if;
	fl.fl_ip_dport = usin->sin6_port;
	fl.fl_ip_sport = inet->sport;

	if (np->opt && np->opt->srcrt) {
		struct rt0_hdr *rt0 = (struct rt0_hdr *)np->opt->srcrt;
		ipv6_addr_copy(&final, &fl.fl6_dst);
		ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
		final_p = &final;
	}

	err = ip6_dst_lookup(sk, &dst, &fl);
	if (err)
		goto failure;
	if (final_p)
		ipv6_addr_copy(&fl.fl6_dst, final_p);

	if ((err = xfrm_lookup(&dst, &fl, sk, 0)) < 0)
		goto failure;

	if (saddr == NULL) {
		saddr = &fl.fl6_src;
		ipv6_addr_copy(&np->rcv_saddr, saddr);
	}

	/* set the source address */
	ipv6_addr_copy(&np->saddr, saddr);
	inet->rcv_saddr = LOOPBACK4_IPV6;

	ip6_dst_store(sk, dst, NULL);

	icsk->icsk_ext_hdr_len = 0;
	if (np->opt)
		icsk->icsk_ext_hdr_len = (np->opt->opt_flen +
					  np->opt->opt_nflen);

	inet->dport = usin->sin6_port;

	dccp_set_state(sk, DCCP_REQUESTING);
	err = inet6_hash_connect(&dccp_death_row, sk);
	if (err)
		goto late_failure;
	/* FIXME */
#if 0
	dp->dccps_gar = secure_dccp_v6_sequence_number(np->saddr.s6_addr32,
						       np->daddr.s6_addr32,
						       inet->sport,
						       inet->dport);
#endif
	err = dccp_connect(sk);
	if (err)
		goto late_failure;

	return 0;

late_failure:
	dccp_set_state(sk, DCCP_CLOSED);
	__sk_dst_reset(sk);
failure:
	inet->dport = 0;
	sk->sk_route_caps = 0;
	return err;
}

static void dccp_v6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
			int type, int code, int offset, __u32 info)
{
	struct ipv6hdr *hdr = (struct ipv6hdr *)skb->data;
	const struct dccp_hdr *dh = (struct dccp_hdr *)(skb->data + offset);
	struct ipv6_pinfo *np;
	struct sock *sk;
	int err;
	__u64 seq;

	sk = inet6_lookup(&dccp_hashinfo, &hdr->daddr, dh->dccph_dport,
			  &hdr->saddr, dh->dccph_sport, skb->dev->ifindex);

	if (sk == NULL) {
		ICMP6_INC_STATS_BH(__in6_dev_get(skb->dev), ICMP6_MIB_INERRORS);
		return;
	}

	if (sk->sk_state == DCCP_TIME_WAIT) {
		inet_twsk_put((struct inet_timewait_sock *)sk);
		return;
	}

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk))
		NET_INC_STATS_BH(LINUX_MIB_LOCKDROPPEDICMPS);

	if (sk->sk_state == DCCP_CLOSED)
		goto out;

	np = inet6_sk(sk);

	if (type == ICMPV6_PKT_TOOBIG) {
		struct dst_entry *dst = NULL;

		if (sock_owned_by_user(sk))
			goto out;
		if ((1 << sk->sk_state) & (DCCPF_LISTEN | DCCPF_CLOSED))
			goto out;

		/* icmp should have updated the destination cache entry */
		dst = __sk_dst_check(sk, np->dst_cookie);

		if (dst == NULL) {
			struct inet_sock *inet = inet_sk(sk);
			struct flowi fl;

			/* BUGGG_FUTURE: Again, it is not clear how
			   to handle rthdr case. Ignore this complexity
			   for now.
			 */
			memset(&fl, 0, sizeof(fl));
			fl.proto = IPPROTO_DCCP;
			ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
			ipv6_addr_copy(&fl.fl6_src, &np->saddr);
			fl.oif = sk->sk_bound_dev_if;
			fl.fl_ip_dport = inet->dport;
			fl.fl_ip_sport = inet->sport;

			if ((err = ip6_dst_lookup(sk, &dst, &fl))) {
				sk->sk_err_soft = -err;
				goto out;
			}

			if ((err = xfrm_lookup(&dst, &fl, sk, 0)) < 0) {
				sk->sk_err_soft = -err;
				goto out;
			}

		} else
			dst_hold(dst);

		if (inet_csk(sk)->icsk_pmtu_cookie > dst_mtu(dst)) {
			dccp_sync_mss(sk, dst_mtu(dst));
		} /* else let the usual retransmit timer handle it */
		dst_release(dst);
		goto out;
	}

	icmpv6_err_convert(type, code, &err);

	seq = DCCP_SKB_CB(skb)->dccpd_seq;
	/* Might be for an request_sock */
	switch (sk->sk_state) {
		struct request_sock *req, **prev;
	case DCCP_LISTEN:
		if (sock_owned_by_user(sk))
			goto out;

		req = inet6_csk_search_req(sk, &prev, dh->dccph_dport,
					   &hdr->daddr, &hdr->saddr,
					   inet6_iif(skb));
		if (!req)
			goto out;

		/* ICMPs are not backlogged, hence we cannot get
		 * an established socket here.
		 */
		BUG_TRAP(req->sk == NULL);

		if (seq != dccp_rsk(req)->dreq_iss) {
			NET_INC_STATS_BH(LINUX_MIB_OUTOFWINDOWICMPS);
			goto out;
		}

		inet_csk_reqsk_queue_drop(sk, req, prev);
		goto out;

	case DCCP_REQUESTING:
	case DCCP_RESPOND:  /* Cannot happen.
			       It can, it SYNs are crossed. --ANK */ 
		if (!sock_owned_by_user(sk)) {
			DCCP_INC_STATS_BH(DCCP_MIB_ATTEMPTFAILS);
			sk->sk_err = err;
			/*
			 * Wake people up to see the error
			 * (see connect in sock.c)
			 */
			sk->sk_error_report(sk);

			dccp_done(sk);
		} else
			sk->sk_err_soft = err;
		goto out;
	}

	if (!sock_owned_by_user(sk) && np->recverr) {
		sk->sk_err = err;
		sk->sk_error_report(sk);
	} else
		sk->sk_err_soft = err;

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}


static int dccp_v6_send_response(struct sock *sk, struct request_sock *req,
				 struct dst_entry *dst)
{
	struct inet6_request_sock *ireq6 = inet6_rsk(req);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sk_buff *skb;
	struct ipv6_txoptions *opt = NULL;
	struct in6_addr *final_p = NULL, final;
	struct flowi fl;
	int err = -1;

	memset(&fl, 0, sizeof(fl));
	fl.proto = IPPROTO_DCCP;
	ipv6_addr_copy(&fl.fl6_dst, &ireq6->rmt_addr);
	ipv6_addr_copy(&fl.fl6_src, &ireq6->loc_addr);
	fl.fl6_flowlabel = 0;
	fl.oif = ireq6->iif;
	fl.fl_ip_dport = inet_rsk(req)->rmt_port;
	fl.fl_ip_sport = inet_sk(sk)->sport;

	if (dst == NULL) {
		opt = np->opt;
		if (opt == NULL &&
		    np->rxopt.bits.osrcrt == 2 &&
		    ireq6->pktopts) {
			struct sk_buff *pktopts = ireq6->pktopts;
			struct inet6_skb_parm *rxopt = IP6CB(pktopts);
			if (rxopt->srcrt)
				opt = ipv6_invert_rthdr(sk,
					(struct ipv6_rt_hdr *)(pktopts->nh.raw +
							       rxopt->srcrt));
		}

		if (opt && opt->srcrt) {
			struct rt0_hdr *rt0 = (struct rt0_hdr *)opt->srcrt;
			ipv6_addr_copy(&final, &fl.fl6_dst);
			ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
			final_p = &final;
		}

		err = ip6_dst_lookup(sk, &dst, &fl);
		if (err)
			goto done;
		if (final_p)
			ipv6_addr_copy(&fl.fl6_dst, final_p);
		if ((err = xfrm_lookup(&dst, &fl, sk, 0)) < 0)
			goto done;
	}

	skb = dccp_make_response(sk, dst, req);
	if (skb != NULL) {
		struct dccp_hdr *dh = dccp_hdr(skb);
		dh->dccph_checksum = dccp_v6_check(dh, skb->len,
						   &ireq6->loc_addr,
						   &ireq6->rmt_addr,
						   csum_partial((char *)dh,
								skb->len,
								skb->csum));
		ipv6_addr_copy(&fl.fl6_dst, &ireq6->rmt_addr);
		err = ip6_xmit(sk, skb, &fl, opt, 0);
		if (err == NET_XMIT_CN)
			err = 0;
	}

done:
        if (opt && opt != np->opt)
		sock_kfree_s(sk, opt, opt->tot_len);
	return err;
}

static void dccp_v6_reqsk_destructor(struct request_sock *req)
{
	if (inet6_rsk(req)->pktopts != NULL)
		kfree_skb(inet6_rsk(req)->pktopts);
}

static struct request_sock_ops dccp6_request_sock_ops = {
	.family		= AF_INET6,
	.obj_size	= sizeof(struct dccp6_request_sock),
	.rtx_syn_ack	= dccp_v6_send_response,
	.send_ack	= dccp_v6_reqsk_send_ack,
	.destructor	= dccp_v6_reqsk_destructor,
	.send_reset	= dccp_v6_ctl_send_reset,
};

static struct timewait_sock_ops dccp6_timewait_sock_ops = {
	.twsk_obj_size	= sizeof(struct dccp6_timewait_sock),
};

static void dccp_v6_send_check(struct sock *sk, int len, struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct dccp_hdr *dh = dccp_hdr(skb);

	dh->dccph_checksum = csum_ipv6_magic(&np->saddr, &np->daddr,
					     len, IPPROTO_DCCP, 
					     csum_partial((char *)dh,
							  dh->dccph_doff << 2,
							  skb->csum));
}

static void dccp_v6_ctl_send_reset(struct sk_buff *rxskb)
{
	struct dccp_hdr *rxdh = dccp_hdr(rxskb), *dh; 
	const int dccp_hdr_reset_len = sizeof(struct dccp_hdr) +
				       sizeof(struct dccp_hdr_ext) +
				       sizeof(struct dccp_hdr_reset);
	struct sk_buff *skb;
	struct flowi fl;
	u64 seqno;

	if (rxdh->dccph_type == DCCP_PKT_RESET)
		return;

	if (!ipv6_unicast_destination(rxskb))
		return; 

	/*
	 * We need to grab some memory, and put together an RST,
	 * and then put it into the queue to be sent.
	 */

	skb = alloc_skb(MAX_HEADER + sizeof(struct ipv6hdr) +
			dccp_hdr_reset_len, GFP_ATOMIC);
	if (skb == NULL) 
	  	return;

	skb_reserve(skb, MAX_HEADER + sizeof(struct ipv6hdr) +
		    dccp_hdr_reset_len);

	skb->h.raw = skb_push(skb, dccp_hdr_reset_len);
	dh = dccp_hdr(skb);
	memset(dh, 0, dccp_hdr_reset_len);

	/* Swap the send and the receive. */
	dh->dccph_type	= DCCP_PKT_RESET;
	dh->dccph_sport	= rxdh->dccph_dport;
	dh->dccph_dport	= rxdh->dccph_sport;
	dh->dccph_doff	= dccp_hdr_reset_len / 4;
	dh->dccph_x	= 1;
	dccp_hdr_reset(skb)->dccph_reset_code =
				DCCP_SKB_CB(rxskb)->dccpd_reset_code;

	/* See "8.3.1. Abnormal Termination" in draft-ietf-dccp-spec-11 */
	seqno = 0;
	if (DCCP_SKB_CB(rxskb)->dccpd_ack_seq != DCCP_PKT_WITHOUT_ACK_SEQ)
		dccp_set_seqno(&seqno, DCCP_SKB_CB(rxskb)->dccpd_ack_seq + 1);

	dccp_hdr_set_seq(dh, seqno);
	dccp_hdr_set_ack(dccp_hdr_ack_bits(skb),
			 DCCP_SKB_CB(rxskb)->dccpd_seq);

	memset(&fl, 0, sizeof(fl));
	ipv6_addr_copy(&fl.fl6_dst, &rxskb->nh.ipv6h->saddr);
	ipv6_addr_copy(&fl.fl6_src, &rxskb->nh.ipv6h->daddr);
	dh->dccph_checksum = csum_ipv6_magic(&fl.fl6_src, &fl.fl6_dst,
					     sizeof(*dh), IPPROTO_DCCP,
					     skb->csum);
	fl.proto = IPPROTO_DCCP;
	fl.oif = inet6_iif(rxskb);
	fl.fl_ip_dport = dh->dccph_dport;
	fl.fl_ip_sport = dh->dccph_sport;

	/* sk = NULL, but it is safe for now. RST socket required. */
	if (!ip6_dst_lookup(NULL, &skb->dst, &fl)) {
		if (xfrm_lookup(&skb->dst, &fl, NULL, 0) >= 0) {
			ip6_xmit(NULL, skb, &fl, NULL, 0);
			DCCP_INC_STATS_BH(DCCP_MIB_OUTSEGS);
			DCCP_INC_STATS_BH(DCCP_MIB_OUTRSTS);
			return;
		}
	}

	kfree_skb(skb);
}

static void dccp_v6_ctl_send_ack(struct sk_buff *rxskb)
{
	struct flowi fl;
	struct dccp_hdr *rxdh = dccp_hdr(rxskb), *dh;
	const int dccp_hdr_ack_len = sizeof(struct dccp_hdr) +
				     sizeof(struct dccp_hdr_ext) +
				     sizeof(struct dccp_hdr_ack_bits);
	struct sk_buff *skb;

	skb = alloc_skb(MAX_HEADER + sizeof(struct ipv6hdr) +
			dccp_hdr_ack_len, GFP_ATOMIC);
	if (skb == NULL)
		return;

	skb_reserve(skb, MAX_HEADER + sizeof(struct ipv6hdr) +
			 dccp_hdr_ack_len);

	skb->h.raw = skb_push(skb, dccp_hdr_ack_len);
	dh = dccp_hdr(skb);
	memset(dh, 0, dccp_hdr_ack_len);

	/* Build DCCP header and checksum it. */
	dh->dccph_type	= DCCP_PKT_ACK;
	dh->dccph_sport = rxdh->dccph_dport;
	dh->dccph_dport = rxdh->dccph_sport;
	dh->dccph_doff	= dccp_hdr_ack_len / 4;
	dh->dccph_x	= 1;
	
	dccp_hdr_set_seq(dh, DCCP_SKB_CB(rxskb)->dccpd_ack_seq);
	dccp_hdr_set_ack(dccp_hdr_ack_bits(skb),
			 DCCP_SKB_CB(rxskb)->dccpd_seq);

	memset(&fl, 0, sizeof(fl));
	ipv6_addr_copy(&fl.fl6_dst, &rxskb->nh.ipv6h->saddr);
	ipv6_addr_copy(&fl.fl6_src, &rxskb->nh.ipv6h->daddr);

	/* FIXME: calculate checksum, IPv4 also should... */

	fl.proto = IPPROTO_DCCP;
	fl.oif = inet6_iif(rxskb);
	fl.fl_ip_dport = dh->dccph_dport;
	fl.fl_ip_sport = dh->dccph_sport;

	if (!ip6_dst_lookup(NULL, &skb->dst, &fl)) {
		if (xfrm_lookup(&skb->dst, &fl, NULL, 0) >= 0) {
			ip6_xmit(NULL, skb, &fl, NULL, 0);
			DCCP_INC_STATS_BH(DCCP_MIB_OUTSEGS);
			return;
		}
	}

	kfree_skb(skb);
}

static void dccp_v6_reqsk_send_ack(struct sk_buff *skb,
				   struct request_sock *req)
{
	dccp_v6_ctl_send_ack(skb);
}

static struct sock *dccp_v6_hnd_req(struct sock *sk,struct sk_buff *skb)
{
	const struct dccp_hdr *dh = dccp_hdr(skb);
	const struct ipv6hdr *iph = skb->nh.ipv6h;
	struct sock *nsk;
	struct request_sock **prev;
	/* Find possible connection requests. */
	struct request_sock *req = inet6_csk_search_req(sk, &prev,
							dh->dccph_sport,
							&iph->saddr,
							&iph->daddr,
							inet6_iif(skb));
	if (req != NULL)
		return dccp_check_req(sk, skb, req, prev);

	nsk = __inet6_lookup_established(&dccp_hashinfo,
					 &iph->saddr, dh->dccph_sport,
					 &iph->daddr, ntohs(dh->dccph_dport),
					 inet6_iif(skb));

	if (nsk != NULL) {
		if (nsk->sk_state != DCCP_TIME_WAIT) {
			bh_lock_sock(nsk);
			return nsk;
		}
		inet_twsk_put((struct inet_timewait_sock *)nsk);
		return NULL;
	}

	return sk;
}

static int dccp_v6_conn_request(struct sock *sk, struct sk_buff *skb)
{
	struct inet_request_sock *ireq;
	struct dccp_sock dp;
	struct request_sock *req;
	struct dccp_request_sock *dreq;
	struct inet6_request_sock *ireq6;
	struct ipv6_pinfo *np = inet6_sk(sk);
 	const __u32 service = dccp_hdr_request(skb)->dccph_req_service;
	struct dccp_skb_cb *dcb = DCCP_SKB_CB(skb);
	__u8 reset_code = DCCP_RESET_CODE_TOO_BUSY;

	if (skb->protocol == htons(ETH_P_IP))
		return dccp_v4_conn_request(sk, skb);

	if (!ipv6_unicast_destination(skb))
		goto drop; 

	if (dccp_bad_service_code(sk, service)) {
		reset_code = DCCP_RESET_CODE_BAD_SERVICE_CODE;
		goto drop;
 	}
	/*
	 *	There are no SYN attacks on IPv6, yet...	
	 */
	if (inet_csk_reqsk_queue_is_full(sk))
		goto drop;		

	if (sk_acceptq_is_full(sk) && inet_csk_reqsk_queue_young(sk) > 1)
		goto drop;

	req = inet6_reqsk_alloc(sk->sk_prot->rsk_prot);
	if (req == NULL)
		goto drop;

	/* FIXME: process options */

	dccp_openreq_init(req, &dp, skb);

	ireq6 = inet6_rsk(req);
	ireq = inet_rsk(req);
	ipv6_addr_copy(&ireq6->rmt_addr, &skb->nh.ipv6h->saddr);
	ipv6_addr_copy(&ireq6->loc_addr, &skb->nh.ipv6h->daddr);
	req->rcv_wnd	= 100; /* Fake, option parsing will get the
				  right value */
	ireq6->pktopts	= NULL;

	if (ipv6_opt_accepted(sk, skb) ||
	    np->rxopt.bits.rxinfo || np->rxopt.bits.rxoinfo ||
	    np->rxopt.bits.rxhlim || np->rxopt.bits.rxohlim) {
		atomic_inc(&skb->users);
		ireq6->pktopts = skb;
	}
	ireq6->iif = sk->sk_bound_dev_if;

	/* So that link locals have meaning */
	if (!sk->sk_bound_dev_if &&
	    ipv6_addr_type(&ireq6->rmt_addr) & IPV6_ADDR_LINKLOCAL)
		ireq6->iif = inet6_iif(skb);

	/* 
	 * Step 3: Process LISTEN state
	 *
	 * Set S.ISR, S.GSR, S.SWL, S.SWH from packet or Init Cookie
	 *
	 * In fact we defer setting S.GSR, S.SWL, S.SWH to
	 * dccp_create_openreq_child.
	 */
	dreq = dccp_rsk(req);
	dreq->dreq_isr	   = dcb->dccpd_seq;
	dreq->dreq_iss	   = dccp_v6_init_sequence(sk, skb);
	dreq->dreq_service = service;

	if (dccp_v6_send_response(sk, req, NULL))
		goto drop_and_free;

	inet6_csk_reqsk_queue_hash_add(sk, req, DCCP_TIMEOUT_INIT);
	return 0;

drop_and_free:
	reqsk_free(req);
drop:
	DCCP_INC_STATS_BH(DCCP_MIB_ATTEMPTFAILS);
	dcb->dccpd_reset_code = reset_code;
	return -1;
}

static struct sock *dccp_v6_request_recv_sock(struct sock *sk,
					      struct sk_buff *skb,
					      struct request_sock *req,
					      struct dst_entry *dst)
{
	struct inet6_request_sock *ireq6 = inet6_rsk(req);
	struct ipv6_pinfo *newnp, *np = inet6_sk(sk);
	struct inet_sock *newinet;
	struct dccp_sock *newdp;
	struct dccp6_sock *newdp6;
	struct sock *newsk;
	struct ipv6_txoptions *opt;

	if (skb->protocol == htons(ETH_P_IP)) {
		/*
		 *	v6 mapped
		 */

		newsk = dccp_v4_request_recv_sock(sk, skb, req, dst);
		if (newsk == NULL) 
			return NULL;

		newdp6 = (struct dccp6_sock *)newsk;
		newdp = dccp_sk(newsk);
		newinet = inet_sk(newsk);
		newinet->pinet6 = &newdp6->inet6;
		newnp = inet6_sk(newsk);

		memcpy(newnp, np, sizeof(struct ipv6_pinfo));

		ipv6_addr_set(&newnp->daddr, 0, 0, htonl(0x0000FFFF),
			      newinet->daddr);

		ipv6_addr_set(&newnp->saddr, 0, 0, htonl(0x0000FFFF),
			      newinet->saddr);

		ipv6_addr_copy(&newnp->rcv_saddr, &newnp->saddr);

		inet_csk(newsk)->icsk_af_ops = &dccp_ipv6_mapped;
		newsk->sk_backlog_rcv = dccp_v4_do_rcv;
		newnp->pktoptions  = NULL;
		newnp->opt	   = NULL;
		newnp->mcast_oif   = inet6_iif(skb);
		newnp->mcast_hops  = skb->nh.ipv6h->hop_limit;

		/*
		 * No need to charge this sock to the relevant IPv6 refcnt debug socks count
		 * here, dccp_create_openreq_child now does this for us, see the comment in
		 * that function for the gory details. -acme
		 */

		/* It is tricky place. Until this moment IPv4 tcp
		   worked with IPv6 icsk.icsk_af_ops.
		   Sync it now.
		 */
		dccp_sync_mss(newsk, inet_csk(newsk)->icsk_pmtu_cookie);

		return newsk;
	}

	opt = np->opt;

	if (sk_acceptq_is_full(sk))
		goto out_overflow;

	if (np->rxopt.bits.osrcrt == 2 &&
	    opt == NULL && ireq6->pktopts) {
		struct inet6_skb_parm *rxopt = IP6CB(ireq6->pktopts);
		if (rxopt->srcrt)
			opt = ipv6_invert_rthdr(sk,
				(struct ipv6_rt_hdr *)(ireq6->pktopts->nh.raw +
						       rxopt->srcrt));
	}

	if (dst == NULL) {
		struct in6_addr *final_p = NULL, final;
		struct flowi fl;

		memset(&fl, 0, sizeof(fl));
		fl.proto = IPPROTO_DCCP;
		ipv6_addr_copy(&fl.fl6_dst, &ireq6->rmt_addr);
		if (opt && opt->srcrt) {
			struct rt0_hdr *rt0 = (struct rt0_hdr *) opt->srcrt;
			ipv6_addr_copy(&final, &fl.fl6_dst);
			ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
			final_p = &final;
		}
		ipv6_addr_copy(&fl.fl6_src, &ireq6->loc_addr);
		fl.oif = sk->sk_bound_dev_if;
		fl.fl_ip_dport = inet_rsk(req)->rmt_port;
		fl.fl_ip_sport = inet_sk(sk)->sport;

		if (ip6_dst_lookup(sk, &dst, &fl))
			goto out;

		if (final_p)
			ipv6_addr_copy(&fl.fl6_dst, final_p);

		if ((xfrm_lookup(&dst, &fl, sk, 0)) < 0)
			goto out;
	} 

	newsk = dccp_create_openreq_child(sk, req, skb);
	if (newsk == NULL)
		goto out;

	/*
	 * No need to charge this sock to the relevant IPv6 refcnt debug socks
	 * count here, dccp_create_openreq_child now does this for us, see the
	 * comment in that function for the gory details. -acme
	 */

	ip6_dst_store(newsk, dst, NULL);
	newsk->sk_route_caps = dst->dev->features &
		~(NETIF_F_IP_CSUM | NETIF_F_TSO);

	newdp6 = (struct dccp6_sock *)newsk;
	newinet = inet_sk(newsk);
	newinet->pinet6 = &newdp6->inet6;
	newdp = dccp_sk(newsk);
	newnp = inet6_sk(newsk);

	memcpy(newnp, np, sizeof(struct ipv6_pinfo));

	ipv6_addr_copy(&newnp->daddr, &ireq6->rmt_addr);
	ipv6_addr_copy(&newnp->saddr, &ireq6->loc_addr);
	ipv6_addr_copy(&newnp->rcv_saddr, &ireq6->loc_addr);
	newsk->sk_bound_dev_if = ireq6->iif;

	/* Now IPv6 options... 

	   First: no IPv4 options.
	 */
	newinet->opt = NULL;

	/* Clone RX bits */
	newnp->rxopt.all = np->rxopt.all;

	/* Clone pktoptions received with SYN */
	newnp->pktoptions = NULL;
	if (ireq6->pktopts != NULL) {
		newnp->pktoptions = skb_clone(ireq6->pktopts, GFP_ATOMIC);
		kfree_skb(ireq6->pktopts);
		ireq6->pktopts = NULL;
		if (newnp->pktoptions)
			skb_set_owner_r(newnp->pktoptions, newsk);
	}
	newnp->opt	  = NULL;
	newnp->mcast_oif  = inet6_iif(skb);
	newnp->mcast_hops = skb->nh.ipv6h->hop_limit;

	/* Clone native IPv6 options from listening socket (if any)

	   Yes, keeping reference count would be much more clever,
	   but we make one more one thing there: reattach optmem
	   to newsk.
	 */
	if (opt) {
		newnp->opt = ipv6_dup_options(newsk, opt);
		if (opt != np->opt)
			sock_kfree_s(sk, opt, opt->tot_len);
	}

	inet_csk(newsk)->icsk_ext_hdr_len = 0;
	if (newnp->opt)
		inet_csk(newsk)->icsk_ext_hdr_len = (newnp->opt->opt_nflen +
						     newnp->opt->opt_flen);

	dccp_sync_mss(newsk, dst_mtu(dst));

	newinet->daddr = newinet->saddr = newinet->rcv_saddr = LOOPBACK4_IPV6;

	__inet6_hash(&dccp_hashinfo, newsk);
	inet_inherit_port(&dccp_hashinfo, sk, newsk);

	return newsk;

out_overflow:
	NET_INC_STATS_BH(LINUX_MIB_LISTENOVERFLOWS);
out:
	NET_INC_STATS_BH(LINUX_MIB_LISTENDROPS);
	if (opt && opt != np->opt)
		sock_kfree_s(sk, opt, opt->tot_len);
	dst_release(dst);
	return NULL;
}

/* The socket must have it's spinlock held when we get
 * here.
 *
 * We have a potential double-lock case here, so even when
 * doing backlog processing we use the BH locking scheme.
 * This is because we cannot sleep with the original spinlock
 * held.
 */
static int dccp_v6_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sk_buff *opt_skb = NULL;

	/* Imagine: socket is IPv6. IPv4 packet arrives,
	   goes to IPv4 receive handler and backlogged.
	   From backlog it always goes here. Kerboom...
	   Fortunately, dccp_rcv_established and rcv_established
	   handle them correctly, but it is not case with
	   dccp_v6_hnd_req and dccp_v6_ctl_send_reset().   --ANK
	 */

	if (skb->protocol == htons(ETH_P_IP))
		return dccp_v4_do_rcv(sk, skb);

	if (sk_filter(sk, skb, 0))
		goto discard;

	/*
	 *	socket locking is here for SMP purposes as backlog rcv
	 *	is currently called with bh processing disabled.
	 */

	/* Do Stevens' IPV6_PKTOPTIONS.

	   Yes, guys, it is the only place in our code, where we
	   may make it not affecting IPv4.
	   The rest of code is protocol independent,
	   and I do not like idea to uglify IPv4.

	   Actually, all the idea behind IPV6_PKTOPTIONS
	   looks not very well thought. For now we latch
	   options, received in the last packet, enqueued
	   by tcp. Feel free to propose better solution.
	                                       --ANK (980728)
	 */
	if (np->rxopt.all)
		opt_skb = skb_clone(skb, GFP_ATOMIC);

	if (sk->sk_state == DCCP_OPEN) { /* Fast path */
		if (dccp_rcv_established(sk, skb, dccp_hdr(skb), skb->len))
			goto reset;
		return 0;
	}

	if (sk->sk_state == DCCP_LISTEN) { 
		struct sock *nsk = dccp_v6_hnd_req(sk, skb);
		if (!nsk)
			goto discard;

		/*
		 * Queue it on the new socket if the new socket is active,
		 * otherwise we just shortcircuit this and continue with
		 * the new socket..
		 */
 		if(nsk != sk) {
			if (dccp_child_process(sk, nsk, skb))
				goto reset;
			if (opt_skb)
				__kfree_skb(opt_skb);
			return 0;
		}
	}

	if (dccp_rcv_state_process(sk, skb, dccp_hdr(skb), skb->len))
		goto reset;
	return 0;

reset:
	dccp_v6_ctl_send_reset(skb);
discard:
	if (opt_skb)
		__kfree_skb(opt_skb);
	kfree_skb(skb);
	return 0;
}

static int dccp_v6_rcv(struct sk_buff **pskb)
{
	const struct dccp_hdr *dh;
	struct sk_buff *skb = *pskb;
	struct sock *sk;

	/* Step 1: Check header basics: */

	if (dccp_invalid_packet(skb))
		goto discard_it;

	dh = dccp_hdr(skb);

	DCCP_SKB_CB(skb)->dccpd_seq  = dccp_hdr_seq(skb);
	DCCP_SKB_CB(skb)->dccpd_type = dh->dccph_type;

	if (dccp_packet_without_ack(skb))
		DCCP_SKB_CB(skb)->dccpd_ack_seq = DCCP_PKT_WITHOUT_ACK_SEQ;
	else
		DCCP_SKB_CB(skb)->dccpd_ack_seq = dccp_hdr_ack_seq(skb);

	/* Step 2:
	 * 	Look up flow ID in table and get corresponding socket */
	sk = __inet6_lookup(&dccp_hashinfo, &skb->nh.ipv6h->saddr,
			    dh->dccph_sport,
			    &skb->nh.ipv6h->daddr, ntohs(dh->dccph_dport),
			    inet6_iif(skb));
	/* 
	 * Step 2:
	 * 	If no socket ...
	 *		Generate Reset(No Connection) unless P.type == Reset
	 *		Drop packet and return
	 */
	if (sk == NULL)
		goto no_dccp_socket;

	/* 
	 * Step 2:
	 * 	... or S.state == TIMEWAIT,
	 *		Generate Reset(No Connection) unless P.type == Reset
	 *		Drop packet and return
	 */
	       
	if (sk->sk_state == DCCP_TIME_WAIT)
                goto do_time_wait;

	if (!xfrm6_policy_check(sk, XFRM_POLICY_IN, skb))
		goto discard_and_relse;

	return sk_receive_skb(sk, skb) ? -1 : 0;

no_dccp_socket:
	if (!xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb))
		goto discard_it;
	/*
	 * Step 2:
	 *		Generate Reset(No Connection) unless P.type == Reset
	 *		Drop packet and return
	 */
	if (dh->dccph_type != DCCP_PKT_RESET) {
		DCCP_SKB_CB(skb)->dccpd_reset_code =
					DCCP_RESET_CODE_NO_CONNECTION;
		dccp_v6_ctl_send_reset(skb);
	}
discard_it:

	/*
	 *	Discard frame
	 */

	kfree_skb(skb);
	return 0;

discard_and_relse:
	sock_put(sk);
	goto discard_it;

do_time_wait:
	inet_twsk_put((struct inet_timewait_sock *)sk);
	goto no_dccp_socket;
}

static struct inet_connection_sock_af_ops dccp_ipv6_af_ops = {
	.queue_xmit	=	inet6_csk_xmit,
	.send_check	=	dccp_v6_send_check,
	.rebuild_header	=	inet6_sk_rebuild_header,
	.conn_request	=	dccp_v6_conn_request,
	.syn_recv_sock	=	dccp_v6_request_recv_sock,
	.net_header_len	=	sizeof(struct ipv6hdr),
	.setsockopt	=	ipv6_setsockopt,
	.getsockopt	=	ipv6_getsockopt,
	.addr2sockaddr	=	inet6_csk_addr2sockaddr,
	.sockaddr_len	=	sizeof(struct sockaddr_in6)
};

/*
 *	DCCP over IPv4 via INET6 API
 */
static struct inet_connection_sock_af_ops dccp_ipv6_mapped = {
	.queue_xmit	=	ip_queue_xmit,
	.send_check	=	dccp_v4_send_check,
	.rebuild_header	=	inet_sk_rebuild_header,
	.conn_request	=	dccp_v6_conn_request,
	.syn_recv_sock	=	dccp_v6_request_recv_sock,
	.net_header_len	=	sizeof(struct iphdr),
	.setsockopt	=	ipv6_setsockopt,
	.getsockopt	=	ipv6_getsockopt,
	.addr2sockaddr	=	inet6_csk_addr2sockaddr,
	.sockaddr_len	=	sizeof(struct sockaddr_in6)
};

/* NOTE: A lot of things set to zero explicitly by call to
 *       sk_alloc() so need not be done here.
 */
static int dccp_v6_init_sock(struct sock *sk)
{
	int err = dccp_v4_init_sock(sk);

	if (err == 0)
		inet_csk(sk)->icsk_af_ops = &dccp_ipv6_af_ops;

	return err;
}

static int dccp_v6_destroy_sock(struct sock *sk)
{
	dccp_v4_destroy_sock(sk);
	return inet6_destroy_sock(sk);
}

static struct proto dccp_v6_prot = {
	.name			= "DCCPv6",
	.owner			= THIS_MODULE,
	.close			= dccp_close,
	.connect		= dccp_v6_connect,
	.disconnect		= dccp_disconnect,
	.ioctl			= dccp_ioctl,
	.init			= dccp_v6_init_sock,
	.setsockopt		= dccp_setsockopt,
	.getsockopt		= dccp_getsockopt,
	.sendmsg		= dccp_sendmsg,
	.recvmsg		= dccp_recvmsg,
	.backlog_rcv		= dccp_v6_do_rcv,
	.hash			= dccp_v6_hash,
	.unhash			= dccp_unhash,
	.accept			= inet_csk_accept,
	.get_port		= dccp_v6_get_port,
	.shutdown		= dccp_shutdown,
	.destroy		= dccp_v6_destroy_sock,
	.orphan_count		= &dccp_orphan_count,
	.max_header		= MAX_DCCP_HEADER,
	.obj_size		= sizeof(struct dccp6_sock),
	.rsk_prot		= &dccp6_request_sock_ops,
	.twsk_prot		= &dccp6_timewait_sock_ops,
};

static struct inet6_protocol dccp_v6_protocol = {
	.handler	=	dccp_v6_rcv,
	.err_handler	=	dccp_v6_err,
	.flags		=	INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL,
};

static struct proto_ops inet6_dccp_ops = {
	.family		= PF_INET6,
	.owner		= THIS_MODULE,
	.release	= inet6_release,
	.bind		= inet6_bind,
	.connect	= inet_stream_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= inet_accept,
	.getname	= inet6_getname,
	.poll		= dccp_poll,
	.ioctl		= inet6_ioctl,
	.listen		= inet_dccp_listen,
	.shutdown	= inet_shutdown,
	.setsockopt	= sock_common_setsockopt,
	.getsockopt	= sock_common_getsockopt,
	.sendmsg	= inet_sendmsg,
	.recvmsg	= sock_common_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage,
};

static struct inet_protosw dccp_v6_protosw = {
	.type		= SOCK_DCCP,
	.protocol	= IPPROTO_DCCP,
	.prot		= &dccp_v6_prot,
	.ops		= &inet6_dccp_ops,
	.capability	= -1,
	.flags		= INET_PROTOSW_ICSK,
};

static int __init dccp_v6_init(void)
{
	int err = proto_register(&dccp_v6_prot, 1);

	if (err != 0)
		goto out;

	err = inet6_add_protocol(&dccp_v6_protocol, IPPROTO_DCCP);
	if (err != 0)
		goto out_unregister_proto;

	inet6_register_protosw(&dccp_v6_protosw);
out:
	return err;
out_unregister_proto:
	proto_unregister(&dccp_v6_prot);
	goto out;
}

static void __exit dccp_v6_exit(void)
{
	inet6_del_protocol(&dccp_v6_protocol, IPPROTO_DCCP);
	inet6_unregister_protosw(&dccp_v6_protosw);
	proto_unregister(&dccp_v6_prot);
}

module_init(dccp_v6_init);
module_exit(dccp_v6_exit);

/*
 * __stringify doesn't likes enums, so use SOCK_DCCP (6) and IPPROTO_DCCP (33)
 * values directly, Also cover the case where the protocol is not specified,
 * i.e. net-pf-PF_INET6-proto-0-type-SOCK_DCCP
 */
MODULE_ALIAS("net-pf-" __stringify(PF_INET6) "-proto-33-type-6");
MODULE_ALIAS("net-pf-" __stringify(PF_INET6) "-proto-0-type-6");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arnaldo Carvalho de Melo <acme@mandriva.com>");
MODULE_DESCRIPTION("DCCPv6 - Datagram Congestion Controlled Protocol");
