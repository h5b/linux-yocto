/*
 * Device driver for the via-pmu on Apple Powermacs.
 *
 * The VIA (versatile interface adapter) interfaces to the PMU,
 * a 6805 microprocessor core whose primary function is to control
 * battery charging and system power on the PowerBook 3400 and 2400.
 * The PMU also controls the ADB (Apple Desktop Bus) which connects
 * to the keyboard and mouse, as well as the non-volatile RAM
 * and the RTC (real time clock) chip.
 *
 * Copyright (C) 1998 Paul Mackerras and Fabio Riccardi.
 * Copyright (C) 2001-2002 Benjamin Herrenschmidt
 *
 * THIS DRIVER IS BECOMING A TOTAL MESS !
 *  - Cleanup atomically disabling reply to PMU events after
 *    a sleep or a freq. switch
 *  - Move sleep code out of here to pmac_pm, merge into new
 *    common PM infrastructure
 *  - Move backlight code out as well
 *  - Save/Restore PCI space properly
 *
 */
#include <stdarg.h>
#include <linux/config.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/adb.h>
#include <linux/pmu.h>
#include <linux/cuda.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/sysdev.h>
#include <linux/suspend.h>
#include <linux/syscalls.h>
#include <linux/cpu.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/sections.h>
#include <asm/irq.h>
#include <asm/pmac_feature.h>
#include <asm/pmac_pfunc.h>
#include <asm/pmac_low_i2c.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cputable.h>
#include <asm/time.h>
#ifdef CONFIG_PMAC_BACKLIGHT
#include <asm/backlight.h>
#endif

#ifdef CONFIG_PPC32
#include <asm/open_pic.h>
#endif

/* Some compile options */
#undef SUSPEND_USES_PMU
#define DEBUG_SLEEP
#undef HACKED_PCI_SAVE

/* Misc minor number allocated for /dev/pmu */
#define PMU_MINOR		154

/* How many iterations between battery polls */
#define BATTERY_POLLING_COUNT	2

static volatile unsigned char __iomem *via;

/* VIA registers - spaced 0x200 bytes apart */
#define RS		0x200		/* skip between registers */
#define B		0		/* B-side data */
#define A		RS		/* A-side data */
#define DIRB		(2*RS)		/* B-side direction (1=output) */
#define DIRA		(3*RS)		/* A-side direction (1=output) */
#define T1CL		(4*RS)		/* Timer 1 ctr/latch (low 8 bits) */
#define T1CH		(5*RS)		/* Timer 1 counter (high 8 bits) */
#define T1LL		(6*RS)		/* Timer 1 latch (low 8 bits) */
#define T1LH		(7*RS)		/* Timer 1 latch (high 8 bits) */
#define T2CL		(8*RS)		/* Timer 2 ctr/latch (low 8 bits) */
#define T2CH		(9*RS)		/* Timer 2 counter (high 8 bits) */
#define SR		(10*RS)		/* Shift register */
#define ACR		(11*RS)		/* Auxiliary control register */
#define PCR		(12*RS)		/* Peripheral control register */
#define IFR		(13*RS)		/* Interrupt flag register */
#define IER		(14*RS)		/* Interrupt enable register */
#define ANH		(15*RS)		/* A-side data, no handshake */

/* Bits in B data register: both active low */
#define TACK		0x08		/* Transfer acknowledge (input) */
#define TREQ		0x10		/* Transfer request (output) */

/* Bits in ACR */
#define SR_CTRL		0x1c		/* Shift register control bits */
#define SR_EXT		0x0c		/* Shift on external clock */
#define SR_OUT		0x10		/* Shift out if 1 */

/* Bits in IFR and IER */
#define IER_SET		0x80		/* set bits in IER */
#define IER_CLR		0		/* clear bits in IER */
#define SR_INT		0x04		/* Shift register full/empty */
#define CB2_INT		0x08
#define CB1_INT		0x10		/* transition on CB1 input */

static volatile enum pmu_state {
	idle,
	sending,
	intack,
	reading,
	reading_intr,
	locked,
} pmu_state;

static volatile enum int_data_state {
	int_data_empty,
	int_data_fill,
	int_data_ready,
	int_data_flush
} int_data_state[2] = { int_data_empty, int_data_empty };

static struct adb_request *current_req;
static struct adb_request *last_req;
static struct adb_request *req_awaiting_reply;
static unsigned char interrupt_data[2][32];
static int interrupt_data_len[2];
static int int_data_last;
static unsigned char *reply_ptr;
static int data_index;
static int data_len;
static volatile int adb_int_pending;
static volatile int disable_poll;
static struct adb_request bright_req_1, bright_req_2;
static struct device_node *vias;
static int pmu_kind = PMU_UNKNOWN;
static int pmu_fully_inited = 0;
static int pmu_has_adb;
static struct device_node *gpio_node;
static unsigned char __iomem *gpio_reg = NULL;
static int gpio_irq = -1;
static int gpio_irq_enabled = -1;
static volatile int pmu_suspended = 0;
static spinlock_t pmu_lock;
static u8 pmu_intr_mask;
static int pmu_version;
static int drop_interrupts;
#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
static int option_lid_wakeup = 1;
#endif /* CONFIG_PM && CONFIG_PPC32 */
static int sleep_in_progress;
static unsigned long async_req_locks;
static unsigned int pmu_irq_stats[11];

static struct proc_dir_entry *proc_pmu_root;
static struct proc_dir_entry *proc_pmu_info;
static struct proc_dir_entry *proc_pmu_irqstats;
static struct proc_dir_entry *proc_pmu_options;
static int option_server_mode;

int pmu_battery_count;
int pmu_cur_battery;
unsigned int pmu_power_flags;
struct pmu_battery_info pmu_batteries[PMU_MAX_BATTERIES];
static int query_batt_timer = BATTERY_POLLING_COUNT;
static struct adb_request batt_req;
static struct proc_dir_entry *proc_pmu_batt[PMU_MAX_BATTERIES];

#if defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT)
extern int disable_kernel_backlight;
#endif /* defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT) */

int __fake_sleep;
int asleep;
struct notifier_block *sleep_notifier_list;

#ifdef CONFIG_ADB
static int adb_dev_map = 0;
static int pmu_adb_flags;

static int pmu_probe(void);
static int pmu_init(void);
static int pmu_send_request(struct adb_request *req, int sync);
static int pmu_adb_autopoll(int devs);
static int pmu_adb_reset_bus(void);
#endif /* CONFIG_ADB */

static int init_pmu(void);
static void pmu_start(void);
static irqreturn_t via_pmu_interrupt(int irq, void *arg, struct pt_regs *regs);
static irqreturn_t gpio1_interrupt(int irq, void *arg, struct pt_regs *regs);
static int proc_get_info(char *page, char **start, off_t off,
			  int count, int *eof, void *data);
static int proc_get_irqstats(char *page, char **start, off_t off,
			  int count, int *eof, void *data);
#ifdef CONFIG_PMAC_BACKLIGHT
static int pmu_set_backlight_level(int level, void* data);
static int pmu_set_backlight_enable(int on, int level, void* data);
#endif /* CONFIG_PMAC_BACKLIGHT */
static void pmu_pass_intr(unsigned char *data, int len);
static int proc_get_batt(char *page, char **start, off_t off,
			int count, int *eof, void *data);
static int proc_read_options(char *page, char **start, off_t off,
			int count, int *eof, void *data);
static int proc_write_options(struct file *file, const char __user *buffer,
			unsigned long count, void *data);

#ifdef CONFIG_ADB
struct adb_driver via_pmu_driver = {
	"PMU",
	pmu_probe,
	pmu_init,
	pmu_send_request,
	pmu_adb_autopoll,
	pmu_poll_adb,
	pmu_adb_reset_bus
};
#endif /* CONFIG_ADB */

extern void low_sleep_handler(void);
extern void enable_kernel_altivec(void);
extern void enable_kernel_fp(void);

#ifdef DEBUG_SLEEP
int pmu_polled_request(struct adb_request *req);
int pmu_wink(struct adb_request *req);
#endif

/*
 * This table indicates for each PMU opcode:
 * - the number of data bytes to be sent with the command, or -1
 *   if a length byte should be sent,
 * - the number of response bytes which the PMU will return, or
 *   -1 if it will send a length byte.
 */
static const s8 pmu_data_len[256][2] = {
/*	   0	   1	   2	   3	   4	   5	   6	   7  */
/*00*/	{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*08*/	{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*10*/	{ 1, 0},{ 1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*18*/	{ 0, 1},{ 0, 1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{ 0, 0},
/*20*/	{-1, 0},{ 0, 0},{ 2, 0},{ 1, 0},{ 1, 0},{-1, 0},{-1, 0},{-1, 0},
/*28*/	{ 0,-1},{ 0,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{ 0,-1},
/*30*/	{ 4, 0},{20, 0},{-1, 0},{ 3, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*38*/	{ 0, 4},{ 0,20},{ 2,-1},{ 2, 1},{ 3,-1},{-1,-1},{-1,-1},{ 4, 0},
/*40*/	{ 1, 0},{ 1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*48*/	{ 0, 1},{ 0, 1},{-1,-1},{ 1, 0},{ 1, 0},{-1,-1},{-1,-1},{-1,-1},
/*50*/	{ 1, 0},{ 0, 0},{ 2, 0},{ 2, 0},{-1, 0},{ 1, 0},{ 3, 0},{ 1, 0},
/*58*/	{ 0, 1},{ 1, 0},{ 0, 2},{ 0, 2},{ 0,-1},{-1,-1},{-1,-1},{-1,-1},
/*60*/	{ 2, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*68*/	{ 0, 3},{ 0, 3},{ 0, 2},{ 0, 8},{ 0,-1},{ 0,-1},{-1,-1},{-1,-1},
/*70*/	{ 1, 0},{ 1, 0},{ 1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*78*/	{ 0,-1},{ 0,-1},{-1,-1},{-1,-1},{-1,-1},{ 5, 1},{ 4, 1},{ 4, 1},
/*80*/	{ 4, 0},{-1, 0},{ 0, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*88*/	{ 0, 5},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*90*/	{ 1, 0},{ 2, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*98*/	{ 0, 1},{ 0, 1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*a0*/	{ 2, 0},{ 2, 0},{ 2, 0},{ 4, 0},{-1, 0},{ 0, 0},{-1, 0},{-1, 0},
/*a8*/	{ 1, 1},{ 1, 0},{ 3, 0},{ 2, 0},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*b0*/	{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*b8*/	{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*c0*/	{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*c8*/	{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
/*d0*/	{ 0, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*d8*/	{ 1, 1},{ 1, 1},{-1,-1},{-1,-1},{ 0, 1},{ 0,-1},{-1,-1},{-1,-1},
/*e0*/	{-1, 0},{ 4, 0},{ 0, 1},{-1, 0},{-1, 0},{ 4, 0},{-1, 0},{-1, 0},
/*e8*/	{ 3,-1},{-1,-1},{ 0, 1},{-1,-1},{ 0,-1},{-1,-1},{-1,-1},{ 0, 0},
/*f0*/	{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},{-1, 0},
/*f8*/	{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1},
};

static char *pbook_type[] = {
	"Unknown PowerBook",
	"PowerBook 2400/3400/3500(G3)",
	"PowerBook G3 Series",
	"1999 PowerBook G3",
	"Core99"
};

#ifdef CONFIG_PMAC_BACKLIGHT
static struct backlight_controller pmu_backlight_controller = {
	pmu_set_backlight_enable,
	pmu_set_backlight_level
};
#endif /* CONFIG_PMAC_BACKLIGHT */

int __init find_via_pmu(void)
{
	u64 taddr;
	u32 *reg;

	if (via != 0)
		return 1;
	vias = of_find_node_by_name(NULL, "via-pmu");
	if (vias == NULL)
		return 0;

	reg = (u32 *)get_property(vias, "reg", NULL);
	if (reg == NULL) {
		printk(KERN_ERR "via-pmu: No \"reg\" property !\n");
		goto fail;
	}
	taddr = of_translate_address(vias, reg);
	if (taddr == OF_BAD_ADDR) {
		printk(KERN_ERR "via-pmu: Can't translate address !\n");
		goto fail;
	}

	spin_lock_init(&pmu_lock);

	pmu_has_adb = 1;

	pmu_intr_mask =	PMU_INT_PCEJECT |
			PMU_INT_SNDBRT |
			PMU_INT_ADB |
			PMU_INT_TICK;
	
	if (vias->parent->name && ((strcmp(vias->parent->name, "ohare") == 0)
	    || device_is_compatible(vias->parent, "ohare")))
		pmu_kind = PMU_OHARE_BASED;
	else if (device_is_compatible(vias->parent, "paddington"))
		pmu_kind = PMU_PADDINGTON_BASED;
	else if (device_is_compatible(vias->parent, "heathrow"))
		pmu_kind = PMU_HEATHROW_BASED;
	else if (device_is_compatible(vias->parent, "Keylargo")
		 || device_is_compatible(vias->parent, "K2-Keylargo")) {
		struct device_node *gpiop;
		u64 gaddr = OF_BAD_ADDR;

		pmu_kind = PMU_KEYLARGO_BASED;
		pmu_has_adb = (find_type_devices("adb") != NULL);
		pmu_intr_mask =	PMU_INT_PCEJECT |
				PMU_INT_SNDBRT |
				PMU_INT_ADB |
				PMU_INT_TICK |
				PMU_INT_ENVIRONMENT;
		
		gpiop = of_find_node_by_name(NULL, "gpio");
		if (gpiop) {
			reg = (u32 *)get_property(gpiop, "reg", NULL);
			if (reg)
				gaddr = of_translate_address(gpiop, reg);
			if (gaddr != OF_BAD_ADDR)
				gpio_reg = ioremap(gaddr, 0x10);
		}
		if (gpio_reg == NULL)
			printk(KERN_ERR "via-pmu: Can't find GPIO reg !\n");
	} else
		pmu_kind = PMU_UNKNOWN;

	via = ioremap(taddr, 0x2000);
	if (via == NULL) {
		printk(KERN_ERR "via-pmu: Can't map address !\n");
		goto fail;
	}
	
	out_8(&via[IER], IER_CLR | 0x7f);	/* disable all intrs */
	out_8(&via[IFR], 0x7f);			/* clear IFR */

	pmu_state = idle;

	if (!init_pmu()) {
		via = NULL;
		return 0;
	}

	printk(KERN_INFO "PMU driver v%d initialized for %s, firmware: %02x\n",
	       PMU_DRIVER_VERSION, pbook_type[pmu_kind], pmu_version);
	       
	sys_ctrler = SYS_CTRLER_PMU;
	
	return 1;
 fail:
	of_node_put(vias);
	vias = NULL;
	return 0;
}

#ifdef CONFIG_ADB
static int pmu_probe(void)
{
	return vias == NULL? -ENODEV: 0;
}

static int __init pmu_init(void)
{
	if (vias == NULL)
		return -ENODEV;
	return 0;
}
#endif /* CONFIG_ADB */

/*
 * We can't wait until pmu_init gets called, that happens too late.
 * It happens after IDE and SCSI initialization, which can take a few
 * seconds, and by that time the PMU could have given up on us and
 * turned us off.
 * Thus this is called with arch_initcall rather than device_initcall.
 */
static int __init via_pmu_start(void)
{
	if (vias == NULL)
		return -ENODEV;

	bright_req_1.complete = 1;
	bright_req_2.complete = 1;
	batt_req.complete = 1;

#ifndef CONFIG_PPC_MERGE
	if (pmu_kind == PMU_KEYLARGO_BASED)
		openpic_set_irq_priority(vias->intrs[0].line,
					 OPENPIC_PRIORITY_DEFAULT + 1);
#endif

	if (request_irq(vias->intrs[0].line, via_pmu_interrupt, 0, "VIA-PMU",
			(void *)0)) {
		printk(KERN_ERR "VIA-PMU: can't get irq %d\n",
		       vias->intrs[0].line);
		return -EAGAIN;
	}

	if (pmu_kind == PMU_KEYLARGO_BASED) {
		gpio_node = of_find_node_by_name(NULL, "extint-gpio1");
		if (gpio_node == NULL)
			gpio_node = of_find_node_by_name(NULL,
							 "pmu-interrupt");
		if (gpio_node && gpio_node->n_intrs > 0)
			gpio_irq = gpio_node->intrs[0].line;

		if (gpio_irq != -1) {
			if (request_irq(gpio_irq, gpio1_interrupt, 0,
					"GPIO1 ADB", (void *)0))
				printk(KERN_ERR "pmu: can't get irq %d"
				       " (GPIO1)\n", gpio_irq);
			else
				gpio_irq_enabled = 1;
		}
	}

	/* Enable interrupts */
	out_8(&via[IER], IER_SET | SR_INT | CB1_INT);

	pmu_fully_inited = 1;

	/* Make sure PMU settle down before continuing. This is _very_ important
	 * since the IDE probe may shut interrupts down for quite a bit of time. If
	 * a PMU communication is pending while this happens, the PMU may timeout
	 * Not that on Core99 machines, the PMU keeps sending us environement
	 * messages, we should find a way to either fix IDE or make it call
	 * pmu_suspend() before masking interrupts. This can also happens while
	 * scolling with some fbdevs.
	 */
	do {
		pmu_poll();
	} while (pmu_state != idle);

	return 0;
}

arch_initcall(via_pmu_start);

/*
 * This has to be done after pci_init, which is a subsys_initcall.
 */
static int __init via_pmu_dev_init(void)
{
	if (vias == NULL)
		return -ENODEV;

#ifdef CONFIG_PMAC_BACKLIGHT
	/* Enable backlight */
	register_backlight_controller(&pmu_backlight_controller, NULL, "pmu");
#endif /* CONFIG_PMAC_BACKLIGHT */

#ifdef CONFIG_PPC32
  	if (machine_is_compatible("AAPL,3400/2400") ||
  		machine_is_compatible("AAPL,3500")) {
		int mb = pmac_call_feature(PMAC_FTR_GET_MB_INFO,
			NULL, PMAC_MB_INFO_MODEL, 0);
		pmu_battery_count = 1;
		if (mb == PMAC_TYPE_COMET)
			pmu_batteries[0].flags |= PMU_BATT_TYPE_COMET;
		else
			pmu_batteries[0].flags |= PMU_BATT_TYPE_HOOPER;
	} else if (machine_is_compatible("AAPL,PowerBook1998") ||
		machine_is_compatible("PowerBook1,1")) {
		pmu_battery_count = 2;
		pmu_batteries[0].flags |= PMU_BATT_TYPE_SMART;
		pmu_batteries[1].flags |= PMU_BATT_TYPE_SMART;
	} else {
		struct device_node* prim = find_devices("power-mgt");
		u32 *prim_info = NULL;
		if (prim)
			prim_info = (u32 *)get_property(prim, "prim-info", NULL);
		if (prim_info) {
			/* Other stuffs here yet unknown */
			pmu_battery_count = (prim_info[6] >> 16) & 0xff;
			pmu_batteries[0].flags |= PMU_BATT_TYPE_SMART;
			if (pmu_battery_count > 1)
				pmu_batteries[1].flags |= PMU_BATT_TYPE_SMART;
		}
	}
#endif /* CONFIG_PPC32 */

	/* Create /proc/pmu */
	proc_pmu_root = proc_mkdir("pmu", NULL);
	if (proc_pmu_root) {
		long i;

		for (i=0; i<pmu_battery_count; i++) {
			char title[16];
			sprintf(title, "battery_%ld", i);
			proc_pmu_batt[i] = create_proc_read_entry(title, 0, proc_pmu_root,
						proc_get_batt, (void *)i);
		}

		proc_pmu_info = create_proc_read_entry("info", 0, proc_pmu_root,
					proc_get_info, NULL);
		proc_pmu_irqstats = create_proc_read_entry("interrupts", 0, proc_pmu_root,
					proc_get_irqstats, NULL);
		proc_pmu_options = create_proc_entry("options", 0600, proc_pmu_root);
		if (proc_pmu_options) {
			proc_pmu_options->nlink = 1;
			proc_pmu_options->read_proc = proc_read_options;
			proc_pmu_options->write_proc = proc_write_options;
		}
	}
	return 0;
}

device_initcall(via_pmu_dev_init);

static int
init_pmu(void)
{
	int timeout;
	struct adb_request req;

	out_8(&via[B], via[B] | TREQ);			/* negate TREQ */
	out_8(&via[DIRB], (via[DIRB] | TREQ) & ~TACK);	/* TACK in, TREQ out */

	pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, pmu_intr_mask);
	timeout =  100000;
	while (!req.complete) {
		if (--timeout < 0) {
			printk(KERN_ERR "init_pmu: no response from PMU\n");
			return 0;
		}
		udelay(10);
		pmu_poll();
	}

	/* ack all pending interrupts */
	timeout = 100000;
	interrupt_data[0][0] = 1;
	while (interrupt_data[0][0] || pmu_state != idle) {
		if (--timeout < 0) {
			printk(KERN_ERR "init_pmu: timed out acking intrs\n");
			return 0;
		}
		if (pmu_state == idle)
			adb_int_pending = 1;
		via_pmu_interrupt(0, NULL, NULL);
		udelay(10);
	}

	/* Tell PMU we are ready.  */
	if (pmu_kind == PMU_KEYLARGO_BASED) {
		pmu_request(&req, NULL, 2, PMU_SYSTEM_READY, 2);
		while (!req.complete)
			pmu_poll();
	}

	/* Read PMU version */
	pmu_request(&req, NULL, 1, PMU_GET_VERSION);
	pmu_wait_complete(&req);
	if (req.reply_len > 0)
		pmu_version = req.reply[0];
	
	/* Read server mode setting */
	if (pmu_kind == PMU_KEYLARGO_BASED) {
		pmu_request(&req, NULL, 2, PMU_POWER_EVENTS,
			    PMU_PWR_GET_POWERUP_EVENTS);
		pmu_wait_complete(&req);
		if (req.reply_len == 2) {
			if (req.reply[1] & PMU_PWR_WAKEUP_AC_INSERT)
				option_server_mode = 1;
			printk(KERN_INFO "via-pmu: Server Mode is %s\n",
			       option_server_mode ? "enabled" : "disabled");
		}
	}
	return 1;
}

int
pmu_get_model(void)
{
	return pmu_kind;
}

static void pmu_set_server_mode(int server_mode)
{
	struct adb_request req;

	if (pmu_kind != PMU_KEYLARGO_BASED)
		return;

	option_server_mode = server_mode;
	pmu_request(&req, NULL, 2, PMU_POWER_EVENTS, PMU_PWR_GET_POWERUP_EVENTS);
	pmu_wait_complete(&req);
	if (req.reply_len < 2)
		return;
	if (server_mode)
		pmu_request(&req, NULL, 4, PMU_POWER_EVENTS,
			    PMU_PWR_SET_POWERUP_EVENTS,
			    req.reply[0], PMU_PWR_WAKEUP_AC_INSERT); 
	else
		pmu_request(&req, NULL, 4, PMU_POWER_EVENTS,
			    PMU_PWR_CLR_POWERUP_EVENTS,
			    req.reply[0], PMU_PWR_WAKEUP_AC_INSERT); 
	pmu_wait_complete(&req);
}

/* This new version of the code for 2400/3400/3500 powerbooks
 * is inspired from the implementation in gkrellm-pmu
 */
static void
done_battery_state_ohare(struct adb_request* req)
{
	/* format:
	 *  [0]    :  flags
	 *    0x01 :  AC indicator
	 *    0x02 :  charging
	 *    0x04 :  battery exist
	 *    0x08 :  
	 *    0x10 :  
	 *    0x20 :  full charged
	 *    0x40 :  pcharge reset
	 *    0x80 :  battery exist
	 *
	 *  [1][2] :  battery voltage
	 *  [3]    :  CPU temperature
	 *  [4]    :  battery temperature
	 *  [5]    :  current
	 *  [6][7] :  pcharge
	 *              --tkoba
	 */
	unsigned int bat_flags = PMU_BATT_TYPE_HOOPER;
	long pcharge, charge, vb, vmax, lmax;
	long vmax_charging, vmax_charged;
	long amperage, voltage, time, max;
	int mb = pmac_call_feature(PMAC_FTR_GET_MB_INFO,
			NULL, PMAC_MB_INFO_MODEL, 0);

	if (req->reply[0] & 0x01)
		pmu_power_flags |= PMU_PWR_AC_PRESENT;
	else
		pmu_power_flags &= ~PMU_PWR_AC_PRESENT;
	
	if (mb == PMAC_TYPE_COMET) {
		vmax_charged = 189;
		vmax_charging = 213;
		lmax = 6500;
	} else {
		vmax_charged = 330;
		vmax_charging = 330;
		lmax = 6500;
	}
	vmax = vmax_charged;

	/* If battery installed */
	if (req->reply[0] & 0x04) {
		bat_flags |= PMU_BATT_PRESENT;
		if (req->reply[0] & 0x02)
			bat_flags |= PMU_BATT_CHARGING;
		vb = (req->reply[1] << 8) | req->reply[2];
		voltage = (vb * 265 + 72665) / 10;
		amperage = req->reply[5];
		if ((req->reply[0] & 0x01) == 0) {
			if (amperage > 200)
				vb += ((amperage - 200) * 15)/100;
		} else if (req->reply[0] & 0x02) {
			vb = (vb * 97) / 100;
			vmax = vmax_charging;
		}
		charge = (100 * vb) / vmax;
		if (req->reply[0] & 0x40) {
			pcharge = (req->reply[6] << 8) + req->reply[7];
			if (pcharge > lmax)
				pcharge = lmax;
			pcharge *= 100;
			pcharge = 100 - pcharge / lmax;
			if (pcharge < charge)
				charge = pcharge;
		}
		if (amperage > 0)
			time = (charge * 16440) / amperage;
		else
			time = 0;
		max = 100;
		amperage = -amperage;
	} else
		charge = max = amperage = voltage = time = 0;

	pmu_batteries[pmu_cur_battery].flags = bat_flags;
	pmu_batteries[pmu_cur_battery].charge = charge;
	pmu_batteries[pmu_cur_battery].max_charge = max;
	pmu_batteries[pmu_cur_battery].amperage = amperage;
	pmu_batteries[pmu_cur_battery].voltage = voltage;
	pmu_batteries[pmu_cur_battery].time_remaining = time;

	clear_bit(0, &async_req_locks);
}

static void
done_battery_state_smart(struct adb_request* req)
{
	/* format:
	 *  [0] : format of this structure (known: 3,4,5)
	 *  [1] : flags
	 *  
	 *  format 3 & 4:
	 *  
	 *  [2] : charge
	 *  [3] : max charge
	 *  [4] : current
	 *  [5] : voltage
	 *  
	 *  format 5:
	 *  
	 *  [2][3] : charge
	 *  [4][5] : max charge
	 *  [6][7] : current
	 *  [8][9] : voltage
	 */
	 
	unsigned int bat_flags = PMU_BATT_TYPE_SMART;
	int amperage;
	unsigned int capa, max, voltage;
	
	if (req->reply[1] & 0x01)
		pmu_power_flags |= PMU_PWR_AC_PRESENT;
	else
		pmu_power_flags &= ~PMU_PWR_AC_PRESENT;


	capa = max = amperage = voltage = 0;
	
	if (req->reply[1] & 0x04) {
		bat_flags |= PMU_BATT_PRESENT;
		switch(req->reply[0]) {
			case 3:
			case 4: capa = req->reply[2];
				max = req->reply[3];
				amperage = *((signed char *)&req->reply[4]);
				voltage = req->reply[5];
				break;
			case 5: capa = (req->reply[2] << 8) | req->reply[3];
				max = (req->reply[4] << 8) | req->reply[5];
				amperage = *((signed short *)&req->reply[6]);
				voltage = (req->reply[8] << 8) | req->reply[9];
				break;
			default:
				printk(KERN_WARNING "pmu.c : unrecognized battery info, len: %d, %02x %02x %02x %02x\n",
					req->reply_len, req->reply[0], req->reply[1], req->reply[2], req->reply[3]);
				break;
		}
	}

	if ((req->reply[1] & 0x01) && (amperage > 0))
		bat_flags |= PMU_BATT_CHARGING;

	pmu_batteries[pmu_cur_battery].flags = bat_flags;
	pmu_batteries[pmu_cur_battery].charge = capa;
	pmu_batteries[pmu_cur_battery].max_charge = max;
	pmu_batteries[pmu_cur_battery].amperage = amperage;
	pmu_batteries[pmu_cur_battery].voltage = voltage;
	if (amperage) {
		if ((req->reply[1] & 0x01) && (amperage > 0))
			pmu_batteries[pmu_cur_battery].time_remaining
				= ((max-capa) * 3600) / amperage;
		else
			pmu_batteries[pmu_cur_battery].time_remaining
				= (capa * 3600) / (-amperage);
	} else
		pmu_batteries[pmu_cur_battery].time_remaining = 0;

	pmu_cur_battery = (pmu_cur_battery + 1) % pmu_battery_count;

	clear_bit(0, &async_req_locks);
}

static void
query_battery_state(void)
{
	if (test_and_set_bit(0, &async_req_locks))
		return;
	if (pmu_kind == PMU_OHARE_BASED)
		pmu_request(&batt_req, done_battery_state_ohare,
			1, PMU_BATTERY_STATE);
	else
		pmu_request(&batt_req, done_battery_state_smart,
			2, PMU_SMART_BATTERY_STATE, pmu_cur_battery+1);
}

static int
proc_get_info(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	char* p = page;

	p += sprintf(p, "PMU driver version     : %d\n", PMU_DRIVER_VERSION);
	p += sprintf(p, "PMU firmware version   : %02x\n", pmu_version);
	p += sprintf(p, "AC Power               : %d\n",
		((pmu_power_flags & PMU_PWR_AC_PRESENT) != 0));
	p += sprintf(p, "Battery count          : %d\n", pmu_battery_count);

	return p - page;
}

static int
proc_get_irqstats(char *page, char **start, off_t off,
		  int count, int *eof, void *data)
{
	int i;
	char* p = page;
	static const char *irq_names[] = {
		"Total CB1 triggered events",
		"Total GPIO1 triggered events",
		"PC-Card eject button",
		"Sound/Brightness button",
		"ADB message",
		"Battery state change",
		"Environment interrupt",
		"Tick timer",
		"Ghost interrupt (zero len)",
		"Empty interrupt (empty mask)",
		"Max irqs in a row"
        };

	for (i=0; i<11; i++) {
		p += sprintf(p, " %2u: %10u (%s)\n",
			     i, pmu_irq_stats[i], irq_names[i]);
	}
	return p - page;
}

static int
proc_get_batt(char *page, char **start, off_t off,
		int count, int *eof, void *data)
{
	long batnum = (long)data;
	char *p = page;
	
	p += sprintf(p, "\n");
	p += sprintf(p, "flags      : %08x\n",
		pmu_batteries[batnum].flags);
	p += sprintf(p, "charge     : %d\n",
		pmu_batteries[batnum].charge);
	p += sprintf(p, "max_charge : %d\n",
		pmu_batteries[batnum].max_charge);
	p += sprintf(p, "current    : %d\n",
		pmu_batteries[batnum].amperage);
	p += sprintf(p, "voltage    : %d\n",
		pmu_batteries[batnum].voltage);
	p += sprintf(p, "time rem.  : %d\n",
		pmu_batteries[batnum].time_remaining);

	return p - page;
}

static int
proc_read_options(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	char *p = page;

#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
	if (pmu_kind == PMU_KEYLARGO_BASED &&
	    pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,-1) >= 0)
		p += sprintf(p, "lid_wakeup=%d\n", option_lid_wakeup);
#endif
	if (pmu_kind == PMU_KEYLARGO_BASED)
		p += sprintf(p, "server_mode=%d\n", option_server_mode);

	return p - page;
}
			
static int
proc_write_options(struct file *file, const char __user *buffer,
			unsigned long count, void *data)
{
	char tmp[33];
	char *label, *val;
	unsigned long fcount = count;
	
	if (!count)
		return -EINVAL;
	if (count > 32)
		count = 32;
	if (copy_from_user(tmp, buffer, count))
		return -EFAULT;
	tmp[count] = 0;

	label = tmp;
	while(*label == ' ')
		label++;
	val = label;
	while(*val && (*val != '=')) {
		if (*val == ' ')
			*val = 0;
		val++;
	}
	if ((*val) == 0)
		return -EINVAL;
	*(val++) = 0;
	while(*val == ' ')
		val++;
#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
	if (pmu_kind == PMU_KEYLARGO_BASED &&
	    pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,-1) >= 0)
		if (!strcmp(label, "lid_wakeup"))
			option_lid_wakeup = ((*val) == '1');
#endif
	if (pmu_kind == PMU_KEYLARGO_BASED && !strcmp(label, "server_mode")) {
		int new_value;
		new_value = ((*val) == '1');
		if (new_value != option_server_mode)
			pmu_set_server_mode(new_value);
	}
	return fcount;
}

#ifdef CONFIG_ADB
/* Send an ADB command */
static int
pmu_send_request(struct adb_request *req, int sync)
{
	int i, ret;

	if ((vias == NULL) || (!pmu_fully_inited)) {
		req->complete = 1;
		return -ENXIO;
	}

	ret = -EINVAL;

	switch (req->data[0]) {
	case PMU_PACKET:
		for (i = 0; i < req->nbytes - 1; ++i)
			req->data[i] = req->data[i+1];
		--req->nbytes;
		if (pmu_data_len[req->data[0]][1] != 0) {
			req->reply[0] = ADB_RET_OK;
			req->reply_len = 1;
		} else
			req->reply_len = 0;
		ret = pmu_queue_request(req);
		break;
	case CUDA_PACKET:
		switch (req->data[1]) {
		case CUDA_GET_TIME:
			if (req->nbytes != 2)
				break;
			req->data[0] = PMU_READ_RTC;
			req->nbytes = 1;
			req->reply_len = 3;
			req->reply[0] = CUDA_PACKET;
			req->reply[1] = 0;
			req->reply[2] = CUDA_GET_TIME;
			ret = pmu_queue_request(req);
			break;
		case CUDA_SET_TIME:
			if (req->nbytes != 6)
				break;
			req->data[0] = PMU_SET_RTC;
			req->nbytes = 5;
			for (i = 1; i <= 4; ++i)
				req->data[i] = req->data[i+1];
			req->reply_len = 3;
			req->reply[0] = CUDA_PACKET;
			req->reply[1] = 0;
			req->reply[2] = CUDA_SET_TIME;
			ret = pmu_queue_request(req);
			break;
		}
		break;
	case ADB_PACKET:
	    	if (!pmu_has_adb)
    			return -ENXIO;
		for (i = req->nbytes - 1; i > 1; --i)
			req->data[i+2] = req->data[i];
		req->data[3] = req->nbytes - 2;
		req->data[2] = pmu_adb_flags;
		/*req->data[1] = req->data[1];*/
		req->data[0] = PMU_ADB_CMD;
		req->nbytes += 2;
		req->reply_expected = 1;
		req->reply_len = 0;
		ret = pmu_queue_request(req);
		break;
	}
	if (ret) {
		req->complete = 1;
		return ret;
	}

	if (sync)
		while (!req->complete)
			pmu_poll();

	return 0;
}

/* Enable/disable autopolling */
static int
pmu_adb_autopoll(int devs)
{
	struct adb_request req;

	if ((vias == NULL) || (!pmu_fully_inited) || !pmu_has_adb)
		return -ENXIO;

	if (devs) {
		adb_dev_map = devs;
		pmu_request(&req, NULL, 5, PMU_ADB_CMD, 0, 0x86,
			    adb_dev_map >> 8, adb_dev_map);
		pmu_adb_flags = 2;
	} else {
		pmu_request(&req, NULL, 1, PMU_ADB_POLL_OFF);
		pmu_adb_flags = 0;
	}
	while (!req.complete)
		pmu_poll();
	return 0;
}

/* Reset the ADB bus */
static int
pmu_adb_reset_bus(void)
{
	struct adb_request req;
	int save_autopoll = adb_dev_map;

	if ((vias == NULL) || (!pmu_fully_inited) || !pmu_has_adb)
		return -ENXIO;

	/* anyone got a better idea?? */
	pmu_adb_autopoll(0);

	req.nbytes = 5;
	req.done = NULL;
	req.data[0] = PMU_ADB_CMD;
	req.data[1] = 0;
	req.data[2] = ADB_BUSRESET;
	req.data[3] = 0;
	req.data[4] = 0;
	req.reply_len = 0;
	req.reply_expected = 1;
	if (pmu_queue_request(&req) != 0) {
		printk(KERN_ERR "pmu_adb_reset_bus: pmu_queue_request failed\n");
		return -EIO;
	}
	pmu_wait_complete(&req);

	if (save_autopoll != 0)
		pmu_adb_autopoll(save_autopoll);

	return 0;
}
#endif /* CONFIG_ADB */

/* Construct and send a pmu request */
int
pmu_request(struct adb_request *req, void (*done)(struct adb_request *),
	    int nbytes, ...)
{
	va_list list;
	int i;

	if (vias == NULL)
		return -ENXIO;

	if (nbytes < 0 || nbytes > 32) {
		printk(KERN_ERR "pmu_request: bad nbytes (%d)\n", nbytes);
		req->complete = 1;
		return -EINVAL;
	}
	req->nbytes = nbytes;
	req->done = done;
	va_start(list, nbytes);
	for (i = 0; i < nbytes; ++i)
		req->data[i] = va_arg(list, int);
	va_end(list);
	req->reply_len = 0;
	req->reply_expected = 0;
	return pmu_queue_request(req);
}

int
pmu_queue_request(struct adb_request *req)
{
	unsigned long flags;
	int nsend;

	if (via == NULL) {
		req->complete = 1;
		return -ENXIO;
	}
	if (req->nbytes <= 0) {
		req->complete = 1;
		return 0;
	}
	nsend = pmu_data_len[req->data[0]][0];
	if (nsend >= 0 && req->nbytes != nsend + 1) {
		req->complete = 1;
		return -EINVAL;
	}

	req->next = NULL;
	req->sent = 0;
	req->complete = 0;

	spin_lock_irqsave(&pmu_lock, flags);
	if (current_req != 0) {
		last_req->next = req;
		last_req = req;
	} else {
		current_req = req;
		last_req = req;
		if (pmu_state == idle)
			pmu_start();
	}
	spin_unlock_irqrestore(&pmu_lock, flags);

	return 0;
}

static inline void
wait_for_ack(void)
{
	/* Sightly increased the delay, I had one occurrence of the message
	 * reported
	 */
	int timeout = 4000;
	while ((in_8(&via[B]) & TACK) == 0) {
		if (--timeout < 0) {
			printk(KERN_ERR "PMU not responding (!ack)\n");
			return;
		}
		udelay(10);
	}
}

/* New PMU seems to be very sensitive to those timings, so we make sure
 * PCI is flushed immediately */
static inline void
send_byte(int x)
{
	volatile unsigned char __iomem *v = via;

	out_8(&v[ACR], in_8(&v[ACR]) | SR_OUT | SR_EXT);
	out_8(&v[SR], x);
	out_8(&v[B], in_8(&v[B]) & ~TREQ);		/* assert TREQ */
	(void)in_8(&v[B]);
}

static inline void
recv_byte(void)
{
	volatile unsigned char __iomem *v = via;

	out_8(&v[ACR], (in_8(&v[ACR]) & ~SR_OUT) | SR_EXT);
	in_8(&v[SR]);		/* resets SR */
	out_8(&v[B], in_8(&v[B]) & ~TREQ);
	(void)in_8(&v[B]);
}

static inline void
pmu_done(struct adb_request *req)
{
	void (*done)(struct adb_request *) = req->done;
	mb();
	req->complete = 1;
    	/* Here, we assume that if the request has a done member, the
    	 * struct request will survive to setting req->complete to 1
    	 */
	if (done)
		(*done)(req);
}

static void
pmu_start(void)
{
	struct adb_request *req;

	/* assert pmu_state == idle */
	/* get the packet to send */
	req = current_req;
	if (req == 0 || pmu_state != idle
	    || (/*req->reply_expected && */req_awaiting_reply))
		return;

	pmu_state = sending;
	data_index = 1;
	data_len = pmu_data_len[req->data[0]][0];

	/* Sounds safer to make sure ACK is high before writing. This helped
	 * kill a problem with ADB and some iBooks
	 */
	wait_for_ack();
	/* set the shift register to shift out and send a byte */
	send_byte(req->data[0]);
}

void
pmu_poll(void)
{
	if (!via)
		return;
	if (disable_poll)
		return;
	via_pmu_interrupt(0, NULL, NULL);
}

void
pmu_poll_adb(void)
{
	if (!via)
		return;
	if (disable_poll)
		return;
	/* Kicks ADB read when PMU is suspended */
	adb_int_pending = 1;
	do {
		via_pmu_interrupt(0, NULL, NULL);
	} while (pmu_suspended && (adb_int_pending || pmu_state != idle
		|| req_awaiting_reply));
}

void
pmu_wait_complete(struct adb_request *req)
{
	if (!via)
		return;
	while((pmu_state != idle && pmu_state != locked) || !req->complete)
		via_pmu_interrupt(0, NULL, NULL);
}

/* This function loops until the PMU is idle and prevents it from
 * anwsering to ADB interrupts. pmu_request can still be called.
 * This is done to avoid spurrious shutdowns when we know we'll have
 * interrupts switched off for a long time
 */
void
pmu_suspend(void)
{
	unsigned long flags;
#ifdef SUSPEND_USES_PMU
	struct adb_request *req;
#endif
	if (!via)
		return;
	
	spin_lock_irqsave(&pmu_lock, flags);
	pmu_suspended++;
	if (pmu_suspended > 1) {
		spin_unlock_irqrestore(&pmu_lock, flags);
		return;
	}

	do {
		spin_unlock_irqrestore(&pmu_lock, flags);
		if (req_awaiting_reply)
			adb_int_pending = 1;
		via_pmu_interrupt(0, NULL, NULL);
		spin_lock_irqsave(&pmu_lock, flags);
		if (!adb_int_pending && pmu_state == idle && !req_awaiting_reply) {
#ifdef SUSPEND_USES_PMU
			pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, 0);
			spin_unlock_irqrestore(&pmu_lock, flags);
			while(!req.complete)
				pmu_poll();
#else /* SUSPEND_USES_PMU */
			if (gpio_irq >= 0)
				disable_irq_nosync(gpio_irq);
			out_8(&via[IER], CB1_INT | IER_CLR);
			spin_unlock_irqrestore(&pmu_lock, flags);
#endif /* SUSPEND_USES_PMU */
			break;
		}
	} while (1);
}

void
pmu_resume(void)
{
	unsigned long flags;

	if (!via || (pmu_suspended < 1))
		return;

	spin_lock_irqsave(&pmu_lock, flags);
	pmu_suspended--;
	if (pmu_suspended > 0) {
		spin_unlock_irqrestore(&pmu_lock, flags);
		return;
	}
	adb_int_pending = 1;
#ifdef SUSPEND_USES_PMU
	pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, pmu_intr_mask);
	spin_unlock_irqrestore(&pmu_lock, flags);
	while(!req.complete)
		pmu_poll();
#else /* SUSPEND_USES_PMU */
	if (gpio_irq >= 0)
		enable_irq(gpio_irq);
	out_8(&via[IER], CB1_INT | IER_SET);
	spin_unlock_irqrestore(&pmu_lock, flags);
	pmu_poll();
#endif /* SUSPEND_USES_PMU */
}

/* Interrupt data could be the result data from an ADB cmd */
static void
pmu_handle_data(unsigned char *data, int len, struct pt_regs *regs)
{
	unsigned char ints, pirq;
	int i = 0;

	asleep = 0;
	if (drop_interrupts || len < 1) {
		adb_int_pending = 0;
		pmu_irq_stats[8]++;
		return;
	}

	/* Get PMU interrupt mask */
	ints = data[0];

	/* Record zero interrupts for stats */
	if (ints == 0)
		pmu_irq_stats[9]++;

	/* Hack to deal with ADB autopoll flag */
	if (ints & PMU_INT_ADB)
		ints &= ~(PMU_INT_ADB_AUTO | PMU_INT_AUTO_SRQ_POLL);

next:

	if (ints == 0) {
		if (i > pmu_irq_stats[10])
			pmu_irq_stats[10] = i;
		return;
	}

	for (pirq = 0; pirq < 8; pirq++)
		if (ints & (1 << pirq))
			break;
	pmu_irq_stats[pirq]++;
	i++;
	ints &= ~(1 << pirq);

	/* Note: for some reason, we get an interrupt with len=1,
	 * data[0]==0 after each normal ADB interrupt, at least
	 * on the Pismo. Still investigating...  --BenH
	 */
	if ((1 << pirq) & PMU_INT_ADB) {
		if ((data[0] & PMU_INT_ADB_AUTO) == 0) {
			struct adb_request *req = req_awaiting_reply;
			if (req == 0) {
				printk(KERN_ERR "PMU: extra ADB reply\n");
				return;
			}
			req_awaiting_reply = NULL;
			if (len <= 2)
				req->reply_len = 0;
			else {
				memcpy(req->reply, data + 1, len - 1);
				req->reply_len = len - 1;
			}
			pmu_done(req);
		} else {
			if (len == 4 && data[1] == 0x2c) {
				extern int xmon_wants_key, xmon_adb_keycode;
				if (xmon_wants_key) {
					xmon_adb_keycode = data[2];
					return;
				}
			}
#ifdef CONFIG_ADB
			/*
			 * XXX On the [23]400 the PMU gives us an up
			 * event for keycodes 0x74 or 0x75 when the PC
			 * card eject buttons are released, so we
			 * ignore those events.
			 */
			if (!(pmu_kind == PMU_OHARE_BASED && len == 4
			      && data[1] == 0x2c && data[3] == 0xff
			      && (data[2] & ~1) == 0xf4))
				adb_input(data+1, len-1, regs, 1);
#endif /* CONFIG_ADB */		
		}
	}
	/* Sound/brightness button pressed */
	else if ((1 << pirq) & PMU_INT_SNDBRT) {
#ifdef CONFIG_PMAC_BACKLIGHT
		if (len == 3)
#ifdef CONFIG_INPUT_ADBHID
			if (!disable_kernel_backlight)
#endif /* CONFIG_INPUT_ADBHID */
				set_backlight_level(data[1] >> 4);
#endif /* CONFIG_PMAC_BACKLIGHT */
	}
	/* Tick interrupt */
	else if ((1 << pirq) & PMU_INT_TICK) {
		/* Environement or tick interrupt, query batteries */
		if (pmu_battery_count) {
			if ((--query_batt_timer) == 0) {
				query_battery_state();
				query_batt_timer = BATTERY_POLLING_COUNT;
			}
		}
        }
	else if ((1 << pirq) & PMU_INT_ENVIRONMENT) {
		if (pmu_battery_count)
			query_battery_state();
		pmu_pass_intr(data, len);
	} else {
	       pmu_pass_intr(data, len);
	}
	goto next;
}

static struct adb_request*
pmu_sr_intr(struct pt_regs *regs)
{
	struct adb_request *req;
	int bite = 0;

	if (via[B] & TREQ) {
		printk(KERN_ERR "PMU: spurious SR intr (%x)\n", via[B]);
		out_8(&via[IFR], SR_INT);
		return NULL;
	}
	/* The ack may not yet be low when we get the interrupt */
	while ((in_8(&via[B]) & TACK) != 0)
			;

	/* if reading grab the byte, and reset the interrupt */
	if (pmu_state == reading || pmu_state == reading_intr)
		bite = in_8(&via[SR]);

	/* reset TREQ and wait for TACK to go high */
	out_8(&via[B], in_8(&via[B]) | TREQ);
	wait_for_ack();

	switch (pmu_state) {
	case sending:
		req = current_req;
		if (data_len < 0) {
			data_len = req->nbytes - 1;
			send_byte(data_len);
			break;
		}
		if (data_index <= data_len) {
			send_byte(req->data[data_index++]);
			break;
		}
		req->sent = 1;
		data_len = pmu_data_len[req->data[0]][1];
		if (data_len == 0) {
			pmu_state = idle;
			current_req = req->next;
			if (req->reply_expected)
				req_awaiting_reply = req;
			else
				return req;
		} else {
			pmu_state = reading;
			data_index = 0;
			reply_ptr = req->reply + req->reply_len;
			recv_byte();
		}
		break;

	case intack:
		data_index = 0;
		data_len = -1;
		pmu_state = reading_intr;
		reply_ptr = interrupt_data[int_data_last];
		recv_byte();
		if (gpio_irq >= 0 && !gpio_irq_enabled) {
			enable_irq(gpio_irq);
			gpio_irq_enabled = 1;
		}
		break;

	case reading:
	case reading_intr:
		if (data_len == -1) {
			data_len = bite;
			if (bite > 32)
				printk(KERN_ERR "PMU: bad reply len %d\n", bite);
		} else if (data_index < 32) {
			reply_ptr[data_index++] = bite;
		}
		if (data_index < data_len) {
			recv_byte();
			break;
		}

		if (pmu_state == reading_intr) {
			pmu_state = idle;
			int_data_state[int_data_last] = int_data_ready;
			interrupt_data_len[int_data_last] = data_len;
		} else {
			req = current_req;
			/* 
			 * For PMU sleep and freq change requests, we lock the
			 * PMU until it's explicitely unlocked. This avoids any
			 * spurrious event polling getting in
			 */
			current_req = req->next;
			req->reply_len += data_index;
			if (req->data[0] == PMU_SLEEP || req->data[0] == PMU_CPU_SPEED)
				pmu_state = locked;
			else
				pmu_state = idle;
			return req;
		}
		break;

	default:
		printk(KERN_ERR "via_pmu_interrupt: unknown state %d?\n",
		       pmu_state);
	}
	return NULL;
}

static irqreturn_t
via_pmu_interrupt(int irq, void *arg, struct pt_regs *regs)
{
	unsigned long flags;
	int intr;
	int nloop = 0;
	int int_data = -1;
	struct adb_request *req = NULL;
	int handled = 0;

	/* This is a bit brutal, we can probably do better */
	spin_lock_irqsave(&pmu_lock, flags);
	++disable_poll;
	
	for (;;) {
		intr = in_8(&via[IFR]) & (SR_INT | CB1_INT);
		if (intr == 0)
			break;
		handled = 1;
		if (++nloop > 1000) {
			printk(KERN_DEBUG "PMU: stuck in intr loop, "
			       "intr=%x, ier=%x pmu_state=%d\n",
			       intr, in_8(&via[IER]), pmu_state);
			break;
		}
		out_8(&via[IFR], intr);
		if (intr & CB1_INT) {
			adb_int_pending = 1;
			pmu_irq_stats[0]++;
		}
		if (intr & SR_INT) {
			req = pmu_sr_intr(regs);
			if (req)
				break;
		}
	}

recheck:
	if (pmu_state == idle) {
		if (adb_int_pending) {
			if (int_data_state[0] == int_data_empty)
				int_data_last = 0;
			else if (int_data_state[1] == int_data_empty)
				int_data_last = 1;
			else
				goto no_free_slot;
			pmu_state = intack;
			int_data_state[int_data_last] = int_data_fill;
			/* Sounds safer to make sure ACK is high before writing.
			 * This helped kill a problem with ADB and some iBooks
			 */
			wait_for_ack();
			send_byte(PMU_INT_ACK);
			adb_int_pending = 0;
		} else if (current_req)
			pmu_start();
	}
no_free_slot:			
	/* Mark the oldest buffer for flushing */
	if (int_data_state[!int_data_last] == int_data_ready) {
		int_data_state[!int_data_last] = int_data_flush;
		int_data = !int_data_last;
	} else if (int_data_state[int_data_last] == int_data_ready) {
		int_data_state[int_data_last] = int_data_flush;
		int_data = int_data_last;
	}
	--disable_poll;
	spin_unlock_irqrestore(&pmu_lock, flags);

	/* Deal with completed PMU requests outside of the lock */
	if (req) {
		pmu_done(req);
		req = NULL;
	}
		
	/* Deal with interrupt datas outside of the lock */
	if (int_data >= 0) {
		pmu_handle_data(interrupt_data[int_data], interrupt_data_len[int_data], regs);
		spin_lock_irqsave(&pmu_lock, flags);
		++disable_poll;
		int_data_state[int_data] = int_data_empty;
		int_data = -1;
		goto recheck;
	}

	return IRQ_RETVAL(handled);
}

void
pmu_unlock(void)
{
	unsigned long flags;

	spin_lock_irqsave(&pmu_lock, flags);
	if (pmu_state == locked)
		pmu_state = idle;
	adb_int_pending = 1;
	spin_unlock_irqrestore(&pmu_lock, flags);
}


static irqreturn_t
gpio1_interrupt(int irq, void *arg, struct pt_regs *regs)
{
	unsigned long flags;

	if ((in_8(gpio_reg + 0x9) & 0x02) == 0) {
		spin_lock_irqsave(&pmu_lock, flags);
		if (gpio_irq_enabled > 0) {
			disable_irq_nosync(gpio_irq);
			gpio_irq_enabled = 0;
		}
		pmu_irq_stats[1]++;
		adb_int_pending = 1;
		spin_unlock_irqrestore(&pmu_lock, flags);
		via_pmu_interrupt(0, NULL, NULL);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

#ifdef CONFIG_PMAC_BACKLIGHT
static int backlight_to_bright[] = {
	0x7f, 0x46, 0x42, 0x3e, 0x3a, 0x36, 0x32, 0x2e,
	0x2a, 0x26, 0x22, 0x1e, 0x1a, 0x16, 0x12, 0x0e
};
 
static int
pmu_set_backlight_enable(int on, int level, void* data)
{
	struct adb_request req;
	
	if (vias == NULL)
		return -ENODEV;

	if (on) {
		pmu_request(&req, NULL, 2, PMU_BACKLIGHT_BRIGHT,
			    backlight_to_bright[level]);
		pmu_wait_complete(&req);
	}
	pmu_request(&req, NULL, 2, PMU_POWER_CTRL,
		    PMU_POW_BACKLIGHT | (on ? PMU_POW_ON : PMU_POW_OFF));
       	pmu_wait_complete(&req);

	return 0;
}

static void
pmu_bright_complete(struct adb_request *req)
{
	if (req == &bright_req_1)
		clear_bit(1, &async_req_locks);
	if (req == &bright_req_2)
		clear_bit(2, &async_req_locks);
}

static int
pmu_set_backlight_level(int level, void* data)
{
	if (vias == NULL)
		return -ENODEV;

	if (test_and_set_bit(1, &async_req_locks))
		return -EAGAIN;
	pmu_request(&bright_req_1, pmu_bright_complete, 2, PMU_BACKLIGHT_BRIGHT,
		backlight_to_bright[level]);
	if (test_and_set_bit(2, &async_req_locks))
		return -EAGAIN;
	pmu_request(&bright_req_2, pmu_bright_complete, 2, PMU_POWER_CTRL,
		    PMU_POW_BACKLIGHT | (level > BACKLIGHT_OFF ?
					 PMU_POW_ON : PMU_POW_OFF));

	return 0;
}
#endif /* CONFIG_PMAC_BACKLIGHT */

void
pmu_enable_irled(int on)
{
	struct adb_request req;

	if (vias == NULL)
		return ;
	if (pmu_kind == PMU_KEYLARGO_BASED)
		return ;

	pmu_request(&req, NULL, 2, PMU_POWER_CTRL, PMU_POW_IRLED |
	    (on ? PMU_POW_ON : PMU_POW_OFF));
	pmu_wait_complete(&req);
}

void
pmu_restart(void)
{
	struct adb_request req;

	if (via == NULL)
		return;

	local_irq_disable();

	drop_interrupts = 1;
	
	if (pmu_kind != PMU_KEYLARGO_BASED) {
		pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, PMU_INT_ADB |
						PMU_INT_TICK );
		while(!req.complete)
			pmu_poll();
	}

	pmu_request(&req, NULL, 1, PMU_RESET);
	pmu_wait_complete(&req);
	for (;;)
		;
}

void
pmu_shutdown(void)
{
	struct adb_request req;

	if (via == NULL)
		return;

	local_irq_disable();

	drop_interrupts = 1;

	if (pmu_kind != PMU_KEYLARGO_BASED) {
		pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, PMU_INT_ADB |
						PMU_INT_TICK );
		pmu_wait_complete(&req);
	} else {
		/* Disable server mode on shutdown or we'll just
		 * wake up again
		 */
		pmu_set_server_mode(0);
	}

	pmu_request(&req, NULL, 5, PMU_SHUTDOWN,
		    'M', 'A', 'T', 'T');
	pmu_wait_complete(&req);
	for (;;)
		;
}

int
pmu_present(void)
{
	return via != 0;
}

#ifdef CONFIG_PM

static LIST_HEAD(sleep_notifiers);

int
pmu_register_sleep_notifier(struct pmu_sleep_notifier *n)
{
	struct list_head *list;
	struct pmu_sleep_notifier *notifier;

	for (list = sleep_notifiers.next; list != &sleep_notifiers;
	     list = list->next) {
		notifier = list_entry(list, struct pmu_sleep_notifier, list);
		if (n->priority > notifier->priority)
			break;
	}
	__list_add(&n->list, list->prev, list);
	return 0;
}
EXPORT_SYMBOL(pmu_register_sleep_notifier);

int
pmu_unregister_sleep_notifier(struct pmu_sleep_notifier* n)
{
	if (n->list.next == 0)
		return -ENOENT;
	list_del(&n->list);
	n->list.next = NULL;
	return 0;
}
EXPORT_SYMBOL(pmu_unregister_sleep_notifier);
#endif /* CONFIG_PM */

#if defined(CONFIG_PM) && defined(CONFIG_PPC32)

/* Sleep is broadcast last-to-first */
static int
broadcast_sleep(int when, int fallback)
{
	int ret = PBOOK_SLEEP_OK;
	struct list_head *list;
	struct pmu_sleep_notifier *notifier;

	for (list = sleep_notifiers.prev; list != &sleep_notifiers;
	     list = list->prev) {
		notifier = list_entry(list, struct pmu_sleep_notifier, list);
		ret = notifier->notifier_call(notifier, when);
		if (ret != PBOOK_SLEEP_OK) {
			printk(KERN_DEBUG "sleep %d rejected by %p (%p)\n",
			       when, notifier, notifier->notifier_call);
			for (; list != &sleep_notifiers; list = list->next) {
				notifier = list_entry(list, struct pmu_sleep_notifier, list);
				notifier->notifier_call(notifier, fallback);
			}
			return ret;
		}
	}
	return ret;
}

/* Wake is broadcast first-to-last */
static int
broadcast_wake(void)
{
	int ret = PBOOK_SLEEP_OK;
	struct list_head *list;
	struct pmu_sleep_notifier *notifier;

	for (list = sleep_notifiers.next; list != &sleep_notifiers;
	     list = list->next) {
		notifier = list_entry(list, struct pmu_sleep_notifier, list);
		notifier->notifier_call(notifier, PBOOK_WAKE);
	}
	return ret;
}

/*
 * This struct is used to store config register values for
 * PCI devices which may get powered off when we sleep.
 */
static struct pci_save {
#ifndef HACKED_PCI_SAVE
	u16	command;
	u16	cache_lat;
	u16	intr;
	u32	rom_address;
#else
	u32	config[16];
#endif	
} *pbook_pci_saves;
static int pbook_npci_saves;

static void
pbook_alloc_pci_save(void)
{
	int npci;
	struct pci_dev *pd = NULL;

	npci = 0;
	while ((pd = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, pd)) != NULL) {
		++npci;
	}
	if (npci == 0)
		return;
	pbook_pci_saves = (struct pci_save *)
		kmalloc(npci * sizeof(struct pci_save), GFP_KERNEL);
	pbook_npci_saves = npci;
}

static void
pbook_free_pci_save(void)
{
	if (pbook_pci_saves == NULL)
		return;
	kfree(pbook_pci_saves);
	pbook_pci_saves = NULL;
	pbook_npci_saves = 0;
}

static void
pbook_pci_save(void)
{
	struct pci_save *ps = pbook_pci_saves;
	struct pci_dev *pd = NULL;
	int npci = pbook_npci_saves;
	
	if (ps == NULL)
		return;

	while ((pd = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, pd)) != NULL) {
		if (npci-- == 0)
			return;
#ifndef HACKED_PCI_SAVE
		pci_read_config_word(pd, PCI_COMMAND, &ps->command);
		pci_read_config_word(pd, PCI_CACHE_LINE_SIZE, &ps->cache_lat);
		pci_read_config_word(pd, PCI_INTERRUPT_LINE, &ps->intr);
		pci_read_config_dword(pd, PCI_ROM_ADDRESS, &ps->rom_address);
#else
		int i;
		for (i=1;i<16;i++)
			pci_read_config_dword(pd, i<<4, &ps->config[i]);
#endif
		++ps;
	}
}

/* For this to work, we must take care of a few things: If gmac was enabled
 * during boot, it will be in the pci dev list. If it's disabled at this point
 * (and it will probably be), then you can't access it's config space.
 */
static void
pbook_pci_restore(void)
{
	u16 cmd;
	struct pci_save *ps = pbook_pci_saves - 1;
	struct pci_dev *pd = NULL;
	int npci = pbook_npci_saves;
	int j;

	while ((pd = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, pd)) != NULL) {
#ifdef HACKED_PCI_SAVE
		int i;
		if (npci-- == 0)
			return;
		ps++;
		for (i=2;i<16;i++)
			pci_write_config_dword(pd, i<<4, ps->config[i]);
		pci_write_config_dword(pd, 4, ps->config[1]);
#else
		if (npci-- == 0)
			return;
		ps++;
		if (ps->command == 0)
			continue;
		pci_read_config_word(pd, PCI_COMMAND, &cmd);
		if ((ps->command & ~cmd) == 0)
			continue;
		switch (pd->hdr_type) {
		case PCI_HEADER_TYPE_NORMAL:
			for (j = 0; j < 6; ++j)
				pci_write_config_dword(pd,
					PCI_BASE_ADDRESS_0 + j*4,
					pd->resource[j].start);
			pci_write_config_dword(pd, PCI_ROM_ADDRESS,
				ps->rom_address);
			pci_write_config_word(pd, PCI_CACHE_LINE_SIZE,
				ps->cache_lat);
			pci_write_config_word(pd, PCI_INTERRUPT_LINE,
				ps->intr);
			pci_write_config_word(pd, PCI_COMMAND, ps->command);
			break;
		}
#endif	
	}
}

#ifdef DEBUG_SLEEP
/* N.B. This doesn't work on the 3400 */
void 
pmu_blink(int n)
{
	struct adb_request req;

	memset(&req, 0, sizeof(req));

	for (; n > 0; --n) {
		req.nbytes = 4;
		req.done = NULL;
		req.data[0] = 0xee;
		req.data[1] = 4;
		req.data[2] = 0;
		req.data[3] = 1;
		req.reply[0] = ADB_RET_OK;
		req.reply_len = 1;
		req.reply_expected = 0;
		pmu_polled_request(&req);
		mdelay(50);
		req.nbytes = 4;
		req.done = NULL;
		req.data[0] = 0xee;
		req.data[1] = 4;
		req.data[2] = 0;
		req.data[3] = 0;
		req.reply[0] = ADB_RET_OK;
		req.reply_len = 1;
		req.reply_expected = 0;
		pmu_polled_request(&req);
		mdelay(50);
	}
	mdelay(50);
}
#endif

/*
 * Put the powerbook to sleep.
 */
 
static u32 save_via[8];

static void
save_via_state(void)
{
	save_via[0] = in_8(&via[ANH]);
	save_via[1] = in_8(&via[DIRA]);
	save_via[2] = in_8(&via[B]);
	save_via[3] = in_8(&via[DIRB]);
	save_via[4] = in_8(&via[PCR]);
	save_via[5] = in_8(&via[ACR]);
	save_via[6] = in_8(&via[T1CL]);
	save_via[7] = in_8(&via[T1CH]);
}
static void
restore_via_state(void)
{
	out_8(&via[ANH], save_via[0]);
	out_8(&via[DIRA], save_via[1]);
	out_8(&via[B], save_via[2]);
	out_8(&via[DIRB], save_via[3]);
	out_8(&via[PCR], save_via[4]);
	out_8(&via[ACR], save_via[5]);
	out_8(&via[T1CL], save_via[6]);
	out_8(&via[T1CH], save_via[7]);
	out_8(&via[IER], IER_CLR | 0x7f);	/* disable all intrs */
	out_8(&via[IFR], 0x7f);				/* clear IFR */
	out_8(&via[IER], IER_SET | SR_INT | CB1_INT);
}

static int
pmac_suspend_devices(void)
{
	int ret;

	pm_prepare_console();
	
	/* Notify old-style device drivers & userland */
	ret = broadcast_sleep(PBOOK_SLEEP_REQUEST, PBOOK_SLEEP_REJECT);
	if (ret != PBOOK_SLEEP_OK) {
		printk(KERN_ERR "Sleep rejected by drivers\n");
		return -EBUSY;
	}

	/* Sync the disks. */
	/* XXX It would be nice to have some way to ensure that
	 * nobody is dirtying any new buffers while we wait. That
	 * could be achieved using the refrigerator for processes
	 * that swsusp uses
	 */
	sys_sync();

	/* Sleep can fail now. May not be very robust but useful for debugging */
	ret = broadcast_sleep(PBOOK_SLEEP_NOW, PBOOK_WAKE);
	if (ret != PBOOK_SLEEP_OK) {
		printk(KERN_ERR "Driver sleep failed\n");
		return -EBUSY;
	}

	/* Send suspend call to devices, hold the device core's dpm_sem */
	ret = device_suspend(PMSG_SUSPEND);
	if (ret) {
		broadcast_wake();
		printk(KERN_ERR "Driver sleep failed\n");
		return -EBUSY;
	}

	/* Call platform functions marked "on sleep" */
	pmac_pfunc_i2c_suspend();
	pmac_pfunc_base_suspend();

	/* Stop preemption */
	preempt_disable();

	/* Make sure the decrementer won't interrupt us */
	asm volatile("mtdec %0" : : "r" (0x7fffffff));
	/* Make sure any pending DEC interrupt occurring while we did
	 * the above didn't re-enable the DEC */
	mb();
	asm volatile("mtdec %0" : : "r" (0x7fffffff));

	/* We can now disable MSR_EE. This code of course works properly only
	 * on UP machines... For SMP, if we ever implement sleep, we'll have to
	 * stop the "other" CPUs way before we do all that stuff.
	 */
	local_irq_disable();

	/* Broadcast power down irq
	 * This isn't that useful in most cases (only directly wired devices can
	 * use this but still... This will take care of sysdev's as well, so
	 * we exit from here with local irqs disabled and PIC off.
	 */
	ret = device_power_down(PMSG_SUSPEND);
	if (ret) {
		wakeup_decrementer();
		local_irq_enable();
		preempt_enable();
		device_resume();
		broadcast_wake();
		printk(KERN_ERR "Driver powerdown failed\n");
		return -EBUSY;
	}

	/* Wait for completion of async backlight requests */
	while (!bright_req_1.complete || !bright_req_2.complete ||
			!batt_req.complete)
		pmu_poll();

	/* Giveup the lazy FPU & vec so we don't have to back them
	 * up from the low level code
	 */
	enable_kernel_fp();

#ifdef CONFIG_ALTIVEC
	if (cpu_has_feature(CPU_FTR_ALTIVEC))
		enable_kernel_altivec();
#endif /* CONFIG_ALTIVEC */

	return 0;
}

static int
pmac_wakeup_devices(void)
{
	mdelay(100);

	/* Power back up system devices (including the PIC) */
	device_power_up();

	/* Force a poll of ADB interrupts */
	adb_int_pending = 1;
	via_pmu_interrupt(0, NULL, NULL);

	/* Restart jiffies & scheduling */
	wakeup_decrementer();

	/* Re-enable local CPU interrupts */
	local_irq_enable();
	mdelay(10);
	preempt_enable();

	/* Call platform functions marked "on wake" */
	pmac_pfunc_base_resume();
	pmac_pfunc_i2c_resume();

	/* Resume devices */
	device_resume();

	/* Notify old style drivers */
	broadcast_wake();

	pm_restore_console();

	return 0;
}

#define	GRACKLE_PM	(1<<7)
#define GRACKLE_DOZE	(1<<5)
#define	GRACKLE_NAP	(1<<4)
#define	GRACKLE_SLEEP	(1<<3)

int
powerbook_sleep_grackle(void)
{
	unsigned long save_l2cr;
	unsigned short pmcr1;
	struct adb_request req;
	int ret;
	struct pci_dev *grackle;

	grackle = pci_find_slot(0, 0);
	if (!grackle)
		return -ENODEV;

	ret = pmac_suspend_devices();
	if (ret) {
		printk(KERN_ERR "Sleep rejected by devices\n");
		return ret;
	}
	
	/* Turn off various things. Darwin does some retry tests here... */
	pmu_request(&req, NULL, 2, PMU_POWER_CTRL0, PMU_POW0_OFF|PMU_POW0_HARD_DRIVE);
	pmu_wait_complete(&req);
	pmu_request(&req, NULL, 2, PMU_POWER_CTRL,
		PMU_POW_OFF|PMU_POW_BACKLIGHT|PMU_POW_IRLED|PMU_POW_MEDIABAY);
	pmu_wait_complete(&req);

	/* For 750, save backside cache setting and disable it */
	save_l2cr = _get_L2CR();	/* (returns -1 if not available) */

	if (!__fake_sleep) {
		/* Ask the PMU to put us to sleep */
		pmu_request(&req, NULL, 5, PMU_SLEEP, 'M', 'A', 'T', 'T');
		pmu_wait_complete(&req);
	}

	/* The VIA is supposed not to be restored correctly*/
	save_via_state();
	/* We shut down some HW */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,1);

	pci_read_config_word(grackle, 0x70, &pmcr1);
	/* Apparently, MacOS uses NAP mode for Grackle ??? */
	pmcr1 &= ~(GRACKLE_DOZE|GRACKLE_SLEEP); 
	pmcr1 |= GRACKLE_PM|GRACKLE_NAP;
	pci_write_config_word(grackle, 0x70, pmcr1);

	/* Call low-level ASM sleep handler */
	if (__fake_sleep)
		mdelay(5000);
	else
		low_sleep_handler();

	/* We're awake again, stop grackle PM */
	pci_read_config_word(grackle, 0x70, &pmcr1);
	pmcr1 &= ~(GRACKLE_PM|GRACKLE_DOZE|GRACKLE_SLEEP|GRACKLE_NAP); 
	pci_write_config_word(grackle, 0x70, pmcr1);

	/* Make sure the PMU is idle */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,0);
	restore_via_state();
	
	/* Restore L2 cache */
	if (save_l2cr != 0xffffffff && (save_l2cr & L2CR_L2E) != 0)
 		_set_L2CR(save_l2cr);
	
	/* Restore userland MMU context */
	set_context(current->active_mm->context, current->active_mm->pgd);

	/* Power things up */
	pmu_unlock();
	pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, pmu_intr_mask);
	pmu_wait_complete(&req);
	pmu_request(&req, NULL, 2, PMU_POWER_CTRL0,
			PMU_POW0_ON|PMU_POW0_HARD_DRIVE);
	pmu_wait_complete(&req);
	pmu_request(&req, NULL, 2, PMU_POWER_CTRL,
			PMU_POW_ON|PMU_POW_BACKLIGHT|PMU_POW_CHARGER|PMU_POW_IRLED|PMU_POW_MEDIABAY);
	pmu_wait_complete(&req);

	pmac_wakeup_devices();

	return 0;
}

static int
powerbook_sleep_Core99(void)
{
	unsigned long save_l2cr;
	unsigned long save_l3cr;
	struct adb_request req;
	int ret;
	
	if (pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,-1) < 0) {
		printk(KERN_ERR "Sleep mode not supported on this machine\n");
		return -ENOSYS;
	}

	if (num_online_cpus() > 1 || cpu_is_offline(0))
		return -EAGAIN;

	ret = pmac_suspend_devices();
	if (ret) {
		printk(KERN_ERR "Sleep rejected by devices\n");
		return ret;
	}

	/* Stop environment and ADB interrupts */
	pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, 0);
	pmu_wait_complete(&req);

	/* Tell PMU what events will wake us up */
	pmu_request(&req, NULL, 4, PMU_POWER_EVENTS, PMU_PWR_CLR_WAKEUP_EVENTS,
		0xff, 0xff);
	pmu_wait_complete(&req);
	pmu_request(&req, NULL, 4, PMU_POWER_EVENTS, PMU_PWR_SET_WAKEUP_EVENTS,
		0, PMU_PWR_WAKEUP_KEY |
		(option_lid_wakeup ? PMU_PWR_WAKEUP_LID_OPEN : 0));
	pmu_wait_complete(&req);

	/* Save the state of the L2 and L3 caches */
	save_l3cr = _get_L3CR();	/* (returns -1 if not available) */
	save_l2cr = _get_L2CR();	/* (returns -1 if not available) */

	if (!__fake_sleep) {
		/* Ask the PMU to put us to sleep */
		pmu_request(&req, NULL, 5, PMU_SLEEP, 'M', 'A', 'T', 'T');
		pmu_wait_complete(&req);
	}

	/* The VIA is supposed not to be restored correctly*/
	save_via_state();

	/* Shut down various ASICs. There's a chance that we can no longer
	 * talk to the PMU after this, so I moved it to _after_ sending the
	 * sleep command to it. Still need to be checked.
	 */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE, NULL, 0, 1);

	/* Call low-level ASM sleep handler */
	if (__fake_sleep)
		mdelay(5000);
	else
		low_sleep_handler();

	/* Restore Apple core ASICs state */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE, NULL, 0, 0);

	/* Restore VIA */
	restore_via_state();

	/* tweak LPJ before cpufreq is there */
	loops_per_jiffy *= 2;

	/* Restore video */
	pmac_call_early_video_resume();

	/* Restore L2 cache */
	if (save_l2cr != 0xffffffff && (save_l2cr & L2CR_L2E) != 0)
 		_set_L2CR(save_l2cr);
	/* Restore L3 cache */
	if (save_l3cr != 0xffffffff && (save_l3cr & L3CR_L3E) != 0)
 		_set_L3CR(save_l3cr);
	
	/* Restore userland MMU context */
	set_context(current->active_mm->context, current->active_mm->pgd);

	/* Tell PMU we are ready */
	pmu_unlock();
	pmu_request(&req, NULL, 2, PMU_SYSTEM_READY, 2);
	pmu_wait_complete(&req);
	pmu_request(&req, NULL, 2, PMU_SET_INTR_MASK, pmu_intr_mask);
	pmu_wait_complete(&req);

	/* Restore LPJ, cpufreq will adjust the cpu frequency */
	loops_per_jiffy /= 2;

	pmac_wakeup_devices();

	return 0;
}

#define PB3400_MEM_CTRL		0xf8000000
#define PB3400_MEM_CTRL_SLEEP	0x70

static int
powerbook_sleep_3400(void)
{
	int ret, i, x;
	unsigned int hid0;
	unsigned long p;
	struct adb_request sleep_req;
	void __iomem *mem_ctrl;
	unsigned int __iomem *mem_ctrl_sleep;

	/* first map in the memory controller registers */
	mem_ctrl = ioremap(PB3400_MEM_CTRL, 0x100);
	if (mem_ctrl == NULL) {
		printk("powerbook_sleep_3400: ioremap failed\n");
		return -ENOMEM;
	}
	mem_ctrl_sleep = mem_ctrl + PB3400_MEM_CTRL_SLEEP;

	/* Allocate room for PCI save */
	pbook_alloc_pci_save();

	ret = pmac_suspend_devices();
	if (ret) {
		pbook_free_pci_save();
		printk(KERN_ERR "Sleep rejected by devices\n");
		return ret;
	}

	/* Save the state of PCI config space for some slots */
	pbook_pci_save();

	/* Set the memory controller to keep the memory refreshed
	   while we're asleep */
	for (i = 0x403f; i >= 0x4000; --i) {
		out_be32(mem_ctrl_sleep, i);
		do {
			x = (in_be32(mem_ctrl_sleep) >> 16) & 0x3ff;
		} while (x == 0);
		if (x >= 0x100)
			break;
	}

	/* Ask the PMU to put us to sleep */
	pmu_request(&sleep_req, NULL, 5, PMU_SLEEP, 'M', 'A', 'T', 'T');
	while (!sleep_req.complete)
		mb();

	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,1);

	/* displacement-flush the L2 cache - necessary? */
	for (p = KERNELBASE; p < KERNELBASE + 0x100000; p += 0x1000)
		i = *(volatile int *)p;
	asleep = 1;

	/* Put the CPU into sleep mode */
	hid0 = mfspr(SPRN_HID0);
	hid0 = (hid0 & ~(HID0_NAP | HID0_DOZE)) | HID0_SLEEP;
	mtspr(SPRN_HID0, hid0);
	mtmsr(mfmsr() | MSR_POW | MSR_EE);
	udelay(10);

	/* OK, we're awake again, start restoring things */
	out_be32(mem_ctrl_sleep, 0x3f);
	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,0);
	pbook_pci_restore();
	pmu_unlock();

	/* wait for the PMU interrupt sequence to complete */
	while (asleep)
		mb();

	pmac_wakeup_devices();
	pbook_free_pci_save();
	iounmap(mem_ctrl);

	return 0;
}

#endif /* CONFIG_PM && CONFIG_PPC32 */

/*
 * Support for /dev/pmu device
 */
#define RB_SIZE		0x10
struct pmu_private {
	struct list_head list;
	int	rb_get;
	int	rb_put;
	struct rb_entry {
		unsigned short len;
		unsigned char data[16];
	}	rb_buf[RB_SIZE];
	wait_queue_head_t wait;
	spinlock_t lock;
#if defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT)
	int	backlight_locker;
#endif /* defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT) */	
};

static LIST_HEAD(all_pmu_pvt);
static DEFINE_SPINLOCK(all_pvt_lock);

static void
pmu_pass_intr(unsigned char *data, int len)
{
	struct pmu_private *pp;
	struct list_head *list;
	int i;
	unsigned long flags;

	if (len > sizeof(pp->rb_buf[0].data))
		len = sizeof(pp->rb_buf[0].data);
	spin_lock_irqsave(&all_pvt_lock, flags);
	for (list = &all_pmu_pvt; (list = list->next) != &all_pmu_pvt; ) {
		pp = list_entry(list, struct pmu_private, list);
		spin_lock(&pp->lock);
		i = pp->rb_put + 1;
		if (i >= RB_SIZE)
			i = 0;
		if (i != pp->rb_get) {
			struct rb_entry *rp = &pp->rb_buf[pp->rb_put];
			rp->len = len;
			memcpy(rp->data, data, len);
			pp->rb_put = i;
			wake_up_interruptible(&pp->wait);
		}
		spin_unlock(&pp->lock);
	}
	spin_unlock_irqrestore(&all_pvt_lock, flags);
}

static int
pmu_open(struct inode *inode, struct file *file)
{
	struct pmu_private *pp;
	unsigned long flags;

	pp = kmalloc(sizeof(struct pmu_private), GFP_KERNEL);
	if (pp == 0)
		return -ENOMEM;
	pp->rb_get = pp->rb_put = 0;
	spin_lock_init(&pp->lock);
	init_waitqueue_head(&pp->wait);
	spin_lock_irqsave(&all_pvt_lock, flags);
#if defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT)
	pp->backlight_locker = 0;
#endif /* defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT) */	
	list_add(&pp->list, &all_pmu_pvt);
	spin_unlock_irqrestore(&all_pvt_lock, flags);
	file->private_data = pp;
	return 0;
}

static ssize_t 
pmu_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct pmu_private *pp = file->private_data;
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;
	int ret = 0;

	if (count < 1 || pp == 0)
		return -EINVAL;
	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	spin_lock_irqsave(&pp->lock, flags);
	add_wait_queue(&pp->wait, &wait);
	current->state = TASK_INTERRUPTIBLE;

	for (;;) {
		ret = -EAGAIN;
		if (pp->rb_get != pp->rb_put) {
			int i = pp->rb_get;
			struct rb_entry *rp = &pp->rb_buf[i];
			ret = rp->len;
			spin_unlock_irqrestore(&pp->lock, flags);
			if (ret > count)
				ret = count;
			if (ret > 0 && copy_to_user(buf, rp->data, ret))
				ret = -EFAULT;
			if (++i >= RB_SIZE)
				i = 0;
			spin_lock_irqsave(&pp->lock, flags);
			pp->rb_get = i;
		}
		if (ret >= 0)
			break;
		if (file->f_flags & O_NONBLOCK)
			break;
		ret = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		spin_unlock_irqrestore(&pp->lock, flags);
		schedule();
		spin_lock_irqsave(&pp->lock, flags);
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&pp->wait, &wait);
	spin_unlock_irqrestore(&pp->lock, flags);
	
	return ret;
}

static ssize_t
pmu_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	return 0;
}

static unsigned int
pmu_fpoll(struct file *filp, poll_table *wait)
{
	struct pmu_private *pp = filp->private_data;
	unsigned int mask = 0;
	unsigned long flags;
	
	if (pp == 0)
		return 0;
	poll_wait(filp, &pp->wait, wait);
	spin_lock_irqsave(&pp->lock, flags);
	if (pp->rb_get != pp->rb_put)
		mask |= POLLIN;
	spin_unlock_irqrestore(&pp->lock, flags);
	return mask;
}

static int
pmu_release(struct inode *inode, struct file *file)
{
	struct pmu_private *pp = file->private_data;
	unsigned long flags;

	lock_kernel();
	if (pp != 0) {
		file->private_data = NULL;
		spin_lock_irqsave(&all_pvt_lock, flags);
		list_del(&pp->list);
		spin_unlock_irqrestore(&all_pvt_lock, flags);
#if defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT)
		if (pp->backlight_locker) {
			spin_lock_irqsave(&pmu_lock, flags);
			disable_kernel_backlight--;
			spin_unlock_irqrestore(&pmu_lock, flags);
		}
#endif /* defined(CONFIG_INPUT_ADBHID) && defined(CONFIG_PMAC_BACKLIGHT) */
		kfree(pp);
	}
	unlock_kernel();
	return 0;
}

static int
pmu_ioctl(struct inode * inode, struct file *filp,
		     u_int cmd, u_long arg)
{
	__u32 __user *argp = (__u32 __user *)arg;
	int error = -EINVAL;

	switch (cmd) {
#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
	case PMU_IOC_SLEEP:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (sleep_in_progress)
			return -EBUSY;
		sleep_in_progress = 1;
		switch (pmu_kind) {
		case PMU_OHARE_BASED:
			error = powerbook_sleep_3400();
			break;
		case PMU_HEATHROW_BASED:
		case PMU_PADDINGTON_BASED:
			error = powerbook_sleep_grackle();
			break;
		case PMU_KEYLARGO_BASED:
			error = powerbook_sleep_Core99();
			break;
		default:
			error = -ENOSYS;
		}
		sleep_in_progress = 0;
		break;
	case PMU_IOC_CAN_SLEEP:
		if (pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,0,-1) < 0)
			return put_user(0, argp);
		else
			return put_user(1, argp);
#endif /* CONFIG_PM && CONFIG_PPC32 */

#ifdef CONFIG_PMAC_BACKLIGHT
	/* Backlight should have its own device or go via
	 * the fbdev
	 */
	case PMU_IOC_GET_BACKLIGHT:
		if (sleep_in_progress)
			return -EBUSY;
		error = get_backlight_level();
		if (error < 0)
			return error;
		return put_user(error, argp);
	case PMU_IOC_SET_BACKLIGHT:
	{
		__u32 value;
		if (sleep_in_progress)
			return -EBUSY;
		error = get_user(value, argp);
		if (!error)
			error = set_backlight_level(value);
		break;
	}
#ifdef CONFIG_INPUT_ADBHID
	case PMU_IOC_GRAB_BACKLIGHT: {
		struct pmu_private *pp = filp->private_data;
		unsigned long flags;

		if (pp->backlight_locker)
			return 0;
		pp->backlight_locker = 1;
		spin_lock_irqsave(&pmu_lock, flags);
		disable_kernel_backlight++;
		spin_unlock_irqrestore(&pmu_lock, flags);
		return 0;
	}
#endif /* CONFIG_INPUT_ADBHID */
#endif /* CONFIG_PMAC_BACKLIGHT */
	case PMU_IOC_GET_MODEL:
	    	return put_user(pmu_kind, argp);
	case PMU_IOC_HAS_ADB:
		return put_user(pmu_has_adb, argp);
	}
	return error;
}

static struct file_operations pmu_device_fops = {
	.read		= pmu_read,
	.write		= pmu_write,
	.poll		= pmu_fpoll,
	.ioctl		= pmu_ioctl,
	.open		= pmu_open,
	.release	= pmu_release,
};

static struct miscdevice pmu_device = {
	PMU_MINOR, "pmu", &pmu_device_fops
};

static int pmu_device_init(void)
{
	if (!via)
		return 0;
	if (misc_register(&pmu_device) < 0)
		printk(KERN_ERR "via-pmu: cannot register misc device.\n");
	return 0;
}
device_initcall(pmu_device_init);


#ifdef DEBUG_SLEEP
static inline void 
polled_handshake(volatile unsigned char __iomem *via)
{
	via[B] &= ~TREQ; eieio();
	while ((via[B] & TACK) != 0)
		;
	via[B] |= TREQ; eieio();
	while ((via[B] & TACK) == 0)
		;
}

static inline void 
polled_send_byte(volatile unsigned char __iomem *via, int x)
{
	via[ACR] |= SR_OUT | SR_EXT; eieio();
	via[SR] = x; eieio();
	polled_handshake(via);
}

static inline int
polled_recv_byte(volatile unsigned char __iomem *via)
{
	int x;

	via[ACR] = (via[ACR] & ~SR_OUT) | SR_EXT; eieio();
	x = via[SR]; eieio();
	polled_handshake(via);
	x = via[SR]; eieio();
	return x;
}

int
pmu_polled_request(struct adb_request *req)
{
	unsigned long flags;
	int i, l, c;
	volatile unsigned char __iomem *v = via;

	req->complete = 1;
	c = req->data[0];
	l = pmu_data_len[c][0];
	if (l >= 0 && req->nbytes != l + 1)
		return -EINVAL;

	local_irq_save(flags);
	while (pmu_state != idle)
		pmu_poll();

	while ((via[B] & TACK) == 0)
		;
	polled_send_byte(v, c);
	if (l < 0) {
		l = req->nbytes - 1;
		polled_send_byte(v, l);
	}
	for (i = 1; i <= l; ++i)
		polled_send_byte(v, req->data[i]);

	l = pmu_data_len[c][1];
	if (l < 0)
		l = polled_recv_byte(v);
	for (i = 0; i < l; ++i)
		req->reply[i + req->reply_len] = polled_recv_byte(v);

	if (req->done)
		(*req->done)(req);

	local_irq_restore(flags);
	return 0;
}
#endif /* DEBUG_SLEEP */


/* FIXME: This is a temporary set of callbacks to enable us
 * to do suspend-to-disk.
 */

#if defined(CONFIG_PM) && defined(CONFIG_PPC32)

static int pmu_sys_suspended = 0;

static int pmu_sys_suspend(struct sys_device *sysdev, pm_message_t state)
{
	if (state.event != PM_EVENT_SUSPEND || pmu_sys_suspended)
		return 0;

	/* Suspend PMU event interrupts */
	pmu_suspend();

	pmu_sys_suspended = 1;
	return 0;
}

static int pmu_sys_resume(struct sys_device *sysdev)
{
	struct adb_request req;

	if (!pmu_sys_suspended)
		return 0;

	/* Tell PMU we are ready */
	pmu_request(&req, NULL, 2, PMU_SYSTEM_READY, 2);
	pmu_wait_complete(&req);

	/* Resume PMU event interrupts */
	pmu_resume();

	pmu_sys_suspended = 0;

	return 0;
}

#endif /* CONFIG_PM && CONFIG_PPC32 */

static struct sysdev_class pmu_sysclass = {
	set_kset_name("pmu"),
};

static struct sys_device device_pmu = {
	.id		= 0,
	.cls		= &pmu_sysclass,
};

static struct sysdev_driver driver_pmu = {
#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
	.suspend	= &pmu_sys_suspend,
	.resume		= &pmu_sys_resume,
#endif /* CONFIG_PM && CONFIG_PPC32 */
};

static int __init init_pmu_sysfs(void)
{
	int rc;

	rc = sysdev_class_register(&pmu_sysclass);
	if (rc) {
		printk(KERN_ERR "Failed registering PMU sys class\n");
		return -ENODEV;
	}
	rc = sysdev_register(&device_pmu);
	if (rc) {
		printk(KERN_ERR "Failed registering PMU sys device\n");
		return -ENODEV;
	}
	rc = sysdev_driver_register(&pmu_sysclass, &driver_pmu);
	if (rc) {
		printk(KERN_ERR "Failed registering PMU sys driver\n");
		return -ENODEV;
	}
	return 0;
}

subsys_initcall(init_pmu_sysfs);

EXPORT_SYMBOL(pmu_request);
EXPORT_SYMBOL(pmu_queue_request);
EXPORT_SYMBOL(pmu_poll);
EXPORT_SYMBOL(pmu_poll_adb);
EXPORT_SYMBOL(pmu_wait_complete);
EXPORT_SYMBOL(pmu_suspend);
EXPORT_SYMBOL(pmu_resume);
EXPORT_SYMBOL(pmu_unlock);
#if defined(CONFIG_PM) && defined(CONFIG_PPC32)
EXPORT_SYMBOL(pmu_enable_irled);
EXPORT_SYMBOL(pmu_battery_count);
EXPORT_SYMBOL(pmu_batteries);
EXPORT_SYMBOL(pmu_power_flags);
#endif /* CONFIG_PM && CONFIG_PPC32 */

