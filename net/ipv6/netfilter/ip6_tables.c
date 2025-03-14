/*
 * Packet matching code.
 *
 * Copyright (C) 1999 Paul `Rusty' Russell & Michael J. Neuling
 * Copyright (C) 2000-2005 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * 19 Jan 2002 Harald Welte <laforge@gnumonks.org>
 * 	- increase module usage count as soon as we have rules inside
 * 	  a table
 * 06 Jun 2002 Andras Kis-Szabo <kisza@sch.bme.hu>
 *      - new extension header parser code
 */

#include <linux/capability.h>
#include <linux/config.h>
#include <linux/in.h>
#include <linux/skbuff.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/netdevice.h>
#include <linux/module.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmpv6.h>
#include <net/ipv6.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>
#include <linux/proc_fs.h>
#include <linux/cpumask.h>

#include <linux/netfilter_ipv6/ip6_tables.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Netfilter Core Team <coreteam@netfilter.org>");
MODULE_DESCRIPTION("IPv6 packet filter");

#define IPV6_HDR_LEN	(sizeof(struct ipv6hdr))
#define IPV6_OPTHDR_LEN	(sizeof(struct ipv6_opt_hdr))

/*#define DEBUG_IP_FIREWALL*/
/*#define DEBUG_ALLOW_ALL*/ /* Useful for remote debugging */
/*#define DEBUG_IP_FIREWALL_USER*/

#ifdef DEBUG_IP_FIREWALL
#define dprintf(format, args...)  printk(format , ## args)
#else
#define dprintf(format, args...)
#endif

#ifdef DEBUG_IP_FIREWALL_USER
#define duprintf(format, args...) printk(format , ## args)
#else
#define duprintf(format, args...)
#endif

#ifdef CONFIG_NETFILTER_DEBUG
#define IP_NF_ASSERT(x)						\
do {								\
	if (!(x))						\
		printk("IP_NF_ASSERT: %s:%s:%u\n",		\
		       __FUNCTION__, __FILE__, __LINE__);	\
} while(0)
#else
#define IP_NF_ASSERT(x)
#endif
#define SMP_ALIGN(x) (((x) + SMP_CACHE_BYTES-1) & ~(SMP_CACHE_BYTES-1))

static DECLARE_MUTEX(ip6t_mutex);

/* Must have mutex */
#define ASSERT_READ_LOCK(x) IP_NF_ASSERT(down_trylock(&ip6t_mutex) != 0)
#define ASSERT_WRITE_LOCK(x) IP_NF_ASSERT(down_trylock(&ip6t_mutex) != 0)
#include <linux/netfilter_ipv4/listhelp.h>

#if 0
/* All the better to debug you with... */
#define static
#define inline
#endif

/*
   We keep a set of rules for each CPU, so we can avoid write-locking
   them in the softirq when updating the counters and therefore
   only need to read-lock in the softirq; doing a write_lock_bh() in user
   context stops packets coming through and allows user context to read
   the counters or update the rules.

   Hence the start of any table is given by get_table() below.  */

/* The table itself */
struct ip6t_table_info
{
	/* Size per table */
	unsigned int size;
	/* Number of entries: FIXME. --RR */
	unsigned int number;
	/* Initial number of entries. Needed for module usage count */
	unsigned int initial_entries;

	/* Entry points and underflows */
	unsigned int hook_entry[NF_IP6_NUMHOOKS];
	unsigned int underflow[NF_IP6_NUMHOOKS];

	/* ip6t_entry tables: one per CPU */
	void *entries[NR_CPUS];
};

static LIST_HEAD(ip6t_target);
static LIST_HEAD(ip6t_match);
static LIST_HEAD(ip6t_tables);
#define SET_COUNTER(c,b,p) do { (c).bcnt = (b); (c).pcnt = (p); } while(0)
#define ADD_COUNTER(c,b,p) do { (c).bcnt += (b); (c).pcnt += (p); } while(0)

#if 0
#define down(x) do { printk("DOWN:%u:" #x "\n", __LINE__); down(x); } while(0)
#define down_interruptible(x) ({ int __r; printk("DOWNi:%u:" #x "\n", __LINE__); __r = down_interruptible(x); if (__r != 0) printk("ABORT-DOWNi:%u\n", __LINE__); __r; })
#define up(x) do { printk("UP:%u:" #x "\n", __LINE__); up(x); } while(0)
#endif

int
ip6_masked_addrcmp(const struct in6_addr *addr1, const struct in6_addr *mask,
                   const struct in6_addr *addr2)
{
	int i;
	for( i = 0; i < 16; i++){
		if((addr1->s6_addr[i] & mask->s6_addr[i]) != 
		   (addr2->s6_addr[i] & mask->s6_addr[i]))
			return 1;
	}
	return 0;
}

/* Check for an extension */
int 
ip6t_ext_hdr(u8 nexthdr)
{
        return ( (nexthdr == IPPROTO_HOPOPTS)   ||
                 (nexthdr == IPPROTO_ROUTING)   ||
                 (nexthdr == IPPROTO_FRAGMENT)  ||
                 (nexthdr == IPPROTO_ESP)       ||
                 (nexthdr == IPPROTO_AH)        ||
                 (nexthdr == IPPROTO_NONE)      ||
                 (nexthdr == IPPROTO_DSTOPTS) );
}

/* Returns whether matches rule or not. */
static inline int
ip6_packet_match(const struct sk_buff *skb,
		 const char *indev,
		 const char *outdev,
		 const struct ip6t_ip6 *ip6info,
		 unsigned int *protoff,
		 int *fragoff)
{
	size_t i;
	unsigned long ret;
	const struct ipv6hdr *ipv6 = skb->nh.ipv6h;

#define FWINV(bool,invflg) ((bool) ^ !!(ip6info->invflags & invflg))

	if (FWINV(ip6_masked_addrcmp(&ipv6->saddr, &ip6info->smsk,
	                             &ip6info->src), IP6T_INV_SRCIP)
	    || FWINV(ip6_masked_addrcmp(&ipv6->daddr, &ip6info->dmsk,
	                                &ip6info->dst), IP6T_INV_DSTIP)) {
		dprintf("Source or dest mismatch.\n");
/*
		dprintf("SRC: %u. Mask: %u. Target: %u.%s\n", ip->saddr,
			ipinfo->smsk.s_addr, ipinfo->src.s_addr,
			ipinfo->invflags & IP6T_INV_SRCIP ? " (INV)" : "");
		dprintf("DST: %u. Mask: %u. Target: %u.%s\n", ip->daddr,
			ipinfo->dmsk.s_addr, ipinfo->dst.s_addr,
			ipinfo->invflags & IP6T_INV_DSTIP ? " (INV)" : "");*/
		return 0;
	}

	/* Look for ifname matches; this should unroll nicely. */
	for (i = 0, ret = 0; i < IFNAMSIZ/sizeof(unsigned long); i++) {
		ret |= (((const unsigned long *)indev)[i]
			^ ((const unsigned long *)ip6info->iniface)[i])
			& ((const unsigned long *)ip6info->iniface_mask)[i];
	}

	if (FWINV(ret != 0, IP6T_INV_VIA_IN)) {
		dprintf("VIA in mismatch (%s vs %s).%s\n",
			indev, ip6info->iniface,
			ip6info->invflags&IP6T_INV_VIA_IN ?" (INV)":"");
		return 0;
	}

	for (i = 0, ret = 0; i < IFNAMSIZ/sizeof(unsigned long); i++) {
		ret |= (((const unsigned long *)outdev)[i]
			^ ((const unsigned long *)ip6info->outiface)[i])
			& ((const unsigned long *)ip6info->outiface_mask)[i];
	}

	if (FWINV(ret != 0, IP6T_INV_VIA_OUT)) {
		dprintf("VIA out mismatch (%s vs %s).%s\n",
			outdev, ip6info->outiface,
			ip6info->invflags&IP6T_INV_VIA_OUT ?" (INV)":"");
		return 0;
	}

/* ... might want to do something with class and flowlabel here ... */

	/* look for the desired protocol header */
	if((ip6info->flags & IP6T_F_PROTO)) {
		int protohdr;
		unsigned short _frag_off;

		protohdr = ipv6_find_hdr(skb, protoff, -1, &_frag_off);
		if (protohdr < 0)
			return 0;

		*fragoff = _frag_off;

		dprintf("Packet protocol %hi ?= %s%hi.\n",
				protohdr, 
				ip6info->invflags & IP6T_INV_PROTO ? "!":"",
				ip6info->proto);

		if (ip6info->proto == protohdr) {
			if(ip6info->invflags & IP6T_INV_PROTO) {
				return 0;
			}
			return 1;
		}

		/* We need match for the '-p all', too! */
		if ((ip6info->proto != 0) &&
			!(ip6info->invflags & IP6T_INV_PROTO))
			return 0;
	}
	return 1;
}

/* should be ip6 safe */
static inline int 
ip6_checkentry(const struct ip6t_ip6 *ipv6)
{
	if (ipv6->flags & ~IP6T_F_MASK) {
		duprintf("Unknown flag bits set: %08X\n",
			 ipv6->flags & ~IP6T_F_MASK);
		return 0;
	}
	if (ipv6->invflags & ~IP6T_INV_MASK) {
		duprintf("Unknown invflag bits set: %08X\n",
			 ipv6->invflags & ~IP6T_INV_MASK);
		return 0;
	}
	return 1;
}

static unsigned int
ip6t_error(struct sk_buff **pskb,
	  const struct net_device *in,
	  const struct net_device *out,
	  unsigned int hooknum,
	  const void *targinfo,
	  void *userinfo)
{
	if (net_ratelimit())
		printk("ip6_tables: error: `%s'\n", (char *)targinfo);

	return NF_DROP;
}

static inline
int do_match(struct ip6t_entry_match *m,
	     const struct sk_buff *skb,
	     const struct net_device *in,
	     const struct net_device *out,
	     int offset,
	     unsigned int protoff,
	     int *hotdrop)
{
	/* Stop iteration if it doesn't match */
	if (!m->u.kernel.match->match(skb, in, out, m->data,
				      offset, protoff, hotdrop))
		return 1;
	else
		return 0;
}

static inline struct ip6t_entry *
get_entry(void *base, unsigned int offset)
{
	return (struct ip6t_entry *)(base + offset);
}

/* Returns one of the generic firewall policies, like NF_ACCEPT. */
unsigned int
ip6t_do_table(struct sk_buff **pskb,
	      unsigned int hook,
	      const struct net_device *in,
	      const struct net_device *out,
	      struct ip6t_table *table,
	      void *userdata)
{
	static const char nulldevname[IFNAMSIZ] __attribute__((aligned(sizeof(long))));
	int offset = 0;
	unsigned int protoff = 0;
	int hotdrop = 0;
	/* Initializing verdict to NF_DROP keeps gcc happy. */
	unsigned int verdict = NF_DROP;
	const char *indev, *outdev;
	void *table_base;
	struct ip6t_entry *e, *back;

	/* Initialization */
	indev = in ? in->name : nulldevname;
	outdev = out ? out->name : nulldevname;
	/* We handle fragments by dealing with the first fragment as
	 * if it was a normal packet.  All other fragments are treated
	 * normally, except that they will NEVER match rules that ask
	 * things we don't know, ie. tcp syn flag or ports).  If the
	 * rule is also a fragment-specific rule, non-fragments won't
	 * match it. */

	read_lock_bh(&table->lock);
	IP_NF_ASSERT(table->valid_hooks & (1 << hook));
	table_base = (void *)table->private->entries[smp_processor_id()];
	e = get_entry(table_base, table->private->hook_entry[hook]);

#ifdef CONFIG_NETFILTER_DEBUG
	/* Check noone else using our table */
	if (((struct ip6t_entry *)table_base)->comefrom != 0xdead57ac
	    && ((struct ip6t_entry *)table_base)->comefrom != 0xeeeeeeec) {
		printk("ASSERT: CPU #%u, %s comefrom(%p) = %X\n",
		       smp_processor_id(),
		       table->name,
		       &((struct ip6t_entry *)table_base)->comefrom,
		       ((struct ip6t_entry *)table_base)->comefrom);
	}
	((struct ip6t_entry *)table_base)->comefrom = 0x57acc001;
#endif

	/* For return from builtin chain */
	back = get_entry(table_base, table->private->underflow[hook]);

	do {
		IP_NF_ASSERT(e);
		IP_NF_ASSERT(back);
		if (ip6_packet_match(*pskb, indev, outdev, &e->ipv6,
			&protoff, &offset)) {
			struct ip6t_entry_target *t;

			if (IP6T_MATCH_ITERATE(e, do_match,
					       *pskb, in, out,
					       offset, protoff, &hotdrop) != 0)
				goto no_match;

			ADD_COUNTER(e->counters,
				    ntohs((*pskb)->nh.ipv6h->payload_len)
				    + IPV6_HDR_LEN,
				    1);

			t = ip6t_get_target(e);
			IP_NF_ASSERT(t->u.kernel.target);
			/* Standard target? */
			if (!t->u.kernel.target->target) {
				int v;

				v = ((struct ip6t_standard_target *)t)->verdict;
				if (v < 0) {
					/* Pop from stack? */
					if (v != IP6T_RETURN) {
						verdict = (unsigned)(-v) - 1;
						break;
					}
					e = back;
					back = get_entry(table_base,
							 back->comefrom);
					continue;
				}
				if (table_base + v != (void *)e + e->next_offset
				    && !(e->ipv6.flags & IP6T_F_GOTO)) {
					/* Save old back ptr in next entry */
					struct ip6t_entry *next
						= (void *)e + e->next_offset;
					next->comefrom
						= (void *)back - table_base;
					/* set back pointer to next entry */
					back = next;
				}

				e = get_entry(table_base, v);
			} else {
				/* Targets which reenter must return
                                   abs. verdicts */
#ifdef CONFIG_NETFILTER_DEBUG
				((struct ip6t_entry *)table_base)->comefrom
					= 0xeeeeeeec;
#endif
				verdict = t->u.kernel.target->target(pskb,
								     in, out,
								     hook,
								     t->data,
								     userdata);

#ifdef CONFIG_NETFILTER_DEBUG
				if (((struct ip6t_entry *)table_base)->comefrom
				    != 0xeeeeeeec
				    && verdict == IP6T_CONTINUE) {
					printk("Target %s reentered!\n",
					       t->u.kernel.target->name);
					verdict = NF_DROP;
				}
				((struct ip6t_entry *)table_base)->comefrom
					= 0x57acc001;
#endif
				if (verdict == IP6T_CONTINUE)
					e = (void *)e + e->next_offset;
				else
					/* Verdict */
					break;
			}
		} else {

		no_match:
			e = (void *)e + e->next_offset;
		}
	} while (!hotdrop);

#ifdef CONFIG_NETFILTER_DEBUG
	((struct ip6t_entry *)table_base)->comefrom = 0xdead57ac;
#endif
	read_unlock_bh(&table->lock);

#ifdef DEBUG_ALLOW_ALL
	return NF_ACCEPT;
#else
	if (hotdrop)
		return NF_DROP;
	else return verdict;
#endif
}

/*
 * These are weird, but module loading must not be done with mutex
 * held (since they will register), and we have to have a single
 * function to use try_then_request_module().
 */

/* Find table by name, grabs mutex & ref.  Returns ERR_PTR() on error. */
static inline struct ip6t_table *find_table_lock(const char *name)
{
	struct ip6t_table *t;

	if (down_interruptible(&ip6t_mutex) != 0)
		return ERR_PTR(-EINTR);

	list_for_each_entry(t, &ip6t_tables, list)
		if (strcmp(t->name, name) == 0 && try_module_get(t->me))
			return t;
	up(&ip6t_mutex);
	return NULL;
}

/* Find match, grabs ref.  Returns ERR_PTR() on error. */
static inline struct ip6t_match *find_match(const char *name, u8 revision)
{
	struct ip6t_match *m;
	int err = 0;

	if (down_interruptible(&ip6t_mutex) != 0)
		return ERR_PTR(-EINTR);

	list_for_each_entry(m, &ip6t_match, list) {
		if (strcmp(m->name, name) == 0) {
			if (m->revision == revision) {
				if (try_module_get(m->me)) {
					up(&ip6t_mutex);
					return m;
				}
			} else
				err = -EPROTOTYPE; /* Found something. */
		}
	}
	up(&ip6t_mutex);
	return ERR_PTR(err);
}

/* Find target, grabs ref.  Returns ERR_PTR() on error. */
static inline struct ip6t_target *find_target(const char *name, u8 revision)
{
	struct ip6t_target *t;
	int err = 0;

	if (down_interruptible(&ip6t_mutex) != 0)
		return ERR_PTR(-EINTR);

	list_for_each_entry(t, &ip6t_target, list) {
		if (strcmp(t->name, name) == 0) {
			if (t->revision == revision) {
				if (try_module_get(t->me)) {
					up(&ip6t_mutex);
					return t;
				}
			} else
				err = -EPROTOTYPE; /* Found something. */
		}
	}
	up(&ip6t_mutex);
	return ERR_PTR(err);
}

struct ip6t_target *ip6t_find_target(const char *name, u8 revision)
{
	struct ip6t_target *target;

	target = try_then_request_module(find_target(name, revision),
					 "ip6t_%s", name);
	if (IS_ERR(target) || !target)
		return NULL;
	return target;
}

static int match_revfn(const char *name, u8 revision, int *bestp)
{
	struct ip6t_match *m;
	int have_rev = 0;

	list_for_each_entry(m, &ip6t_match, list) {
		if (strcmp(m->name, name) == 0) {
			if (m->revision > *bestp)
				*bestp = m->revision;
			if (m->revision == revision)
				have_rev = 1;
		}
	}
	return have_rev;
}

static int target_revfn(const char *name, u8 revision, int *bestp)
{
	struct ip6t_target *t;
	int have_rev = 0;

	list_for_each_entry(t, &ip6t_target, list) {
		if (strcmp(t->name, name) == 0) {
			if (t->revision > *bestp)
				*bestp = t->revision;
			if (t->revision == revision)
				have_rev = 1;
		}
	}
	return have_rev;
}

/* Returns true or fals (if no such extension at all) */
static inline int find_revision(const char *name, u8 revision,
				int (*revfn)(const char *, u8, int *),
				int *err)
{
	int have_rev, best = -1;

	if (down_interruptible(&ip6t_mutex) != 0) {
		*err = -EINTR;
		return 1;
	}
	have_rev = revfn(name, revision, &best);
	up(&ip6t_mutex);

	/* Nothing at all?  Return 0 to try loading module. */
	if (best == -1) {
		*err = -ENOENT;
		return 0;
	}

	*err = best;
	if (!have_rev)
		*err = -EPROTONOSUPPORT;
	return 1;
}


/* All zeroes == unconditional rule. */
static inline int
unconditional(const struct ip6t_ip6 *ipv6)
{
	unsigned int i;

	for (i = 0; i < sizeof(*ipv6); i++)
		if (((char *)ipv6)[i])
			break;

	return (i == sizeof(*ipv6));
}

/* Figures out from what hook each rule can be called: returns 0 if
   there are loops.  Puts hook bitmask in comefrom. */
static int
mark_source_chains(struct ip6t_table_info *newinfo,
		   unsigned int valid_hooks, void *entry0)
{
	unsigned int hook;

	/* No recursion; use packet counter to save back ptrs (reset
	   to 0 as we leave), and comefrom to save source hook bitmask */
	for (hook = 0; hook < NF_IP6_NUMHOOKS; hook++) {
		unsigned int pos = newinfo->hook_entry[hook];
		struct ip6t_entry *e
			= (struct ip6t_entry *)(entry0 + pos);

		if (!(valid_hooks & (1 << hook)))
			continue;

		/* Set initial back pointer. */
		e->counters.pcnt = pos;

		for (;;) {
			struct ip6t_standard_target *t
				= (void *)ip6t_get_target(e);

			if (e->comefrom & (1 << NF_IP6_NUMHOOKS)) {
				printk("iptables: loop hook %u pos %u %08X.\n",
				       hook, pos, e->comefrom);
				return 0;
			}
			e->comefrom
				|= ((1 << hook) | (1 << NF_IP6_NUMHOOKS));

			/* Unconditional return/END. */
			if (e->target_offset == sizeof(struct ip6t_entry)
			    && (strcmp(t->target.u.user.name,
				       IP6T_STANDARD_TARGET) == 0)
			    && t->verdict < 0
			    && unconditional(&e->ipv6)) {
				unsigned int oldpos, size;

				/* Return: backtrack through the last
				   big jump. */
				do {
					e->comefrom ^= (1<<NF_IP6_NUMHOOKS);
#ifdef DEBUG_IP_FIREWALL_USER
					if (e->comefrom
					    & (1 << NF_IP6_NUMHOOKS)) {
						duprintf("Back unset "
							 "on hook %u "
							 "rule %u\n",
							 hook, pos);
					}
#endif
					oldpos = pos;
					pos = e->counters.pcnt;
					e->counters.pcnt = 0;

					/* We're at the start. */
					if (pos == oldpos)
						goto next;

					e = (struct ip6t_entry *)
						(entry0 + pos);
				} while (oldpos == pos + e->next_offset);

				/* Move along one */
				size = e->next_offset;
				e = (struct ip6t_entry *)
					(entry0 + pos + size);
				e->counters.pcnt = pos;
				pos += size;
			} else {
				int newpos = t->verdict;

				if (strcmp(t->target.u.user.name,
					   IP6T_STANDARD_TARGET) == 0
				    && newpos >= 0) {
					/* This a jump; chase it. */
					duprintf("Jump rule %u -> %u\n",
						 pos, newpos);
				} else {
					/* ... this is a fallthru */
					newpos = pos + e->next_offset;
				}
				e = (struct ip6t_entry *)
					(entry0 + newpos);
				e->counters.pcnt = pos;
				pos = newpos;
			}
		}
		next:
		duprintf("Finished chain %u\n", hook);
	}
	return 1;
}

static inline int
cleanup_match(struct ip6t_entry_match *m, unsigned int *i)
{
	if (i && (*i)-- == 0)
		return 1;

	if (m->u.kernel.match->destroy)
		m->u.kernel.match->destroy(m->data,
					   m->u.match_size - sizeof(*m));
	module_put(m->u.kernel.match->me);
	return 0;
}

static inline int
standard_check(const struct ip6t_entry_target *t,
	       unsigned int max_offset)
{
	struct ip6t_standard_target *targ = (void *)t;

	/* Check standard info. */
	if (t->u.target_size
	    != IP6T_ALIGN(sizeof(struct ip6t_standard_target))) {
		duprintf("standard_check: target size %u != %u\n",
			 t->u.target_size,
			 IP6T_ALIGN(sizeof(struct ip6t_standard_target)));
		return 0;
	}

	if (targ->verdict >= 0
	    && targ->verdict > max_offset - sizeof(struct ip6t_entry)) {
		duprintf("ip6t_standard_check: bad verdict (%i)\n",
			 targ->verdict);
		return 0;
	}

	if (targ->verdict < -NF_MAX_VERDICT - 1) {
		duprintf("ip6t_standard_check: bad negative verdict (%i)\n",
			 targ->verdict);
		return 0;
	}
	return 1;
}

static inline int
check_match(struct ip6t_entry_match *m,
	    const char *name,
	    const struct ip6t_ip6 *ipv6,
	    unsigned int hookmask,
	    unsigned int *i)
{
	struct ip6t_match *match;

	match = try_then_request_module(find_match(m->u.user.name,
						   m->u.user.revision),
					"ip6t_%s", m->u.user.name);
	if (IS_ERR(match) || !match) {
		duprintf("check_match: `%s' not found\n", m->u.user.name);
		return match ? PTR_ERR(match) : -ENOENT;
	}
	m->u.kernel.match = match;

	if (m->u.kernel.match->checkentry
	    && !m->u.kernel.match->checkentry(name, ipv6, m->data,
					      m->u.match_size - sizeof(*m),
					      hookmask)) {
		module_put(m->u.kernel.match->me);
		duprintf("ip_tables: check failed for `%s'.\n",
			 m->u.kernel.match->name);
		return -EINVAL;
	}

	(*i)++;
	return 0;
}

static struct ip6t_target ip6t_standard_target;

static inline int
check_entry(struct ip6t_entry *e, const char *name, unsigned int size,
	    unsigned int *i)
{
	struct ip6t_entry_target *t;
	struct ip6t_target *target;
	int ret;
	unsigned int j;

	if (!ip6_checkentry(&e->ipv6)) {
		duprintf("ip_tables: ip check failed %p %s.\n", e, name);
		return -EINVAL;
	}

	j = 0;
	ret = IP6T_MATCH_ITERATE(e, check_match, name, &e->ipv6, e->comefrom, &j);
	if (ret != 0)
		goto cleanup_matches;

	t = ip6t_get_target(e);
	target = try_then_request_module(find_target(t->u.user.name,
						     t->u.user.revision),
					 "ip6t_%s", t->u.user.name);
	if (IS_ERR(target) || !target) {
		duprintf("check_entry: `%s' not found\n", t->u.user.name);
		ret = target ? PTR_ERR(target) : -ENOENT;
		goto cleanup_matches;
	}
	t->u.kernel.target = target;

	if (t->u.kernel.target == &ip6t_standard_target) {
		if (!standard_check(t, size)) {
			ret = -EINVAL;
			goto cleanup_matches;
		}
	} else if (t->u.kernel.target->checkentry
		   && !t->u.kernel.target->checkentry(name, e, t->data,
						      t->u.target_size
						      - sizeof(*t),
						      e->comefrom)) {
		module_put(t->u.kernel.target->me);
		duprintf("ip_tables: check failed for `%s'.\n",
			 t->u.kernel.target->name);
		ret = -EINVAL;
		goto cleanup_matches;
	}

	(*i)++;
	return 0;

 cleanup_matches:
	IP6T_MATCH_ITERATE(e, cleanup_match, &j);
	return ret;
}

static inline int
check_entry_size_and_hooks(struct ip6t_entry *e,
			   struct ip6t_table_info *newinfo,
			   unsigned char *base,
			   unsigned char *limit,
			   const unsigned int *hook_entries,
			   const unsigned int *underflows,
			   unsigned int *i)
{
	unsigned int h;

	if ((unsigned long)e % __alignof__(struct ip6t_entry) != 0
	    || (unsigned char *)e + sizeof(struct ip6t_entry) >= limit) {
		duprintf("Bad offset %p\n", e);
		return -EINVAL;
	}

	if (e->next_offset
	    < sizeof(struct ip6t_entry) + sizeof(struct ip6t_entry_target)) {
		duprintf("checking: element %p size %u\n",
			 e, e->next_offset);
		return -EINVAL;
	}

	/* Check hooks & underflows */
	for (h = 0; h < NF_IP6_NUMHOOKS; h++) {
		if ((unsigned char *)e - base == hook_entries[h])
			newinfo->hook_entry[h] = hook_entries[h];
		if ((unsigned char *)e - base == underflows[h])
			newinfo->underflow[h] = underflows[h];
	}

	/* FIXME: underflows must be unconditional, standard verdicts
           < 0 (not IP6T_RETURN). --RR */

	/* Clear counters and comefrom */
	e->counters = ((struct ip6t_counters) { 0, 0 });
	e->comefrom = 0;

	(*i)++;
	return 0;
}

static inline int
cleanup_entry(struct ip6t_entry *e, unsigned int *i)
{
	struct ip6t_entry_target *t;

	if (i && (*i)-- == 0)
		return 1;

	/* Cleanup all matches */
	IP6T_MATCH_ITERATE(e, cleanup_match, NULL);
	t = ip6t_get_target(e);
	if (t->u.kernel.target->destroy)
		t->u.kernel.target->destroy(t->data,
					    t->u.target_size - sizeof(*t));
	module_put(t->u.kernel.target->me);
	return 0;
}

/* Checks and translates the user-supplied table segment (held in
   newinfo) */
static int
translate_table(const char *name,
		unsigned int valid_hooks,
		struct ip6t_table_info *newinfo,
		void *entry0,
		unsigned int size,
		unsigned int number,
		const unsigned int *hook_entries,
		const unsigned int *underflows)
{
	unsigned int i;
	int ret;

	newinfo->size = size;
	newinfo->number = number;

	/* Init all hooks to impossible value. */
	for (i = 0; i < NF_IP6_NUMHOOKS; i++) {
		newinfo->hook_entry[i] = 0xFFFFFFFF;
		newinfo->underflow[i] = 0xFFFFFFFF;
	}

	duprintf("translate_table: size %u\n", newinfo->size);
	i = 0;
	/* Walk through entries, checking offsets. */
	ret = IP6T_ENTRY_ITERATE(entry0, newinfo->size,
				check_entry_size_and_hooks,
				newinfo,
				entry0,
				entry0 + size,
				hook_entries, underflows, &i);
	if (ret != 0)
		return ret;

	if (i != number) {
		duprintf("translate_table: %u not %u entries\n",
			 i, number);
		return -EINVAL;
	}

	/* Check hooks all assigned */
	for (i = 0; i < NF_IP6_NUMHOOKS; i++) {
		/* Only hooks which are valid */
		if (!(valid_hooks & (1 << i)))
			continue;
		if (newinfo->hook_entry[i] == 0xFFFFFFFF) {
			duprintf("Invalid hook entry %u %u\n",
				 i, hook_entries[i]);
			return -EINVAL;
		}
		if (newinfo->underflow[i] == 0xFFFFFFFF) {
			duprintf("Invalid underflow %u %u\n",
				 i, underflows[i]);
			return -EINVAL;
		}
	}

	if (!mark_source_chains(newinfo, valid_hooks, entry0))
		return -ELOOP;

	/* Finally, each sanity check must pass */
	i = 0;
	ret = IP6T_ENTRY_ITERATE(entry0, newinfo->size,
				check_entry, name, size, &i);

	if (ret != 0) {
		IP6T_ENTRY_ITERATE(entry0, newinfo->size,
				  cleanup_entry, &i);
		return ret;
	}

	/* And one copy for every other CPU */
	for_each_cpu(i) {
		if (newinfo->entries[i] && newinfo->entries[i] != entry0)
			memcpy(newinfo->entries[i], entry0, newinfo->size);
	}

	return ret;
}

static struct ip6t_table_info *
replace_table(struct ip6t_table *table,
	      unsigned int num_counters,
	      struct ip6t_table_info *newinfo,
	      int *error)
{
	struct ip6t_table_info *oldinfo;

#ifdef CONFIG_NETFILTER_DEBUG
	{
		int cpu;

		for_each_cpu(cpu) {
			struct ip6t_entry *table_base = newinfo->entries[cpu];
			if (table_base)
				table_base->comefrom = 0xdead57ac;
		}
	}
#endif

	/* Do the substitution. */
	write_lock_bh(&table->lock);
	/* Check inside lock: is the old number correct? */
	if (num_counters != table->private->number) {
		duprintf("num_counters != table->private->number (%u/%u)\n",
			 num_counters, table->private->number);
		write_unlock_bh(&table->lock);
		*error = -EAGAIN;
		return NULL;
	}
	oldinfo = table->private;
	table->private = newinfo;
	newinfo->initial_entries = oldinfo->initial_entries;
	write_unlock_bh(&table->lock);

	return oldinfo;
}

/* Gets counters. */
static inline int
add_entry_to_counter(const struct ip6t_entry *e,
		     struct ip6t_counters total[],
		     unsigned int *i)
{
	ADD_COUNTER(total[*i], e->counters.bcnt, e->counters.pcnt);

	(*i)++;
	return 0;
}

static inline int
set_entry_to_counter(const struct ip6t_entry *e,
		     struct ip6t_counters total[],
		     unsigned int *i)
{
	SET_COUNTER(total[*i], e->counters.bcnt, e->counters.pcnt);

	(*i)++;
	return 0;
}

static void
get_counters(const struct ip6t_table_info *t,
	     struct ip6t_counters counters[])
{
	unsigned int cpu;
	unsigned int i;
	unsigned int curcpu;

	/* Instead of clearing (by a previous call to memset())
	 * the counters and using adds, we set the counters
	 * with data used by 'current' CPU
	 * We dont care about preemption here.
	 */
	curcpu = raw_smp_processor_id();

	i = 0;
	IP6T_ENTRY_ITERATE(t->entries[curcpu],
			   t->size,
			   set_entry_to_counter,
			   counters,
			   &i);

	for_each_cpu(cpu) {
		if (cpu == curcpu)
			continue;
		i = 0;
		IP6T_ENTRY_ITERATE(t->entries[cpu],
				  t->size,
				  add_entry_to_counter,
				  counters,
				  &i);
	}
}

static int
copy_entries_to_user(unsigned int total_size,
		     struct ip6t_table *table,
		     void __user *userptr)
{
	unsigned int off, num, countersize;
	struct ip6t_entry *e;
	struct ip6t_counters *counters;
	int ret = 0;
	void *loc_cpu_entry;

	/* We need atomic snapshot of counters: rest doesn't change
	   (other than comefrom, which userspace doesn't care
	   about). */
	countersize = sizeof(struct ip6t_counters) * table->private->number;
	counters = vmalloc(countersize);

	if (counters == NULL)
		return -ENOMEM;

	/* First, sum counters... */
	write_lock_bh(&table->lock);
	get_counters(table->private, counters);
	write_unlock_bh(&table->lock);

	/* choose the copy that is on ourc node/cpu */
	loc_cpu_entry = table->private->entries[raw_smp_processor_id()];
	if (copy_to_user(userptr, loc_cpu_entry, total_size) != 0) {
		ret = -EFAULT;
		goto free_counters;
	}

	/* FIXME: use iterator macros --RR */
	/* ... then go back and fix counters and names */
	for (off = 0, num = 0; off < total_size; off += e->next_offset, num++){
		unsigned int i;
		struct ip6t_entry_match *m;
		struct ip6t_entry_target *t;

		e = (struct ip6t_entry *)(loc_cpu_entry + off);
		if (copy_to_user(userptr + off
				 + offsetof(struct ip6t_entry, counters),
				 &counters[num],
				 sizeof(counters[num])) != 0) {
			ret = -EFAULT;
			goto free_counters;
		}

		for (i = sizeof(struct ip6t_entry);
		     i < e->target_offset;
		     i += m->u.match_size) {
			m = (void *)e + i;

			if (copy_to_user(userptr + off + i
					 + offsetof(struct ip6t_entry_match,
						    u.user.name),
					 m->u.kernel.match->name,
					 strlen(m->u.kernel.match->name)+1)
			    != 0) {
				ret = -EFAULT;
				goto free_counters;
			}
		}

		t = ip6t_get_target(e);
		if (copy_to_user(userptr + off + e->target_offset
				 + offsetof(struct ip6t_entry_target,
					    u.user.name),
				 t->u.kernel.target->name,
				 strlen(t->u.kernel.target->name)+1) != 0) {
			ret = -EFAULT;
			goto free_counters;
		}
	}

 free_counters:
	vfree(counters);
	return ret;
}

static int
get_entries(const struct ip6t_get_entries *entries,
	    struct ip6t_get_entries __user *uptr)
{
	int ret;
	struct ip6t_table *t;

	t = find_table_lock(entries->name);
	if (t && !IS_ERR(t)) {
		duprintf("t->private->number = %u\n",
			 t->private->number);
		if (entries->size == t->private->size)
			ret = copy_entries_to_user(t->private->size,
						   t, uptr->entrytable);
		else {
			duprintf("get_entries: I've got %u not %u!\n",
				 t->private->size,
				 entries->size);
			ret = -EINVAL;
		}
		module_put(t->me);
		up(&ip6t_mutex);
	} else
		ret = t ? PTR_ERR(t) : -ENOENT;

	return ret;
}

static void free_table_info(struct ip6t_table_info *info)
{
	int cpu;
	for_each_cpu(cpu) {
		if (info->size <= PAGE_SIZE)
			kfree(info->entries[cpu]);
		else
			vfree(info->entries[cpu]);
	}
	kfree(info);
}

static struct ip6t_table_info *alloc_table_info(unsigned int size)
{
	struct ip6t_table_info *newinfo;
	int cpu;

	newinfo = kzalloc(sizeof(struct ip6t_table_info), GFP_KERNEL);
	if (!newinfo)
		return NULL;

	newinfo->size = size;

	for_each_cpu(cpu) {
		if (size <= PAGE_SIZE)
			newinfo->entries[cpu] = kmalloc_node(size,
							GFP_KERNEL,
							cpu_to_node(cpu));
		else
			newinfo->entries[cpu] = vmalloc_node(size,
							     cpu_to_node(cpu));
		if (newinfo->entries[cpu] == NULL) {
			free_table_info(newinfo);
			return NULL;
		}
	}

	return newinfo;
}

static int
do_replace(void __user *user, unsigned int len)
{
	int ret;
	struct ip6t_replace tmp;
	struct ip6t_table *t;
	struct ip6t_table_info *newinfo, *oldinfo;
	struct ip6t_counters *counters;
	void *loc_cpu_entry, *loc_cpu_old_entry;

	if (copy_from_user(&tmp, user, sizeof(tmp)) != 0)
		return -EFAULT;

	/* Pedantry: prevent them from hitting BUG() in vmalloc.c --RR */
	if ((SMP_ALIGN(tmp.size) >> PAGE_SHIFT) + 2 > num_physpages)
		return -ENOMEM;

	newinfo = alloc_table_info(tmp.size);
	if (!newinfo)
		return -ENOMEM;

	/* choose the copy that is on our node/cpu */
	loc_cpu_entry = newinfo->entries[raw_smp_processor_id()];
	if (copy_from_user(loc_cpu_entry, user + sizeof(tmp),
			   tmp.size) != 0) {
		ret = -EFAULT;
		goto free_newinfo;
	}

	counters = vmalloc(tmp.num_counters * sizeof(struct ip6t_counters));
	if (!counters) {
		ret = -ENOMEM;
		goto free_newinfo;
	}

	ret = translate_table(tmp.name, tmp.valid_hooks,
			      newinfo, loc_cpu_entry, tmp.size, tmp.num_entries,
			      tmp.hook_entry, tmp.underflow);
	if (ret != 0)
		goto free_newinfo_counters;

	duprintf("ip_tables: Translated table\n");

	t = try_then_request_module(find_table_lock(tmp.name),
				    "ip6table_%s", tmp.name);
	if (!t || IS_ERR(t)) {
		ret = t ? PTR_ERR(t) : -ENOENT;
		goto free_newinfo_counters_untrans;
	}

	/* You lied! */
	if (tmp.valid_hooks != t->valid_hooks) {
		duprintf("Valid hook crap: %08X vs %08X\n",
			 tmp.valid_hooks, t->valid_hooks);
		ret = -EINVAL;
		goto put_module;
	}

	oldinfo = replace_table(t, tmp.num_counters, newinfo, &ret);
	if (!oldinfo)
		goto put_module;

	/* Update module usage count based on number of rules */
	duprintf("do_replace: oldnum=%u, initnum=%u, newnum=%u\n",
		oldinfo->number, oldinfo->initial_entries, newinfo->number);
	if ((oldinfo->number > oldinfo->initial_entries) || 
	    (newinfo->number <= oldinfo->initial_entries)) 
		module_put(t->me);
	if ((oldinfo->number > oldinfo->initial_entries) &&
	    (newinfo->number <= oldinfo->initial_entries))
		module_put(t->me);

	/* Get the old counters. */
	get_counters(oldinfo, counters);
	/* Decrease module usage counts and free resource */
	loc_cpu_old_entry = oldinfo->entries[raw_smp_processor_id()];
	IP6T_ENTRY_ITERATE(loc_cpu_old_entry, oldinfo->size, cleanup_entry,NULL);
	free_table_info(oldinfo);
	if (copy_to_user(tmp.counters, counters,
			 sizeof(struct ip6t_counters) * tmp.num_counters) != 0)
		ret = -EFAULT;
	vfree(counters);
	up(&ip6t_mutex);
	return ret;

 put_module:
	module_put(t->me);
	up(&ip6t_mutex);
 free_newinfo_counters_untrans:
	IP6T_ENTRY_ITERATE(loc_cpu_entry, newinfo->size, cleanup_entry,NULL);
 free_newinfo_counters:
	vfree(counters);
 free_newinfo:
	free_table_info(newinfo);
	return ret;
}

/* We're lazy, and add to the first CPU; overflow works its fey magic
 * and everything is OK. */
static inline int
add_counter_to_entry(struct ip6t_entry *e,
		     const struct ip6t_counters addme[],
		     unsigned int *i)
{
#if 0
	duprintf("add_counter: Entry %u %lu/%lu + %lu/%lu\n",
		 *i,
		 (long unsigned int)e->counters.pcnt,
		 (long unsigned int)e->counters.bcnt,
		 (long unsigned int)addme[*i].pcnt,
		 (long unsigned int)addme[*i].bcnt);
#endif

	ADD_COUNTER(e->counters, addme[*i].bcnt, addme[*i].pcnt);

	(*i)++;
	return 0;
}

static int
do_add_counters(void __user *user, unsigned int len)
{
	unsigned int i;
	struct ip6t_counters_info tmp, *paddc;
	struct ip6t_table *t;
	int ret = 0;
	void *loc_cpu_entry;

	if (copy_from_user(&tmp, user, sizeof(tmp)) != 0)
		return -EFAULT;

	if (len != sizeof(tmp) + tmp.num_counters*sizeof(struct ip6t_counters))
		return -EINVAL;

	paddc = vmalloc(len);
	if (!paddc)
		return -ENOMEM;

	if (copy_from_user(paddc, user, len) != 0) {
		ret = -EFAULT;
		goto free;
	}

	t = find_table_lock(tmp.name);
	if (!t || IS_ERR(t)) {
		ret = t ? PTR_ERR(t) : -ENOENT;
		goto free;
	}

	write_lock_bh(&t->lock);
	if (t->private->number != paddc->num_counters) {
		ret = -EINVAL;
		goto unlock_up_free;
	}

	i = 0;
	/* Choose the copy that is on our node */
	loc_cpu_entry = t->private->entries[smp_processor_id()];
	IP6T_ENTRY_ITERATE(loc_cpu_entry,
			  t->private->size,
			  add_counter_to_entry,
			  paddc->counters,
			  &i);
 unlock_up_free:
	write_unlock_bh(&t->lock);
	up(&ip6t_mutex);
	module_put(t->me);
 free:
	vfree(paddc);

	return ret;
}

static int
do_ip6t_set_ctl(struct sock *sk, int cmd, void __user *user, unsigned int len)
{
	int ret;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	switch (cmd) {
	case IP6T_SO_SET_REPLACE:
		ret = do_replace(user, len);
		break;

	case IP6T_SO_SET_ADD_COUNTERS:
		ret = do_add_counters(user, len);
		break;

	default:
		duprintf("do_ip6t_set_ctl:  unknown request %i\n", cmd);
		ret = -EINVAL;
	}

	return ret;
}

static int
do_ip6t_get_ctl(struct sock *sk, int cmd, void __user *user, int *len)
{
	int ret;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	switch (cmd) {
	case IP6T_SO_GET_INFO: {
		char name[IP6T_TABLE_MAXNAMELEN];
		struct ip6t_table *t;

		if (*len != sizeof(struct ip6t_getinfo)) {
			duprintf("length %u != %u\n", *len,
				 sizeof(struct ip6t_getinfo));
			ret = -EINVAL;
			break;
		}

		if (copy_from_user(name, user, sizeof(name)) != 0) {
			ret = -EFAULT;
			break;
		}
		name[IP6T_TABLE_MAXNAMELEN-1] = '\0';

		t = try_then_request_module(find_table_lock(name),
					    "ip6table_%s", name);
		if (t && !IS_ERR(t)) {
			struct ip6t_getinfo info;

			info.valid_hooks = t->valid_hooks;
			memcpy(info.hook_entry, t->private->hook_entry,
			       sizeof(info.hook_entry));
			memcpy(info.underflow, t->private->underflow,
			       sizeof(info.underflow));
			info.num_entries = t->private->number;
			info.size = t->private->size;
			memcpy(info.name, name, sizeof(info.name));

			if (copy_to_user(user, &info, *len) != 0)
				ret = -EFAULT;
			else
				ret = 0;
			up(&ip6t_mutex);
			module_put(t->me);
		} else
			ret = t ? PTR_ERR(t) : -ENOENT;
	}
	break;

	case IP6T_SO_GET_ENTRIES: {
		struct ip6t_get_entries get;

		if (*len < sizeof(get)) {
			duprintf("get_entries: %u < %u\n", *len, sizeof(get));
			ret = -EINVAL;
		} else if (copy_from_user(&get, user, sizeof(get)) != 0) {
			ret = -EFAULT;
		} else if (*len != sizeof(struct ip6t_get_entries) + get.size) {
			duprintf("get_entries: %u != %u\n", *len,
				 sizeof(struct ip6t_get_entries) + get.size);
			ret = -EINVAL;
		} else
			ret = get_entries(&get, user);
		break;
	}

	case IP6T_SO_GET_REVISION_MATCH:
	case IP6T_SO_GET_REVISION_TARGET: {
		struct ip6t_get_revision rev;
		int (*revfn)(const char *, u8, int *);

		if (*len != sizeof(rev)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(&rev, user, sizeof(rev)) != 0) {
			ret = -EFAULT;
			break;
		}

		if (cmd == IP6T_SO_GET_REVISION_TARGET)
			revfn = target_revfn;
		else
			revfn = match_revfn;

		try_then_request_module(find_revision(rev.name, rev.revision,
						      revfn, &ret),
					"ip6t_%s", rev.name);
		break;
	}

	default:
		duprintf("do_ip6t_get_ctl: unknown request %i\n", cmd);
		ret = -EINVAL;
	}

	return ret;
}

/* Registration hooks for targets. */
int
ip6t_register_target(struct ip6t_target *target)
{
	int ret;

	ret = down_interruptible(&ip6t_mutex);
	if (ret != 0)
		return ret;
	list_add(&target->list, &ip6t_target);
	up(&ip6t_mutex);
	return ret;
}

void
ip6t_unregister_target(struct ip6t_target *target)
{
	down(&ip6t_mutex);
	LIST_DELETE(&ip6t_target, target);
	up(&ip6t_mutex);
}

int
ip6t_register_match(struct ip6t_match *match)
{
	int ret;

	ret = down_interruptible(&ip6t_mutex);
	if (ret != 0)
		return ret;

	list_add(&match->list, &ip6t_match);
	up(&ip6t_mutex);

	return ret;
}

void
ip6t_unregister_match(struct ip6t_match *match)
{
	down(&ip6t_mutex);
	LIST_DELETE(&ip6t_match, match);
	up(&ip6t_mutex);
}

int ip6t_register_table(struct ip6t_table *table,
			const struct ip6t_replace *repl)
{
	int ret;
	struct ip6t_table_info *newinfo;
	static struct ip6t_table_info bootstrap
		= { 0, 0, 0, { 0 }, { 0 }, { } };
	void *loc_cpu_entry;

	newinfo = alloc_table_info(repl->size);
	if (!newinfo)
		return -ENOMEM;

	/* choose the copy on our node/cpu */
	loc_cpu_entry = newinfo->entries[raw_smp_processor_id()];
	memcpy(loc_cpu_entry, repl->entries, repl->size);

	ret = translate_table(table->name, table->valid_hooks,
			      newinfo, loc_cpu_entry, repl->size,
			      repl->num_entries,
			      repl->hook_entry,
			      repl->underflow);
	if (ret != 0) {
		free_table_info(newinfo);
		return ret;
	}

	ret = down_interruptible(&ip6t_mutex);
	if (ret != 0) {
		free_table_info(newinfo);
		return ret;
	}

	/* Don't autoload: we'd eat our tail... */
	if (list_named_find(&ip6t_tables, table->name)) {
		ret = -EEXIST;
		goto free_unlock;
	}

	/* Simplifies replace_table code. */
	table->private = &bootstrap;
	if (!replace_table(table, 0, newinfo, &ret))
		goto free_unlock;

	duprintf("table->private->number = %u\n",
		 table->private->number);

	/* save number of initial entries */
	table->private->initial_entries = table->private->number;

	rwlock_init(&table->lock);
	list_prepend(&ip6t_tables, table);

 unlock:
	up(&ip6t_mutex);
	return ret;

 free_unlock:
	free_table_info(newinfo);
	goto unlock;
}

void ip6t_unregister_table(struct ip6t_table *table)
{
	void *loc_cpu_entry;

	down(&ip6t_mutex);
	LIST_DELETE(&ip6t_tables, table);
	up(&ip6t_mutex);

	/* Decrease module usage counts and free resources */
	loc_cpu_entry = table->private->entries[raw_smp_processor_id()];
	IP6T_ENTRY_ITERATE(loc_cpu_entry, table->private->size,
			  cleanup_entry, NULL);
	free_table_info(table->private);
}

/* Returns 1 if the port is matched by the range, 0 otherwise */
static inline int
port_match(u_int16_t min, u_int16_t max, u_int16_t port, int invert)
{
	int ret;

	ret = (port >= min && port <= max) ^ invert;
	return ret;
}

static int
tcp_find_option(u_int8_t option,
		const struct sk_buff *skb,
		unsigned int tcpoff,
		unsigned int optlen,
		int invert,
		int *hotdrop)
{
	/* tcp.doff is only 4 bits, ie. max 15 * 4 bytes */
	u_int8_t _opt[60 - sizeof(struct tcphdr)], *op;
	unsigned int i;

	duprintf("tcp_match: finding option\n");
	if (!optlen)
		return invert;
	/* If we don't have the whole header, drop packet. */
	op = skb_header_pointer(skb, tcpoff + sizeof(struct tcphdr), optlen,
				_opt);
	if (op == NULL) {
		*hotdrop = 1;
		return 0;
	}

	for (i = 0; i < optlen; ) {
		if (op[i] == option) return !invert;
		if (op[i] < 2) i++;
		else i += op[i+1]?:1;
	}

	return invert;
}

static int
tcp_match(const struct sk_buff *skb,
	  const struct net_device *in,
	  const struct net_device *out,
	  const void *matchinfo,
	  int offset,
	  unsigned int protoff,
	  int *hotdrop)
{
	struct tcphdr _tcph, *th;
	const struct ip6t_tcp *tcpinfo = matchinfo;

	if (offset) {
		/* To quote Alan:

		   Don't allow a fragment of TCP 8 bytes in. Nobody normal
		   causes this. Its a cracker trying to break in by doing a
		   flag overwrite to pass the direction checks.
		*/
		if (offset == 1) {
			duprintf("Dropping evil TCP offset=1 frag.\n");
			*hotdrop = 1;
		}
		/* Must not be a fragment. */
		return 0;
	}

#define FWINVTCP(bool,invflg) ((bool) ^ !!(tcpinfo->invflags & invflg))

	th = skb_header_pointer(skb, protoff, sizeof(_tcph), &_tcph);
	if (th == NULL) {
		/* We've been asked to examine this packet, and we
		   can't.  Hence, no choice but to drop. */
		duprintf("Dropping evil TCP offset=0 tinygram.\n");
		*hotdrop = 1;
		return 0;
	}

	if (!port_match(tcpinfo->spts[0], tcpinfo->spts[1],
			ntohs(th->source),
			!!(tcpinfo->invflags & IP6T_TCP_INV_SRCPT)))
		return 0;
	if (!port_match(tcpinfo->dpts[0], tcpinfo->dpts[1],
			ntohs(th->dest),
			!!(tcpinfo->invflags & IP6T_TCP_INV_DSTPT)))
		return 0;
	if (!FWINVTCP((((unsigned char *)th)[13] & tcpinfo->flg_mask)
		      == tcpinfo->flg_cmp,
		      IP6T_TCP_INV_FLAGS))
		return 0;
	if (tcpinfo->option) {
		if (th->doff * 4 < sizeof(_tcph)) {
			*hotdrop = 1;
			return 0;
		}
		if (!tcp_find_option(tcpinfo->option, skb, protoff,
				     th->doff*4 - sizeof(*th),
				     tcpinfo->invflags & IP6T_TCP_INV_OPTION,
				     hotdrop))
			return 0;
	}
	return 1;
}

/* Called when user tries to insert an entry of this type. */
static int
tcp_checkentry(const char *tablename,
	       const struct ip6t_ip6 *ipv6,
	       void *matchinfo,
	       unsigned int matchsize,
	       unsigned int hook_mask)
{
	const struct ip6t_tcp *tcpinfo = matchinfo;

	/* Must specify proto == TCP, and no unknown invflags */
	return ipv6->proto == IPPROTO_TCP
		&& !(ipv6->invflags & IP6T_INV_PROTO)
		&& matchsize == IP6T_ALIGN(sizeof(struct ip6t_tcp))
		&& !(tcpinfo->invflags & ~IP6T_TCP_INV_MASK);
}

static int
udp_match(const struct sk_buff *skb,
	  const struct net_device *in,
	  const struct net_device *out,
	  const void *matchinfo,
	  int offset,
	  unsigned int protoff,
	  int *hotdrop)
{
	struct udphdr _udph, *uh;
	const struct ip6t_udp *udpinfo = matchinfo;

	/* Must not be a fragment. */
	if (offset)
		return 0;

	uh = skb_header_pointer(skb, protoff, sizeof(_udph), &_udph);
	if (uh == NULL) {
		/* We've been asked to examine this packet, and we
		   can't.  Hence, no choice but to drop. */
		duprintf("Dropping evil UDP tinygram.\n");
		*hotdrop = 1;
		return 0;
	}

	return port_match(udpinfo->spts[0], udpinfo->spts[1],
			  ntohs(uh->source),
			  !!(udpinfo->invflags & IP6T_UDP_INV_SRCPT))
		&& port_match(udpinfo->dpts[0], udpinfo->dpts[1],
			      ntohs(uh->dest),
			      !!(udpinfo->invflags & IP6T_UDP_INV_DSTPT));
}

/* Called when user tries to insert an entry of this type. */
static int
udp_checkentry(const char *tablename,
	       const struct ip6t_ip6 *ipv6,
	       void *matchinfo,
	       unsigned int matchinfosize,
	       unsigned int hook_mask)
{
	const struct ip6t_udp *udpinfo = matchinfo;

	/* Must specify proto == UDP, and no unknown invflags */
	if (ipv6->proto != IPPROTO_UDP || (ipv6->invflags & IP6T_INV_PROTO)) {
		duprintf("ip6t_udp: Protocol %u != %u\n", ipv6->proto,
			 IPPROTO_UDP);
		return 0;
	}
	if (matchinfosize != IP6T_ALIGN(sizeof(struct ip6t_udp))) {
		duprintf("ip6t_udp: matchsize %u != %u\n",
			 matchinfosize, IP6T_ALIGN(sizeof(struct ip6t_udp)));
		return 0;
	}
	if (udpinfo->invflags & ~IP6T_UDP_INV_MASK) {
		duprintf("ip6t_udp: unknown flags %X\n",
			 udpinfo->invflags);
		return 0;
	}

	return 1;
}

/* Returns 1 if the type and code is matched by the range, 0 otherwise */
static inline int
icmp6_type_code_match(u_int8_t test_type, u_int8_t min_code, u_int8_t max_code,
		     u_int8_t type, u_int8_t code,
		     int invert)
{
	return (type == test_type && code >= min_code && code <= max_code)
		^ invert;
}

static int
icmp6_match(const struct sk_buff *skb,
	   const struct net_device *in,
	   const struct net_device *out,
	   const void *matchinfo,
	   int offset,
	   unsigned int protoff,
	   int *hotdrop)
{
	struct icmp6hdr _icmp, *ic;
	const struct ip6t_icmp *icmpinfo = matchinfo;

	/* Must not be a fragment. */
	if (offset)
		return 0;

	ic = skb_header_pointer(skb, protoff, sizeof(_icmp), &_icmp);
	if (ic == NULL) {
		/* We've been asked to examine this packet, and we
		   can't.  Hence, no choice but to drop. */
		duprintf("Dropping evil ICMP tinygram.\n");
		*hotdrop = 1;
		return 0;
	}

	return icmp6_type_code_match(icmpinfo->type,
				     icmpinfo->code[0],
				     icmpinfo->code[1],
				     ic->icmp6_type, ic->icmp6_code,
				     !!(icmpinfo->invflags&IP6T_ICMP_INV));
}

/* Called when user tries to insert an entry of this type. */
static int
icmp6_checkentry(const char *tablename,
	   const struct ip6t_ip6 *ipv6,
	   void *matchinfo,
	   unsigned int matchsize,
	   unsigned int hook_mask)
{
	const struct ip6t_icmp *icmpinfo = matchinfo;

	/* Must specify proto == ICMP, and no unknown invflags */
	return ipv6->proto == IPPROTO_ICMPV6
		&& !(ipv6->invflags & IP6T_INV_PROTO)
		&& matchsize == IP6T_ALIGN(sizeof(struct ip6t_icmp))
		&& !(icmpinfo->invflags & ~IP6T_ICMP_INV);
}

/* The built-in targets: standard (NULL) and error. */
static struct ip6t_target ip6t_standard_target = {
	.name		= IP6T_STANDARD_TARGET,
};

static struct ip6t_target ip6t_error_target = {
	.name		= IP6T_ERROR_TARGET,
	.target		= ip6t_error,
};

static struct nf_sockopt_ops ip6t_sockopts = {
	.pf		= PF_INET6,
	.set_optmin	= IP6T_BASE_CTL,
	.set_optmax	= IP6T_SO_SET_MAX+1,
	.set		= do_ip6t_set_ctl,
	.get_optmin	= IP6T_BASE_CTL,
	.get_optmax	= IP6T_SO_GET_MAX+1,
	.get		= do_ip6t_get_ctl,
};

static struct ip6t_match tcp_matchstruct = {
	.name		= "tcp",
	.match		= &tcp_match,
	.checkentry	= &tcp_checkentry,
};

static struct ip6t_match udp_matchstruct = {
	.name		= "udp",
	.match		= &udp_match,
	.checkentry	= &udp_checkentry,
};

static struct ip6t_match icmp6_matchstruct = {
	.name		= "icmp6",
	.match		= &icmp6_match,
	.checkentry	= &icmp6_checkentry,
};

#ifdef CONFIG_PROC_FS
static inline int print_name(const char *i,
			     off_t start_offset, char *buffer, int length,
			     off_t *pos, unsigned int *count)
{
	if ((*count)++ >= start_offset) {
		unsigned int namelen;

		namelen = sprintf(buffer + *pos, "%s\n",
				  i + sizeof(struct list_head));
		if (*pos + namelen > length) {
			/* Stop iterating */
			return 1;
		}
		*pos += namelen;
	}
	return 0;
}

static inline int print_target(const struct ip6t_target *t,
                               off_t start_offset, char *buffer, int length,
                               off_t *pos, unsigned int *count)
{
	if (t == &ip6t_standard_target || t == &ip6t_error_target)
		return 0;
	return print_name((char *)t, start_offset, buffer, length, pos, count);
}

static int ip6t_get_tables(char *buffer, char **start, off_t offset, int length)
{
	off_t pos = 0;
	unsigned int count = 0;

	if (down_interruptible(&ip6t_mutex) != 0)
		return 0;

	LIST_FIND(&ip6t_tables, print_name, char *,
		  offset, buffer, length, &pos, &count);

	up(&ip6t_mutex);

	/* `start' hack - see fs/proc/generic.c line ~105 */
	*start=(char *)((unsigned long)count-offset);
	return pos;
}

static int ip6t_get_targets(char *buffer, char **start, off_t offset, int length)
{
	off_t pos = 0;
	unsigned int count = 0;

	if (down_interruptible(&ip6t_mutex) != 0)
		return 0;

	LIST_FIND(&ip6t_target, print_target, struct ip6t_target *,
		  offset, buffer, length, &pos, &count);

	up(&ip6t_mutex);

	*start = (char *)((unsigned long)count - offset);
	return pos;
}

static int ip6t_get_matches(char *buffer, char **start, off_t offset, int length)
{
	off_t pos = 0;
	unsigned int count = 0;

	if (down_interruptible(&ip6t_mutex) != 0)
		return 0;

	LIST_FIND(&ip6t_match, print_name, char *,
		  offset, buffer, length, &pos, &count);

	up(&ip6t_mutex);

	*start = (char *)((unsigned long)count - offset);
	return pos;
}

static const struct { char *name; get_info_t *get_info; } ip6t_proc_entry[] =
{ { "ip6_tables_names", ip6t_get_tables },
  { "ip6_tables_targets", ip6t_get_targets },
  { "ip6_tables_matches", ip6t_get_matches },
  { NULL, NULL} };
#endif /*CONFIG_PROC_FS*/

static int __init init(void)
{
	int ret;

	/* Noone else will be downing sem now, so we won't sleep */
	down(&ip6t_mutex);
	list_append(&ip6t_target, &ip6t_standard_target);
	list_append(&ip6t_target, &ip6t_error_target);
	list_append(&ip6t_match, &tcp_matchstruct);
	list_append(&ip6t_match, &udp_matchstruct);
	list_append(&ip6t_match, &icmp6_matchstruct);
	up(&ip6t_mutex);

	/* Register setsockopt */
	ret = nf_register_sockopt(&ip6t_sockopts);
	if (ret < 0) {
		duprintf("Unable to register sockopts.\n");
		return ret;
	}

#ifdef CONFIG_PROC_FS
	{
		struct proc_dir_entry *proc;
		int i;

		for (i = 0; ip6t_proc_entry[i].name; i++) {
			proc = proc_net_create(ip6t_proc_entry[i].name, 0,
					       ip6t_proc_entry[i].get_info);
			if (!proc) {
				while (--i >= 0)
				       proc_net_remove(ip6t_proc_entry[i].name);
				nf_unregister_sockopt(&ip6t_sockopts);
				return -ENOMEM;
			}
			proc->owner = THIS_MODULE;
		}
	}
#endif

	printk("ip6_tables: (C) 2000-2002 Netfilter core team\n");
	return 0;
}

static void __exit fini(void)
{
	nf_unregister_sockopt(&ip6t_sockopts);
#ifdef CONFIG_PROC_FS
	{
		int i;
		for (i = 0; ip6t_proc_entry[i].name; i++)
			proc_net_remove(ip6t_proc_entry[i].name);
	}
#endif
}

/*
 * find the offset to specified header or the protocol number of last header
 * if target < 0. "last header" is transport protocol header, ESP, or
 * "No next header".
 *
 * If target header is found, its offset is set in *offset and return protocol
 * number. Otherwise, return -1.
 *
 * Note that non-1st fragment is special case that "the protocol number
 * of last header" is "next header" field in Fragment header. In this case,
 * *offset is meaningless and fragment offset is stored in *fragoff if fragoff
 * isn't NULL.
 *
 */
int ipv6_find_hdr(const struct sk_buff *skb, unsigned int *offset,
		  int target, unsigned short *fragoff)
{
	unsigned int start = (u8*)(skb->nh.ipv6h + 1) - skb->data;
	u8 nexthdr = skb->nh.ipv6h->nexthdr;
	unsigned int len = skb->len - start;

	if (fragoff)
		*fragoff = 0;

	while (nexthdr != target) {
		struct ipv6_opt_hdr _hdr, *hp;
		unsigned int hdrlen;

		if ((!ipv6_ext_hdr(nexthdr)) || nexthdr == NEXTHDR_NONE) {
			if (target < 0)
				break;
			return -1;
		}

		hp = skb_header_pointer(skb, start, sizeof(_hdr), &_hdr);
		if (hp == NULL)
			return -1;
		if (nexthdr == NEXTHDR_FRAGMENT) {
			unsigned short _frag_off, *fp;
			fp = skb_header_pointer(skb,
						start+offsetof(struct frag_hdr,
							       frag_off),
						sizeof(_frag_off),
						&_frag_off);
			if (fp == NULL)
				return -1;

			_frag_off = ntohs(*fp) & ~0x7;
			if (_frag_off) {
				if (target < 0 &&
				    ((!ipv6_ext_hdr(hp->nexthdr)) ||
				     nexthdr == NEXTHDR_NONE)) {
					if (fragoff)
						*fragoff = _frag_off;
					return hp->nexthdr;
				}
				return -1;
			}
			hdrlen = 8;
		} else if (nexthdr == NEXTHDR_AUTH)
			hdrlen = (hp->hdrlen + 2) << 2; 
		else
			hdrlen = ipv6_optlen(hp); 

		nexthdr = hp->nexthdr;
		len -= hdrlen;
		start += hdrlen;
	}

	*offset = start;
	return nexthdr;
}

EXPORT_SYMBOL(ip6t_register_table);
EXPORT_SYMBOL(ip6t_unregister_table);
EXPORT_SYMBOL(ip6t_do_table);
EXPORT_SYMBOL(ip6t_register_match);
EXPORT_SYMBOL(ip6t_unregister_match);
EXPORT_SYMBOL(ip6t_register_target);
EXPORT_SYMBOL(ip6t_unregister_target);
EXPORT_SYMBOL(ip6t_ext_hdr);
EXPORT_SYMBOL(ipv6_find_hdr);
EXPORT_SYMBOL(ip6_masked_addrcmp);

module_init(init);
module_exit(fini);
