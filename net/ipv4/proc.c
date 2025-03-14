/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		This file implements the various access functions for the
 *		PROC file system.  It is mainly used for debugging and
 *		statistics.
 *
 * Version:	$Id: proc.c,v 1.45 2001/05/16 16:45:35 davem Exp $
 *
 * Authors:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Gerald J. Heim, <heim@peanuts.informatik.uni-tuebingen.de>
 *		Fred Baumgarten, <dc6iq@insu1.etec.uni-karlsruhe.de>
 *		Erik Schoenfelder, <schoenfr@ibr.cs.tu-bs.de>
 *
 * Fixes:
 *		Alan Cox	:	UDP sockets show the rxqueue/txqueue
 *					using hint flag for the netinfo.
 *	Pauline Middelink	:	identd support
 *		Alan Cox	:	Make /proc safer.
 *	Erik Schoenfelder	:	/proc/net/snmp
 *		Alan Cox	:	Handle dead sockets properly.
 *	Gerhard Koerting	:	Show both timers
 *		Alan Cox	:	Allow inode to be NULL (kernel socket)
 *	Andi Kleen		:	Add support for open_requests and
 *					split functions for more readibility.
 *	Andi Kleen		:	Add support for /proc/net/netstat
 *	Arnaldo C. Melo		:	Convert to seq_file
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <net/icmp.h>
#include <net/protocol.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <linux/inetdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/sock.h>
#include <net/raw.h>

static int fold_prot_inuse(struct proto *proto)
{
	int res = 0;
	int cpu;

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		res += proto->stats[cpu].inuse;

	return res;
}

/*
 *	Report socket allocation statistics [mea@utu.fi]
 */
static int sockstat_seq_show(struct seq_file *seq, void *v)
{
	socket_seq_show(seq);
	seq_printf(seq, "TCP: inuse %d orphan %d tw %d alloc %d mem %d\n",
		   fold_prot_inuse(&tcp_prot), atomic_read(&tcp_orphan_count),
		   tcp_death_row.tw_count, atomic_read(&tcp_sockets_allocated),
		   atomic_read(&tcp_memory_allocated));
	seq_printf(seq, "UDP: inuse %d\n", fold_prot_inuse(&udp_prot));
	seq_printf(seq, "RAW: inuse %d\n", fold_prot_inuse(&raw_prot));
	seq_printf(seq,  "FRAG: inuse %d memory %d\n", ip_frag_nqueues,
		   atomic_read(&ip_frag_mem));
	return 0;
}

static int sockstat_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, sockstat_seq_show, NULL);
}

static struct file_operations sockstat_seq_fops = {
	.owner	 = THIS_MODULE,
	.open	 = sockstat_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static unsigned long
fold_field(void *mib[], int offt)
{
	unsigned long res = 0;
	int i;

	for_each_cpu(i) {
		res += *(((unsigned long *) per_cpu_ptr(mib[0], i)) + offt);
		res += *(((unsigned long *) per_cpu_ptr(mib[1], i)) + offt);
	}
	return res;
}

/* snmp items */
static const struct snmp_mib snmp4_ipstats_list[] = {
	SNMP_MIB_ITEM("InReceives", IPSTATS_MIB_INRECEIVES),
	SNMP_MIB_ITEM("InHdrErrors", IPSTATS_MIB_INHDRERRORS),
	SNMP_MIB_ITEM("InAddrErrors", IPSTATS_MIB_INADDRERRORS),
	SNMP_MIB_ITEM("ForwDatagrams", IPSTATS_MIB_OUTFORWDATAGRAMS),
	SNMP_MIB_ITEM("InUnknownProtos", IPSTATS_MIB_INUNKNOWNPROTOS),
	SNMP_MIB_ITEM("InDiscards", IPSTATS_MIB_INDISCARDS),
	SNMP_MIB_ITEM("InDelivers", IPSTATS_MIB_INDELIVERS),
	SNMP_MIB_ITEM("OutRequests", IPSTATS_MIB_OUTREQUESTS),
	SNMP_MIB_ITEM("OutDiscards", IPSTATS_MIB_OUTDISCARDS),
	SNMP_MIB_ITEM("OutNoRoutes", IPSTATS_MIB_OUTNOROUTES),
	SNMP_MIB_ITEM("ReasmTimeout", IPSTATS_MIB_REASMTIMEOUT),
	SNMP_MIB_ITEM("ReasmReqds", IPSTATS_MIB_REASMREQDS),
	SNMP_MIB_ITEM("ReasmOKs", IPSTATS_MIB_REASMOKS),
	SNMP_MIB_ITEM("ReasmFails", IPSTATS_MIB_REASMFAILS),
	SNMP_MIB_ITEM("FragOKs", IPSTATS_MIB_FRAGOKS),
	SNMP_MIB_ITEM("FragFails", IPSTATS_MIB_FRAGFAILS),
	SNMP_MIB_ITEM("FragCreates", IPSTATS_MIB_FRAGCREATES),
	SNMP_MIB_SENTINEL
};

static const struct snmp_mib snmp4_icmp_list[] = {
	SNMP_MIB_ITEM("InMsgs", ICMP_MIB_INMSGS),
	SNMP_MIB_ITEM("InErrors", ICMP_MIB_INERRORS),
	SNMP_MIB_ITEM("InDestUnreachs", ICMP_MIB_INDESTUNREACHS),
	SNMP_MIB_ITEM("InTimeExcds", ICMP_MIB_INTIMEEXCDS),
	SNMP_MIB_ITEM("InParmProbs", ICMP_MIB_INPARMPROBS),
	SNMP_MIB_ITEM("InSrcQuenchs", ICMP_MIB_INSRCQUENCHS),
	SNMP_MIB_ITEM("InRedirects", ICMP_MIB_INREDIRECTS),
	SNMP_MIB_ITEM("InEchos", ICMP_MIB_INECHOS),
	SNMP_MIB_ITEM("InEchoReps", ICMP_MIB_INECHOREPS),
	SNMP_MIB_ITEM("InTimestamps", ICMP_MIB_INTIMESTAMPS),
	SNMP_MIB_ITEM("InTimestampReps", ICMP_MIB_INTIMESTAMPREPS),
	SNMP_MIB_ITEM("InAddrMasks", ICMP_MIB_INADDRMASKS),
	SNMP_MIB_ITEM("InAddrMaskReps", ICMP_MIB_INADDRMASKREPS),
	SNMP_MIB_ITEM("OutMsgs", ICMP_MIB_OUTMSGS),
	SNMP_MIB_ITEM("OutErrors", ICMP_MIB_OUTERRORS),
	SNMP_MIB_ITEM("OutDestUnreachs", ICMP_MIB_OUTDESTUNREACHS),
	SNMP_MIB_ITEM("OutTimeExcds", ICMP_MIB_OUTTIMEEXCDS),
	SNMP_MIB_ITEM("OutParmProbs", ICMP_MIB_OUTPARMPROBS),
	SNMP_MIB_ITEM("OutSrcQuenchs", ICMP_MIB_OUTSRCQUENCHS),
	SNMP_MIB_ITEM("OutRedirects", ICMP_MIB_OUTREDIRECTS),
	SNMP_MIB_ITEM("OutEchos", ICMP_MIB_OUTECHOS),
	SNMP_MIB_ITEM("OutEchoReps", ICMP_MIB_OUTECHOREPS),
	SNMP_MIB_ITEM("OutTimestamps", ICMP_MIB_OUTTIMESTAMPS),
	SNMP_MIB_ITEM("OutTimestampReps", ICMP_MIB_OUTTIMESTAMPREPS),
	SNMP_MIB_ITEM("OutAddrMasks", ICMP_MIB_OUTADDRMASKS),
	SNMP_MIB_ITEM("OutAddrMaskReps", ICMP_MIB_OUTADDRMASKREPS),
	SNMP_MIB_SENTINEL
};

static const struct snmp_mib snmp4_tcp_list[] = {
	SNMP_MIB_ITEM("RtoAlgorithm", TCP_MIB_RTOALGORITHM),
	SNMP_MIB_ITEM("RtoMin", TCP_MIB_RTOMIN),
	SNMP_MIB_ITEM("RtoMax", TCP_MIB_RTOMAX),
	SNMP_MIB_ITEM("MaxConn", TCP_MIB_MAXCONN),
	SNMP_MIB_ITEM("ActiveOpens", TCP_MIB_ACTIVEOPENS),
	SNMP_MIB_ITEM("PassiveOpens", TCP_MIB_PASSIVEOPENS),
	SNMP_MIB_ITEM("AttemptFails", TCP_MIB_ATTEMPTFAILS),
	SNMP_MIB_ITEM("EstabResets", TCP_MIB_ESTABRESETS),
	SNMP_MIB_ITEM("CurrEstab", TCP_MIB_CURRESTAB),
	SNMP_MIB_ITEM("InSegs", TCP_MIB_INSEGS),
	SNMP_MIB_ITEM("OutSegs", TCP_MIB_OUTSEGS),
	SNMP_MIB_ITEM("RetransSegs", TCP_MIB_RETRANSSEGS),
	SNMP_MIB_ITEM("InErrs", TCP_MIB_INERRS),
	SNMP_MIB_ITEM("OutRsts", TCP_MIB_OUTRSTS),
	SNMP_MIB_SENTINEL
};

static const struct snmp_mib snmp4_udp_list[] = {
	SNMP_MIB_ITEM("InDatagrams", UDP_MIB_INDATAGRAMS),
	SNMP_MIB_ITEM("NoPorts", UDP_MIB_NOPORTS),
	SNMP_MIB_ITEM("InErrors", UDP_MIB_INERRORS),
	SNMP_MIB_ITEM("OutDatagrams", UDP_MIB_OUTDATAGRAMS),
	SNMP_MIB_SENTINEL
};

static const struct snmp_mib snmp4_net_list[] = {
	SNMP_MIB_ITEM("SyncookiesSent", LINUX_MIB_SYNCOOKIESSENT),
	SNMP_MIB_ITEM("SyncookiesRecv", LINUX_MIB_SYNCOOKIESRECV),
	SNMP_MIB_ITEM("SyncookiesFailed", LINUX_MIB_SYNCOOKIESFAILED),
	SNMP_MIB_ITEM("EmbryonicRsts", LINUX_MIB_EMBRYONICRSTS),
	SNMP_MIB_ITEM("PruneCalled", LINUX_MIB_PRUNECALLED),
	SNMP_MIB_ITEM("RcvPruned", LINUX_MIB_RCVPRUNED),
	SNMP_MIB_ITEM("OfoPruned", LINUX_MIB_OFOPRUNED),
	SNMP_MIB_ITEM("OutOfWindowIcmps", LINUX_MIB_OUTOFWINDOWICMPS),
	SNMP_MIB_ITEM("LockDroppedIcmps", LINUX_MIB_LOCKDROPPEDICMPS),
	SNMP_MIB_ITEM("ArpFilter", LINUX_MIB_ARPFILTER),
	SNMP_MIB_ITEM("TW", LINUX_MIB_TIMEWAITED),
	SNMP_MIB_ITEM("TWRecycled", LINUX_MIB_TIMEWAITRECYCLED),
	SNMP_MIB_ITEM("TWKilled", LINUX_MIB_TIMEWAITKILLED),
	SNMP_MIB_ITEM("PAWSPassive", LINUX_MIB_PAWSPASSIVEREJECTED),
	SNMP_MIB_ITEM("PAWSActive", LINUX_MIB_PAWSACTIVEREJECTED),
	SNMP_MIB_ITEM("PAWSEstab", LINUX_MIB_PAWSESTABREJECTED),
	SNMP_MIB_ITEM("DelayedACKs", LINUX_MIB_DELAYEDACKS),
	SNMP_MIB_ITEM("DelayedACKLocked", LINUX_MIB_DELAYEDACKLOCKED),
	SNMP_MIB_ITEM("DelayedACKLost", LINUX_MIB_DELAYEDACKLOST),
	SNMP_MIB_ITEM("ListenOverflows", LINUX_MIB_LISTENOVERFLOWS),
	SNMP_MIB_ITEM("ListenDrops", LINUX_MIB_LISTENDROPS),
	SNMP_MIB_ITEM("TCPPrequeued", LINUX_MIB_TCPPREQUEUED),
	SNMP_MIB_ITEM("TCPDirectCopyFromBacklog", LINUX_MIB_TCPDIRECTCOPYFROMBACKLOG),
	SNMP_MIB_ITEM("TCPDirectCopyFromPrequeue", LINUX_MIB_TCPDIRECTCOPYFROMPREQUEUE),
	SNMP_MIB_ITEM("TCPPrequeueDropped", LINUX_MIB_TCPPREQUEUEDROPPED),
	SNMP_MIB_ITEM("TCPHPHits", LINUX_MIB_TCPHPHITS),
	SNMP_MIB_ITEM("TCPHPHitsToUser", LINUX_MIB_TCPHPHITSTOUSER),
	SNMP_MIB_ITEM("TCPPureAcks", LINUX_MIB_TCPPUREACKS),
	SNMP_MIB_ITEM("TCPHPAcks", LINUX_MIB_TCPHPACKS),
	SNMP_MIB_ITEM("TCPRenoRecovery", LINUX_MIB_TCPRENORECOVERY),
	SNMP_MIB_ITEM("TCPSackRecovery", LINUX_MIB_TCPSACKRECOVERY),
	SNMP_MIB_ITEM("TCPSACKReneging", LINUX_MIB_TCPSACKRENEGING),
	SNMP_MIB_ITEM("TCPFACKReorder", LINUX_MIB_TCPFACKREORDER),
	SNMP_MIB_ITEM("TCPSACKReorder", LINUX_MIB_TCPSACKREORDER),
	SNMP_MIB_ITEM("TCPRenoReorder", LINUX_MIB_TCPRENOREORDER),
	SNMP_MIB_ITEM("TCPTSReorder", LINUX_MIB_TCPTSREORDER),
	SNMP_MIB_ITEM("TCPFullUndo", LINUX_MIB_TCPFULLUNDO),
	SNMP_MIB_ITEM("TCPPartialUndo", LINUX_MIB_TCPPARTIALUNDO),
	SNMP_MIB_ITEM("TCPDSACKUndo", LINUX_MIB_TCPDSACKUNDO),
	SNMP_MIB_ITEM("TCPLossUndo", LINUX_MIB_TCPLOSSUNDO),
	SNMP_MIB_ITEM("TCPLoss", LINUX_MIB_TCPLOSS),
	SNMP_MIB_ITEM("TCPLostRetransmit", LINUX_MIB_TCPLOSTRETRANSMIT),
	SNMP_MIB_ITEM("TCPRenoFailures", LINUX_MIB_TCPRENOFAILURES),
	SNMP_MIB_ITEM("TCPSackFailures", LINUX_MIB_TCPSACKFAILURES),
	SNMP_MIB_ITEM("TCPLossFailures", LINUX_MIB_TCPLOSSFAILURES),
	SNMP_MIB_ITEM("TCPFastRetrans", LINUX_MIB_TCPFASTRETRANS),
	SNMP_MIB_ITEM("TCPForwardRetrans", LINUX_MIB_TCPFORWARDRETRANS),
	SNMP_MIB_ITEM("TCPSlowStartRetrans", LINUX_MIB_TCPSLOWSTARTRETRANS),
	SNMP_MIB_ITEM("TCPTimeouts", LINUX_MIB_TCPTIMEOUTS),
	SNMP_MIB_ITEM("TCPRenoRecoveryFail", LINUX_MIB_TCPRENORECOVERYFAIL),
	SNMP_MIB_ITEM("TCPSackRecoveryFail", LINUX_MIB_TCPSACKRECOVERYFAIL),
	SNMP_MIB_ITEM("TCPSchedulerFailed", LINUX_MIB_TCPSCHEDULERFAILED),
	SNMP_MIB_ITEM("TCPRcvCollapsed", LINUX_MIB_TCPRCVCOLLAPSED),
	SNMP_MIB_ITEM("TCPDSACKOldSent", LINUX_MIB_TCPDSACKOLDSENT),
	SNMP_MIB_ITEM("TCPDSACKOfoSent", LINUX_MIB_TCPDSACKOFOSENT),
	SNMP_MIB_ITEM("TCPDSACKRecv", LINUX_MIB_TCPDSACKRECV),
	SNMP_MIB_ITEM("TCPDSACKOfoRecv", LINUX_MIB_TCPDSACKOFORECV),
	SNMP_MIB_ITEM("TCPAbortOnSyn", LINUX_MIB_TCPABORTONSYN),
	SNMP_MIB_ITEM("TCPAbortOnData", LINUX_MIB_TCPABORTONDATA),
	SNMP_MIB_ITEM("TCPAbortOnClose", LINUX_MIB_TCPABORTONCLOSE),
	SNMP_MIB_ITEM("TCPAbortOnMemory", LINUX_MIB_TCPABORTONMEMORY),
	SNMP_MIB_ITEM("TCPAbortOnTimeout", LINUX_MIB_TCPABORTONTIMEOUT),
	SNMP_MIB_ITEM("TCPAbortOnLinger", LINUX_MIB_TCPABORTONLINGER),
	SNMP_MIB_ITEM("TCPAbortFailed", LINUX_MIB_TCPABORTFAILED),
	SNMP_MIB_ITEM("TCPMemoryPressures", LINUX_MIB_TCPMEMORYPRESSURES),
	SNMP_MIB_SENTINEL
};

/*
 *	Called from the PROCfs module. This outputs /proc/net/snmp.
 */
static int snmp_seq_show(struct seq_file *seq, void *v)
{
	int i;

	seq_puts(seq, "Ip: Forwarding DefaultTTL");

	for (i = 0; snmp4_ipstats_list[i].name != NULL; i++)
		seq_printf(seq, " %s", snmp4_ipstats_list[i].name);

	seq_printf(seq, "\nIp: %d %d",
			ipv4_devconf.forwarding ? 1 : 2, sysctl_ip_default_ttl);

	for (i = 0; snmp4_ipstats_list[i].name != NULL; i++)
		seq_printf(seq, " %lu",
			   fold_field((void **) ip_statistics, 
				      snmp4_ipstats_list[i].entry));

	seq_puts(seq, "\nIcmp:");
	for (i = 0; snmp4_icmp_list[i].name != NULL; i++)
		seq_printf(seq, " %s", snmp4_icmp_list[i].name);

	seq_puts(seq, "\nIcmp:");
	for (i = 0; snmp4_icmp_list[i].name != NULL; i++)
		seq_printf(seq, " %lu",
			   fold_field((void **) icmp_statistics, 
				      snmp4_icmp_list[i].entry));

	seq_puts(seq, "\nTcp:");
	for (i = 0; snmp4_tcp_list[i].name != NULL; i++)
		seq_printf(seq, " %s", snmp4_tcp_list[i].name);

	seq_puts(seq, "\nTcp:");
	for (i = 0; snmp4_tcp_list[i].name != NULL; i++) {
		/* MaxConn field is signed, RFC 2012 */
		if (snmp4_tcp_list[i].entry == TCP_MIB_MAXCONN)
			seq_printf(seq, " %ld",
				   fold_field((void **) tcp_statistics, 
					      snmp4_tcp_list[i].entry));
		else
			seq_printf(seq, " %lu",
				   fold_field((void **) tcp_statistics,
					      snmp4_tcp_list[i].entry));
	}

	seq_puts(seq, "\nUdp:");
	for (i = 0; snmp4_udp_list[i].name != NULL; i++)
		seq_printf(seq, " %s", snmp4_udp_list[i].name);

	seq_puts(seq, "\nUdp:");
	for (i = 0; snmp4_udp_list[i].name != NULL; i++)
		seq_printf(seq, " %lu",
			   fold_field((void **) udp_statistics, 
				      snmp4_udp_list[i].entry));

	seq_putc(seq, '\n');
	return 0;
}

static int snmp_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, snmp_seq_show, NULL);
}

static struct file_operations snmp_seq_fops = {
	.owner	 = THIS_MODULE,
	.open	 = snmp_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

/*
 *	Output /proc/net/netstat
 */
static int netstat_seq_show(struct seq_file *seq, void *v)
{
	int i;

	seq_puts(seq, "TcpExt:");
	for (i = 0; snmp4_net_list[i].name != NULL; i++)
		seq_printf(seq, " %s", snmp4_net_list[i].name);

	seq_puts(seq, "\nTcpExt:");
	for (i = 0; snmp4_net_list[i].name != NULL; i++)
		seq_printf(seq, " %lu",
			   fold_field((void **) net_statistics, 
				      snmp4_net_list[i].entry));

	seq_putc(seq, '\n');
	return 0;
}

static int netstat_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, netstat_seq_show, NULL);
}

static struct file_operations netstat_seq_fops = {
	.owner	 = THIS_MODULE,
	.open	 = netstat_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

int __init ip_misc_proc_init(void)
{
	int rc = 0;

	if (!proc_net_fops_create("netstat", S_IRUGO, &netstat_seq_fops))
		goto out_netstat;

	if (!proc_net_fops_create("snmp", S_IRUGO, &snmp_seq_fops))
		goto out_snmp;

	if (!proc_net_fops_create("sockstat", S_IRUGO, &sockstat_seq_fops))
		goto out_sockstat;
out:
	return rc;
out_sockstat:
	proc_net_remove("snmp");
out_snmp:
	proc_net_remove("netstat");
out_netstat:
	rc = -ENOMEM;
	goto out;
}

