/*
 *  drivers/s390/char/con3215.c
 *    3215 line mode terminal driver.
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com),
 *
 *  Updated:
 *   Aug-2000: Added tab support
 *	       Dan Morrison, IBM Corporation (dmorriso@cse.buffalo.edu)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/vt_kern.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/interrupt.h>

#include <linux/slab.h>
#include <linux/bootmem.h>

#include <asm/ccwdev.h>
#include <asm/cio.h>
#include <asm/io.h>
#include <asm/ebcdic.h>
#include <asm/uaccess.h>
#include <asm/delay.h>
#include <asm/cpcmd.h>
#include <asm/setup.h>

#include "ctrlchar.h"

#define NR_3215		    1
#define NR_3215_REQ	    (4*NR_3215)
#define RAW3215_BUFFER_SIZE 65536     /* output buffer size */
#define RAW3215_INBUF_SIZE  256	      /* input buffer size */
#define RAW3215_MIN_SPACE   128	      /* minimum free space for wakeup */
#define RAW3215_MIN_WRITE   1024      /* min. length for immediate output */
#define RAW3215_MAX_BYTES   3968      /* max. bytes to write with one ssch */
#define RAW3215_MAX_NEWLINE 50	      /* max. lines to write with one ssch */
#define RAW3215_NR_CCWS	    3
#define RAW3215_TIMEOUT	    HZ/10     /* time for delayed output */

#define RAW3215_FIXED	    1	      /* 3215 console device is not be freed */
#define RAW3215_ACTIVE	    2	      /* set if the device is in use */
#define RAW3215_WORKING	    4	      /* set if a request is being worked on */
#define RAW3215_THROTTLED   8	      /* set if reading is disabled */
#define RAW3215_STOPPED	    16	      /* set if writing is disabled */
#define RAW3215_CLOSING	    32	      /* set while in close process */
#define RAW3215_TIMER_RUNS  64	      /* set if the output delay timer is on */
#define RAW3215_FLUSHING    128	      /* set to flush buffer (no delay) */

#define TAB_STOP_SIZE	    8	      /* tab stop size */

/*
 * Request types for a 3215 device
 */
enum raw3215_type {
	RAW3215_FREE, RAW3215_READ, RAW3215_WRITE
};

/*
 * Request structure for a 3215 device
 */
struct raw3215_req {
	enum raw3215_type type;	      /* type of the request */
	int start, len;		      /* start index & len in output buffer */
	int delayable;		      /* indication to wait for more data */
	int residual;		      /* residual count for read request */
	struct ccw1 ccws[RAW3215_NR_CCWS]; /* space for the channel program */
	struct raw3215_info *info;    /* pointer to main structure */
	struct raw3215_req *next;     /* pointer to next request */
} __attribute__ ((aligned(8)));

struct raw3215_info {
	struct ccw_device *cdev;      /* device for tty driver */
	spinlock_t *lock;	      /* pointer to irq lock */
	int flags;		      /* state flags */
	char *buffer;		      /* pointer to output buffer */
	char *inbuf;		      /* pointer to input buffer */
	int head;		      /* first free byte in output buffer */
	int count;		      /* number of bytes in output buffer */
	int written;		      /* number of bytes in write requests */
	struct tty_struct *tty;	      /* pointer to tty structure if present */
	struct tasklet_struct tasklet;
	struct raw3215_req *queued_read; /* pointer to queued read requests */
	struct raw3215_req *queued_write;/* pointer to queued write requests */
	wait_queue_head_t empty_wait; /* wait queue for flushing */
	struct timer_list timer;      /* timer for delayed output */
	char *message;		      /* pending message from raw3215_irq */
	int msg_dstat;		      /* dstat for pending message */
	int msg_cstat;		      /* cstat for pending message */
	int line_pos;		      /* position on the line (for tabs) */
	char ubuffer[80];	      /* copy_from_user buffer */
};

/* array of 3215 devices structures */
static struct raw3215_info *raw3215[NR_3215];
/* spinlock to protect the raw3215 array */
static DEFINE_SPINLOCK(raw3215_device_lock);
/* list of free request structures */
static struct raw3215_req *raw3215_freelist;
/* spinlock to protect free list */
static spinlock_t raw3215_freelist_lock;

static struct tty_driver *tty3215_driver;

/*
 * Get a request structure from the free list
 */
static inline struct raw3215_req *
raw3215_alloc_req(void) {
	struct raw3215_req *req;
	unsigned long flags;

	spin_lock_irqsave(&raw3215_freelist_lock, flags);
	req = raw3215_freelist;
	raw3215_freelist = req->next;
	spin_unlock_irqrestore(&raw3215_freelist_lock, flags);
	return req;
}

/*
 * Put a request structure back to the free list
 */
static inline void
raw3215_free_req(struct raw3215_req *req) {
	unsigned long flags;

	if (req->type == RAW3215_FREE)
		return;		/* don't free a free request */
	req->type = RAW3215_FREE;
	spin_lock_irqsave(&raw3215_freelist_lock, flags);
	req->next = raw3215_freelist;
	raw3215_freelist = req;
	spin_unlock_irqrestore(&raw3215_freelist_lock, flags);
}

/*
 * Set up a read request that reads up to 160 byte from the 3215 device.
 * If there is a queued read request it is used, but that shouldn't happen
 * because a 3215 terminal won't accept a new read before the old one is
 * completed.
 */
static void
raw3215_mk_read_req(struct raw3215_info *raw)
{
	struct raw3215_req *req;
	struct ccw1 *ccw;

	/* there can only be ONE read request at a time */
	req = raw->queued_read;
	if (req == NULL) {
		/* no queued read request, use new req structure */
		req = raw3215_alloc_req();
		req->type = RAW3215_READ;
		req->info = raw;
		raw->queued_read = req;
	}

	ccw = req->ccws;
	ccw->cmd_code = 0x0A; /* read inquiry */
	ccw->flags = 0x20;    /* ignore incorrect length */
	ccw->count = 160;
	ccw->cda = (__u32) __pa(raw->inbuf);
}

/*
 * Set up a write request with the information from the main structure.
 * A ccw chain is created that writes as much as possible from the output
 * buffer to the 3215 device. If a queued write exists it is replaced by
 * the new, probably lengthened request.
 */
static void
raw3215_mk_write_req(struct raw3215_info *raw)
{
	struct raw3215_req *req;
	struct ccw1 *ccw;
	int len, count, ix, lines;

	if (raw->count <= raw->written)
		return;
	/* check if there is a queued write request */
	req = raw->queued_write;
	if (req == NULL) {
		/* no queued write request, use new req structure */
		req = raw3215_alloc_req();
		req->type = RAW3215_WRITE;
		req->info = raw;
		raw->queued_write = req;
	} else {
		raw->written -= req->len;
	}

	ccw = req->ccws;
	req->start = (raw->head - raw->count + raw->written) &
		     (RAW3215_BUFFER_SIZE - 1);
	/*
	 * now we have to count newlines. We can at max accept
	 * RAW3215_MAX_NEWLINE newlines in a single ssch due to
	 * a restriction in VM
	 */
	lines = 0;
	ix = req->start;
	while (lines < RAW3215_MAX_NEWLINE && ix != raw->head) {
		if (raw->buffer[ix] == 0x15)
			lines++;
		ix = (ix + 1) & (RAW3215_BUFFER_SIZE - 1);
	}
	len = ((ix - 1 - req->start) & (RAW3215_BUFFER_SIZE - 1)) + 1;
	if (len > RAW3215_MAX_BYTES)
		len = RAW3215_MAX_BYTES;
	req->len = len;
	raw->written += len;

	/* set the indication if we should try to enlarge this request */
	req->delayable = (ix == raw->head) && (len < RAW3215_MIN_WRITE);

	ix = req->start;
	while (len > 0) {
		if (ccw > req->ccws)
			ccw[-1].flags |= 0x40; /* use command chaining */
		ccw->cmd_code = 0x01; /* write, auto carrier return */
		ccw->flags = 0x20;    /* ignore incorrect length ind.  */
		ccw->cda =
			(__u32) __pa(raw->buffer + ix);
		count = len;
		if (ix + count > RAW3215_BUFFER_SIZE)
			count = RAW3215_BUFFER_SIZE - ix;
		ccw->count = count;
		len -= count;
		ix = (ix + count) & (RAW3215_BUFFER_SIZE - 1);
		ccw++;
	}
	/*
	 * Add a NOP to the channel program. 3215 devices are purely
	 * emulated and its much better to avoid the channel end
	 * interrupt in this case.
	 */
	if (ccw > req->ccws)
		ccw[-1].flags |= 0x40; /* use command chaining */
	ccw->cmd_code = 0x03; /* NOP */
	ccw->flags = 0;
	ccw->cda = 0;
	ccw->count = 1;
}

/*
 * Start a read or a write request
 */
static void
raw3215_start_io(struct raw3215_info *raw)
{
	struct raw3215_req *req;
	int res;

	req = raw->queued_read;
	if (req != NULL &&
	    !(raw->flags & (RAW3215_WORKING | RAW3215_THROTTLED))) {
		/* dequeue request */
		raw->queued_read = NULL;
		res = ccw_device_start(raw->cdev, req->ccws,
				       (unsigned long) req, 0, 0);
		if (res != 0) {
			/* do_IO failed, put request back to queue */
			raw->queued_read = req;
		} else {
			raw->flags |= RAW3215_WORKING;
		}
	}
	req = raw->queued_write;
	if (req != NULL &&
	    !(raw->flags & (RAW3215_WORKING | RAW3215_STOPPED))) {
		/* dequeue request */
		raw->queued_write = NULL;
		res = ccw_device_start(raw->cdev, req->ccws,
				       (unsigned long) req, 0, 0);
		if (res != 0) {
			/* do_IO failed, put request back to queue */
			raw->queued_write = req;
		} else {
			raw->flags |= RAW3215_WORKING;
		}
	}
}

/*
 * Function to start a delayed output after RAW3215_TIMEOUT seconds
 */
static void
raw3215_timeout(unsigned long __data)
{
	struct raw3215_info *raw = (struct raw3215_info *) __data;
	unsigned long flags;

	spin_lock_irqsave(raw->lock, flags);
	if (raw->flags & RAW3215_TIMER_RUNS) {
		del_timer(&raw->timer);
		raw->flags &= ~RAW3215_TIMER_RUNS;
		raw3215_mk_write_req(raw);
		raw3215_start_io(raw);
	}
	spin_unlock_irqrestore(raw->lock, flags);
}

/*
 * Function to conditionally start an IO. A read is started immediately,
 * a write is only started immediately if the flush flag is on or the
 * amount of data is bigger than RAW3215_MIN_WRITE. If a write is not
 * done immediately a timer is started with a delay of RAW3215_TIMEOUT.
 */
static inline void
raw3215_try_io(struct raw3215_info *raw)
{
	if (!(raw->flags & RAW3215_ACTIVE))
		return;
	if (raw->queued_read != NULL)
		raw3215_start_io(raw);
	else if (raw->queued_write != NULL) {
		if ((raw->queued_write->delayable == 0) ||
		    (raw->flags & RAW3215_FLUSHING)) {
			/* execute write requests bigger than minimum size */
			raw3215_start_io(raw);
			if (raw->flags & RAW3215_TIMER_RUNS) {
				del_timer(&raw->timer);
				raw->flags &= ~RAW3215_TIMER_RUNS;
			}
		} else if (!(raw->flags & RAW3215_TIMER_RUNS)) {
			/* delay small writes */
			init_timer(&raw->timer);
			raw->timer.expires = RAW3215_TIMEOUT + jiffies;
			raw->timer.data = (unsigned long) raw;
			raw->timer.function = raw3215_timeout;
			add_timer(&raw->timer);
			raw->flags |= RAW3215_TIMER_RUNS;
		}
	}
}

/*
 * The bottom half handler routine for 3215 devices. It tries to start
 * the next IO and wakes up processes waiting on the tty.
 */
static void
raw3215_tasklet(void *data)
{
	struct raw3215_info *raw;
	struct tty_struct *tty;
	unsigned long flags;

	raw = (struct raw3215_info *) data;
	spin_lock_irqsave(raw->lock, flags);
	raw3215_mk_write_req(raw);
	raw3215_try_io(raw);
	spin_unlock_irqrestore(raw->lock, flags);
	/* Check for pending message from raw3215_irq */
	if (raw->message != NULL) {
		printk(raw->message, raw->msg_dstat, raw->msg_cstat);
		raw->message = NULL;
	}
	tty = raw->tty;
	if (tty != NULL &&
	    RAW3215_BUFFER_SIZE - raw->count >= RAW3215_MIN_SPACE) {
	    	tty_wakeup(tty);
	}
}

/*
 * Interrupt routine, called from common io layer
 */
static void
raw3215_irq(struct ccw_device *cdev, unsigned long intparm, struct irb *irb)
{
	struct raw3215_info *raw;
	struct raw3215_req *req;
	struct tty_struct *tty;
	int cstat, dstat;
	int count, slen;

	raw = cdev->dev.driver_data;
	req = (struct raw3215_req *) intparm;
	cstat = irb->scsw.cstat;
	dstat = irb->scsw.dstat;
	if (cstat != 0) {
		raw->message = KERN_WARNING
			"Got nonzero channel status in raw3215_irq "
			"(dev sts 0x%2x, sch sts 0x%2x)";
		raw->msg_dstat = dstat;
		raw->msg_cstat = cstat;
		tasklet_schedule(&raw->tasklet);
	}
	if (dstat & 0x01) { /* we got a unit exception */
		dstat &= ~0x01;	 /* we can ignore it */
	}
	switch (dstat) {
	case 0x80:
		if (cstat != 0)
			break;
		/* Attention interrupt, someone hit the enter key */
		raw3215_mk_read_req(raw);
		if (MACHINE_IS_P390)
			memset(raw->inbuf, 0, RAW3215_INBUF_SIZE);
		tasklet_schedule(&raw->tasklet);
		break;
	case 0x08:
	case 0x0C:
		/* Channel end interrupt. */
		if ((raw = req->info) == NULL)
			return;		     /* That shouldn't happen ... */
		if (req->type == RAW3215_READ) {
			/* store residual count, then wait for device end */
			req->residual = irb->scsw.count;
		}
		if (dstat == 0x08)
			break;
	case 0x04:
		/* Device end interrupt. */
		if ((raw = req->info) == NULL)
			return;		     /* That shouldn't happen ... */
		if (req->type == RAW3215_READ && raw->tty != NULL) {
			unsigned int cchar;

			tty = raw->tty;
			count = 160 - req->residual;
			if (MACHINE_IS_P390) {
				slen = strnlen(raw->inbuf, RAW3215_INBUF_SIZE);
				if (count > slen)
					count = slen;
			} else
			EBCASC(raw->inbuf, count);
			cchar = ctrlchar_handle(raw->inbuf, count, tty);
			switch (cchar & CTRLCHAR_MASK) {
			case CTRLCHAR_SYSRQ:
				break;

			case CTRLCHAR_CTRL:
				tty_insert_flip_char(tty, cchar, TTY_NORMAL);
				tty_flip_buffer_push(raw->tty);
				break;

			case CTRLCHAR_NONE:
				if (count < 2 ||
				    (strncmp(raw->inbuf+count-2, "\252n", 2) &&
				     strncmp(raw->inbuf+count-2, "^n", 2)) ) {
					/* add the auto \n */
					raw->inbuf[count] = '\n';
					count++;
				} else
					count -= 2;
				tty_insert_flip_string(tty, raw->inbuf, count);
				tty_flip_buffer_push(raw->tty);
				break;
			}
		} else if (req->type == RAW3215_WRITE) {
			raw->count -= req->len;
			raw->written -= req->len;
		}
		raw->flags &= ~RAW3215_WORKING;
		raw3215_free_req(req);
		/* check for empty wait */
		if (waitqueue_active(&raw->empty_wait) &&
		    raw->queued_write == NULL &&
		    raw->queued_read == NULL) {
			wake_up_interruptible(&raw->empty_wait);
		}
		tasklet_schedule(&raw->tasklet);
		break;
	default:
		/* Strange interrupt, I'll do my best to clean up */
		if (req != NULL && req->type != RAW3215_FREE) {
			if (req->type == RAW3215_WRITE) {
				raw->count -= req->len;
				raw->written -= req->len;
			}
			raw->flags &= ~RAW3215_WORKING;
			raw3215_free_req(req);
		}
		raw->message = KERN_WARNING
			"Spurious interrupt in in raw3215_irq "
			"(dev sts 0x%2x, sch sts 0x%2x)";
		raw->msg_dstat = dstat;
		raw->msg_cstat = cstat;
		tasklet_schedule(&raw->tasklet);
	}
	return;
}

/*
 * Wait until length bytes are available int the output buffer.
 * Has to be called with the s390irq lock held. Can be called
 * disabled.
 */
static void
raw3215_make_room(struct raw3215_info *raw, unsigned int length)
{
	while (RAW3215_BUFFER_SIZE - raw->count < length) {
		/* there might be a request pending */
		raw->flags |= RAW3215_FLUSHING;
		raw3215_mk_write_req(raw);
		raw3215_try_io(raw);
		raw->flags &= ~RAW3215_FLUSHING;
#ifdef CONFIG_TN3215_CONSOLE
		wait_cons_dev();
#endif
		/* Enough room freed up ? */
		if (RAW3215_BUFFER_SIZE - raw->count >= length)
			break;
		/* there might be another cpu waiting for the lock */
		spin_unlock(raw->lock);
		udelay(100);
		spin_lock(raw->lock);
	}
}

/*
 * String write routine for 3215 devices
 */
static void
raw3215_write(struct raw3215_info *raw, const char *str, unsigned int length)
{
	unsigned long flags;
	int c, count;

	while (length > 0) {
		spin_lock_irqsave(raw->lock, flags);
		count = (length > RAW3215_BUFFER_SIZE) ?
					     RAW3215_BUFFER_SIZE : length;
		length -= count;

		raw3215_make_room(raw, count);

		/* copy string to output buffer and convert it to EBCDIC */
		while (1) {
			c = min_t(int, count,
				  min(RAW3215_BUFFER_SIZE - raw->count,
				      RAW3215_BUFFER_SIZE - raw->head));
			if (c <= 0)
				break;
			memcpy(raw->buffer + raw->head, str, c);
			ASCEBC(raw->buffer + raw->head, c);
			raw->head = (raw->head + c) & (RAW3215_BUFFER_SIZE - 1);
			raw->count += c;
			raw->line_pos += c;
			str += c;
			count -= c;
		}
		if (!(raw->flags & RAW3215_WORKING)) {
			raw3215_mk_write_req(raw);
			/* start or queue request */
			raw3215_try_io(raw);
		}
		spin_unlock_irqrestore(raw->lock, flags);
	}
}

/*
 * Put character routine for 3215 devices
 */
static void
raw3215_putchar(struct raw3215_info *raw, unsigned char ch)
{
	unsigned long flags;
	unsigned int length, i;

	spin_lock_irqsave(raw->lock, flags);
	if (ch == '\t') {
		length = TAB_STOP_SIZE - (raw->line_pos%TAB_STOP_SIZE);
		raw->line_pos += length;
		ch = ' ';
	} else if (ch == '\n') {
		length = 1;
		raw->line_pos = 0;
	} else {
		length = 1;
		raw->line_pos++;
	}
	raw3215_make_room(raw, length);

	for (i = 0; i < length; i++) {
		raw->buffer[raw->head] = (char) _ascebc[(int) ch];
		raw->head = (raw->head + 1) & (RAW3215_BUFFER_SIZE - 1);
		raw->count++;
	}
	if (!(raw->flags & RAW3215_WORKING)) {
		raw3215_mk_write_req(raw);
		/* start or queue request */
		raw3215_try_io(raw);
	}
	spin_unlock_irqrestore(raw->lock, flags);
}

/*
 * Flush routine, it simply sets the flush flag and tries to start
 * pending IO.
 */
static void
raw3215_flush_buffer(struct raw3215_info *raw)
{
	unsigned long flags;

	spin_lock_irqsave(raw->lock, flags);
	if (raw->count > 0) {
		raw->flags |= RAW3215_FLUSHING;
		raw3215_try_io(raw);
		raw->flags &= ~RAW3215_FLUSHING;
	}
	spin_unlock_irqrestore(raw->lock, flags);
}

/*
 * Fire up a 3215 device.
 */
static int
raw3215_startup(struct raw3215_info *raw)
{
	unsigned long flags;

	if (raw->flags & RAW3215_ACTIVE)
		return 0;
	raw->line_pos = 0;
	raw->flags |= RAW3215_ACTIVE;
	spin_lock_irqsave(raw->lock, flags);
	raw3215_try_io(raw);
	spin_unlock_irqrestore(raw->lock, flags);

	return 0;
}

/*
 * Shutdown a 3215 device.
 */
static void
raw3215_shutdown(struct raw3215_info *raw)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;

	if (!(raw->flags & RAW3215_ACTIVE) || (raw->flags & RAW3215_FIXED))
		return;
	/* Wait for outstanding requests, then free irq */
	spin_lock_irqsave(raw->lock, flags);
	if ((raw->flags & RAW3215_WORKING) ||
	    raw->queued_write != NULL ||
	    raw->queued_read != NULL) {
		raw->flags |= RAW3215_CLOSING;
		add_wait_queue(&raw->empty_wait, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(raw->lock, flags);
		schedule();
		spin_lock_irqsave(raw->lock, flags);
		remove_wait_queue(&raw->empty_wait, &wait);
		set_current_state(TASK_RUNNING);
		raw->flags &= ~(RAW3215_ACTIVE | RAW3215_CLOSING);
	}
	spin_unlock_irqrestore(raw->lock, flags);
}

static int
raw3215_probe (struct ccw_device *cdev)
{
	struct raw3215_info *raw;
	int line;

	raw = kmalloc(sizeof(struct raw3215_info) +
		      RAW3215_INBUF_SIZE, GFP_KERNEL|GFP_DMA);
	if (raw == NULL)
		return -ENOMEM;

	spin_lock(&raw3215_device_lock);
	for (line = 0; line < NR_3215; line++) {
		if (!raw3215[line]) {
			raw3215[line] = raw;
			break;
		}
	}
	spin_unlock(&raw3215_device_lock);
	if (line == NR_3215) {
		kfree(raw);
		return -ENODEV;
	}

	raw->cdev = cdev;
	raw->lock = get_ccwdev_lock(cdev);
	raw->inbuf = (char *) raw + sizeof(struct raw3215_info);
	memset(raw, 0, sizeof(struct raw3215_info));
	raw->buffer = (char *) kmalloc(RAW3215_BUFFER_SIZE,
				       GFP_KERNEL|GFP_DMA);
	if (raw->buffer == NULL) {
		spin_lock(&raw3215_device_lock);
		raw3215[line] = 0;
		spin_unlock(&raw3215_device_lock);
		kfree(raw);
		return -ENOMEM;
	}
	tasklet_init(&raw->tasklet,
		     (void (*)(unsigned long)) raw3215_tasklet,
		     (unsigned long) raw);
	init_waitqueue_head(&raw->empty_wait);

	cdev->dev.driver_data = raw;
	cdev->handler = raw3215_irq;

	return 0;
}

static void
raw3215_remove (struct ccw_device *cdev)
{
	struct raw3215_info *raw;

	ccw_device_set_offline(cdev);
	raw = cdev->dev.driver_data;
	if (raw) {
		cdev->dev.driver_data = NULL;
		kfree(raw->buffer);
		kfree(raw);
	}
}

static int
raw3215_set_online (struct ccw_device *cdev)
{
	struct raw3215_info *raw;

	raw = cdev->dev.driver_data;
	if (!raw)
		return -ENODEV;

	return raw3215_startup(raw);
}

static int
raw3215_set_offline (struct ccw_device *cdev)
{
	struct raw3215_info *raw;

	raw = cdev->dev.driver_data;
	if (!raw)
		return -ENODEV;

	raw3215_shutdown(raw);

	return 0;
}

static struct ccw_device_id raw3215_id[] = {
	{ CCW_DEVICE(0x3215, 0) },
	{ /* end of list */ },
};

static struct ccw_driver raw3215_ccw_driver = {
	.name		= "3215",
	.owner		= THIS_MODULE,
	.ids		= raw3215_id,
	.probe		= &raw3215_probe,
	.remove		= &raw3215_remove,
	.set_online	= &raw3215_set_online,
	.set_offline	= &raw3215_set_offline,
};

#ifdef CONFIG_TN3215_CONSOLE
/*
 * Write a string to the 3215 console
 */
static void
con3215_write(struct console *co, const char *str, unsigned int count)
{
	struct raw3215_info *raw;
	int i;

	if (count <= 0)
		return;
	raw = raw3215[0];	/* console 3215 is the first one */
	while (count > 0) {
		for (i = 0; i < count; i++)
			if (str[i] == '\t' || str[i] == '\n')
				break;
		raw3215_write(raw, str, i);
		count -= i;
		str += i;
		if (count > 0) {
			raw3215_putchar(raw, *str);
			count--;
			str++;
		}
	}
}

static struct tty_driver *con3215_device(struct console *c, int *index)
{
	*index = c->index;
	return tty3215_driver;
}

/*
 * panic() calls console_unblank before the system enters a
 * disabled, endless loop.
 */
static void
con3215_unblank(void)
{
	struct raw3215_info *raw;
	unsigned long flags;

	raw = raw3215[0];  /* console 3215 is the first one */
	spin_lock_irqsave(raw->lock, flags);
	raw3215_make_room(raw, RAW3215_BUFFER_SIZE);
	spin_unlock_irqrestore(raw->lock, flags);
}

static int __init 
con3215_consetup(struct console *co, char *options)
{
	return 0;
}

/*
 *  The console structure for the 3215 console
 */
static struct console con3215 = {
	.name	 = "ttyS",
	.write	 = con3215_write,
	.device	 = con3215_device,
	.unblank = con3215_unblank,
	.setup	 = con3215_consetup,
	.flags	 = CON_PRINTBUFFER,
};

/*
 * 3215 console initialization code called from console_init().
 * NOTE: This is called before kmalloc is available.
 */
static int __init
con3215_init(void)
{
	struct ccw_device *cdev;
	struct raw3215_info *raw;
	struct raw3215_req *req;
	int i;

	/* Check if 3215 is to be the console */
	if (!CONSOLE_IS_3215)
		return -ENODEV;

	/* Set the console mode for VM */
	if (MACHINE_IS_VM) {
		cpcmd("TERM CONMODE 3215", NULL, 0, NULL);
		cpcmd("TERM AUTOCR OFF", NULL, 0, NULL);
	}

	/* allocate 3215 request structures */
	raw3215_freelist = NULL;
	spin_lock_init(&raw3215_freelist_lock);
	for (i = 0; i < NR_3215_REQ; i++) {
		req = (struct raw3215_req *) alloc_bootmem_low(sizeof(struct raw3215_req));
		req->next = raw3215_freelist;
		raw3215_freelist = req;
	}

	cdev = ccw_device_probe_console();
	if (!cdev)
		return -ENODEV;

	raw3215[0] = raw = (struct raw3215_info *)
		alloc_bootmem_low(sizeof(struct raw3215_info));
	memset(raw, 0, sizeof(struct raw3215_info));
	raw->buffer = (char *) alloc_bootmem_low(RAW3215_BUFFER_SIZE);
	raw->inbuf = (char *) alloc_bootmem_low(RAW3215_INBUF_SIZE);
	raw->cdev = cdev;
	raw->lock = get_ccwdev_lock(cdev);
	cdev->dev.driver_data = raw;
	cdev->handler = raw3215_irq;

	raw->flags |= RAW3215_FIXED;
	tasklet_init(&raw->tasklet,
		     (void (*)(unsigned long)) raw3215_tasklet,
		     (unsigned long) raw);
	init_waitqueue_head(&raw->empty_wait);

	/* Request the console irq */
	if (raw3215_startup(raw) != 0) {
		free_bootmem((unsigned long) raw->inbuf, RAW3215_INBUF_SIZE);
		free_bootmem((unsigned long) raw->buffer, RAW3215_BUFFER_SIZE);
		free_bootmem((unsigned long) raw, sizeof(struct raw3215_info));
		raw3215[0] = NULL;
		printk("Couldn't find a 3215 console device\n");
		return -ENODEV;
	}
	register_console(&con3215);
	return 0;
}
console_initcall(con3215_init);
#endif

/*
 * tty3215_open
 *
 * This routine is called whenever a 3215 tty is opened.
 */
static int
tty3215_open(struct tty_struct *tty, struct file * filp)
{
	struct raw3215_info *raw;
	int retval, line;

	line = tty->index;
	if ((line < 0) || (line >= NR_3215))
		return -ENODEV;

	raw = raw3215[line];
	if (raw == NULL)
		return -ENODEV;

	tty->driver_data = raw;
	raw->tty = tty;

	tty->low_latency = 0;  /* don't use bottom half for pushing chars */
	/*
	 * Start up 3215 device
	 */
	retval = raw3215_startup(raw);
	if (retval)
		return retval;

	return 0;
}

/*
 * tty3215_close()
 *
 * This routine is called when the 3215 tty is closed. We wait
 * for the remaining request to be completed. Then we clean up.
 */
static void
tty3215_close(struct tty_struct *tty, struct file * filp)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;
	if (raw == NULL || tty->count > 1)
		return;
	tty->closing = 1;
	/* Shutdown the terminal */
	raw3215_shutdown(raw);
	tty->closing = 0;
	raw->tty = NULL;
}

/*
 * Returns the amount of free space in the output buffer.
 */
static int
tty3215_write_room(struct tty_struct *tty)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;

	/* Subtract TAB_STOP_SIZE to allow for a tab, 8 <<< 64K */
	if ((RAW3215_BUFFER_SIZE - raw->count - TAB_STOP_SIZE) >= 0)
		return RAW3215_BUFFER_SIZE - raw->count - TAB_STOP_SIZE;
	else
		return 0;
}

/*
 * String write routine for 3215 ttys
 */
static int
tty3215_write(struct tty_struct * tty,
	      const unsigned char *buf, int count)
{
	struct raw3215_info *raw;

	if (!tty)
		return 0;
	raw = (struct raw3215_info *) tty->driver_data;
	raw3215_write(raw, buf, count);
	return count;
}

/*
 * Put character routine for 3215 ttys
 */
static void
tty3215_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct raw3215_info *raw;

	if (!tty)
		return;
	raw = (struct raw3215_info *) tty->driver_data;
	raw3215_putchar(raw, ch);
}

static void
tty3215_flush_chars(struct tty_struct *tty)
{
}

/*
 * Returns the number of characters in the output buffer
 */
static int
tty3215_chars_in_buffer(struct tty_struct *tty)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;
	return raw->count;
}

static void
tty3215_flush_buffer(struct tty_struct *tty)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;
	raw3215_flush_buffer(raw);
	tty_wakeup(tty);
}

/*
 * Currently we don't have any io controls for 3215 ttys
 */
static int
tty3215_ioctl(struct tty_struct *tty, struct file * file,
	      unsigned int cmd, unsigned long arg)
{
	if (tty->flags & (1 << TTY_IO_ERROR))
		return -EIO;

	switch (cmd) {
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

/*
 * Disable reading from a 3215 tty
 */
static void
tty3215_throttle(struct tty_struct * tty)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;
	raw->flags |= RAW3215_THROTTLED;
}

/*
 * Enable reading from a 3215 tty
 */
static void
tty3215_unthrottle(struct tty_struct * tty)
{
	struct raw3215_info *raw;
	unsigned long flags;

	raw = (struct raw3215_info *) tty->driver_data;
	if (raw->flags & RAW3215_THROTTLED) {
		spin_lock_irqsave(raw->lock, flags);
		raw->flags &= ~RAW3215_THROTTLED;
		raw3215_try_io(raw);
		spin_unlock_irqrestore(raw->lock, flags);
	}
}

/*
 * Disable writing to a 3215 tty
 */
static void
tty3215_stop(struct tty_struct *tty)
{
	struct raw3215_info *raw;

	raw = (struct raw3215_info *) tty->driver_data;
	raw->flags |= RAW3215_STOPPED;
}

/*
 * Enable writing to a 3215 tty
 */
static void
tty3215_start(struct tty_struct *tty)
{
	struct raw3215_info *raw;
	unsigned long flags;

	raw = (struct raw3215_info *) tty->driver_data;
	if (raw->flags & RAW3215_STOPPED) {
		spin_lock_irqsave(raw->lock, flags);
		raw->flags &= ~RAW3215_STOPPED;
		raw3215_try_io(raw);
		spin_unlock_irqrestore(raw->lock, flags);
	}
}

static struct tty_operations tty3215_ops = {
	.open = tty3215_open,
	.close = tty3215_close,
	.write = tty3215_write,
	.put_char = tty3215_put_char,
	.flush_chars = tty3215_flush_chars,
	.write_room = tty3215_write_room,
	.chars_in_buffer = tty3215_chars_in_buffer,
	.flush_buffer = tty3215_flush_buffer,
	.ioctl = tty3215_ioctl,
	.throttle = tty3215_throttle,
	.unthrottle = tty3215_unthrottle,
	.stop = tty3215_stop,
	.start = tty3215_start,
};

/*
 * 3215 tty registration code called from tty_init().
 * Most kernel services (incl. kmalloc) are available at this poimt.
 */
int __init
tty3215_init(void)
{
	struct tty_driver *driver;
	int ret;

	if (!CONSOLE_IS_3215)
		return 0;

	driver = alloc_tty_driver(NR_3215);
	if (!driver)
		return -ENOMEM;

	ret = ccw_driver_register(&raw3215_ccw_driver);
	if (ret) {
		put_tty_driver(driver);
		return ret;
	}
	/*
	 * Initialize the tty_driver structure
	 * Entries in tty3215_driver that are NOT initialized:
	 * proc_entry, set_termios, flush_buffer, set_ldisc, write_proc
	 */

	driver->owner = THIS_MODULE;
	driver->driver_name = "tty3215";
	driver->name = "ttyS";
	driver->major = TTY_MAJOR;
	driver->minor_start = 64;
	driver->type = TTY_DRIVER_TYPE_SYSTEM;
	driver->subtype = SYSTEM_TYPE_TTY;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_iflag = IGNBRK | IGNPAR;
	driver->init_termios.c_oflag = ONLCR | XTABS;
	driver->init_termios.c_lflag = ISIG;
	driver->flags = TTY_DRIVER_REAL_RAW;
	tty_set_operations(driver, &tty3215_ops);
	ret = tty_register_driver(driver);
	if (ret) {
		printk("Couldn't register tty3215 driver\n");
		put_tty_driver(driver);
		return ret;
	}
	tty3215_driver = driver;
	return 0;
}

static void __exit
tty3215_exit(void)
{
	tty_unregister_driver(tty3215_driver);
	put_tty_driver(tty3215_driver);
	ccw_driver_unregister(&raw3215_ccw_driver);
}

module_init(tty3215_init);
module_exit(tty3215_exit);
