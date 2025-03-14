/******************************************************************************
 *  cxacru.c  -  driver for USB ADSL modems based on
 *               Conexant AccessRunner chipset
 *
 *  Copyright (C) 2004 David Woodhouse, Duncan Sands, Roman Kagan
 *  Copyright (C) 2005 Duncan Sands, Roman Kagan (rkagan % mail ! ru)
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along with
 *  this program; if not, write to the Free Software Foundation, Inc., 59
 *  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 ******************************************************************************/

/*
 *  Credit is due for Josep Comas, who created the original patch to speedtch.c
 *  to support the different padding used by the AccessRunner (now generalized
 *  into usbatm), and the userspace firmware loading utility.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>	/* FIXME: linux/firmware.h should include it itself */
#include <linux/firmware.h>

#include "usbatm.h"

#define DRIVER_AUTHOR	"Roman Kagan, David Woodhouse, Duncan Sands"
#define DRIVER_VERSION	"0.2"
#define DRIVER_DESC	"Conexant AccessRunner ADSL USB modem driver"

static const char cxacru_driver_name[] = "cxacru";

#define CXACRU_EP_CMD		0x01	/* Bulk/interrupt in/out */
#define CXACRU_EP_DATA		0x02	/* Bulk in/out */

#define CMD_PACKET_SIZE		64	/* Should be maxpacket(ep)? */

/* Addresses */
#define PLLFCLK_ADDR	0x00350068
#define PLLBCLK_ADDR	0x0035006c
#define SDRAMEN_ADDR	0x00350010
#define FW_ADDR		0x00801000
#define BR_ADDR		0x00180600
#define SIG_ADDR	0x00180500
#define BR_STACK_ADDR	0x00187f10

/* Values */
#define SDRAM_ENA	0x1

#define CMD_TIMEOUT	2000	/* msecs */
#define POLL_INTERVAL	5000	/* msecs */

/* commands for interaction with the modem through the control channel before
 * firmware is loaded  */
enum cxacru_fw_request {
	FW_CMD_ERR,
	FW_GET_VER,
	FW_READ_MEM,
	FW_WRITE_MEM,
	FW_RMW_MEM,
	FW_CHECKSUM_MEM,
	FW_GOTO_MEM,
};

/* commands for interaction with the modem through the control channel once
 * firmware is loaded  */
enum cxacru_cm_request {
	CM_REQUEST_UNDEFINED = 0x80,
	CM_REQUEST_TEST,
	CM_REQUEST_CHIP_GET_MAC_ADDRESS,
	CM_REQUEST_CHIP_GET_DP_VERSIONS,
	CM_REQUEST_CHIP_ADSL_LINE_START,
	CM_REQUEST_CHIP_ADSL_LINE_STOP,
	CM_REQUEST_CHIP_ADSL_LINE_GET_STATUS,
	CM_REQUEST_CHIP_ADSL_LINE_GET_SPEED,
	CM_REQUEST_CARD_INFO_GET,
	CM_REQUEST_CARD_DATA_GET,
	CM_REQUEST_CARD_DATA_SET,
	CM_REQUEST_COMMAND_HW_IO,
	CM_REQUEST_INTERFACE_HW_IO,
	CM_REQUEST_CARD_SERIAL_DATA_PATH_GET,
	CM_REQUEST_CARD_SERIAL_DATA_PATH_SET,
	CM_REQUEST_CARD_CONTROLLER_VERSION_GET,
	CM_REQUEST_CARD_GET_STATUS,
	CM_REQUEST_CARD_GET_MAC_ADDRESS,
	CM_REQUEST_CARD_GET_DATA_LINK_STATUS,
	CM_REQUEST_MAX,
};

/* reply codes to the commands above */
enum cxacru_cm_status {
	CM_STATUS_UNDEFINED,
	CM_STATUS_SUCCESS,
	CM_STATUS_ERROR,
	CM_STATUS_UNSUPPORTED,
	CM_STATUS_UNIMPLEMENTED,
	CM_STATUS_PARAMETER_ERROR,
	CM_STATUS_DBG_LOOPBACK,
	CM_STATUS_MAX,
};

/* indices into CARD_INFO_GET return array */
enum cxacru_info_idx {
	CXINF_DOWNSTREAM_RATE,
	CXINF_UPSTREAM_RATE,
	CXINF_LINK_STATUS,
	CXINF_LINE_STATUS,
	CXINF_MAC_ADDRESS_HIGH,
	CXINF_MAC_ADDRESS_LOW,
	CXINF_UPSTREAM_SNR_MARGIN,
	CXINF_DOWNSTREAM_SNR_MARGIN,
	CXINF_UPSTREAM_ATTENUATION,
	CXINF_DOWNSTREAM_ATTENUATION,
	CXINF_TRANSMITTER_POWER,
	CXINF_UPSTREAM_BITS_PER_FRAME,
	CXINF_DOWNSTREAM_BITS_PER_FRAME,
	CXINF_STARTUP_ATTEMPTS,
	CXINF_UPSTREAM_CRC_ERRORS,
	CXINF_DOWNSTREAM_CRC_ERRORS,
	CXINF_UPSTREAM_FEC_ERRORS,
	CXINF_DOWNSTREAM_FEC_ERRORS,
	CXINF_UPSTREAM_HEC_ERRORS,
	CXINF_DOWNSTREAM_HEC_ERRORS,
	CXINF_LINE_STARTABLE,
	CXINF_MODULATION,
	CXINF_ADSL_HEADEND,
	CXINF_ADSL_HEADEND_ENVIRONMENT,
	CXINF_CONTROLLER_VERSION,
	/* dunno what the missing two mean */
	CXINF_MAX = 0x1c,
};

struct cxacru_modem_type {
	u32 pll_f_clk;
	u32 pll_b_clk;
	int boot_rom_patch;
};

struct cxacru_data {
	struct usbatm_data *usbatm;

	const struct cxacru_modem_type *modem_type;

	int line_status;
	struct work_struct poll_work;

	/* contol handles */
	struct semaphore cm_serialize;
	u8 *rcv_buf;
	u8 *snd_buf;
	struct urb *rcv_urb;
	struct urb *snd_urb;
	struct completion rcv_done;
	struct completion snd_done;
};

/* the following three functions are stolen from drivers/usb/core/message.c */
static void cxacru_blocking_completion(struct urb *urb, struct pt_regs *regs)
{
	complete((struct completion *)urb->context);
}

static void cxacru_timeout_kill(unsigned long data)
{
	usb_unlink_urb((struct urb *) data);
}

static int cxacru_start_wait_urb(struct urb *urb, struct completion *done,
				 int* actual_length)
{
	struct timer_list timer;
	int status;

	init_timer(&timer);
	timer.expires = jiffies + msecs_to_jiffies(CMD_TIMEOUT);
	timer.data = (unsigned long) urb;
	timer.function = cxacru_timeout_kill;
	add_timer(&timer);
	wait_for_completion(done);
	status = urb->status;
	if (status == -ECONNRESET)
		status = -ETIMEDOUT;
	del_timer_sync(&timer);

	if (actual_length)
		*actual_length = urb->actual_length;
	return status;
}

static int cxacru_cm(struct cxacru_data *instance, enum cxacru_cm_request cm,
		     u8 *wdata, int wsize, u8 *rdata, int rsize)
{
	int ret, actlen;
	int offb, offd;
	const int stride = CMD_PACKET_SIZE - 4;
	u8 *wbuf = instance->snd_buf;
	u8 *rbuf = instance->rcv_buf;
	int wbuflen = ((wsize - 1) / stride + 1) * CMD_PACKET_SIZE;
	int rbuflen = ((rsize - 1) / stride + 1) * CMD_PACKET_SIZE;

	if (wbuflen > PAGE_SIZE || rbuflen > PAGE_SIZE) {
		dbg("too big transfer requested");
		ret = -ENOMEM;
		goto fail;
	}

	down(&instance->cm_serialize);

	/* submit reading urb before the writing one */
	init_completion(&instance->rcv_done);
	ret = usb_submit_urb(instance->rcv_urb, GFP_KERNEL);
	if (ret < 0) {
		dbg("submitting read urb for cm %#x failed", cm);
		ret = ret;
		goto fail;
	}

	memset(wbuf, 0, wbuflen);
	/* handle wsize == 0 */
	wbuf[0] = cm;
	for (offb = offd = 0; offd < wsize; offd += stride, offb += CMD_PACKET_SIZE) {
		wbuf[offb] = cm;
		memcpy(wbuf + offb + 4, wdata + offd, min_t(int, stride, wsize - offd));
	}

	instance->snd_urb->transfer_buffer_length = wbuflen;
	init_completion(&instance->snd_done);
	ret = usb_submit_urb(instance->snd_urb, GFP_KERNEL);
	if (ret < 0) {
		dbg("submitting write urb for cm %#x failed", cm);
		ret = ret;
		goto fail;
	}

	ret = cxacru_start_wait_urb(instance->snd_urb, &instance->snd_done, NULL);
	if (ret < 0) {
		dbg("sending cm %#x failed", cm);
		ret = ret;
		goto fail;
	}

	ret = cxacru_start_wait_urb(instance->rcv_urb, &instance->rcv_done, &actlen);
	if (ret < 0) {
		dbg("receiving cm %#x failed", cm);
		ret = ret;
		goto fail;
	}
	if (actlen % CMD_PACKET_SIZE || !actlen) {
		dbg("response is not a positive multiple of %d: %#x",
				CMD_PACKET_SIZE, actlen);
		ret = -EIO;
		goto fail;
	}

	/* check the return status and copy the data to the output buffer, if needed */
	for (offb = offd = 0; offd < rsize && offb < actlen; offb += CMD_PACKET_SIZE) {
		if (rbuf[offb] != cm) {
			dbg("wrong cm %#x in response", rbuf[offb]);
			ret = -EIO;
			goto fail;
		}
		if (rbuf[offb + 1] != CM_STATUS_SUCCESS) {
			dbg("response failed: %#x", rbuf[offb + 1]);
			ret = -EIO;
			goto fail;
		}
		if (offd >= rsize)
			break;
		memcpy(rdata + offd, rbuf + offb + 4, min_t(int, stride, rsize - offd));
		offd += stride;
	}

	ret = offd;
	dbg("cm %#x", cm);
fail:
	up(&instance->cm_serialize);
	return ret;
}

static int cxacru_cm_get_array(struct cxacru_data *instance, enum cxacru_cm_request cm,
			       u32 *data, int size)
{
	int ret, len;
	u32 *buf;
	int offb, offd;
	const int stride = CMD_PACKET_SIZE / (4 * 2) - 1;
	int buflen =  ((size - 1) / stride + 1 + size * 2) * 4;

	buf = kmalloc(buflen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = cxacru_cm(instance, cm, NULL, 0, (u8 *) buf, buflen);
	if (ret < 0)
		goto cleanup;

	/* len > 0 && len % 4 == 0 guaranteed by cxacru_cm() */
	len = ret / 4;
	for (offb = 0; offb < len; ) {
		int l = le32_to_cpu(buf[offb++]);
		if (l > stride || l > (len - offb) / 2) {
			dbg("wrong data length %#x in response", l);
			ret = -EIO;
			goto cleanup;
		}
		while (l--) {
			offd = le32_to_cpu(buf[offb++]);
			if (offd >= size) {
				dbg("wrong index %#x in response", offd);
				ret = -EIO;
				goto cleanup;
			}
			data[offd] = le32_to_cpu(buf[offb++]);
		}
	}

	ret = 0;

cleanup:
	kfree(buf);
	return ret;
}

static int cxacru_card_status(struct cxacru_data *instance)
{
	int ret = cxacru_cm(instance, CM_REQUEST_CARD_GET_STATUS, NULL, 0, NULL, 0);
	if (ret < 0) {		/* firmware not loaded */
		dbg("cxacru_adsl_start: CARD_GET_STATUS returned %d", ret);
		return ret;
	}
	return 0;
}

static void cxacru_poll_status(struct cxacru_data *instance);

static int cxacru_atm_start(struct usbatm_data *usbatm_instance,
		struct atm_dev *atm_dev)
{
	struct cxacru_data *instance = usbatm_instance->driver_data;
	struct device *dev = &usbatm_instance->usb_intf->dev;
	/*
	struct atm_dev *atm_dev = usbatm_instance->atm_dev;
	*/
	int ret;

	dbg("cxacru_atm_start");

	/* Read MAC address */
	ret = cxacru_cm(instance, CM_REQUEST_CARD_GET_MAC_ADDRESS, NULL, 0,
			atm_dev->esi, sizeof(atm_dev->esi));
	if (ret < 0) {
		dev_err(dev, "cxacru_atm_start: CARD_GET_MAC_ADDRESS returned %d\n", ret);
		return ret;
	}

	/* start ADSL */
	ret = cxacru_cm(instance, CM_REQUEST_CHIP_ADSL_LINE_START, NULL, 0, NULL, 0);
	if (ret < 0) {
		dev_err(dev, "cxacru_atm_start: CHIP_ADSL_LINE_START returned %d\n", ret);
		return ret;
	}

	/* Start status polling */
	cxacru_poll_status(instance);
	return 0;
}

static void cxacru_poll_status(struct cxacru_data *instance)
{
	u32 buf[CXINF_MAX] = {};
	struct device *dev = &instance->usbatm->usb_intf->dev;
	struct atm_dev *atm_dev = instance->usbatm->atm_dev;
	int ret;

	ret = cxacru_cm_get_array(instance, CM_REQUEST_CARD_INFO_GET, buf, CXINF_MAX);
	if (ret < 0) {
		dev_warn(dev, "poll status: error %d\n", ret);
		goto reschedule;
	}

	if (instance->line_status == buf[CXINF_LINE_STATUS])
		goto reschedule;

	instance->line_status = buf[CXINF_LINE_STATUS];
	switch (instance->line_status) {
	case 0:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: down\n");
		break;

	case 1:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: attemtping to activate\n");
		break;

	case 2:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: training\n");
		break;

	case 3:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: channel analysis\n");
		break;

	case 4:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: exchange\n");
		break;

	case 5:
		atm_dev->link_rate = buf[CXINF_DOWNSTREAM_RATE] * 1000 / 424;
		atm_dev->signal = ATM_PHY_SIG_FOUND;

		dev_info(dev, "ADSL line: up (%d kb/s down | %d kb/s up)\n",
		     buf[CXINF_DOWNSTREAM_RATE], buf[CXINF_UPSTREAM_RATE]);
		break;

	case 6:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: waiting\n");
		break;

	case 7:
		atm_dev->signal = ATM_PHY_SIG_LOST;
		dev_info(dev, "ADSL line: initializing\n");
		break;

	default:
		atm_dev->signal = ATM_PHY_SIG_UNKNOWN;
		dev_info(dev, "Unknown line state %02x\n", instance->line_status);
		break;
	}
reschedule:
	schedule_delayed_work(&instance->poll_work, msecs_to_jiffies(POLL_INTERVAL));
}

static int cxacru_fw(struct usb_device *usb_dev, enum cxacru_fw_request fw,
		     u8 code1, u8 code2, u32 addr, u8 *data, int size)
{
	int ret;
	u8 *buf;
	int offd, offb;
	const int stride = CMD_PACKET_SIZE - 8;

	buf = (u8 *) __get_free_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	offb = offd = 0;
	do {
		int l = min_t(int, stride, size - offd);
		buf[offb++] = fw;
		buf[offb++] = l;
		buf[offb++] = code1;
		buf[offb++] = code2;
		*((u32 *) (buf + offb)) = cpu_to_le32(addr);
		offb += 4;
		addr += l;
		if(l)
			memcpy(buf + offb, data + offd, l);
		if (l < stride)
			memset(buf + offb + l, 0, stride - l);
		offb += stride;
		offd += stride;
		if ((offb >= PAGE_SIZE) || (offd >= size)) {
			ret = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, CXACRU_EP_CMD),
					   buf, offb, NULL, CMD_TIMEOUT);
			if (ret < 0) {
				dbg("sending fw %#x failed", fw);
				goto cleanup;
			}
			offb = 0;
		}
	} while(offd < size);
	dbg("sent fw %#x", fw);

	ret = 0;

cleanup:
	free_page((unsigned long) buf);
	return ret;
}

static void cxacru_upload_firmware(struct cxacru_data *instance,
				   const struct firmware *fw,
				   const struct firmware *bp,
				   const struct firmware *cf)
{
	int ret;
	int off;
	struct usb_device *usb_dev = instance->usbatm->usb_dev;
	struct device *dev = &instance->usbatm->usb_intf->dev;
	u16 signature[] = { usb_dev->descriptor.idVendor, usb_dev->descriptor.idProduct };
	u32 val;

	dbg("cxacru_upload_firmware");

	/* FirmwarePllFClkValue */
	val = cpu_to_le32(instance->modem_type->pll_f_clk);
	ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, PLLFCLK_ADDR, (u8 *) &val, 4);
	if (ret) {
		dev_err(dev, "FirmwarePllFClkValue failed: %d\n", ret);
		return;
	}

	/* FirmwarePllBClkValue */
	val = cpu_to_le32(instance->modem_type->pll_b_clk);
	ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, PLLBCLK_ADDR, (u8 *) &val, 4);
	if (ret) {
		dev_err(dev, "FirmwarePllBClkValue failed: %d\n", ret);
		return;
	}

	/* Enable SDRAM */
	val = cpu_to_le32(SDRAM_ENA);
	ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, SDRAMEN_ADDR, (u8 *) &val, 4);
	if (ret) {
		dev_err(dev, "Enable SDRAM failed: %d\n", ret);
		return;
	}

	/* Firmware */
	ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, FW_ADDR, fw->data, fw->size);
	if (ret) {
		dev_err(dev, "Firmware upload failed: %d\n", ret);
		return;
	}

	/* Boot ROM patch */
	if (instance->modem_type->boot_rom_patch) {
		ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, BR_ADDR, bp->data, bp->size);
		if (ret) {
			dev_err(dev, "Boot ROM patching failed: %d\n", ret);
			return;
		}
	}

	/* Signature */
	ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, SIG_ADDR, (u8 *) signature, 4);
	if (ret) {
		dev_err(dev, "Signature storing failed: %d\n", ret);
		return;
	}

	if (instance->modem_type->boot_rom_patch) {
		val = cpu_to_le32(BR_ADDR);
		ret = cxacru_fw(usb_dev, FW_WRITE_MEM, 0x2, 0x0, BR_STACK_ADDR, (u8 *) &val, 4);
	}
	else {
		ret = cxacru_fw(usb_dev, FW_GOTO_MEM, 0x0, 0x0, FW_ADDR, NULL, 0);
	}
	if (ret) {
		dev_err(dev, "Passing control to firmware failed: %d\n", ret);
		return;
	}

	/* Delay to allow firmware to start up. */
	msleep_interruptible(1000);

	usb_clear_halt(usb_dev, usb_sndbulkpipe(usb_dev, CXACRU_EP_CMD));
	usb_clear_halt(usb_dev, usb_rcvbulkpipe(usb_dev, CXACRU_EP_CMD));
	usb_clear_halt(usb_dev, usb_sndbulkpipe(usb_dev, CXACRU_EP_DATA));
	usb_clear_halt(usb_dev, usb_rcvbulkpipe(usb_dev, CXACRU_EP_DATA));

	ret = cxacru_cm(instance, CM_REQUEST_CARD_GET_STATUS, NULL, 0, NULL, 0);
	if (ret < 0) {
		dev_err(dev, "modem failed to initialize: %d\n", ret);
		return;
	}

	/* Load config data (le32), doing one packet at a time */
	if (cf)
		for (off = 0; off < cf->size / 4; ) {
			u32 buf[CMD_PACKET_SIZE / 4 - 1];
			int i, len = min_t(int, cf->size / 4 - off, CMD_PACKET_SIZE / 4 / 2 - 1);
			buf[0] = cpu_to_le32(len);
			for (i = 0; i < len; i++, off++) {
				buf[i * 2 + 1] = cpu_to_le32(off);
				memcpy(buf + i * 2 + 2, cf->data + off * 4, 4);
			}
			ret = cxacru_cm(instance, CM_REQUEST_CARD_DATA_SET,
					(u8 *) buf, len, NULL, 0);
			if (ret < 0) {
				dev_err(dev, "load config data failed: %d\n", ret);
				return;
			}
		}

	msleep_interruptible(4000);
}

static int cxacru_find_firmware(struct cxacru_data *instance,
				char* phase, const struct firmware **fw_p)
{
	struct device *dev = &instance->usbatm->usb_intf->dev;
	char buf[16];

	sprintf(buf, "cxacru-%s.bin", phase);
	dbg("cxacru_find_firmware: looking for %s", buf);

	if (request_firmware(fw_p, buf, dev)) {
		dev_dbg(dev, "no stage %s firmware found\n", phase);
		return -ENOENT;
	}

	dev_info(dev, "found firmware %s\n", buf);

	return 0;
}

static int cxacru_heavy_init(struct usbatm_data *usbatm_instance,
			     struct usb_interface *usb_intf)
{
	struct device *dev = &usbatm_instance->usb_intf->dev;
	const struct firmware *fw, *bp, *cf;
	struct cxacru_data *instance = usbatm_instance->driver_data;

	int ret = cxacru_find_firmware(instance, "fw", &fw);
	if (ret) {
		dev_warn(dev, "firmware (cxacru-fw.bin) unavailable (hotplug misconfiguration?)\n");
		return ret;
	}

	if (instance->modem_type->boot_rom_patch) {
		ret = cxacru_find_firmware(instance, "bp", &bp);
		if (ret) {
			dev_warn(dev, "boot ROM patch (cxacru-bp.bin) unavailable (hotplug misconfiguration?)\n");
			release_firmware(fw);
			return ret;
		}
	}

	if (cxacru_find_firmware(instance, "cf", &cf))		/* optional */
		cf = NULL;

	cxacru_upload_firmware(instance, fw, bp, cf);

	if (cf)
		release_firmware(cf);
	if (instance->modem_type->boot_rom_patch)
		release_firmware(bp);
	release_firmware(fw);

	ret = cxacru_card_status(instance);
	if (ret)
		dbg("modem initialisation failed");
	else
		dbg("done setting up the modem");

	return ret;
}

static int cxacru_bind(struct usbatm_data *usbatm_instance,
		       struct usb_interface *intf, const struct usb_device_id *id,
		       int *need_heavy_init)
{
	struct cxacru_data *instance;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	int ret;

	/* instance init */
	instance = kmalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance) {
		dbg("cxacru_bind: no memory for instance data");
		return -ENOMEM;
	}

	memset(instance, 0, sizeof(*instance));

	instance->usbatm = usbatm_instance;
	instance->modem_type = (struct cxacru_modem_type *) id->driver_info;

	instance->rcv_buf = (u8 *) __get_free_page(GFP_KERNEL);
	if (!instance->rcv_buf) {
		dbg("cxacru_bind: no memory for rcv_buf");
		ret = -ENOMEM;
		goto fail;
	}
	instance->snd_buf = (u8 *) __get_free_page(GFP_KERNEL);
	if (!instance->snd_buf) {
		dbg("cxacru_bind: no memory for snd_buf");
		ret = -ENOMEM;
		goto fail;
	}
	instance->rcv_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!instance->rcv_urb) {
		dbg("cxacru_bind: no memory for rcv_urb");
		ret = -ENOMEM;
		goto fail;
	}
	instance->snd_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!instance->snd_urb) {
		dbg("cxacru_bind: no memory for snd_urb");
		ret = -ENOMEM;
		goto fail;
	}

	usb_fill_int_urb(instance->rcv_urb,
			usb_dev, usb_rcvintpipe(usb_dev, CXACRU_EP_CMD),
			instance->rcv_buf, PAGE_SIZE,
			cxacru_blocking_completion, &instance->rcv_done, 1);

	usb_fill_int_urb(instance->snd_urb,
			usb_dev, usb_sndintpipe(usb_dev, CXACRU_EP_CMD),
			instance->snd_buf, PAGE_SIZE,
			cxacru_blocking_completion, &instance->snd_done, 4);

	init_MUTEX(&instance->cm_serialize);

	INIT_WORK(&instance->poll_work, (void *)cxacru_poll_status, instance);

	usbatm_instance->driver_data = instance;

	*need_heavy_init = cxacru_card_status(instance);

	return 0;

 fail:
	free_page((unsigned long) instance->snd_buf);
	free_page((unsigned long) instance->rcv_buf);
	usb_free_urb(instance->snd_urb);
	usb_free_urb(instance->rcv_urb);
	kfree(instance);

	return ret;
}

static void cxacru_unbind(struct usbatm_data *usbatm_instance,
		struct usb_interface *intf)
{
	struct cxacru_data *instance = usbatm_instance->driver_data;

	dbg("cxacru_unbind entered");

	if (!instance) {
		dbg("cxacru_unbind: NULL instance!");
		return;
	}

	while (!cancel_delayed_work(&instance->poll_work))
	       flush_scheduled_work();

	usb_kill_urb(instance->snd_urb);
	usb_kill_urb(instance->rcv_urb);
	usb_free_urb(instance->snd_urb);
	usb_free_urb(instance->rcv_urb);

	free_page((unsigned long) instance->snd_buf);
	free_page((unsigned long) instance->rcv_buf);
	kfree(instance);

	usbatm_instance->driver_data = NULL;
}

static const struct cxacru_modem_type cxacru_cafe = {
	.pll_f_clk = 0x02d874df,
	.pll_b_clk = 0x0196a51a,
	.boot_rom_patch = 1,
};

static const struct cxacru_modem_type cxacru_cb00 = {
	.pll_f_clk = 0x5,
	.pll_b_clk = 0x3,
	.boot_rom_patch = 0,
};

static const struct usb_device_id cxacru_usb_ids[] = {
	{ /* V = Conexant			P = ADSL modem (Euphrates project)	*/
		USB_DEVICE(0x0572, 0xcafe),	.driver_info = (unsigned long) &cxacru_cafe
	},
	{ /* V = Conexant			P = ADSL modem (Hasbani project)	*/
		USB_DEVICE(0x0572, 0xcb00),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Conexant             P = ADSL modem (Well PTI-800 */
		USB_DEVICE(0x0572, 0xcb02),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Conexant			P = ADSL modem				*/
		USB_DEVICE(0x0572, 0xcb01),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Conexant			P = ADSL modem				*/
		USB_DEVICE(0x0572, 0xcb06),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Olitec				P = ADSL modem version 2		*/
		USB_DEVICE(0x08e3, 0x0100),	.driver_info = (unsigned long) &cxacru_cafe
	},
	{ /* V = Olitec				P = ADSL modem version 3		*/
		USB_DEVICE(0x08e3, 0x0102),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Trust/Amigo Technology Co.	P = AMX-CA86U				*/
		USB_DEVICE(0x0eb0, 0x3457),	.driver_info = (unsigned long) &cxacru_cafe
	},
	{ /* V = Zoom				P = 5510				*/
		USB_DEVICE(0x1803, 0x5510),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Draytek			P = Vigor 318				*/
		USB_DEVICE(0x0675, 0x0200),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Zyxel				P = 630-C1 aka OMNI ADSL USB (Annex A)	*/
		USB_DEVICE(0x0586, 0x330a),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Zyxel				P = 630-C3 aka OMNI ADSL USB (Annex B)	*/
		USB_DEVICE(0x0586, 0x330b),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Aethra				P = Starmodem UM1020			*/
		USB_DEVICE(0x0659, 0x0020),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Aztech Systems			P = ? AKA Pirelli AUA-010		*/
		USB_DEVICE(0x0509, 0x0812),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Netopia			P = Cayman 3341(Annex A)/3351(Annex B)	*/
		USB_DEVICE(0x100d, 0xcb01),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{ /* V = Netopia			P = Cayman 3342(Annex A)/3352(Annex B)	*/
		USB_DEVICE(0x100d, 0x3342),	.driver_info = (unsigned long) &cxacru_cb00
	},
	{}
};

MODULE_DEVICE_TABLE(usb, cxacru_usb_ids);

static struct usbatm_driver cxacru_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= cxacru_driver_name,
	.bind		= cxacru_bind,
	.heavy_init	= cxacru_heavy_init,
	.unbind		= cxacru_unbind,
	.atm_start	= cxacru_atm_start,
	.in		= CXACRU_EP_DATA,
	.out		= CXACRU_EP_DATA,
	.rx_padding	= 3,
	.tx_padding	= 11,
};

static int cxacru_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	return usbatm_usb_probe(intf, id, &cxacru_driver);
}

static struct usb_driver cxacru_usb_driver = {
	.name		= cxacru_driver_name,
	.probe		= cxacru_usb_probe,
	.disconnect	= usbatm_usb_disconnect,
	.id_table	= cxacru_usb_ids
};

static int __init cxacru_init(void)
{
	return usb_register(&cxacru_usb_driver);
}

static void __exit cxacru_cleanup(void)
{
	usb_deregister(&cxacru_usb_driver);
}

module_init(cxacru_init);
module_exit(cxacru_cleanup);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
