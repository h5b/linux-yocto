/*
 *	Low-Level PCI Support for PC -- Routing of Interrupts
 *
 *	(c) 1999--2000 Martin Mares <mj@ucw.cz>
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/dmi.h>
#include <asm/io.h>
#include <asm/smp.h>
#include <asm/io_apic.h>
#include <linux/irq.h>
#include <linux/acpi.h>

#include "pci.h"

#define PIRQ_SIGNATURE	(('$' << 0) + ('P' << 8) + ('I' << 16) + ('R' << 24))
#define PIRQ_VERSION 0x0100

static int broken_hp_bios_irq9;
static int acer_tm360_irqrouting;

static struct irq_routing_table *pirq_table;

static int pirq_enable_irq(struct pci_dev *dev);

/*
 * Never use: 0, 1, 2 (timer, keyboard, and cascade)
 * Avoid using: 13, 14 and 15 (FP error and IDE).
 * Penalize: 3, 4, 6, 7, 12 (known ISA uses: serial, floppy, parallel and mouse)
 */
unsigned int pcibios_irq_mask = 0xfff8;

static int pirq_penalty[16] = {
	1000000, 1000000, 1000000, 1000, 1000, 0, 1000, 1000,
	0, 0, 0, 0, 1000, 100000, 100000, 100000
};

struct irq_router {
	char *name;
	u16 vendor, device;
	int (*get)(struct pci_dev *router, struct pci_dev *dev, int pirq);
	int (*set)(struct pci_dev *router, struct pci_dev *dev, int pirq, int new);
};

struct irq_router_handler {
	u16 vendor;
	int (*probe)(struct irq_router *r, struct pci_dev *router, u16 device);
};

int (*pcibios_enable_irq)(struct pci_dev *dev) = NULL;
void (*pcibios_disable_irq)(struct pci_dev *dev) = NULL;

/*
 *  Check passed address for the PCI IRQ Routing Table signature
 *  and perform checksum verification.
 */

static inline struct irq_routing_table * pirq_check_routing_table(u8 *addr)
{
	struct irq_routing_table *rt;
	int i;
	u8 sum;

	rt = (struct irq_routing_table *) addr;
	if (rt->signature != PIRQ_SIGNATURE ||
	    rt->version != PIRQ_VERSION ||
	    rt->size % 16 ||
	    rt->size < sizeof(struct irq_routing_table))
		return NULL;
	sum = 0;
	for (i=0; i < rt->size; i++)
		sum += addr[i];
	if (!sum) {
		DBG(KERN_DEBUG "PCI: Interrupt Routing Table found at 0x%p\n", rt);
		return rt;
	}
	return NULL;
}



/*
 *  Search 0xf0000 -- 0xfffff for the PCI IRQ Routing Table.
 */

static struct irq_routing_table * __init pirq_find_routing_table(void)
{
	u8 *addr;
	struct irq_routing_table *rt;

	if (pirq_table_addr) {
		rt = pirq_check_routing_table((u8 *) __va(pirq_table_addr));
		if (rt)
			return rt;
		printk(KERN_WARNING "PCI: PIRQ table NOT found at pirqaddr\n");
	}
	for(addr = (u8 *) __va(0xf0000); addr < (u8 *) __va(0x100000); addr += 16) {
		rt = pirq_check_routing_table(addr);
		if (rt)
			return rt;
	}
	return NULL;
}

/*
 *  If we have a IRQ routing table, use it to search for peer host
 *  bridges.  It's a gross hack, but since there are no other known
 *  ways how to get a list of buses, we have to go this way.
 */

static void __init pirq_peer_trick(void)
{
	struct irq_routing_table *rt = pirq_table;
	u8 busmap[256];
	int i;
	struct irq_info *e;

	memset(busmap, 0, sizeof(busmap));
	for(i=0; i < (rt->size - sizeof(struct irq_routing_table)) / sizeof(struct irq_info); i++) {
		e = &rt->slots[i];
#ifdef DEBUG
		{
			int j;
			DBG(KERN_DEBUG "%02x:%02x slot=%02x", e->bus, e->devfn/8, e->slot);
			for(j=0; j<4; j++)
				DBG(" %d:%02x/%04x", j, e->irq[j].link, e->irq[j].bitmap);
			DBG("\n");
		}
#endif
		busmap[e->bus] = 1;
	}
	for(i = 1; i < 256; i++) {
		if (!busmap[i] || pci_find_bus(0, i))
			continue;
		if (pci_scan_bus(i, &pci_root_ops, NULL))
			printk(KERN_INFO "PCI: Discovered primary peer bus %02x [IRQ]\n", i);
	}
	pcibios_last_bus = -1;
}

/*
 *  Code for querying and setting of IRQ routes on various interrupt routers.
 */

void eisa_set_level_irq(unsigned int irq)
{
	unsigned char mask = 1 << (irq & 7);
	unsigned int port = 0x4d0 + (irq >> 3);
	unsigned char val;
	static u16 eisa_irq_mask;

	if (irq >= 16 || (1 << irq) & eisa_irq_mask)
		return;

	eisa_irq_mask |= (1 << irq);
	printk(KERN_DEBUG "PCI: setting IRQ %u as level-triggered\n", irq);
	val = inb(port);
	if (!(val & mask)) {
		DBG(KERN_DEBUG " -> edge");
		outb(val | mask, port);
	}
}

/*
 * Common IRQ routing practice: nybbles in config space,
 * offset by some magic constant.
 */
static unsigned int read_config_nybble(struct pci_dev *router, unsigned offset, unsigned nr)
{
	u8 x;
	unsigned reg = offset + (nr >> 1);

	pci_read_config_byte(router, reg, &x);
	return (nr & 1) ? (x >> 4) : (x & 0xf);
}

static void write_config_nybble(struct pci_dev *router, unsigned offset, unsigned nr, unsigned int val)
{
	u8 x;
	unsigned reg = offset + (nr >> 1);

	pci_read_config_byte(router, reg, &x);
	x = (nr & 1) ? ((x & 0x0f) | (val << 4)) : ((x & 0xf0) | val);
	pci_write_config_byte(router, reg, x);
}

/*
 * ALI pirq entries are damn ugly, and completely undocumented.
 * This has been figured out from pirq tables, and it's not a pretty
 * picture.
 */
static int pirq_ali_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	static unsigned char irqmap[16] = { 0, 9, 3, 10, 4, 5, 7, 6, 1, 11, 0, 12, 0, 14, 0, 15 };

	return irqmap[read_config_nybble(router, 0x48, pirq-1)];
}

static int pirq_ali_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	static unsigned char irqmap[16] = { 0, 8, 0, 2, 4, 5, 7, 6, 0, 1, 3, 9, 11, 0, 13, 15 };
	unsigned int val = irqmap[irq];
		
	if (val) {
		write_config_nybble(router, 0x48, pirq-1, val);
		return 1;
	}
	return 0;
}

/*
 * The Intel PIIX4 pirq rules are fairly simple: "pirq" is
 * just a pointer to the config space.
 */
static int pirq_piix_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	u8 x;

	pci_read_config_byte(router, pirq, &x);
	return (x < 16) ? x : 0;
}

static int pirq_piix_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	pci_write_config_byte(router, pirq, irq);
	return 1;
}

/*
 * The VIA pirq rules are nibble-based, like ALI,
 * but without the ugly irq number munging.
 * However, PIRQD is in the upper instead of lower 4 bits.
 */
static int pirq_via_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	return read_config_nybble(router, 0x55, pirq == 4 ? 5 : pirq);
}

static int pirq_via_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	write_config_nybble(router, 0x55, pirq == 4 ? 5 : pirq, irq);
	return 1;
}

/*
 * The VIA pirq rules are nibble-based, like ALI,
 * but without the ugly irq number munging.
 * However, for 82C586, nibble map is different .
 */
static int pirq_via586_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	static unsigned int pirqmap[4] = { 3, 2, 5, 1 };
	return read_config_nybble(router, 0x55, pirqmap[pirq-1]);
}

static int pirq_via586_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	static unsigned int pirqmap[4] = { 3, 2, 5, 1 };
	write_config_nybble(router, 0x55, pirqmap[pirq-1], irq);
	return 1;
}

/*
 * ITE 8330G pirq rules are nibble-based
 * FIXME: pirqmap may be { 1, 0, 3, 2 },
 * 	  2+3 are both mapped to irq 9 on my system
 */
static int pirq_ite_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	static unsigned char pirqmap[4] = { 1, 0, 2, 3 };
	return read_config_nybble(router,0x43, pirqmap[pirq-1]);
}

static int pirq_ite_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	static unsigned char pirqmap[4] = { 1, 0, 2, 3 };
	write_config_nybble(router, 0x43, pirqmap[pirq-1], irq);
	return 1;
}

/*
 * OPTI: high four bits are nibble pointer..
 * I wonder what the low bits do?
 */
static int pirq_opti_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	return read_config_nybble(router, 0xb8, pirq >> 4);
}

static int pirq_opti_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	write_config_nybble(router, 0xb8, pirq >> 4, irq);
	return 1;
}

/*
 * Cyrix: nibble offset 0x5C
 * 0x5C bits 7:4 is INTB bits 3:0 is INTA 
 * 0x5D bits 7:4 is INTD bits 3:0 is INTC
 */
static int pirq_cyrix_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	return read_config_nybble(router, 0x5C, (pirq-1)^1);
}

static int pirq_cyrix_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	write_config_nybble(router, 0x5C, (pirq-1)^1, irq);
	return 1;
}

/*
 *	PIRQ routing for SiS 85C503 router used in several SiS chipsets.
 *	We have to deal with the following issues here:
 *	- vendors have different ideas about the meaning of link values
 *	- some onboard devices (integrated in the chipset) have special
 *	  links and are thus routed differently (i.e. not via PCI INTA-INTD)
 *	- different revision of the router have a different layout for
 *	  the routing registers, particularly for the onchip devices
 *
 *	For all routing registers the common thing is we have one byte
 *	per routeable link which is defined as:
 *		 bit 7      IRQ mapping enabled (0) or disabled (1)
 *		 bits [6:4] reserved (sometimes used for onchip devices)
 *		 bits [3:0] IRQ to map to
 *		     allowed: 3-7, 9-12, 14-15
 *		     reserved: 0, 1, 2, 8, 13
 *
 *	The config-space registers located at 0x41/0x42/0x43/0x44 are
 *	always used to route the normal PCI INT A/B/C/D respectively.
 *	Apparently there are systems implementing PCI routing table using
 *	link values 0x01-0x04 and others using 0x41-0x44 for PCI INTA..D.
 *	We try our best to handle both link mappings.
 *	
 *	Currently (2003-05-21) it appears most SiS chipsets follow the
 *	definition of routing registers from the SiS-5595 southbridge.
 *	According to the SiS 5595 datasheets the revision id's of the
 *	router (ISA-bridge) should be 0x01 or 0xb0.
 *
 *	Furthermore we've also seen lspci dumps with revision 0x00 and 0xb1.
 *	Looks like these are used in a number of SiS 5xx/6xx/7xx chipsets.
 *	They seem to work with the current routing code. However there is
 *	some concern because of the two USB-OHCI HCs (original SiS 5595
 *	had only one). YMMV.
 *
 *	Onchip routing for router rev-id 0x01/0xb0 and probably 0x00/0xb1:
 *
 *	0x61:	IDEIRQ:
 *		bits [6:5] must be written 01
 *		bit 4 channel-select primary (0), secondary (1)
 *
 *	0x62:	USBIRQ:
 *		bit 6 OHCI function disabled (0), enabled (1)
 *	
 *	0x6a:	ACPI/SCI IRQ: bits 4-6 reserved
 *
 *	0x7e:	Data Acq. Module IRQ - bits 4-6 reserved
 *
 *	We support USBIRQ (in addition to INTA-INTD) and keep the
 *	IDE, ACPI and DAQ routing untouched as set by the BIOS.
 *
 *	Currently the only reported exception is the new SiS 65x chipset
 *	which includes the SiS 69x southbridge. Here we have the 85C503
 *	router revision 0x04 and there are changes in the register layout
 *	mostly related to the different USB HCs with USB 2.0 support.
 *
 *	Onchip routing for router rev-id 0x04 (try-and-error observation)
 *
 *	0x60/0x61/0x62/0x63:	1xEHCI and 3xOHCI (companion) USB-HCs
 *				bit 6-4 are probably unused, not like 5595
 */

#define PIRQ_SIS_IRQ_MASK	0x0f
#define PIRQ_SIS_IRQ_DISABLE	0x80
#define PIRQ_SIS_USB_ENABLE	0x40

static int pirq_sis_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	u8 x;
	int reg;

	reg = pirq;
	if (reg >= 0x01 && reg <= 0x04)
		reg += 0x40;
	pci_read_config_byte(router, reg, &x);
	return (x & PIRQ_SIS_IRQ_DISABLE) ? 0 : (x & PIRQ_SIS_IRQ_MASK);
}

static int pirq_sis_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	u8 x;
	int reg;

	reg = pirq;
	if (reg >= 0x01 && reg <= 0x04)
		reg += 0x40;
	pci_read_config_byte(router, reg, &x);
	x &= ~(PIRQ_SIS_IRQ_MASK | PIRQ_SIS_IRQ_DISABLE);
	x |= irq ? irq: PIRQ_SIS_IRQ_DISABLE;
	pci_write_config_byte(router, reg, x);
	return 1;
}


/*
 * VLSI: nibble offset 0x74 - educated guess due to routing table and
 *       config space of VLSI 82C534 PCI-bridge/router (1004:0102)
 *       Tested on HP OmniBook 800 covering PIRQ 1, 2, 4, 8 for onboard
 *       devices, PIRQ 3 for non-pci(!) soundchip and (untested) PIRQ 6
 *       for the busbridge to the docking station.
 */

static int pirq_vlsi_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	if (pirq > 8) {
		printk(KERN_INFO "VLSI router pirq escape (%d)\n", pirq);
		return 0;
	}
	return read_config_nybble(router, 0x74, pirq-1);
}

static int pirq_vlsi_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	if (pirq > 8) {
		printk(KERN_INFO "VLSI router pirq escape (%d)\n", pirq);
		return 0;
	}
	write_config_nybble(router, 0x74, pirq-1, irq);
	return 1;
}

/*
 * ServerWorks: PCI interrupts mapped to system IRQ lines through Index
 * and Redirect I/O registers (0x0c00 and 0x0c01).  The Index register
 * format is (PCIIRQ## | 0x10), e.g.: PCIIRQ10=0x1a.  The Redirect
 * register is a straight binary coding of desired PIC IRQ (low nibble).
 *
 * The 'link' value in the PIRQ table is already in the correct format
 * for the Index register.  There are some special index values:
 * 0x00 for ACPI (SCI), 0x01 for USB, 0x02 for IDE0, 0x04 for IDE1,
 * and 0x03 for SMBus.
 */
static int pirq_serverworks_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	outb_p(pirq, 0xc00);
	return inb(0xc01) & 0xf;
}

static int pirq_serverworks_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	outb_p(pirq, 0xc00);
	outb_p(irq, 0xc01);
	return 1;
}

/* Support for AMD756 PCI IRQ Routing
 * Jhon H. Caicedo <jhcaiced@osso.org.co>
 * Jun/21/2001 0.2.0 Release, fixed to use "nybble" functions... (jhcaiced)
 * Jun/19/2001 Alpha Release 0.1.0 (jhcaiced)
 * The AMD756 pirq rules are nibble-based
 * offset 0x56 0-3 PIRQA  4-7  PIRQB
 * offset 0x57 0-3 PIRQC  4-7  PIRQD
 */
static int pirq_amd756_get(struct pci_dev *router, struct pci_dev *dev, int pirq)
{
	u8 irq;
	irq = 0;
	if (pirq <= 4)
	{
		irq = read_config_nybble(router, 0x56, pirq - 1);
	}
	printk(KERN_INFO "AMD756: dev %04x:%04x, router pirq : %d get irq : %2d\n",
		dev->vendor, dev->device, pirq, irq);
	return irq;
}

static int pirq_amd756_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	printk(KERN_INFO "AMD756: dev %04x:%04x, router pirq : %d SET irq : %2d\n", 
		dev->vendor, dev->device, pirq, irq);
	if (pirq <= 4)
	{
		write_config_nybble(router, 0x56, pirq - 1, irq);
	}
	return 1;
}

#ifdef CONFIG_PCI_BIOS

static int pirq_bios_set(struct pci_dev *router, struct pci_dev *dev, int pirq, int irq)
{
	struct pci_dev *bridge;
	int pin = pci_get_interrupt_pin(dev, &bridge);
	return pcibios_set_irq_routing(bridge, pin, irq);
}

#endif

static __init int intel_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	static struct pci_device_id pirq_440gx[] = {
		{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82443GX_0) },
		{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82443GX_2) },
		{ },
	};

	/* 440GX has a proprietary PIRQ router -- don't use it */
	if (pci_dev_present(pirq_440gx))
		return 0;

	switch(device)
	{
		case PCI_DEVICE_ID_INTEL_82371FB_0:
		case PCI_DEVICE_ID_INTEL_82371SB_0:
		case PCI_DEVICE_ID_INTEL_82371AB_0:
		case PCI_DEVICE_ID_INTEL_82371MX:
		case PCI_DEVICE_ID_INTEL_82443MX_0:
		case PCI_DEVICE_ID_INTEL_82801AA_0:
		case PCI_DEVICE_ID_INTEL_82801AB_0:
		case PCI_DEVICE_ID_INTEL_82801BA_0:
		case PCI_DEVICE_ID_INTEL_82801BA_10:
		case PCI_DEVICE_ID_INTEL_82801CA_0:
		case PCI_DEVICE_ID_INTEL_82801CA_12:
		case PCI_DEVICE_ID_INTEL_82801DB_0:
		case PCI_DEVICE_ID_INTEL_82801E_0:
		case PCI_DEVICE_ID_INTEL_82801EB_0:
		case PCI_DEVICE_ID_INTEL_ESB_1:
		case PCI_DEVICE_ID_INTEL_ICH6_0:
		case PCI_DEVICE_ID_INTEL_ICH6_1:
		case PCI_DEVICE_ID_INTEL_ICH7_0:
		case PCI_DEVICE_ID_INTEL_ICH7_1:
		case PCI_DEVICE_ID_INTEL_ICH7_30:
		case PCI_DEVICE_ID_INTEL_ICH7_31:
		case PCI_DEVICE_ID_INTEL_ESB2_0:
			r->name = "PIIX/ICH";
			r->get = pirq_piix_get;
			r->set = pirq_piix_set;
			return 1;
	}
	return 0;
}

static __init int via_router_probe(struct irq_router *r,
				struct pci_dev *router, u16 device)
{
	/* FIXME: We should move some of the quirk fixup stuff here */

	/*
	 * work arounds for some buggy BIOSes
	 */
	if (device == PCI_DEVICE_ID_VIA_82C586_0) {
		switch(router->device) {
		case PCI_DEVICE_ID_VIA_82C686:
			/*
			 * Asus k7m bios wrongly reports 82C686A
			 * as 586-compatible
			 */
			device = PCI_DEVICE_ID_VIA_82C686;
			break;
		case PCI_DEVICE_ID_VIA_8235:
			/**
			 * Asus a7v-x bios wrongly reports 8235
			 * as 586-compatible
			 */
			device = PCI_DEVICE_ID_VIA_8235;
			break;
		}
	}

	switch(device) {
	case PCI_DEVICE_ID_VIA_82C586_0:
		r->name = "VIA";
		r->get = pirq_via586_get;
		r->set = pirq_via586_set;
		return 1;
	case PCI_DEVICE_ID_VIA_82C596:
	case PCI_DEVICE_ID_VIA_82C686:
	case PCI_DEVICE_ID_VIA_8231:
	case PCI_DEVICE_ID_VIA_8235:
		/* FIXME: add new ones for 8233/5 */
		r->name = "VIA";
		r->get = pirq_via_get;
		r->set = pirq_via_set;
		return 1;
	}
	return 0;
}

static __init int vlsi_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_VLSI_82C534:
			r->name = "VLSI 82C534";
			r->get = pirq_vlsi_get;
			r->set = pirq_vlsi_set;
			return 1;
	}
	return 0;
}


static __init int serverworks_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_SERVERWORKS_OSB4:
		case PCI_DEVICE_ID_SERVERWORKS_CSB5:
			r->name = "ServerWorks";
			r->get = pirq_serverworks_get;
			r->set = pirq_serverworks_set;
			return 1;
	}
	return 0;
}

static __init int sis_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	if (device != PCI_DEVICE_ID_SI_503)
		return 0;
		
	r->name = "SIS";
	r->get = pirq_sis_get;
	r->set = pirq_sis_set;
	return 1;
}

static __init int cyrix_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_CYRIX_5520:
			r->name = "NatSemi";
			r->get = pirq_cyrix_get;
			r->set = pirq_cyrix_set;
			return 1;
	}
	return 0;
}

static __init int opti_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_OPTI_82C700:
			r->name = "OPTI";
			r->get = pirq_opti_get;
			r->set = pirq_opti_set;
			return 1;
	}
	return 0;
}

static __init int ite_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_ITE_IT8330G_0:
			r->name = "ITE";
			r->get = pirq_ite_get;
			r->set = pirq_ite_set;
			return 1;
	}
	return 0;
}

static __init int ali_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
	case PCI_DEVICE_ID_AL_M1533:
	case PCI_DEVICE_ID_AL_M1563:
		printk(KERN_DEBUG "PCI: Using ALI IRQ Router\n");
		r->name = "ALI";
		r->get = pirq_ali_get;
		r->set = pirq_ali_set;
		return 1;
	}
	return 0;
}

static __init int amd_router_probe(struct irq_router *r, struct pci_dev *router, u16 device)
{
	switch(device)
	{
		case PCI_DEVICE_ID_AMD_VIPER_740B:
			r->name = "AMD756";
			break;
		case PCI_DEVICE_ID_AMD_VIPER_7413:
			r->name = "AMD766";
			break;
		case PCI_DEVICE_ID_AMD_VIPER_7443:
			r->name = "AMD768";
			break;
		default:
			return 0;
	}
	r->get = pirq_amd756_get;
	r->set = pirq_amd756_set;
	return 1;
}
		
static __initdata struct irq_router_handler pirq_routers[] = {
	{ PCI_VENDOR_ID_INTEL, intel_router_probe },
	{ PCI_VENDOR_ID_AL, ali_router_probe },
	{ PCI_VENDOR_ID_ITE, ite_router_probe },
	{ PCI_VENDOR_ID_VIA, via_router_probe },
	{ PCI_VENDOR_ID_OPTI, opti_router_probe },
	{ PCI_VENDOR_ID_SI, sis_router_probe },
	{ PCI_VENDOR_ID_CYRIX, cyrix_router_probe },
	{ PCI_VENDOR_ID_VLSI, vlsi_router_probe },
	{ PCI_VENDOR_ID_SERVERWORKS, serverworks_router_probe },
	{ PCI_VENDOR_ID_AMD, amd_router_probe },
	/* Someone with docs needs to add the ATI Radeon IGP */
	{ 0, NULL }
};
static struct irq_router pirq_router;
static struct pci_dev *pirq_router_dev;


/*
 *	FIXME: should we have an option to say "generic for
 *	chipset" ?
 */
 
static void __init pirq_find_router(struct irq_router *r)
{
	struct irq_routing_table *rt = pirq_table;
	struct irq_router_handler *h;

#ifdef CONFIG_PCI_BIOS
	if (!rt->signature) {
		printk(KERN_INFO "PCI: Using BIOS for IRQ routing\n");
		r->set = pirq_bios_set;
		r->name = "BIOS";
		return;
	}
#endif

	/* Default unless a driver reloads it */
	r->name = "default";
	r->get = NULL;
	r->set = NULL;
	
	DBG(KERN_DEBUG "PCI: Attempting to find IRQ router for %04x:%04x\n",
	    rt->rtr_vendor, rt->rtr_device);

	pirq_router_dev = pci_find_slot(rt->rtr_bus, rt->rtr_devfn);
	if (!pirq_router_dev) {
		DBG(KERN_DEBUG "PCI: Interrupt router not found at "
			"%02x:%02x\n", rt->rtr_bus, rt->rtr_devfn);
		return;
	}

	for( h = pirq_routers; h->vendor; h++) {
		/* First look for a router match */
		if (rt->rtr_vendor == h->vendor && h->probe(r, pirq_router_dev, rt->rtr_device))
			break;
		/* Fall back to a device match */
		if (pirq_router_dev->vendor == h->vendor && h->probe(r, pirq_router_dev, pirq_router_dev->device))
			break;
	}
	printk(KERN_INFO "PCI: Using IRQ router %s [%04x/%04x] at %s\n",
		pirq_router.name,
		pirq_router_dev->vendor,
		pirq_router_dev->device,
		pci_name(pirq_router_dev));
}

static struct irq_info *pirq_get_info(struct pci_dev *dev)
{
	struct irq_routing_table *rt = pirq_table;
	int entries = (rt->size - sizeof(struct irq_routing_table)) / sizeof(struct irq_info);
	struct irq_info *info;

	for (info = rt->slots; entries--; info++)
		if (info->bus == dev->bus->number && PCI_SLOT(info->devfn) == PCI_SLOT(dev->devfn))
			return info;
	return NULL;
}

static int pcibios_lookup_irq(struct pci_dev *dev, int assign)
{
	u8 pin;
	struct irq_info *info;
	int i, pirq, newirq;
	int irq = 0;
	u32 mask;
	struct irq_router *r = &pirq_router;
	struct pci_dev *dev2 = NULL;
	char *msg = NULL;

	/* Find IRQ pin */
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	if (!pin) {
		DBG(KERN_DEBUG " -> no interrupt pin\n");
		return 0;
	}
	pin = pin - 1;

	/* Find IRQ routing entry */

	if (!pirq_table)
		return 0;
	
	DBG(KERN_DEBUG "IRQ for %s[%c]", pci_name(dev), 'A' + pin);
	info = pirq_get_info(dev);
	if (!info) {
		DBG(" -> not found in routing table\n" KERN_DEBUG);
		return 0;
	}
	pirq = info->irq[pin].link;
	mask = info->irq[pin].bitmap;
	if (!pirq) {
		DBG(" -> not routed\n" KERN_DEBUG);
		return 0;
	}
	DBG(" -> PIRQ %02x, mask %04x, excl %04x", pirq, mask, pirq_table->exclusive_irqs);
	mask &= pcibios_irq_mask;

	/* Work around broken HP Pavilion Notebooks which assign USB to
	   IRQ 9 even though it is actually wired to IRQ 11 */

	if (broken_hp_bios_irq9 && pirq == 0x59 && dev->irq == 9) {
		dev->irq = 11;
		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, 11);
		r->set(pirq_router_dev, dev, pirq, 11);
	}

	/* same for Acer Travelmate 360, but with CB and irq 11 -> 10 */
	if (acer_tm360_irqrouting && dev->irq == 11 && dev->vendor == PCI_VENDOR_ID_O2) {
		pirq = 0x68;
		mask = 0x400;
		dev->irq = r->get(pirq_router_dev, dev, pirq);
		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
	}

	/*
	 * Find the best IRQ to assign: use the one
	 * reported by the device if possible.
	 */
	newirq = dev->irq;
	if (newirq && !((1 << newirq) & mask)) {
		if ( pci_probe & PCI_USE_PIRQ_MASK) newirq = 0;
		else printk("\n" KERN_WARNING
			"PCI: IRQ %i for device %s doesn't match PIRQ mask "
			"- try pci=usepirqmask\n" KERN_DEBUG, newirq,
			pci_name(dev));
	}
	if (!newirq && assign) {
		for (i = 0; i < 16; i++) {
			if (!(mask & (1 << i)))
				continue;
			if (pirq_penalty[i] < pirq_penalty[newirq] && can_request_irq(i, SA_SHIRQ))
				newirq = i;
		}
	}
	DBG(" -> newirq=%d", newirq);

	/* Check if it is hardcoded */
	if ((pirq & 0xf0) == 0xf0) {
		irq = pirq & 0xf;
		DBG(" -> hardcoded IRQ %d\n", irq);
		msg = "Hardcoded";
	} else if ( r->get && (irq = r->get(pirq_router_dev, dev, pirq)) && \
	((!(pci_probe & PCI_USE_PIRQ_MASK)) || ((1 << irq) & mask)) ) {
		DBG(" -> got IRQ %d\n", irq);
		msg = "Found";
	} else if (newirq && r->set && (dev->class >> 8) != PCI_CLASS_DISPLAY_VGA) {
		DBG(" -> assigning IRQ %d", newirq);
		if (r->set(pirq_router_dev, dev, pirq, newirq)) {
			eisa_set_level_irq(newirq);
			DBG(" ... OK\n");
			msg = "Assigned";
			irq = newirq;
		}
	}

	if (!irq) {
		DBG(" ... failed\n");
		if (newirq && mask == (1 << newirq)) {
			msg = "Guessed";
			irq = newirq;
		} else
			return 0;
	}
	printk(KERN_INFO "PCI: %s IRQ %d for device %s\n", msg, irq, pci_name(dev));

	/* Update IRQ for all devices with the same pirq value */
	while ((dev2 = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev2)) != NULL) {
		pci_read_config_byte(dev2, PCI_INTERRUPT_PIN, &pin);
		if (!pin)
			continue;
		pin--;
		info = pirq_get_info(dev2);
		if (!info)
			continue;
		if (info->irq[pin].link == pirq) {
			/* We refuse to override the dev->irq information. Give a warning! */
		    	if ( dev2->irq && dev2->irq != irq && \
			(!(pci_probe & PCI_USE_PIRQ_MASK) || \
			((1 << dev2->irq) & mask)) ) {
#ifndef CONFIG_PCI_MSI
		    		printk(KERN_INFO "IRQ routing conflict for %s, have irq %d, want irq %d\n",
				       pci_name(dev2), dev2->irq, irq);
#endif
		    		continue;
		    	}
			dev2->irq = irq;
			pirq_penalty[irq]++;
			if (dev != dev2)
				printk(KERN_INFO "PCI: Sharing IRQ %d with %s\n", irq, pci_name(dev2));
		}
	}
	return 1;
}

static void __init pcibios_fixup_irqs(void)
{
	struct pci_dev *dev = NULL;
	u8 pin;

	DBG(KERN_DEBUG "PCI: IRQ fixup\n");
	while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		/*
		 * If the BIOS has set an out of range IRQ number, just ignore it.
		 * Also keep track of which IRQ's are already in use.
		 */
		if (dev->irq >= 16) {
			DBG(KERN_DEBUG "%s: ignoring bogus IRQ %d\n", pci_name(dev), dev->irq);
			dev->irq = 0;
		}
		/* If the IRQ is already assigned to a PCI device, ignore its ISA use penalty */
		if (pirq_penalty[dev->irq] >= 100 && pirq_penalty[dev->irq] < 100000)
			pirq_penalty[dev->irq] = 0;
		pirq_penalty[dev->irq]++;
	}

	dev = NULL;
	while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
#ifdef CONFIG_X86_IO_APIC
		/*
		 * Recalculate IRQ numbers if we use the I/O APIC.
		 */
		if (io_apic_assign_pci_irqs)
		{
			int irq;

			if (pin) {
				pin--;		/* interrupt pins are numbered starting from 1 */
				irq = IO_APIC_get_PCI_irq_vector(dev->bus->number, PCI_SLOT(dev->devfn), pin);
	/*
	 * Busses behind bridges are typically not listed in the MP-table.
	 * In this case we have to look up the IRQ based on the parent bus,
	 * parent slot, and pin number. The SMP code detects such bridged
	 * busses itself so we should get into this branch reliably.
	 */
				if (irq < 0 && dev->bus->parent) { /* go back to the bridge */
					struct pci_dev * bridge = dev->bus->self;

					pin = (pin + PCI_SLOT(dev->devfn)) % 4;
					irq = IO_APIC_get_PCI_irq_vector(bridge->bus->number, 
							PCI_SLOT(bridge->devfn), pin);
					if (irq >= 0)
						printk(KERN_WARNING "PCI: using PPB %s[%c] to get irq %d\n",
							pci_name(bridge), 'A' + pin, irq);
				}
				if (irq >= 0) {
					if (use_pci_vector() &&
						!platform_legacy_irq(irq))
						irq = IO_APIC_VECTOR(irq);

					printk(KERN_INFO "PCI->APIC IRQ transform: %s[%c] -> IRQ %d\n",
						pci_name(dev), 'A' + pin, irq);
					dev->irq = irq;
				}
			}
		}
#endif
		/*
		 * Still no IRQ? Try to lookup one...
		 */
		if (pin && !dev->irq)
			pcibios_lookup_irq(dev, 0);
	}
}

/*
 * Work around broken HP Pavilion Notebooks which assign USB to
 * IRQ 9 even though it is actually wired to IRQ 11
 */
static int __init fix_broken_hp_bios_irq9(struct dmi_system_id *d)
{
	if (!broken_hp_bios_irq9) {
		broken_hp_bios_irq9 = 1;
		printk(KERN_INFO "%s detected - fixing broken IRQ routing\n", d->ident);
	}
	return 0;
}

/*
 * Work around broken Acer TravelMate 360 Notebooks which assign
 * Cardbus to IRQ 11 even though it is actually wired to IRQ 10
 */
static int __init fix_acer_tm360_irqrouting(struct dmi_system_id *d)
{
	if (!acer_tm360_irqrouting) {
		acer_tm360_irqrouting = 1;
		printk(KERN_INFO "%s detected - fixing broken IRQ routing\n", d->ident);
	}
	return 0;
}

static struct dmi_system_id __initdata pciirq_dmi_table[] = {
	{
		.callback = fix_broken_hp_bios_irq9,
		.ident = "HP Pavilion N5400 Series Laptop",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_BIOS_VERSION, "GE.M1.03"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "HP Pavilion Notebook Model GE"),
			DMI_MATCH(DMI_BOARD_VERSION, "OmniBook N32N-736"),
		},
	},
	{
		.callback = fix_acer_tm360_irqrouting,
		.ident = "Acer TravelMate 36x Laptop",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 360"),
		},
	},
	{ }
};

static int __init pcibios_irq_init(void)
{
	DBG(KERN_DEBUG "PCI: IRQ init\n");

	if (pcibios_enable_irq || raw_pci_ops == NULL)
		return 0;

	dmi_check_system(pciirq_dmi_table);

	pirq_table = pirq_find_routing_table();

#ifdef CONFIG_PCI_BIOS
	if (!pirq_table && (pci_probe & PCI_BIOS_IRQ_SCAN))
		pirq_table = pcibios_get_irq_routing_table();
#endif
	if (pirq_table) {
		pirq_peer_trick();
		pirq_find_router(&pirq_router);
		if (pirq_table->exclusive_irqs) {
			int i;
			for (i=0; i<16; i++)
				if (!(pirq_table->exclusive_irqs & (1 << i)))
					pirq_penalty[i] += 100;
		}
		/* If we're using the I/O APIC, avoid using the PCI IRQ routing table */
		if (io_apic_assign_pci_irqs)
			pirq_table = NULL;
	}

	pcibios_enable_irq = pirq_enable_irq;

	pcibios_fixup_irqs();
	return 0;
}

subsys_initcall(pcibios_irq_init);


static void pirq_penalize_isa_irq(int irq, int active)
{
	/*
	 *  If any ISAPnP device reports an IRQ in its list of possible
	 *  IRQ's, we try to avoid assigning it to PCI devices.
	 */
	if (irq < 16) {
		if (active)
			pirq_penalty[irq] += 1000;
		else
			pirq_penalty[irq] += 100;
	}
}

void pcibios_penalize_isa_irq(int irq, int active)
{
#ifdef CONFIG_ACPI
	if (!acpi_noirq)
		acpi_penalize_isa_irq(irq, active);
	else
#endif
		pirq_penalize_isa_irq(irq, active);
}

static int pirq_enable_irq(struct pci_dev *dev)
{
	u8 pin;
	struct pci_dev *temp_dev;

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	if (pin && !pcibios_lookup_irq(dev, 1) && !dev->irq) {
		char *msg = "";

		pin--;		/* interrupt pins are numbered starting from 1 */

		if (io_apic_assign_pci_irqs) {
			int irq;

			irq = IO_APIC_get_PCI_irq_vector(dev->bus->number, PCI_SLOT(dev->devfn), pin);
			/*
			 * Busses behind bridges are typically not listed in the MP-table.
			 * In this case we have to look up the IRQ based on the parent bus,
			 * parent slot, and pin number. The SMP code detects such bridged
			 * busses itself so we should get into this branch reliably.
			 */
			temp_dev = dev;
			while (irq < 0 && dev->bus->parent) { /* go back to the bridge */
				struct pci_dev * bridge = dev->bus->self;

				pin = (pin + PCI_SLOT(dev->devfn)) % 4;
				irq = IO_APIC_get_PCI_irq_vector(bridge->bus->number, 
						PCI_SLOT(bridge->devfn), pin);
				if (irq >= 0)
					printk(KERN_WARNING "PCI: using PPB %s[%c] to get irq %d\n",
						pci_name(bridge), 'A' + pin, irq);
				dev = bridge;
			}
			dev = temp_dev;
			if (irq >= 0) {
#ifdef CONFIG_PCI_MSI
				if (!platform_legacy_irq(irq))
					irq = IO_APIC_VECTOR(irq);
#endif
				printk(KERN_INFO "PCI->APIC IRQ transform: %s[%c] -> IRQ %d\n",
					pci_name(dev), 'A' + pin, irq);
				dev->irq = irq;
				return 0;
			} else
				msg = " Probably buggy MP table.";
		} else if (pci_probe & PCI_BIOS_IRQ_SCAN)
			msg = "";
		else
			msg = " Please try using pci=biosirq.";

		/* With IDE legacy devices the IRQ lookup failure is not a problem.. */
		if (dev->class >> 8 == PCI_CLASS_STORAGE_IDE && !(dev->class & 0x5))
			return 0;

		printk(KERN_WARNING "PCI: No IRQ known for interrupt pin %c of device %s.%s\n",
		       'A' + pin, pci_name(dev), msg);
	}
	return 0;
}

int pci_vector_resources(int last, int nr_released)
{
	int count = nr_released;

	int next = last;
	int offset = (last % 8);

	while (next < FIRST_SYSTEM_VECTOR) {
		next += 8;
#ifdef CONFIG_X86_64
		if (next == IA32_SYSCALL_VECTOR)
			continue;
#else
		if (next == SYSCALL_VECTOR)
			continue;
#endif
		count++;
		if (next >= FIRST_SYSTEM_VECTOR) {
			if (offset%8) {
				next = FIRST_DEVICE_VECTOR + offset;
				offset++;
				continue;
			}
			count--;
		}
	}

	return count;
}
