/*
 * hfc_usb.c
 *
 * $Id: hfc_usb.c,v 4.36 2005/04/08 09:55:13 martinb1 Exp $
 *
 * modular HiSax ISDN driver for Colognechip HFC-S USB chip
 *
 * Authors : Peter Sprenger  (sprenger@moving-bytes.de)
 *           Martin Bachem   (info@colognechip.com)
 *
 *           based on the first hfc_usb driver of
 *           Werner Cornelius (werner@isdn-development.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * See Version Histroy at the bottom of this file
 *
*/

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/timer.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/smp_lock.h>
#include <linux/sched.h>
#include "hisax.h"
#include "hisax_if.h"
#include "hfc_usb.h"

static const char *hfcusb_revision =
    "$Revision: 4.36 $ $Date: 2005/04/08 09:55:13 $ ";

/* Hisax debug support
* use "modprobe debug=x" where x is bitfield of USB_DBG & ISDN_DBG
*/
#ifdef CONFIG_HISAX_DEBUG
#include <linux/moduleparam.h>
#define __debug_variable hfc_debug
#include "hisax_debug.h"
static u_int debug;
module_param(debug, uint, 0);
static int hfc_debug;
#endif

/* private vendor specific data */
typedef struct {
	__u8 led_scheme;	// led display scheme
	signed short led_bits[8];	// array of 8 possible LED bitmask settings
	char *vend_name;	// device name
} hfcsusb_vdata;

/****************************************/
/* data defining the devices to be used */
/****************************************/
static struct usb_device_id hfcusb_idtab[] = {
	{
	 USB_DEVICE(0x0959, 0x2bd0),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_OFF, {4, 0, 2, 1},
			   "ISDN USB TA (Cologne Chip HFC-S USB based)"}),
	},
	{
	 USB_DEVICE(0x0675, 0x1688),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {1, 2, 0, 0},
			   "DrayTek miniVigor 128 USB ISDN TA"}),
	},
	{
	 USB_DEVICE(0x07b0, 0x0007),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {0x80, -64, -32, -16},
			   "Billion tiny USB ISDN TA 128"}),
	},
	{
	 USB_DEVICE(0x0742, 0x2008),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {4, 0, 2, 1},
			   "Stollmann USB TA"}),
	 },
	{
	 USB_DEVICE(0x0742, 0x2009),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {4, 0, 2, 1},
			   "Aceex USB ISDN TA"}),
	 },
	{
	 USB_DEVICE(0x0742, 0x200A),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {4, 0, 2, 1},
			   "OEM USB ISDN TA"}),
	 },
	{
	 USB_DEVICE(0x08e3, 0x0301),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {2, 0, 1, 4},
			   "Olitec USB RNIS"}),
	 },
	{
	 USB_DEVICE(0x07fa, 0x0846),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {0x80, -64, -32, -16},
			   "Bewan Modem RNIS USB"}),
	 },
	{
	 USB_DEVICE(0x07fa, 0x0847),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {0x80, -64, -32, -16},
			   "Djinn Numeris USB"}),
	 },
	{
	 USB_DEVICE(0x07b0, 0x0006),
	 .driver_info = (unsigned long) &((hfcsusb_vdata)
			  {LED_SCHEME1, {0x80, -64, -32, -16},
			   "Twister ISDN TA"}),
	 },
	{ }
};

/***************************************************************/
/* structure defining input+output fifos (interrupt/bulk mode) */
/***************************************************************/
struct usb_fifo;		/* forward definition */
typedef struct iso_urb_struct {
	struct urb *purb;
	__u8 buffer[ISO_BUFFER_SIZE];	/* buffer incoming/outgoing data */
	struct usb_fifo *owner_fifo;	/* pointer to owner fifo */
} iso_urb_struct;


struct hfcusb_data;		/* forward definition */
typedef struct usb_fifo {
	int fifonum;		/* fifo index attached to this structure */
	int active;		/* fifo is currently active */
	struct hfcusb_data *hfc;	/* pointer to main structure */
	int pipe;		/* address of endpoint */
	__u8 usb_packet_maxlen;	/* maximum length for usb transfer */
	unsigned int max_size;	/* maximum size of receive/send packet */
	__u8 intervall;		/* interrupt interval */
	struct sk_buff *skbuff;	/* actual used buffer */
	struct urb *urb;	/* transfer structure for usb routines */
	__u8 buffer[128];	/* buffer incoming/outgoing data */
	int bit_line;		/* how much bits are in the fifo? */

	volatile __u8 usb_transfer_mode;	/* switched between ISO and INT */
	iso_urb_struct iso[2];	/* need two urbs to have one always for pending */
	struct hisax_if *hif;	/* hisax interface */
	int delete_flg;		/* only delete skbuff once */
	int last_urblen;	/* remember length of last packet */

} usb_fifo;

/*********************************************/
/* structure holding all data for one device */
/*********************************************/
typedef struct hfcusb_data {
	/* HiSax Interface for loadable Layer1 drivers */
	struct hisax_d_if d_if;	/* see hisax_if.h */
	struct hisax_b_if b_if[2];	/* see hisax_if.h */
	int protocol;

	struct usb_device *dev;	/* our device */
	int if_used;		/* used interface number */
	int alt_used;		/* used alternate config */
	int ctrl_paksize;	/* control pipe packet size */
	int ctrl_in_pipe, ctrl_out_pipe;	/* handles for control pipe */
	int cfg_used;		/* configuration index used */
	int vend_idx;		/* vendor found */
	int b_mode[2];		/* B-channel mode */
	int l1_activated;	/* layer 1 activated */
	int disc_flag;		/* TRUE if device was disonnected to avoid some USB actions */
	int packet_size, iso_packet_size;

	/* control pipe background handling */
	ctrl_buft ctrl_buff[HFC_CTRL_BUFSIZE];	/* buffer holding queued data */
	volatile int ctrl_in_idx, ctrl_out_idx, ctrl_cnt;	/* input/output pointer + count */
	struct urb *ctrl_urb;	/* transfer structure for control channel */

	struct usb_ctrlrequest ctrl_write;	/* buffer for control write request */
	struct usb_ctrlrequest ctrl_read;	/* same for read request */

	__u8 old_led_state, led_state, led_new_data, led_b_active;

	volatile __u8 threshold_mask;	/* threshold actually reported */
	volatile __u8 bch_enables;	/* or mask for sctrl_r and sctrl register values */

	usb_fifo fifos[HFCUSB_NUM_FIFOS];	/* structure holding all fifo data */

	volatile __u8 l1_state;	/* actual l1 state */
	struct timer_list t3_timer;	/* timer 3 for activation/deactivation */
	struct timer_list t4_timer;	/* timer 4 for activation/deactivation */
} hfcusb_data;


static void collect_rx_frame(usb_fifo * fifo, __u8 * data, int len,
			     int finish);


static inline const char *
symbolic(struct hfcusb_symbolic_list list[], const int num)
{
	int i;
	for (i = 0; list[i].name != NULL; i++)
		if (list[i].num == num)
			return (list[i].name);
	return "<unkown ERROR>";
}


/******************************************************/
/* start next background transfer for control channel */
/******************************************************/
static void
ctrl_start_transfer(hfcusb_data * hfc)
{
	if (hfc->ctrl_cnt) {
		hfc->ctrl_urb->pipe = hfc->ctrl_out_pipe;
		hfc->ctrl_urb->setup_packet = (u_char *) & hfc->ctrl_write;
		hfc->ctrl_urb->transfer_buffer = NULL;
		hfc->ctrl_urb->transfer_buffer_length = 0;
		hfc->ctrl_write.wIndex =
		    hfc->ctrl_buff[hfc->ctrl_out_idx].hfc_reg;
		hfc->ctrl_write.wValue =
		    hfc->ctrl_buff[hfc->ctrl_out_idx].reg_val;

		usb_submit_urb(hfc->ctrl_urb, GFP_ATOMIC);	/* start transfer */
	}
}				/* ctrl_start_transfer */

/************************************/
/* queue a control transfer request */
/* return 0 on success.             */
/************************************/
static int
queue_control_request(hfcusb_data * hfc, __u8 reg, __u8 val, int action)
{
	ctrl_buft *buf;

	if (hfc->ctrl_cnt >= HFC_CTRL_BUFSIZE)
		return (1);	/* no space left */
	buf = &hfc->ctrl_buff[hfc->ctrl_in_idx];	/* pointer to new index */
	buf->hfc_reg = reg;
	buf->reg_val = val;
	buf->action = action;
	if (++hfc->ctrl_in_idx >= HFC_CTRL_BUFSIZE)
		hfc->ctrl_in_idx = 0;	/* pointer wrap */
	if (++hfc->ctrl_cnt == 1)
		ctrl_start_transfer(hfc);
	return (0);
}				/* queue_control_request */

static int
control_action_handler(hfcusb_data * hfc, int reg, int val, int action)
{
	if (!action)
		return (1);	/* no action defined */
	return (0);
}

/***************************************************************/
/* control completion routine handling background control cmds */
/***************************************************************/
static void
ctrl_complete(struct urb *urb, struct pt_regs *regs)
{
	hfcusb_data *hfc = (hfcusb_data *) urb->context;
	ctrl_buft *buf;

	urb->dev = hfc->dev;
	if (hfc->ctrl_cnt) {
		buf = &hfc->ctrl_buff[hfc->ctrl_out_idx];
		control_action_handler(hfc, buf->hfc_reg, buf->reg_val,
				       buf->action);

		hfc->ctrl_cnt--;	/* decrement actual count */
		if (++hfc->ctrl_out_idx >= HFC_CTRL_BUFSIZE)
			hfc->ctrl_out_idx = 0;	/* pointer wrap */

		ctrl_start_transfer(hfc);	/* start next transfer */
	}
}				/* ctrl_complete */

/***************************************************/
/* write led data to auxport & invert if necessary */
/***************************************************/
static void
write_led(hfcusb_data * hfc, __u8 led_state)
{
	if (led_state != hfc->old_led_state) {
		hfc->old_led_state = led_state;
		queue_control_request(hfc, HFCUSB_P_DATA, led_state, 1);
	}
}

/**************************/
/* handle LED bits        */
/**************************/
static void
set_led_bit(hfcusb_data * hfc, signed short led_bits, int unset)
{
	if (unset) {
		if (led_bits < 0)
			hfc->led_state |= abs(led_bits);
		else
			hfc->led_state &= ~led_bits;
	} else {
		if (led_bits < 0)
			hfc->led_state &= ~abs(led_bits);
		else
			hfc->led_state |= led_bits;
	}
}

/**************************/
/* handle LED requests    */
/**************************/
static void
handle_led(hfcusb_data * hfc, int event)
{
	hfcsusb_vdata *driver_info =
	    (hfcsusb_vdata *) hfcusb_idtab[hfc->vend_idx].driver_info;

	/* if no scheme -> no LED action */
	if (driver_info->led_scheme == LED_OFF)
		return;

	switch (event) {
		case LED_POWER_ON:
			set_led_bit(hfc, driver_info->led_bits[0],
				    0);
			set_led_bit(hfc, driver_info->led_bits[1],
				    1);
			set_led_bit(hfc, driver_info->led_bits[2],
				    1);
			set_led_bit(hfc, driver_info->led_bits[3],
				    1);
			break;
		case LED_POWER_OFF:	/* no Power off handling */
			break;
		case LED_S0_ON:
			set_led_bit(hfc, driver_info->led_bits[1],
				    0);
			break;
		case LED_S0_OFF:
			set_led_bit(hfc, driver_info->led_bits[1],
				    1);
			break;
		case LED_B1_ON:
			set_led_bit(hfc, driver_info->led_bits[2],
				    0);
			break;
		case LED_B1_OFF:
			set_led_bit(hfc, driver_info->led_bits[2],
				    1);
			break;
		case LED_B2_ON:
			set_led_bit(hfc, driver_info->led_bits[3],
				    0);
			break;
		case LED_B2_OFF:
			set_led_bit(hfc, driver_info->led_bits[3],
				    1);
			break;
	}
	write_led(hfc, hfc->led_state);
}

/********************************/
/* called when timer t3 expires */
/********************************/
static void
l1_timer_expire_t3(hfcusb_data * hfc)
{
	hfc->d_if.ifc.l1l2(&hfc->d_if.ifc, PH_DEACTIVATE | INDICATION,
			   NULL);
#ifdef CONFIG_HISAX_DEBUG
	DBG(ISDN_DBG,
	    "HFC-S USB: PH_DEACTIVATE | INDICATION sent (T3 expire)");
#endif
	hfc->l1_activated = FALSE;
	handle_led(hfc, LED_S0_OFF);
	/* deactivate : */
	queue_control_request(hfc, HFCUSB_STATES, 0x10, 1);
	queue_control_request(hfc, HFCUSB_STATES, 3, 1);
}

/********************************/
/* called when timer t4 expires */
/********************************/
static void
l1_timer_expire_t4(hfcusb_data * hfc)
{
	hfc->d_if.ifc.l1l2(&hfc->d_if.ifc, PH_DEACTIVATE | INDICATION,
			   NULL);
#ifdef CONFIG_HISAX_DEBUG
	DBG(ISDN_DBG,
	    "HFC-S USB: PH_DEACTIVATE | INDICATION sent (T4 expire)");
#endif
	hfc->l1_activated = FALSE;
	handle_led(hfc, LED_S0_OFF);
}

/*****************************/
/* handle S0 state changes   */
/*****************************/
static void
state_handler(hfcusb_data * hfc, __u8 state)
{
	__u8 old_state;

	old_state = hfc->l1_state;
	if (state == old_state || state < 1 || state > 8)
		return;

#ifdef CONFIG_HISAX_DEBUG
	DBG(ISDN_DBG, "HFC-S USB: new S0 state:%d old_state:%d", state,
	    old_state);
#endif
	if (state < 4 || state == 7 || state == 8) {
		if (timer_pending(&hfc->t3_timer))
			del_timer(&hfc->t3_timer);
#ifdef CONFIG_HISAX_DEBUG
		DBG(ISDN_DBG, "HFC-S USB: T3 deactivated");
#endif
	}
	if (state >= 7) {
		if (timer_pending(&hfc->t4_timer))
			del_timer(&hfc->t4_timer);
#ifdef CONFIG_HISAX_DEBUG
		DBG(ISDN_DBG, "HFC-S USB: T4 deactivated");
#endif
	}

	if (state == 7 && !hfc->l1_activated) {
		hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,
				   PH_ACTIVATE | INDICATION, NULL);
#ifdef CONFIG_HISAX_DEBUG
		DBG(ISDN_DBG, "HFC-S USB: PH_ACTIVATE | INDICATION sent");
#endif
		hfc->l1_activated = TRUE;
		handle_led(hfc, LED_S0_ON);
	} else if (state <= 3 /* && activated */ ) {
		if (old_state == 7 || old_state == 8) {
#ifdef CONFIG_HISAX_DEBUG
			DBG(ISDN_DBG, "HFC-S USB: T4 activated");
#endif
			if (!timer_pending(&hfc->t4_timer)) {
				hfc->t4_timer.expires =
				    jiffies + (HFC_TIMER_T4 * HZ) / 1000;
				add_timer(&hfc->t4_timer);
			}
		} else {
			hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,
					   PH_DEACTIVATE | INDICATION,
					   NULL);
#ifdef CONFIG_HISAX_DEBUG
			DBG(ISDN_DBG,
			    "HFC-S USB: PH_DEACTIVATE | INDICATION sent");
#endif
			hfc->l1_activated = FALSE;
			handle_led(hfc, LED_S0_OFF);
		}
	}
	hfc->l1_state = state;
}

/* prepare iso urb */
static void
fill_isoc_urb(struct urb *urb, struct usb_device *dev, unsigned int pipe,
	      void *buf, int num_packets, int packet_size, int interval,
	      usb_complete_t complete, void *context)
{
	int k;

	spin_lock_init(&urb->lock);
	urb->dev = dev;
	urb->pipe = pipe;
	urb->complete = complete;
	urb->number_of_packets = num_packets;
	urb->transfer_buffer_length = packet_size * num_packets;
	urb->context = context;
	urb->transfer_buffer = buf;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->actual_length = 0;
	urb->interval = interval;
	for (k = 0; k < num_packets; k++) {
		urb->iso_frame_desc[k].offset = packet_size * k;
		urb->iso_frame_desc[k].length = packet_size;
		urb->iso_frame_desc[k].actual_length = 0;
	}
}

/* allocs urbs and start isoc transfer with two pending urbs to avoid
   gaps in the transfer chain */
static int
start_isoc_chain(usb_fifo * fifo, int num_packets_per_urb,
		 usb_complete_t complete, int packet_size)
{
	int i, k, errcode;

	printk(KERN_INFO "HFC-S USB: starting ISO-chain for Fifo %i\n",
	       fifo->fifonum);

	/* allocate Memory for Iso out Urbs */
	for (i = 0; i < 2; i++) {
		if (!(fifo->iso[i].purb)) {
			fifo->iso[i].purb =
			    usb_alloc_urb(num_packets_per_urb, GFP_KERNEL);
			if (!(fifo->iso[i].purb)) {
				printk(KERN_INFO
				       "alloc urb for fifo %i failed!!!",
				       fifo->fifonum);
			}
			fifo->iso[i].owner_fifo = (struct usb_fifo *) fifo;

			/* Init the first iso */
			if (ISO_BUFFER_SIZE >=
			    (fifo->usb_packet_maxlen *
			     num_packets_per_urb)) {
				fill_isoc_urb(fifo->iso[i].purb,
					      fifo->hfc->dev, fifo->pipe,
					      fifo->iso[i].buffer,
					      num_packets_per_urb,
					      fifo->usb_packet_maxlen,
					      fifo->intervall, complete,
					      &fifo->iso[i]);
				memset(fifo->iso[i].buffer, 0,
				       sizeof(fifo->iso[i].buffer));
				/* defining packet delimeters in fifo->buffer */
				for (k = 0; k < num_packets_per_urb; k++) {
					fifo->iso[i].purb->
					    iso_frame_desc[k].offset =
					    k * packet_size;
					fifo->iso[i].purb->
					    iso_frame_desc[k].length =
					    packet_size;
				}
			} else {
				printk(KERN_INFO
				       "HFC-S USB: ISO Buffer size to small!\n");
			}
		}
		fifo->bit_line = BITLINE_INF;

		errcode = usb_submit_urb(fifo->iso[i].purb, GFP_KERNEL);
		fifo->active = (errcode >= 0) ? 1 : 0;
		if (errcode < 0) {
			printk(KERN_INFO "HFC-S USB: %s  URB nr:%d\n",
			       symbolic(urb_errlist, errcode), i);
		};
	}
	return (fifo->active);
}

/* stops running iso chain and frees their pending urbs */
static void
stop_isoc_chain(usb_fifo * fifo)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (fifo->iso[i].purb) {
#ifdef CONFIG_HISAX_DEBUG
			DBG(USB_DBG,
			    "HFC-S USB: Stopping iso chain for fifo %i.%i",
			    fifo->fifonum, i);
#endif
			usb_unlink_urb(fifo->iso[i].purb);
			usb_free_urb(fifo->iso[i].purb);
			fifo->iso[i].purb = NULL;
		}
	}
	if (fifo->urb) {
		usb_unlink_urb(fifo->urb);
		usb_free_urb(fifo->urb);
		fifo->urb = NULL;
	}
	fifo->active = 0;
}

/* defines how much ISO packets are handled in one URB */
static int iso_packets[8] =
    { ISOC_PACKETS_B, ISOC_PACKETS_B, ISOC_PACKETS_B, ISOC_PACKETS_B,
	ISOC_PACKETS_D, ISOC_PACKETS_D, ISOC_PACKETS_D, ISOC_PACKETS_D
};

/*****************************************************/
/* transmit completion routine for all ISO tx fifos */
/*****************************************************/
static void
tx_iso_complete(struct urb *urb, struct pt_regs *regs)
{
	iso_urb_struct *context_iso_urb = (iso_urb_struct *) urb->context;
	usb_fifo *fifo = context_iso_urb->owner_fifo;
	hfcusb_data *hfc = fifo->hfc;
	int k, tx_offset, num_isoc_packets, sink, len, current_len,
	    errcode;
	int frame_complete, transp_mode, fifon, status;
	__u8 threshbit;
	__u8 threshtable[8] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80 };

	fifon = fifo->fifonum;
	status = urb->status;

	tx_offset = 0;

	if (fifo->active && !status) {
		transp_mode = 0;
		if (fifon < 4 && hfc->b_mode[fifon / 2] == L1_MODE_TRANS)
			transp_mode = TRUE;

		/* is FifoFull-threshold set for our channel? */
		threshbit = threshtable[fifon] & hfc->threshold_mask;
		num_isoc_packets = iso_packets[fifon];

		/* predict dataflow to avoid fifo overflow */
		if (fifon >= HFCUSB_D_TX) {
			sink = (threshbit) ? SINK_DMIN : SINK_DMAX;
		} else {
			sink = (threshbit) ? SINK_MIN : SINK_MAX;
		}
		fill_isoc_urb(urb, fifo->hfc->dev, fifo->pipe,
			      context_iso_urb->buffer, num_isoc_packets,
			      fifo->usb_packet_maxlen, fifo->intervall,
			      tx_iso_complete, urb->context);
		memset(context_iso_urb->buffer, 0,
		       sizeof(context_iso_urb->buffer));
		frame_complete = FALSE;
		/* Generate next Iso Packets */
		for (k = 0; k < num_isoc_packets; ++k) {
			if (fifo->skbuff) {
				len = fifo->skbuff->len;
				/* we lower data margin every msec */
				fifo->bit_line -= sink;
				current_len = (0 - fifo->bit_line) / 8;
				/* maximum 15 byte for every ISO packet makes our life easier */
				if (current_len > 14)
					current_len = 14;
				current_len =
				    (len <=
				     current_len) ? len : current_len;
				/* how much bit do we put on the line? */
				fifo->bit_line += current_len * 8;

				context_iso_urb->buffer[tx_offset] = 0;
				if (current_len == len) {
					if (!transp_mode) {
						/* here frame completion */
						context_iso_urb->
						    buffer[tx_offset] = 1;
						/* add 2 byte flags and 16bit CRC at end of ISDN frame */
						fifo->bit_line += 32;
					}
					frame_complete = TRUE;
				}

				memcpy(context_iso_urb->buffer +
				       tx_offset + 1, fifo->skbuff->data,
				       current_len);
				skb_pull(fifo->skbuff, current_len);

				/* define packet delimeters within the URB buffer */
				urb->iso_frame_desc[k].offset = tx_offset;
				urb->iso_frame_desc[k].length =
				    current_len + 1;

				tx_offset += (current_len + 1);
			} else {
				urb->iso_frame_desc[k].offset =
				    tx_offset++;

				urb->iso_frame_desc[k].length = 1;
				fifo->bit_line -= sink;	/* we lower data margin every msec */

				if (fifo->bit_line < BITLINE_INF) {
					fifo->bit_line = BITLINE_INF;
				}
			}

			if (frame_complete) {
				fifo->delete_flg = TRUE;
				fifo->hif->l1l2(fifo->hif,
						PH_DATA | CONFIRM,
						(void *) fifo->skbuff->
						truesize);
				if (fifo->skbuff && fifo->delete_flg) {
					dev_kfree_skb_any(fifo->skbuff);
					fifo->skbuff = NULL;
					fifo->delete_flg = FALSE;
				}
				frame_complete = FALSE;
			}
		}
		errcode = usb_submit_urb(urb, GFP_ATOMIC);
		if (errcode < 0) {
			printk(KERN_INFO
			       "HFC-S USB: error submitting ISO URB: %d \n",
			       errcode);
		}
	} else {
		if (status && !hfc->disc_flag) {
			printk(KERN_INFO
			       "HFC-S USB: tx_iso_complete : urb->status %s (%i), fifonum=%d\n",
			       symbolic(urb_errlist, status), status,
			       fifon);
		}
	}
}				/* tx_iso_complete */

/*****************************************************/
/* receive completion routine for all ISO tx fifos   */
/*****************************************************/
static void
rx_iso_complete(struct urb *urb, struct pt_regs *regs)
{
	iso_urb_struct *context_iso_urb = (iso_urb_struct *) urb->context;
	usb_fifo *fifo = context_iso_urb->owner_fifo;
	hfcusb_data *hfc = fifo->hfc;
	int k, len, errcode, offset, num_isoc_packets, fifon, maxlen,
	    status;
	unsigned int iso_status;
	__u8 *buf;
	static __u8 eof[8];
#ifdef CONFIG_HISAX_DEBUG
	__u8 i;
#endif

	fifon = fifo->fifonum;
	status = urb->status;

	if (urb->status == -EOVERFLOW) {
#ifdef CONFIG_HISAX_DEBUG
		DBG(USB_DBG,
		    "HFC-USB: ignoring USB DATAOVERRUN  for fifo  %i \n",
		    fifon);
#endif
		status = 0;
	}
	if (fifo->active && !status) {
		num_isoc_packets = iso_packets[fifon];
		maxlen = fifo->usb_packet_maxlen;
		for (k = 0; k < num_isoc_packets; ++k) {
			len = urb->iso_frame_desc[k].actual_length;
			offset = urb->iso_frame_desc[k].offset;
			buf = context_iso_urb->buffer + offset;
			iso_status = urb->iso_frame_desc[k].status;
#ifdef CONFIG_HISAX_DEBUG
			if (iso_status && !hfc->disc_flag)
				DBG(USB_DBG,
				    "HFC-S USB: ISO packet failure - status:%x",
				    iso_status);

			if ((fifon == 5) && (debug > 1)) {
				printk(KERN_INFO
				       "HFC-S USB: ISO-D-RX lst_urblen:%2d "
				       "act_urblen:%2d max-urblen:%2d "
				       "EOF:0x%0x DATA: ",
				       fifo->last_urblen, len, maxlen,
				       eof[5]);
				for (i = 0; i < len; i++)
					printk("%.2x ", buf[i]);
				printk("\n");
			}
#endif
			if (fifo->last_urblen != maxlen) {
				/* the threshold mask is in the 2nd status byte */
				hfc->threshold_mask = buf[1];
				/* care for L1 state only for D-Channel
				   to avoid overlapped iso completions */
				if (fifon == 5) {
					/* the S0 state is in the upper half
					   of the 1st status byte */
					state_handler(hfc, buf[0] >> 4);
				}
				eof[fifon] = buf[0] & 1;
				if (len > 2)
					collect_rx_frame(fifo, buf + 2,
							 len - 2,
							 (len <
							  maxlen) ?
							 eof[fifon] : 0);
			} else {
				collect_rx_frame(fifo, buf, len,
						 (len <
						  maxlen) ? eof[fifon] :
						 0);
			}
			fifo->last_urblen = len;
		}

		fill_isoc_urb(urb, fifo->hfc->dev, fifo->pipe,
			      context_iso_urb->buffer, num_isoc_packets,
			      fifo->usb_packet_maxlen, fifo->intervall,
			      rx_iso_complete, urb->context);
		errcode = usb_submit_urb(urb, GFP_ATOMIC);
		if (errcode < 0) {
			printk(KERN_INFO
			       "HFC-S USB: error submitting ISO URB: %d \n",
			       errcode);
		}
	} else {
		if (status && !hfc->disc_flag) {
			printk(KERN_INFO
			       "HFC-S USB: rx_iso_complete : "
			       "urb->status %d, fifonum %d\n",
			       status, fifon);
		}
	}
}				/* rx_iso_complete */

/*****************************************************/
/* collect data from interrupt or isochron in        */
/*****************************************************/
static void
collect_rx_frame(usb_fifo * fifo, __u8 * data, int len, int finish)
{
	hfcusb_data *hfc = fifo->hfc;
	int transp_mode, fifon;
#ifdef CONFIG_HISAX_DEBUG
	int i;
#endif
	fifon = fifo->fifonum;
	transp_mode = 0;
	if (fifon < 4 && hfc->b_mode[fifon / 2] == L1_MODE_TRANS)
		transp_mode = TRUE;

	if (!fifo->skbuff) {
		fifo->skbuff = dev_alloc_skb(fifo->max_size + 3);
		if (!fifo->skbuff) {
			printk(KERN_INFO
			       "HFC-S USB: cannot allocate buffer (dev_alloc_skb) fifo:%d\n",
			       fifon);
			return;
		}
	}
	if (len) {
		if (fifo->skbuff->len + len < fifo->max_size) {
			memcpy(skb_put(fifo->skbuff, len), data, len);
		} else {
#ifdef CONFIG_HISAX_DEBUG
			printk(KERN_INFO "HFC-S USB: ");
			for (i = 0; i < 15; i++)
				printk("%.2x ",
				       fifo->skbuff->data[fifo->skbuff->
							  len - 15 + i]);
			printk("\n");
#endif
			printk(KERN_INFO
			       "HCF-USB: got frame exceeded fifo->max_size:%d on fifo:%d\n",
			       fifo->max_size, fifon);
		}
	}
	if (transp_mode && fifo->skbuff->len >= 128) {
		fifo->hif->l1l2(fifo->hif, PH_DATA | INDICATION,
				fifo->skbuff);
		fifo->skbuff = NULL;
		return;
	}
	/* we have a complete hdlc packet */
	if (finish) {
		if ((!fifo->skbuff->data[fifo->skbuff->len - 1])
		    && (fifo->skbuff->len > 3)) {
			/* remove CRC & status */
			skb_trim(fifo->skbuff, fifo->skbuff->len - 3);
			if (fifon == HFCUSB_PCM_RX) {
				fifo->hif->l1l2(fifo->hif,
						PH_DATA_E | INDICATION,
						fifo->skbuff);
			} else
				fifo->hif->l1l2(fifo->hif,
						PH_DATA | INDICATION,
						fifo->skbuff);
			fifo->skbuff = NULL;	/* buffer was freed from upper layer */
		} else {
			if (fifo->skbuff->len > 3) {
				printk(KERN_INFO
				       "HFC-S USB: got frame %d bytes but CRC ERROR on fifo:%d!!!\n",
				       fifo->skbuff->len, fifon);
#ifdef CONFIG_HISAX_DEBUG
				if (debug > 1) {
					printk(KERN_INFO "HFC-S USB: ");
					for (i = 0; i < 15; i++)
						printk("%.2x ",
						       fifo->skbuff->
						       data[fifo->skbuff->
							    len - 15 + i]);
					printk("\n");
				}
#endif
			}
#ifdef CONFIG_HISAX_DEBUG
			else {
				printk(KERN_INFO
				       "HFC-S USB: frame to small (%d bytes)!!!\n",
				       fifo->skbuff->len);
			}
#endif
			skb_trim(fifo->skbuff, 0);
		}
	}
}

/***********************************************/
/* receive completion routine for all rx fifos */
/***********************************************/
static void
rx_complete(struct urb *urb, struct pt_regs *regs)
{
	int len;
	int status;
	__u8 *buf, maxlen, fifon;
	usb_fifo *fifo = (usb_fifo *) urb->context;
	hfcusb_data *hfc = fifo->hfc;
	static __u8 eof[8];
#ifdef CONFIG_HISAX_DEBUG
	__u8 i;
#endif

	urb->dev = hfc->dev;	/* security init */

	fifon = fifo->fifonum;
	if ((!fifo->active) || (urb->status)) {
#ifdef CONFIG_HISAX_DEBUG
		DBG(USB_DBG, "HFC-S USB: RX-Fifo %i is going down (%i)",
		    fifon, urb->status);
#endif
		fifo->urb->interval = 0;	/* cancel automatic rescheduling */
		if (fifo->skbuff) {
			dev_kfree_skb_any(fifo->skbuff);
			fifo->skbuff = NULL;
		}
		return;
	}
	len = urb->actual_length;
	buf = fifo->buffer;
	maxlen = fifo->usb_packet_maxlen;

#ifdef CONFIG_HISAX_DEBUG
	if ((fifon == 5) && (debug > 1)) {
		printk(KERN_INFO
		       "HFC-S USB: INT-D-RX lst_urblen:%2d act_urblen:%2d max-urblen:%2d EOF:0x%0x DATA: ",
		       fifo->last_urblen, len, maxlen, eof[5]);
		for (i = 0; i < len; i++)
			printk("%.2x ", buf[i]);
		printk("\n");
	}
#endif

	if (fifo->last_urblen != fifo->usb_packet_maxlen) {
		/* the threshold mask is in the 2nd status byte */
		hfc->threshold_mask = buf[1];
		/* the S0 state is in the upper half of the 1st status byte */
		state_handler(hfc, buf[0] >> 4);
		eof[fifon] = buf[0] & 1;
		/* if we have more than the 2 status bytes -> collect data */
		if (len > 2)
			collect_rx_frame(fifo, buf + 2,
					 urb->actual_length - 2,
					 (len < maxlen) ? eof[fifon] : 0);
	} else {
		collect_rx_frame(fifo, buf, urb->actual_length,
				 (len < maxlen) ? eof[fifon] : 0);
	}
	fifo->last_urblen = urb->actual_length;
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status) {
		printk(KERN_INFO
		       "HFC-S USB: error resubmitting URN at rx_complete...\n");
	}
}				/* rx_complete */

/***************************************************/
/* start the interrupt transfer for the given fifo */
/***************************************************/
static void
start_int_fifo(usb_fifo * fifo)
{
	int errcode;

	printk(KERN_INFO "HFC-S USB: starting intr IN fifo:%d\n",
	       fifo->fifonum);

	if (!fifo->urb) {
		fifo->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!fifo->urb)
			return;
	}
	usb_fill_int_urb(fifo->urb, fifo->hfc->dev, fifo->pipe,
			 fifo->buffer, fifo->usb_packet_maxlen,
			 rx_complete, fifo, fifo->intervall);
	fifo->active = 1;	/* must be marked active */
	errcode = usb_submit_urb(fifo->urb, GFP_KERNEL);
	if (errcode) {
		printk(KERN_INFO
		       "HFC-S USB: submit URB error(start_int_info): status:%i\n",
		       errcode);
		fifo->active = 0;
		fifo->skbuff = NULL;
	}
}				/* start_int_fifo */

/*****************************/
/* set the B-channel mode    */
/*****************************/
static void
set_hfcmode(hfcusb_data * hfc, int channel, int mode)
{
	__u8 val, idx_table[2] = { 0, 2 };

	if (hfc->disc_flag) {
		return;
	}
#ifdef CONFIG_HISAX_DEBUG
	DBG(ISDN_DBG, "HFC-S USB: setting channel %d to mode %d", channel,
	    mode);
#endif
	hfc->b_mode[channel] = mode;

	/* setup CON_HDLC */
	val = 0;
	if (mode != L1_MODE_NULL)
		val = 8;	/* enable fifo? */
	if (mode == L1_MODE_TRANS)
		val |= 2;	/* set transparent bit */

	/* set FIFO to transmit register */
	queue_control_request(hfc, HFCUSB_FIFO, idx_table[channel], 1);
	queue_control_request(hfc, HFCUSB_CON_HDLC, val, 1);
	/* reset fifo */
	queue_control_request(hfc, HFCUSB_INC_RES_F, 2, 1);
	/* set FIFO to receive register */
	queue_control_request(hfc, HFCUSB_FIFO, idx_table[channel] + 1, 1);
	queue_control_request(hfc, HFCUSB_CON_HDLC, val, 1);
	/* reset fifo */
	queue_control_request(hfc, HFCUSB_INC_RES_F, 2, 1);

	val = 0x40;
	if (hfc->b_mode[0])
		val |= 1;
	if (hfc->b_mode[1])
		val |= 2;
	queue_control_request(hfc, HFCUSB_SCTRL, val, 1);

	val = 0;
	if (hfc->b_mode[0])
		val |= 1;
	if (hfc->b_mode[1])
		val |= 2;
	queue_control_request(hfc, HFCUSB_SCTRL_R, val, 1);

	if (mode == L1_MODE_NULL) {
		if (channel)
			handle_led(hfc, LED_B2_OFF);
		else
			handle_led(hfc, LED_B1_OFF);
	} else {
		if (channel)
			handle_led(hfc, LED_B2_ON);
		else
			handle_led(hfc, LED_B1_ON);
	}
}

static void
hfc_usb_l2l1(struct hisax_if *my_hisax_if, int pr, void *arg)
{
	usb_fifo *fifo = my_hisax_if->priv;
	hfcusb_data *hfc = fifo->hfc;

	switch (pr) {
		case PH_ACTIVATE | REQUEST:
			if (fifo->fifonum == HFCUSB_D_TX) {
#ifdef CONFIG_HISAX_DEBUG
				DBG(ISDN_DBG,
				    "HFC_USB: hfc_usb_d_l2l1 D-chan: PH_ACTIVATE | REQUEST");
#endif
				if (hfc->l1_state != 3
				    && hfc->l1_state != 7) {
					hfc->d_if.ifc.l1l2(&hfc->d_if.ifc,
							   PH_DEACTIVATE |
							   INDICATION,
							   NULL);
#ifdef CONFIG_HISAX_DEBUG
					DBG(ISDN_DBG,
					    "HFC-S USB: PH_DEACTIVATE | INDICATION sent (not state 3 or 7)");
#endif
				} else {
					if (hfc->l1_state == 7) {	/* l1 already active */
						hfc->d_if.ifc.l1l2(&hfc->
								   d_if.
								   ifc,
								   PH_ACTIVATE
								   |
								   INDICATION,
								   NULL);
#ifdef CONFIG_HISAX_DEBUG
						DBG(ISDN_DBG,
						    "HFC-S USB: PH_ACTIVATE | INDICATION sent again ;)");
#endif
					} else {
						/* force sending sending INFO1 */
						queue_control_request(hfc,
								      HFCUSB_STATES,
								      0x14,
								      1);
						mdelay(1);
						/* start l1 activation */
						queue_control_request(hfc,
								      HFCUSB_STATES,
								      0x04,
								      1);
						if (!timer_pending
						    (&hfc->t3_timer)) {
							hfc->t3_timer.
							    expires =
							    jiffies +
							    (HFC_TIMER_T3 *
							     HZ) / 1000;
							add_timer(&hfc->
								  t3_timer);
						}
					}
				}
			} else {
#ifdef CONFIG_HISAX_DEBUG
				DBG(ISDN_DBG,
				    "HFC_USB: hfc_usb_d_l2l1 Bx-chan: PH_ACTIVATE | REQUEST");
#endif
				set_hfcmode(hfc,
					    (fifo->fifonum ==
					     HFCUSB_B1_TX) ? 0 : 1,
					    (int) arg);
				fifo->hif->l1l2(fifo->hif,
						PH_ACTIVATE | INDICATION,
						NULL);
			}
			break;
		case PH_DEACTIVATE | REQUEST:
			if (fifo->fifonum == HFCUSB_D_TX) {
#ifdef CONFIG_HISAX_DEBUG
				DBG(ISDN_DBG,
				    "HFC_USB: hfc_usb_d_l2l1 D-chan: PH_DEACTIVATE | REQUEST");
#endif
				printk(KERN_INFO
				       "HFC-S USB: ISDN TE device should not deativate...\n");
			} else {
#ifdef CONFIG_HISAX_DEBUG
				DBG(ISDN_DBG,
				    "HFC_USB: hfc_usb_d_l2l1 Bx-chan: PH_DEACTIVATE | REQUEST");
#endif
				set_hfcmode(hfc,
					    (fifo->fifonum ==
					     HFCUSB_B1_TX) ? 0 : 1,
					    (int) L1_MODE_NULL);
				fifo->hif->l1l2(fifo->hif,
						PH_DEACTIVATE | INDICATION,
						NULL);
			}
			break;
		case PH_DATA | REQUEST:
			if (fifo->skbuff && fifo->delete_flg) {
				dev_kfree_skb_any(fifo->skbuff);
				fifo->skbuff = NULL;
				fifo->delete_flg = FALSE;
			}
			fifo->skbuff = arg;	/* we have a new buffer */
			break;
		default:
			printk(KERN_INFO
			       "HFC_USB: hfc_usb_d_l2l1: unkown state : %#x\n",
			       pr);
			break;
	}
}

/***************************************************************************/
/* usb_init is called once when a new matching device is detected to setup */
/* main parameters. It registers the driver at the main hisax module.      */
/* on success 0 is returned.                                               */
/***************************************************************************/
static int
usb_init(hfcusb_data * hfc)
{
	usb_fifo *fifo;
	int i, err;
	u_char b;
	struct hisax_b_if *p_b_if[2];

	/* check the chip id */
	if (read_usb(hfc, HFCUSB_CHIP_ID, &b) != 1) {
		printk(KERN_INFO "HFC-USB: cannot read chip id\n");
		return (1);
	}
	if (b != HFCUSB_CHIPID) {
		printk(KERN_INFO "HFC-S USB: Invalid chip id 0x%02x\n", b);
		return (1);
	}

	/* first set the needed config, interface and alternate */
	err = usb_set_interface(hfc->dev, hfc->if_used, hfc->alt_used);

	/* do Chip reset */
	write_usb(hfc, HFCUSB_CIRM, 8);
	/* aux = output, reset off */
	write_usb(hfc, HFCUSB_CIRM, 0x10);

	/* set USB_SIZE to match the the wMaxPacketSize for INT or BULK transfers */
	write_usb(hfc, HFCUSB_USB_SIZE,
		  (hfc->packet_size / 8) | ((hfc->packet_size / 8) << 4));

	/* set USB_SIZE_I to match the the wMaxPacketSize for ISO transfers */
	write_usb(hfc, HFCUSB_USB_SIZE_I, hfc->iso_packet_size);

	/* enable PCM/GCI master mode */
	write_usb(hfc, HFCUSB_MST_MODE1, 0);	/* set default values */
	write_usb(hfc, HFCUSB_MST_MODE0, 1);	/* enable master mode */

	/* init the fifos */
	write_usb(hfc, HFCUSB_F_THRES,
		  (HFCUSB_TX_THRESHOLD /
		   8) | ((HFCUSB_RX_THRESHOLD / 8) << 4));

	fifo = hfc->fifos;
	for (i = 0; i < HFCUSB_NUM_FIFOS; i++) {
		write_usb(hfc, HFCUSB_FIFO, i);	/* select the desired fifo */
		fifo[i].skbuff = NULL;	/* init buffer pointer */
		fifo[i].max_size =
		    (i <= HFCUSB_B2_RX) ? MAX_BCH_SIZE : MAX_DFRAME_LEN;
		fifo[i].last_urblen = 0;
		/* set 2 bit for D- & E-channel */
		write_usb(hfc, HFCUSB_HDLC_PAR,
			  ((i <= HFCUSB_B2_RX) ? 0 : 2));
		/* rx hdlc, enable IFF for D-channel */
		write_usb(hfc, HFCUSB_CON_HDLC,
			  ((i == HFCUSB_D_TX) ? 0x09 : 0x08));
		write_usb(hfc, HFCUSB_INC_RES_F, 2);	/* reset the fifo */
	}

	write_usb(hfc, HFCUSB_CLKDEL, 0x0f);	/* clock delay value */
	write_usb(hfc, HFCUSB_STATES, 3 | 0x10);	/* set deactivated mode */
	write_usb(hfc, HFCUSB_STATES, 3);	/* enable state machine */

	write_usb(hfc, HFCUSB_SCTRL_R, 0);	/* disable both B receivers */
	write_usb(hfc, HFCUSB_SCTRL, 0x40);	/* disable B transmitters + capacitive mode */

	/* set both B-channel to not connected */
	hfc->b_mode[0] = L1_MODE_NULL;
	hfc->b_mode[1] = L1_MODE_NULL;

	hfc->l1_activated = FALSE;
	hfc->disc_flag = FALSE;
	hfc->led_state = 0;
	hfc->led_new_data = 0;
	hfc->old_led_state = 0;

	/* init the t3 timer */
	init_timer(&hfc->t3_timer);
	hfc->t3_timer.data = (long) hfc;
	hfc->t3_timer.function = (void *) l1_timer_expire_t3;

	/* init the t4 timer */
	init_timer(&hfc->t4_timer);
	hfc->t4_timer.data = (long) hfc;
	hfc->t4_timer.function = (void *) l1_timer_expire_t4;

	/* init the background machinery for control requests */
	hfc->ctrl_read.bRequestType = 0xc0;
	hfc->ctrl_read.bRequest = 1;
	hfc->ctrl_read.wLength = 1;
	hfc->ctrl_write.bRequestType = 0x40;
	hfc->ctrl_write.bRequest = 0;
	hfc->ctrl_write.wLength = 0;
	usb_fill_control_urb(hfc->ctrl_urb,
			     hfc->dev,
			     hfc->ctrl_out_pipe,
			     (u_char *) & hfc->ctrl_write,
			     NULL, 0, ctrl_complete, hfc);
	/* Init All Fifos */
	for (i = 0; i < HFCUSB_NUM_FIFOS; i++) {
		hfc->fifos[i].iso[0].purb = NULL;
		hfc->fifos[i].iso[1].purb = NULL;
		hfc->fifos[i].active = 0;
	}
	/* register Modul to upper Hisax Layers */
	hfc->d_if.owner = THIS_MODULE;
	hfc->d_if.ifc.priv = &hfc->fifos[HFCUSB_D_TX];
	hfc->d_if.ifc.l2l1 = hfc_usb_l2l1;
	for (i = 0; i < 2; i++) {
		hfc->b_if[i].ifc.priv = &hfc->fifos[HFCUSB_B1_TX + i * 2];
		hfc->b_if[i].ifc.l2l1 = hfc_usb_l2l1;
		p_b_if[i] = &hfc->b_if[i];
	}
	/* default Prot: EURO ISDN, should be a module_param */
	hfc->protocol = 2;
	hisax_register(&hfc->d_if, p_b_if, "hfc_usb", hfc->protocol);

#ifdef CONFIG_HISAX_DEBUG
	hfc_debug = debug;
#endif

	for (i = 0; i < 4; i++)
		hfc->fifos[i].hif = &p_b_if[i / 2]->ifc;
	for (i = 4; i < 8; i++)
		hfc->fifos[i].hif = &hfc->d_if.ifc;

	/* 3 (+1) INT IN + 3 ISO OUT */
	if (hfc->cfg_used == CNF_3INT3ISO || hfc->cfg_used == CNF_4INT3ISO) {
		start_int_fifo(hfc->fifos + HFCUSB_D_RX);
		if (hfc->fifos[HFCUSB_PCM_RX].pipe)
			start_int_fifo(hfc->fifos + HFCUSB_PCM_RX);
		start_int_fifo(hfc->fifos + HFCUSB_B1_RX);
		start_int_fifo(hfc->fifos + HFCUSB_B2_RX);
	}
	/* 3 (+1) ISO IN + 3 ISO OUT */
	if (hfc->cfg_used == CNF_3ISO3ISO || hfc->cfg_used == CNF_4ISO3ISO) {
		start_isoc_chain(hfc->fifos + HFCUSB_D_RX, ISOC_PACKETS_D,
				 rx_iso_complete, 16);
		if (hfc->fifos[HFCUSB_PCM_RX].pipe)
			start_isoc_chain(hfc->fifos + HFCUSB_PCM_RX,
					 ISOC_PACKETS_D, rx_iso_complete,
					 16);
		start_isoc_chain(hfc->fifos + HFCUSB_B1_RX, ISOC_PACKETS_B,
				 rx_iso_complete, 16);
		start_isoc_chain(hfc->fifos + HFCUSB_B2_RX, ISOC_PACKETS_B,
				 rx_iso_complete, 16);
	}

	start_isoc_chain(hfc->fifos + HFCUSB_D_TX, ISOC_PACKETS_D,
			 tx_iso_complete, 1);
	start_isoc_chain(hfc->fifos + HFCUSB_B1_TX, ISOC_PACKETS_B,
			 tx_iso_complete, 1);
	start_isoc_chain(hfc->fifos + HFCUSB_B2_TX, ISOC_PACKETS_B,
			 tx_iso_complete, 1);

	handle_led(hfc, LED_POWER_ON);

	return (0);
}				/* usb_init */

/*************************************************/
/* function called to probe a new plugged device */
/*************************************************/
static int
hfc_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	hfcusb_data *context;
	struct usb_host_interface *iface = intf->cur_altsetting;
	struct usb_host_interface *iface_used = NULL;
	struct usb_host_endpoint *ep;
	int ifnum = iface->desc.bInterfaceNumber;
	int i, idx, alt_idx, probe_alt_setting, vend_idx, cfg_used, *vcf,
	    attr, cfg_found, cidx, ep_addr;
	int cmptbl[16], small_match, iso_packet_size, packet_size,
	    alt_used = 0;
	hfcsusb_vdata *driver_info;

	vend_idx = 0xffff;
	for (i = 0; hfcusb_idtab[i].idVendor; i++) {
		if (dev->descriptor.idVendor == hfcusb_idtab[i].idVendor
		    && dev->descriptor.idProduct ==
		    hfcusb_idtab[i].idProduct) {
			vend_idx = i;
			continue;
		}
	}

#ifdef CONFIG_HISAX_DEBUG
	DBG(USB_DBG,
	    "HFC-USB: probing interface(%d) actalt(%d) minor(%d)\n", ifnum,
	    iface->desc.bAlternateSetting, intf->minor);
#endif
	printk(KERN_INFO
	       "HFC-S USB: probing interface(%d) actalt(%d) minor(%d)\n",
	       ifnum, iface->desc.bAlternateSetting, intf->minor);

	if (vend_idx != 0xffff) {
		/* if vendor and product ID is OK, start probing alternate settings */
		alt_idx = 0;
		small_match = 0xffff;

		/* default settings */
		iso_packet_size = 16;
		packet_size = 64;

		while (alt_idx < intf->num_altsetting) {
			iface = intf->altsetting + alt_idx;
			probe_alt_setting = iface->desc.bAlternateSetting;
			cfg_used = 0;

			/* check for config EOL element */
			while (validconf[cfg_used][0]) {
				cfg_found = TRUE;
				vcf = validconf[cfg_used];
				/* first endpoint descriptor */
				ep = iface->endpoint;
#ifdef CONFIG_HISAX_DEBUG
				DBG(USB_DBG,
				    "HFC-S USB: (if=%d alt=%d cfg_used=%d)\n",
				    ifnum, probe_alt_setting, cfg_used);
#endif
				memcpy(cmptbl, vcf, 16 * sizeof(int));

				/* check for all endpoints in this alternate setting */
				for (i = 0; i < iface->desc.bNumEndpoints;
				     i++) {
					ep_addr =
					    ep->desc.bEndpointAddress;
					/* get endpoint base */
					idx = ((ep_addr & 0x7f) - 1) * 2;
					if (ep_addr & 0x80)
						idx++;
					attr = ep->desc.bmAttributes;
					if (cmptbl[idx] == EP_NUL) {
						cfg_found = FALSE;
					}
					if (attr == USB_ENDPOINT_XFER_INT
					    && cmptbl[idx] == EP_INT)
						cmptbl[idx] = EP_NUL;
					if (attr == USB_ENDPOINT_XFER_BULK
					    && cmptbl[idx] == EP_BLK)
						cmptbl[idx] = EP_NUL;
					if (attr == USB_ENDPOINT_XFER_ISOC
					    && cmptbl[idx] == EP_ISO)
						cmptbl[idx] = EP_NUL;

					/* check if all INT endpoints match minimum interval */
					if (attr == USB_ENDPOINT_XFER_INT
					    && ep->desc.bInterval <
					    vcf[17]) {
#ifdef CONFIG_HISAX_DEBUG
						if (cfg_found)
							DBG(USB_DBG,
							    "HFC-S USB: Interrupt Endpoint interval < %d found - skipping config",
							    vcf[17]);
#endif
						cfg_found = FALSE;
					}
					ep++;
				}
				for (i = 0; i < 16; i++) {
					/* all entries must be EP_NOP or EP_NUL for a valid config */
					if (cmptbl[i] != EP_NOP
					    && cmptbl[i] != EP_NUL)
						cfg_found = FALSE;
				}
				if (cfg_found) {
					if (cfg_used < small_match) {
						small_match = cfg_used;
						alt_used =
						    probe_alt_setting;
						iface_used = iface;
					}
#ifdef CONFIG_HISAX_DEBUG
					DBG(USB_DBG,
					    "HFC-USB: small_match=%x %x\n",
					    small_match, alt_used);
#endif
				}
				cfg_used++;
			}
			alt_idx++;
		}		/* (alt_idx < intf->num_altsetting) */

		/* found a valid USB Ta Endpint config */
		if (small_match != 0xffff) {
			iface = iface_used;
			if (!
			    (context =
			     kmalloc(sizeof(hfcusb_data), GFP_KERNEL)))
				return (-ENOMEM);	/* got no mem */
			memset(context, 0, sizeof(hfcusb_data));

			ep = iface->endpoint;
			vcf = validconf[small_match];

			for (i = 0; i < iface->desc.bNumEndpoints; i++) {
				ep_addr = ep->desc.bEndpointAddress;
				/* get endpoint base */
				idx = ((ep_addr & 0x7f) - 1) * 2;
				if (ep_addr & 0x80)
					idx++;
				cidx = idx & 7;
				attr = ep->desc.bmAttributes;

				/* init Endpoints */
				if (vcf[idx] != EP_NOP
				    && vcf[idx] != EP_NUL) {
					switch (attr) {
						case USB_ENDPOINT_XFER_INT:
							context->
							    fifos[cidx].
							    pipe =
							    usb_rcvintpipe
							    (dev,
							     ep->desc.
							     bEndpointAddress);
							context->
							    fifos[cidx].
							    usb_transfer_mode
							    = USB_INT;
							packet_size =
							    ep->desc.
							    wMaxPacketSize;
							break;
						case USB_ENDPOINT_XFER_BULK:
							if (ep_addr & 0x80)
								context->
								    fifos
								    [cidx].
								    pipe =
								    usb_rcvbulkpipe
								    (dev,
								     ep->
								     desc.
								     bEndpointAddress);
							else
								context->
								    fifos
								    [cidx].
								    pipe =
								    usb_sndbulkpipe
								    (dev,
								     ep->
								     desc.
								     bEndpointAddress);
							context->
							    fifos[cidx].
							    usb_transfer_mode
							    = USB_BULK;
							packet_size =
							    ep->desc.
							    wMaxPacketSize;
							break;
						case USB_ENDPOINT_XFER_ISOC:
							if (ep_addr & 0x80)
								context->
								    fifos
								    [cidx].
								    pipe =
								    usb_rcvisocpipe
								    (dev,
								     ep->
								     desc.
								     bEndpointAddress);
							else
								context->
								    fifos
								    [cidx].
								    pipe =
								    usb_sndisocpipe
								    (dev,
								     ep->
								     desc.
								     bEndpointAddress);
							context->
							    fifos[cidx].
							    usb_transfer_mode
							    = USB_ISOC;
							iso_packet_size =
							    ep->desc.
							    wMaxPacketSize;
							break;
						default:
							context->
							    fifos[cidx].
							    pipe = 0;
					}	/* switch attribute */

					if (context->fifos[cidx].pipe) {
						context->fifos[cidx].
						    fifonum = cidx;
						context->fifos[cidx].hfc =
						    context;
						context->fifos[cidx].
						    usb_packet_maxlen =
						    ep->desc.
						    wMaxPacketSize;
						context->fifos[cidx].
						    intervall =
						    ep->desc.bInterval;
						context->fifos[cidx].
						    skbuff = NULL;
					}
				}
				ep++;
			}
			context->dev = dev;	/* save device */
			context->if_used = ifnum;	/* save used interface */
			context->alt_used = alt_used;	/* and alternate config */
			context->ctrl_paksize = dev->descriptor.bMaxPacketSize0;	/* control size */
			context->cfg_used = vcf[16];	/* store used config */
			context->vend_idx = vend_idx;	/* store found vendor */
			context->packet_size = packet_size;
			context->iso_packet_size = iso_packet_size;

			/* create the control pipes needed for register access */
			context->ctrl_in_pipe =
			    usb_rcvctrlpipe(context->dev, 0);
			context->ctrl_out_pipe =
			    usb_sndctrlpipe(context->dev, 0);
			context->ctrl_urb = usb_alloc_urb(0, GFP_KERNEL);

			driver_info =
			    (hfcsusb_vdata *) hfcusb_idtab[vend_idx].
			    driver_info;
			printk(KERN_INFO "HFC-S USB: detected \"%s\"\n",
			       driver_info->vend_name);
#ifdef CONFIG_HISAX_DEBUG
			DBG(USB_DBG,
			    "HFC-S USB: Endpoint-Config: %s (if=%d alt=%d)\n",
			    conf_str[small_match], context->if_used,
			    context->alt_used);
			printk(KERN_INFO
			       "HFC-S USB: E-channel (\"ECHO:\") logging ");
			if (validconf[small_match][18])
				printk(" possible\n");
			else
				printk("NOT possible\n");
#endif
			/* init the chip and register the driver */
			if (usb_init(context)) {
				if (context->ctrl_urb) {
					usb_unlink_urb(context->ctrl_urb);
					usb_free_urb(context->ctrl_urb);
					context->ctrl_urb = NULL;
				}
				kfree(context);
				return (-EIO);
			}
			usb_set_intfdata(intf, context);
			return (0);
		}
	} else {
		printk(KERN_INFO
		       "HFC-S USB: no valid vendor found in USB descriptor\n");
	}
	return (-EIO);
}

/****************************************************/
/* function called when an active device is removed */
/****************************************************/
static void
hfc_usb_disconnect(struct usb_interface
		   *intf)
{
	hfcusb_data *context = usb_get_intfdata(intf);
	int i;
	printk(KERN_INFO "HFC-S USB: device disconnect\n");
	context->disc_flag = TRUE;
	usb_set_intfdata(intf, NULL);
	if (!context)
		return;
	if (timer_pending(&context->t3_timer))
		del_timer(&context->t3_timer);
	if (timer_pending(&context->t4_timer))
		del_timer(&context->t4_timer);
	/* tell all fifos to terminate */
	for (i = 0; i < HFCUSB_NUM_FIFOS; i++) {
		if (context->fifos[i].usb_transfer_mode == USB_ISOC) {
			if (context->fifos[i].active > 0) {
				stop_isoc_chain(&context->fifos[i]);
#ifdef CONFIG_HISAX_DEBUG
				DBG(USB_DBG,
				    "HFC-S USB: hfc_usb_disconnect: stopping ISOC chain Fifo no %i",
				    i);
#endif
			}
		} else {
			if (context->fifos[i].active > 0) {
				context->fifos[i].active = 0;
#ifdef CONFIG_HISAX_DEBUG
				DBG(USB_DBG,
				    "HFC-S USB: hfc_usb_disconnect: unlinking URB for Fifo no %i",
				    i);
#endif
			}
			if (context->fifos[i].urb) {
				usb_unlink_urb(context->fifos[i].urb);
				usb_free_urb(context->fifos[i].urb);
				context->fifos[i].urb = NULL;
			}
		}
		context->fifos[i].active = 0;
	}
	/* wait for all URBS to terminate */
	mdelay(10);
	if (context->ctrl_urb) {
		usb_unlink_urb(context->ctrl_urb);
		usb_free_urb(context->ctrl_urb);
		context->ctrl_urb = NULL;
	}
	hisax_unregister(&context->d_if);
	kfree(context);		/* free our structure again */
}				/* hfc_usb_disconnect */

/************************************/
/* our driver information structure */
/************************************/
static struct usb_driver hfc_drv = {
	.name  = "hfc_usb",
	.id_table = hfcusb_idtab,
	.probe = hfc_usb_probe,
	.disconnect = hfc_usb_disconnect,
};
static void __exit
hfc_usb_exit(void)
{
#ifdef CONFIG_HISAX_DEBUG
	DBG(USB_DBG, "HFC-S USB: calling \"hfc_usb_exit\" ...");
#endif
	usb_deregister(&hfc_drv);	/* release our driver */
	printk(KERN_INFO "HFC-S USB: module removed\n");
}

static int __init
hfc_usb_init(void)
{
#ifndef CONFIG_HISAX_DEBUG
	unsigned int debug = -1;
#endif
	char revstr[30], datestr[30], dummy[30];
	sscanf(hfcusb_revision,
	       "%s %s $ %s %s %s $ ", dummy, revstr,
	       dummy, datestr, dummy);
	printk(KERN_INFO
	       "HFC-S USB: driver module revision %s date %s loaded, (debug=%i)\n",
	       revstr, datestr, debug);
	if (usb_register(&hfc_drv)) {
		printk(KERN_INFO
		       "HFC-S USB: Unable to register HFC-S USB module at usb stack\n");
		return (-1);	/* unable to register */
	}
	return (0);
}

module_init(hfc_usb_init);
module_exit(hfc_usb_exit);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, hfcusb_idtab);
