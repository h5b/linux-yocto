/*
 * ip_conntrack_proto_gre.c - Version 3.0 
 *
 * Connection tracking protocol helper module for GRE.
 *
 * GRE is a generic encapsulation protocol, which is generally not very
 * suited for NAT, as it has no protocol-specific part as port numbers.
 *
 * It has an optional key field, which may help us distinguishing two 
 * connections between the same two hosts.
 *
 * GRE is defined in RFC 1701 and RFC 1702, as well as RFC 2784 
 *
 * PPTP is built on top of a modified version of GRE, and has a mandatory
 * field called "CallID", which serves us for the same purpose as the key
 * field in plain GRE.
 *
 * Documentation about PPTP can be found in RFC 2637
 *
 * (C) 2000-2005 by Harald Welte <laforge@gnumonks.org>
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/netfilter.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/list.h>
#include <linux/seq_file.h>

static DEFINE_RWLOCK(ip_ct_gre_lock);
#define ASSERT_READ_LOCK(x)
#define ASSERT_WRITE_LOCK(x)

#include <linux/netfilter_ipv4/listhelp.h>
#include <linux/netfilter_ipv4/ip_conntrack_protocol.h>
#include <linux/netfilter_ipv4/ip_conntrack_helper.h>
#include <linux/netfilter_ipv4/ip_conntrack_core.h>

#include <linux/netfilter_ipv4/ip_conntrack_proto_gre.h>
#include <linux/netfilter_ipv4/ip_conntrack_pptp.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harald Welte <laforge@gnumonks.org>");
MODULE_DESCRIPTION("netfilter connection tracking protocol helper for GRE");

/* shamelessly stolen from ip_conntrack_proto_udp.c */
#define GRE_TIMEOUT		(30*HZ)
#define GRE_STREAM_TIMEOUT	(180*HZ)

#if 0
#define DEBUGP(format, args...)	printk(KERN_DEBUG "%s:%s: " format, __FILE__, __FUNCTION__, ## args)
#define DUMP_TUPLE_GRE(x) printk("%u.%u.%u.%u:0x%x -> %u.%u.%u.%u:0x%x\n", \
			NIPQUAD((x)->src.ip), ntohs((x)->src.u.gre.key), \
			NIPQUAD((x)->dst.ip), ntohs((x)->dst.u.gre.key))
#else
#define DEBUGP(x, args...)
#define DUMP_TUPLE_GRE(x)
#endif
				
/* GRE KEYMAP HANDLING FUNCTIONS */
static LIST_HEAD(gre_keymap_list);

static inline int gre_key_cmpfn(const struct ip_ct_gre_keymap *km,
				const struct ip_conntrack_tuple *t)
{
	return ((km->tuple.src.ip == t->src.ip) &&
		(km->tuple.dst.ip == t->dst.ip) &&
		(km->tuple.dst.protonum == t->dst.protonum) &&
		(km->tuple.dst.u.all == t->dst.u.all));
}

/* look up the source key for a given tuple */
static u_int32_t gre_keymap_lookup(struct ip_conntrack_tuple *t)
{
	struct ip_ct_gre_keymap *km;
	u_int32_t key = 0;

	read_lock_bh(&ip_ct_gre_lock);
	km = LIST_FIND(&gre_keymap_list, gre_key_cmpfn,
			struct ip_ct_gre_keymap *, t);
	if (km)
		key = km->tuple.src.u.gre.key;
	read_unlock_bh(&ip_ct_gre_lock);
	
	DEBUGP("lookup src key 0x%x up key for ", key);
	DUMP_TUPLE_GRE(t);

	return key;
}

/* add a single keymap entry, associate with specified master ct */
int
ip_ct_gre_keymap_add(struct ip_conntrack *ct,
		     struct ip_conntrack_tuple *t, int reply)
{
	struct ip_ct_gre_keymap **exist_km, *km, *old;

	if (!ct->helper || strcmp(ct->helper->name, "pptp")) {
		DEBUGP("refusing to add GRE keymap to non-pptp session\n");
		return -1;
	}

	if (!reply) 
		exist_km = &ct->help.ct_pptp_info.keymap_orig;
	else
		exist_km = &ct->help.ct_pptp_info.keymap_reply;

	if (*exist_km) {
		/* check whether it's a retransmission */
		old = LIST_FIND(&gre_keymap_list, gre_key_cmpfn,
				struct ip_ct_gre_keymap *, t);
		if (old == *exist_km) {
			DEBUGP("retransmission\n");
			return 0;
		}

		DEBUGP("trying to override keymap_%s for ct %p\n", 
			reply? "reply":"orig", ct);
		return -EEXIST;
	}

	km = kmalloc(sizeof(*km), GFP_ATOMIC);
	if (!km)
		return -ENOMEM;

	memcpy(&km->tuple, t, sizeof(*t));
	*exist_km = km;

	DEBUGP("adding new entry %p: ", km);
	DUMP_TUPLE_GRE(&km->tuple);

	write_lock_bh(&ip_ct_gre_lock);
	list_append(&gre_keymap_list, km);
	write_unlock_bh(&ip_ct_gre_lock);

	return 0;
}

/* destroy the keymap entries associated with specified master ct */
void ip_ct_gre_keymap_destroy(struct ip_conntrack *ct)
{
	DEBUGP("entering for ct %p\n", ct);

	if (!ct->helper || strcmp(ct->helper->name, "pptp")) {
		DEBUGP("refusing to destroy GRE keymap to non-pptp session\n");
		return;
	}

	write_lock_bh(&ip_ct_gre_lock);
	if (ct->help.ct_pptp_info.keymap_orig) {
		DEBUGP("removing %p from list\n", 
			ct->help.ct_pptp_info.keymap_orig);
		list_del(&ct->help.ct_pptp_info.keymap_orig->list);
		kfree(ct->help.ct_pptp_info.keymap_orig);
		ct->help.ct_pptp_info.keymap_orig = NULL;
	}
	if (ct->help.ct_pptp_info.keymap_reply) {
		DEBUGP("removing %p from list\n",
			ct->help.ct_pptp_info.keymap_reply);
		list_del(&ct->help.ct_pptp_info.keymap_reply->list);
		kfree(ct->help.ct_pptp_info.keymap_reply);
		ct->help.ct_pptp_info.keymap_reply = NULL;
	}
	write_unlock_bh(&ip_ct_gre_lock);
}


/* PUBLIC CONNTRACK PROTO HELPER FUNCTIONS */

/* invert gre part of tuple */
static int gre_invert_tuple(struct ip_conntrack_tuple *tuple,
			    const struct ip_conntrack_tuple *orig)
{
	tuple->dst.u.gre.key = orig->src.u.gre.key;
	tuple->src.u.gre.key = orig->dst.u.gre.key;

	return 1;
}

/* gre hdr info to tuple */
static int gre_pkt_to_tuple(const struct sk_buff *skb,
			   unsigned int dataoff,
			   struct ip_conntrack_tuple *tuple)
{
	struct gre_hdr_pptp _pgrehdr, *pgrehdr;
	u_int32_t srckey;
	struct gre_hdr _grehdr, *grehdr;

	/* first only delinearize old RFC1701 GRE header */
	grehdr = skb_header_pointer(skb, dataoff, sizeof(_grehdr), &_grehdr);
	if (!grehdr || grehdr->version != GRE_VERSION_PPTP) {
		/* try to behave like "ip_conntrack_proto_generic" */
		tuple->src.u.all = 0;
		tuple->dst.u.all = 0;
		return 1;
	}

	/* PPTP header is variable length, only need up to the call_id field */
	pgrehdr = skb_header_pointer(skb, dataoff, 8, &_pgrehdr);
	if (!pgrehdr)
		return 1;

	if (ntohs(grehdr->protocol) != GRE_PROTOCOL_PPTP) {
		DEBUGP("GRE_VERSION_PPTP but unknown proto\n");
		return 0;
	}

	tuple->dst.u.gre.key = pgrehdr->call_id;
	srckey = gre_keymap_lookup(tuple);
	tuple->src.u.gre.key = srckey;

	return 1;
}

/* print gre part of tuple */
static int gre_print_tuple(struct seq_file *s,
			   const struct ip_conntrack_tuple *tuple)
{
	return seq_printf(s, "srckey=0x%x dstkey=0x%x ", 
			  ntohs(tuple->src.u.gre.key),
			  ntohs(tuple->dst.u.gre.key));
}

/* print private data for conntrack */
static int gre_print_conntrack(struct seq_file *s,
			       const struct ip_conntrack *ct)
{
	return seq_printf(s, "timeout=%u, stream_timeout=%u ",
			  (ct->proto.gre.timeout / HZ),
			  (ct->proto.gre.stream_timeout / HZ));
}

/* Returns verdict for packet, and may modify conntrack */
static int gre_packet(struct ip_conntrack *ct,
		      const struct sk_buff *skb,
		      enum ip_conntrack_info conntrackinfo)
{
	/* If we've seen traffic both ways, this is a GRE connection.
	 * Extend timeout. */
	if (ct->status & IPS_SEEN_REPLY) {
		ip_ct_refresh_acct(ct, conntrackinfo, skb,
				   ct->proto.gre.stream_timeout);
		/* Also, more likely to be important, and not a probe. */
		set_bit(IPS_ASSURED_BIT, &ct->status);
		ip_conntrack_event_cache(IPCT_STATUS, skb);
	} else
		ip_ct_refresh_acct(ct, conntrackinfo, skb,
				   ct->proto.gre.timeout);
	
	return NF_ACCEPT;
}

/* Called when a new connection for this protocol found. */
static int gre_new(struct ip_conntrack *ct,
		   const struct sk_buff *skb)
{ 
	DEBUGP(": ");
	DUMP_TUPLE_GRE(&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

	/* initialize to sane value.  Ideally a conntrack helper
	 * (e.g. in case of pptp) is increasing them */
	ct->proto.gre.stream_timeout = GRE_STREAM_TIMEOUT;
	ct->proto.gre.timeout = GRE_TIMEOUT;

	return 1;
}

/* Called when a conntrack entry has already been removed from the hashes
 * and is about to be deleted from memory */
static void gre_destroy(struct ip_conntrack *ct)
{
	struct ip_conntrack *master = ct->master;
	DEBUGP(" entering\n");

	if (!master)
		DEBUGP("no master !?!\n");
	else
		ip_ct_gre_keymap_destroy(master);
}

/* protocol helper struct */
static struct ip_conntrack_protocol gre = { 
	.proto		 = IPPROTO_GRE,
	.name		 = "gre", 
	.pkt_to_tuple	 = gre_pkt_to_tuple,
	.invert_tuple	 = gre_invert_tuple,
	.print_tuple	 = gre_print_tuple,
	.print_conntrack = gre_print_conntrack,
	.packet		 = gre_packet,
	.new		 = gre_new,
	.destroy	 = gre_destroy,
	.me 		 = THIS_MODULE,
#if defined(CONFIG_IP_NF_CONNTRACK_NETLINK) || \
    defined(CONFIG_IP_NF_CONNTRACK_NETLINK_MODULE)
	.tuple_to_nfattr = ip_ct_port_tuple_to_nfattr,
	.nfattr_to_tuple = ip_ct_port_nfattr_to_tuple,
#endif
};

/* ip_conntrack_proto_gre initialization */
int __init ip_ct_proto_gre_init(void)
{
	return ip_conntrack_protocol_register(&gre);
}

/* This cannot be __exit, as it is invoked from ip_conntrack_helper_pptp.c's
 * init() code on errors.
 */
void ip_ct_proto_gre_fini(void)
{
	struct list_head *pos, *n;

	/* delete all keymap entries */
	write_lock_bh(&ip_ct_gre_lock);
	list_for_each_safe(pos, n, &gre_keymap_list) {
		DEBUGP("deleting keymap %p at module unload time\n", pos);
		list_del(pos);
		kfree(pos);
	}
	write_unlock_bh(&ip_ct_gre_lock);

	ip_conntrack_protocol_unregister(&gre); 
}

EXPORT_SYMBOL(ip_ct_gre_keymap_add);
EXPORT_SYMBOL(ip_ct_gre_keymap_destroy);
