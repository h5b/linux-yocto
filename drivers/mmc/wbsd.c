/*
 *  linux/drivers/mmc/wbsd.c - Winbond W83L51xD SD/MMC driver
 *
 *  Copyright (C) 2004-2005 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 * Warning!
 *
 * Changes to the FIFO system should be done with extreme care since
 * the hardware is full of bugs related to the FIFO. Known issues are:
 *
 * - FIFO size field in FSR is always zero.
 *
 * - FIFO interrupts tend not to work as they should. Interrupts are
 *   triggered only for full/empty events, not for threshold values.
 *
 * - On APIC systems the FIFO empty interrupt is sometimes lost.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/pnp.h>
#include <linux/highmem.h>
#include <linux/mmc/host.h>
#include <linux/mmc/protocol.h>

#include <asm/io.h>
#include <asm/dma.h>
#include <asm/scatterlist.h>

#include "wbsd.h"

#define DRIVER_NAME "wbsd"
#define DRIVER_VERSION "1.5"

#ifdef CONFIG_MMC_DEBUG
#define DBG(x...) \
	printk(KERN_DEBUG DRIVER_NAME ": " x)
#define DBGF(f, x...) \
	printk(KERN_DEBUG DRIVER_NAME " [%s()]: " f, __func__ , ##x)
#else
#define DBG(x...)	do { } while (0)
#define DBGF(x...)	do { } while (0)
#endif

/*
 * Device resources
 */

#ifdef CONFIG_PNP

static const struct pnp_device_id pnp_dev_table[] = {
	{ "WEC0517", 0 },
	{ "WEC0518", 0 },
	{ "", 0 },
};

MODULE_DEVICE_TABLE(pnp, pnp_dev_table);

#endif /* CONFIG_PNP */

static const int config_ports[] = { 0x2E, 0x4E };
static const int unlock_codes[] = { 0x83, 0x87 };

static const int valid_ids[] = {
	0x7112,
	};

#ifdef CONFIG_PNP
static unsigned int nopnp = 0;
#else
static const unsigned int nopnp = 1;
#endif
static unsigned int io = 0x248;
static unsigned int irq = 6;
static int dma = 2;

/*
 * Basic functions
 */

static inline void wbsd_unlock_config(struct wbsd_host *host)
{
	BUG_ON(host->config == 0);

	outb(host->unlock_code, host->config);
	outb(host->unlock_code, host->config);
}

static inline void wbsd_lock_config(struct wbsd_host *host)
{
	BUG_ON(host->config == 0);

	outb(LOCK_CODE, host->config);
}

static inline void wbsd_write_config(struct wbsd_host *host, u8 reg, u8 value)
{
	BUG_ON(host->config == 0);

	outb(reg, host->config);
	outb(value, host->config + 1);
}

static inline u8 wbsd_read_config(struct wbsd_host *host, u8 reg)
{
	BUG_ON(host->config == 0);

	outb(reg, host->config);
	return inb(host->config + 1);
}

static inline void wbsd_write_index(struct wbsd_host *host, u8 index, u8 value)
{
	outb(index, host->base + WBSD_IDXR);
	outb(value, host->base + WBSD_DATAR);
}

static inline u8 wbsd_read_index(struct wbsd_host *host, u8 index)
{
	outb(index, host->base + WBSD_IDXR);
	return inb(host->base + WBSD_DATAR);
}

/*
 * Common routines
 */

static void wbsd_init_device(struct wbsd_host *host)
{
	u8 setup, ier;

	/*
	 * Reset chip (SD/MMC part) and fifo.
	 */
	setup = wbsd_read_index(host, WBSD_IDX_SETUP);
	setup |= WBSD_FIFO_RESET | WBSD_SOFT_RESET;
	wbsd_write_index(host, WBSD_IDX_SETUP, setup);

	/*
	 * Set DAT3 to input
	 */
	setup &= ~WBSD_DAT3_H;
	wbsd_write_index(host, WBSD_IDX_SETUP, setup);
	host->flags &= ~WBSD_FIGNORE_DETECT;

	/*
	 * Read back default clock.
	 */
	host->clk = wbsd_read_index(host, WBSD_IDX_CLK);

	/*
	 * Power down port.
	 */
	outb(WBSD_POWER_N, host->base + WBSD_CSR);

	/*
	 * Set maximum timeout.
	 */
	wbsd_write_index(host, WBSD_IDX_TAAC, 0x7F);

	/*
	 * Test for card presence
	 */
	if (inb(host->base + WBSD_CSR) & WBSD_CARDPRESENT)
		host->flags |= WBSD_FCARD_PRESENT;
	else
		host->flags &= ~WBSD_FCARD_PRESENT;

	/*
	 * Enable interesting interrupts.
	 */
	ier = 0;
	ier |= WBSD_EINT_CARD;
	ier |= WBSD_EINT_FIFO_THRE;
	ier |= WBSD_EINT_CCRC;
	ier |= WBSD_EINT_TIMEOUT;
	ier |= WBSD_EINT_CRC;
	ier |= WBSD_EINT_TC;

	outb(ier, host->base + WBSD_EIR);

	/*
	 * Clear interrupts.
	 */
	inb(host->base + WBSD_ISR);
}

static void wbsd_reset(struct wbsd_host *host)
{
	u8 setup;

	printk(KERN_ERR "%s: Resetting chip\n", mmc_hostname(host->mmc));

	/*
	 * Soft reset of chip (SD/MMC part).
	 */
	setup = wbsd_read_index(host, WBSD_IDX_SETUP);
	setup |= WBSD_SOFT_RESET;
	wbsd_write_index(host, WBSD_IDX_SETUP, setup);
}

static void wbsd_request_end(struct wbsd_host *host, struct mmc_request *mrq)
{
	unsigned long dmaflags;

	DBGF("Ending request, cmd (%x)\n", mrq->cmd->opcode);

	if (host->dma >= 0) {
		/*
		 * Release ISA DMA controller.
		 */
		dmaflags = claim_dma_lock();
		disable_dma(host->dma);
		clear_dma_ff(host->dma);
		release_dma_lock(dmaflags);

		/*
		 * Disable DMA on host.
		 */
		wbsd_write_index(host, WBSD_IDX_DMA, 0);
	}

	host->mrq = NULL;

	/*
	 * MMC layer might call back into the driver so first unlock.
	 */
	spin_unlock(&host->lock);
	mmc_request_done(host->mmc, mrq);
	spin_lock(&host->lock);
}

/*
 * Scatter/gather functions
 */

static inline void wbsd_init_sg(struct wbsd_host *host, struct mmc_data *data)
{
	/*
	 * Get info. about SG list from data structure.
	 */
	host->cur_sg = data->sg;
	host->num_sg = data->sg_len;

	host->offset = 0;
	host->remain = host->cur_sg->length;
}

static inline int wbsd_next_sg(struct wbsd_host *host)
{
	/*
	 * Skip to next SG entry.
	 */
	host->cur_sg++;
	host->num_sg--;

	/*
	 * Any entries left?
	 */
	if (host->num_sg > 0) {
		host->offset = 0;
		host->remain = host->cur_sg->length;
	}

	return host->num_sg;
}

static inline char *wbsd_kmap_sg(struct wbsd_host *host)
{
	host->mapped_sg = kmap_atomic(host->cur_sg->page, KM_BIO_SRC_IRQ) +
		host->cur_sg->offset;
	return host->mapped_sg;
}

static inline void wbsd_kunmap_sg(struct wbsd_host *host)
{
	kunmap_atomic(host->mapped_sg, KM_BIO_SRC_IRQ);
}

static inline void wbsd_sg_to_dma(struct wbsd_host *host, struct mmc_data *data)
{
	unsigned int len, i, size;
	struct scatterlist *sg;
	char *dmabuf = host->dma_buffer;
	char *sgbuf;

	size = host->size;

	sg = data->sg;
	len = data->sg_len;

	/*
	 * Just loop through all entries. Size might not
	 * be the entire list though so make sure that
	 * we do not transfer too much.
	 */
	for (i = 0; i < len; i++) {
		sgbuf = kmap_atomic(sg[i].page, KM_BIO_SRC_IRQ) + sg[i].offset;
		if (size < sg[i].length)
			memcpy(dmabuf, sgbuf, size);
		else
			memcpy(dmabuf, sgbuf, sg[i].length);
		kunmap_atomic(sgbuf, KM_BIO_SRC_IRQ);
		dmabuf += sg[i].length;

		if (size < sg[i].length)
			size = 0;
		else
			size -= sg[i].length;

		if (size == 0)
			break;
	}

	/*
	 * Check that we didn't get a request to transfer
	 * more data than can fit into the SG list.
	 */

	BUG_ON(size != 0);

	host->size -= size;
}

static inline void wbsd_dma_to_sg(struct wbsd_host *host, struct mmc_data *data)
{
	unsigned int len, i, size;
	struct scatterlist *sg;
	char *dmabuf = host->dma_buffer;
	char *sgbuf;

	size = host->size;

	sg = data->sg;
	len = data->sg_len;

	/*
	 * Just loop through all entries. Size might not
	 * be the entire list though so make sure that
	 * we do not transfer too much.
	 */
	for (i = 0; i < len; i++) {
		sgbuf = kmap_atomic(sg[i].page, KM_BIO_SRC_IRQ) + sg[i].offset;
		if (size < sg[i].length)
			memcpy(sgbuf, dmabuf, size);
		else
			memcpy(sgbuf, dmabuf, sg[i].length);
		kunmap_atomic(sgbuf, KM_BIO_SRC_IRQ);
		dmabuf += sg[i].length;

		if (size < sg[i].length)
			size = 0;
		else
			size -= sg[i].length;

		if (size == 0)
			break;
	}

	/*
	 * Check that we didn't get a request to transfer
	 * more data than can fit into the SG list.
	 */

	BUG_ON(size != 0);

	host->size -= size;
}

/*
 * Command handling
 */

static inline void wbsd_get_short_reply(struct wbsd_host *host,
					struct mmc_command *cmd)
{
	/*
	 * Correct response type?
	 */
	if (wbsd_read_index(host, WBSD_IDX_RSPLEN) != WBSD_RSP_SHORT) {
		cmd->error = MMC_ERR_INVALID;
		return;
	}

	cmd->resp[0]  = wbsd_read_index(host, WBSD_IDX_RESP12) << 24;
	cmd->resp[0] |= wbsd_read_index(host, WBSD_IDX_RESP13) << 16;
	cmd->resp[0] |= wbsd_read_index(host, WBSD_IDX_RESP14) << 8;
	cmd->resp[0] |= wbsd_read_index(host, WBSD_IDX_RESP15) << 0;
	cmd->resp[1]  = wbsd_read_index(host, WBSD_IDX_RESP16) << 24;
}

static inline void wbsd_get_long_reply(struct wbsd_host *host,
	struct mmc_command *cmd)
{
	int i;

	/*
	 * Correct response type?
	 */
	if (wbsd_read_index(host, WBSD_IDX_RSPLEN) != WBSD_RSP_LONG) {
		cmd->error = MMC_ERR_INVALID;
		return;
	}

	for (i = 0; i < 4; i++) {
		cmd->resp[i] =
			wbsd_read_index(host, WBSD_IDX_RESP1 + i * 4) << 24;
		cmd->resp[i] |=
			wbsd_read_index(host, WBSD_IDX_RESP2 + i * 4) << 16;
		cmd->resp[i] |=
			wbsd_read_index(host, WBSD_IDX_RESP3 + i * 4) << 8;
		cmd->resp[i] |=
			wbsd_read_index(host, WBSD_IDX_RESP4 + i * 4) << 0;
	}
}

static void wbsd_send_command(struct wbsd_host *host, struct mmc_command *cmd)
{
	int i;
	u8 status, isr;

	DBGF("Sending cmd (%x)\n", cmd->opcode);

	/*
	 * Clear accumulated ISR. The interrupt routine
	 * will fill this one with events that occur during
	 * transfer.
	 */
	host->isr = 0;

	/*
	 * Send the command (CRC calculated by host).
	 */
	outb(cmd->opcode, host->base + WBSD_CMDR);
	for (i = 3; i >= 0; i--)
		outb((cmd->arg >> (i * 8)) & 0xff, host->base + WBSD_CMDR);

	cmd->error = MMC_ERR_NONE;

	/*
	 * Wait for the request to complete.
	 */
	do {
		status = wbsd_read_index(host, WBSD_IDX_STATUS);
	} while (status & WBSD_CARDTRAFFIC);

	/*
	 * Do we expect a reply?
	 */
	if ((cmd->flags & MMC_RSP_MASK) != MMC_RSP_NONE) {
		/*
		 * Read back status.
		 */
		isr = host->isr;

		/* Card removed? */
		if (isr & WBSD_INT_CARD)
			cmd->error = MMC_ERR_TIMEOUT;
		/* Timeout? */
		else if (isr & WBSD_INT_TIMEOUT)
			cmd->error = MMC_ERR_TIMEOUT;
		/* CRC? */
		else if ((cmd->flags & MMC_RSP_CRC) && (isr & WBSD_INT_CRC))
			cmd->error = MMC_ERR_BADCRC;
		/* All ok */
		else {
			if ((cmd->flags & MMC_RSP_MASK) == MMC_RSP_SHORT)
				wbsd_get_short_reply(host, cmd);
			else
				wbsd_get_long_reply(host, cmd);
		}
	}

	DBGF("Sent cmd (%x), res %d\n", cmd->opcode, cmd->error);
}

/*
 * Data functions
 */

static void wbsd_empty_fifo(struct wbsd_host *host)
{
	struct mmc_data *data = host->mrq->cmd->data;
	char *buffer;
	int i, fsr, fifo;

	/*
	 * Handle excessive data.
	 */
	if (data->bytes_xfered == host->size)
		return;

	buffer = wbsd_kmap_sg(host) + host->offset;

	/*
	 * Drain the fifo. This has a tendency to loop longer
	 * than the FIFO length (usually one block).
	 */
	while (!((fsr = inb(host->base + WBSD_FSR)) & WBSD_FIFO_EMPTY)) {
		/*
		 * The size field in the FSR is broken so we have to
		 * do some guessing.
		 */
		if (fsr & WBSD_FIFO_FULL)
			fifo = 16;
		else if (fsr & WBSD_FIFO_FUTHRE)
			fifo = 8;
		else
			fifo = 1;

		for (i = 0; i < fifo; i++) {
			*buffer = inb(host->base + WBSD_DFR);
			buffer++;
			host->offset++;
			host->remain--;

			data->bytes_xfered++;

			/*
			 * Transfer done?
			 */
			if (data->bytes_xfered == host->size) {
				wbsd_kunmap_sg(host);
				return;
			}

			/*
			 * End of scatter list entry?
			 */
			if (host->remain == 0) {
				wbsd_kunmap_sg(host);

				/*
				 * Get next entry. Check if last.
				 */
				if (!wbsd_next_sg(host)) {
					/*
					 * We should never reach this point.
					 * It means that we're trying to
					 * transfer more blocks than can fit
					 * into the scatter list.
					 */
					BUG_ON(1);

					host->size = data->bytes_xfered;

					return;
				}

				buffer = wbsd_kmap_sg(host);
			}
		}
	}

	wbsd_kunmap_sg(host);

	/*
	 * This is a very dirty hack to solve a
	 * hardware problem. The chip doesn't trigger
	 * FIFO threshold interrupts properly.
	 */
	if ((host->size - data->bytes_xfered) < 16)
		tasklet_schedule(&host->fifo_tasklet);
}

static void wbsd_fill_fifo(struct wbsd_host *host)
{
	struct mmc_data *data = host->mrq->cmd->data;
	char *buffer;
	int i, fsr, fifo;

	/*
	 * Check that we aren't being called after the
	 * entire buffer has been transfered.
	 */
	if (data->bytes_xfered == host->size)
		return;

	buffer = wbsd_kmap_sg(host) + host->offset;

	/*
	 * Fill the fifo. This has a tendency to loop longer
	 * than the FIFO length (usually one block).
	 */
	while (!((fsr = inb(host->base + WBSD_FSR)) & WBSD_FIFO_FULL)) {
		/*
		 * The size field in the FSR is broken so we have to
		 * do some guessing.
		 */
		if (fsr & WBSD_FIFO_EMPTY)
			fifo = 0;
		else if (fsr & WBSD_FIFO_EMTHRE)
			fifo = 8;
		else
			fifo = 15;

		for (i = 16; i > fifo; i--) {
			outb(*buffer, host->base + WBSD_DFR);
			buffer++;
			host->offset++;
			host->remain--;

			data->bytes_xfered++;

			/*
			 * Transfer done?
			 */
			if (data->bytes_xfered == host->size) {
				wbsd_kunmap_sg(host);
				return;
			}

			/*
			 * End of scatter list entry?
			 */
			if (host->remain == 0) {
				wbsd_kunmap_sg(host);

				/*
				 * Get next entry. Check if last.
				 */
				if (!wbsd_next_sg(host)) {
					/*
					 * We should never reach this point.
					 * It means that we're trying to
					 * transfer more blocks than can fit
					 * into the scatter list.
					 */
					BUG_ON(1);

					host->size = data->bytes_xfered;

					return;
				}

				buffer = wbsd_kmap_sg(host);
			}
		}
	}

	wbsd_kunmap_sg(host);

	/*
	 * The controller stops sending interrupts for
	 * 'FIFO empty' under certain conditions. So we
	 * need to be a bit more pro-active.
	 */
	tasklet_schedule(&host->fifo_tasklet);
}

static void wbsd_prepare_data(struct wbsd_host *host, struct mmc_data *data)
{
	u16 blksize;
	u8 setup;
	unsigned long dmaflags;

	DBGF("blksz %04x blks %04x flags %08x\n",
		1 << data->blksz_bits, data->blocks, data->flags);
	DBGF("tsac %d ms nsac %d clk\n",
		data->timeout_ns / 1000000, data->timeout_clks);

	/*
	 * Calculate size.
	 */
	host->size = data->blocks << data->blksz_bits;

	/*
	 * Check timeout values for overflow.
	 * (Yes, some cards cause this value to overflow).
	 */
	if (data->timeout_ns > 127000000)
		wbsd_write_index(host, WBSD_IDX_TAAC, 127);
	else {
		wbsd_write_index(host, WBSD_IDX_TAAC,
			data->timeout_ns / 1000000);
	}

	if (data->timeout_clks > 255)
		wbsd_write_index(host, WBSD_IDX_NSAC, 255);
	else
		wbsd_write_index(host, WBSD_IDX_NSAC, data->timeout_clks);

	/*
	 * Inform the chip of how large blocks will be
	 * sent. It needs this to determine when to
	 * calculate CRC.
	 *
	 * Space for CRC must be included in the size.
	 * Two bytes are needed for each data line.
	 */
	if (host->bus_width == MMC_BUS_WIDTH_1) {
		blksize = (1 << data->blksz_bits) + 2;

		wbsd_write_index(host, WBSD_IDX_PBSMSB, (blksize >> 4) & 0xF0);
		wbsd_write_index(host, WBSD_IDX_PBSLSB, blksize & 0xFF);
	} else if (host->bus_width == MMC_BUS_WIDTH_4) {
		blksize = (1 << data->blksz_bits) + 2 * 4;

		wbsd_write_index(host, WBSD_IDX_PBSMSB,
			((blksize >> 4) & 0xF0) | WBSD_DATA_WIDTH);
		wbsd_write_index(host, WBSD_IDX_PBSLSB, blksize & 0xFF);
	} else {
		data->error = MMC_ERR_INVALID;
		return;
	}

	/*
	 * Clear the FIFO. This is needed even for DMA
	 * transfers since the chip still uses the FIFO
	 * internally.
	 */
	setup = wbsd_read_index(host, WBSD_IDX_SETUP);
	setup |= WBSD_FIFO_RESET;
	wbsd_write_index(host, WBSD_IDX_SETUP, setup);

	/*
	 * DMA transfer?
	 */
	if (host->dma >= 0) {
		/*
		 * The buffer for DMA is only 64 kB.
		 */
		BUG_ON(host->size > 0x10000);
		if (host->size > 0x10000) {
			data->error = MMC_ERR_INVALID;
			return;
		}

		/*
		 * Transfer data from the SG list to
		 * the DMA buffer.
		 */
		if (data->flags & MMC_DATA_WRITE)
			wbsd_sg_to_dma(host, data);

		/*
		 * Initialise the ISA DMA controller.
		 */
		dmaflags = claim_dma_lock();
		disable_dma(host->dma);
		clear_dma_ff(host->dma);
		if (data->flags & MMC_DATA_READ)
			set_dma_mode(host->dma, DMA_MODE_READ & ~0x40);
		else
			set_dma_mode(host->dma, DMA_MODE_WRITE & ~0x40);
		set_dma_addr(host->dma, host->dma_addr);
		set_dma_count(host->dma, host->size);

		enable_dma(host->dma);
		release_dma_lock(dmaflags);

		/*
		 * Enable DMA on the host.
		 */
		wbsd_write_index(host, WBSD_IDX_DMA, WBSD_DMA_ENABLE);
	} else {
		/*
		 * This flag is used to keep printk
		 * output to a minimum.
		 */
		host->firsterr = 1;

		/*
		 * Initialise the SG list.
		 */
		wbsd_init_sg(host, data);

		/*
		 * Turn off DMA.
		 */
		wbsd_write_index(host, WBSD_IDX_DMA, 0);

		/*
		 * Set up FIFO threshold levels (and fill
		 * buffer if doing a write).
		 */
		if (data->flags & MMC_DATA_READ) {
			wbsd_write_index(host, WBSD_IDX_FIFOEN,
				WBSD_FIFOEN_FULL | 8);
		} else {
			wbsd_write_index(host, WBSD_IDX_FIFOEN,
				WBSD_FIFOEN_EMPTY | 8);
			wbsd_fill_fifo(host);
		}
	}

	data->error = MMC_ERR_NONE;
}

static void wbsd_finish_data(struct wbsd_host *host, struct mmc_data *data)
{
	unsigned long dmaflags;
	int count;
	u8 status;

	WARN_ON(host->mrq == NULL);

	/*
	 * Send a stop command if needed.
	 */
	if (data->stop)
		wbsd_send_command(host, data->stop);

	/*
	 * Wait for the controller to leave data
	 * transfer state.
	 */
	do {
		status = wbsd_read_index(host, WBSD_IDX_STATUS);
	} while (status & (WBSD_BLOCK_READ | WBSD_BLOCK_WRITE));

	/*
	 * DMA transfer?
	 */
	if (host->dma >= 0) {
		/*
		 * Disable DMA on the host.
		 */
		wbsd_write_index(host, WBSD_IDX_DMA, 0);

		/*
		 * Turn of ISA DMA controller.
		 */
		dmaflags = claim_dma_lock();
		disable_dma(host->dma);
		clear_dma_ff(host->dma);
		count = get_dma_residue(host->dma);
		release_dma_lock(dmaflags);

		/*
		 * Any leftover data?
		 */
		if (count) {
			printk(KERN_ERR "%s: Incomplete DMA transfer. "
				"%d bytes left.\n",
				mmc_hostname(host->mmc), count);

			data->error = MMC_ERR_FAILED;
		} else {
			/*
			 * Transfer data from DMA buffer to
			 * SG list.
			 */
			if (data->flags & MMC_DATA_READ)
				wbsd_dma_to_sg(host, data);

			data->bytes_xfered = host->size;
		}
	}

	DBGF("Ending data transfer (%d bytes)\n", data->bytes_xfered);

	wbsd_request_end(host, host->mrq);
}

/*****************************************************************************\
 *                                                                           *
 * MMC layer callbacks                                                       *
 *                                                                           *
\*****************************************************************************/

static void wbsd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct wbsd_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;

	/*
	 * Disable tasklets to avoid a deadlock.
	 */
	spin_lock_bh(&host->lock);

	BUG_ON(host->mrq != NULL);

	cmd = mrq->cmd;

	host->mrq = mrq;

	/*
	 * If there is no card in the slot then
	 * timeout immediatly.
	 */
	if (!(host->flags & WBSD_FCARD_PRESENT)) {
		cmd->error = MMC_ERR_TIMEOUT;
		goto done;
	}

	/*
	 * Does the request include data?
	 */
	if (cmd->data) {
		wbsd_prepare_data(host, cmd->data);

		if (cmd->data->error != MMC_ERR_NONE)
			goto done;
	}

	wbsd_send_command(host, cmd);

	/*
	 * If this is a data transfer the request
	 * will be finished after the data has
	 * transfered.
	 */
	if (cmd->data && (cmd->error == MMC_ERR_NONE)) {
		/*
		 * Dirty fix for hardware bug.
		 */
		if (host->dma == -1)
			tasklet_schedule(&host->fifo_tasklet);

		spin_unlock_bh(&host->lock);

		return;
	}

done:
	wbsd_request_end(host, mrq);

	spin_unlock_bh(&host->lock);
}

static void wbsd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct wbsd_host *host = mmc_priv(mmc);
	u8 clk, setup, pwr;

	DBGF("clock %uHz busmode %u powermode %u cs %u Vdd %u width %u\n",
		ios->clock, ios->bus_mode, ios->power_mode, ios->chip_select,
		ios->vdd, ios->bus_width);

	spin_lock_bh(&host->lock);

	/*
	 * Reset the chip on each power off.
	 * Should clear out any weird states.
	 */
	if (ios->power_mode == MMC_POWER_OFF)
		wbsd_init_device(host);

	if (ios->clock >= 24000000)
		clk = WBSD_CLK_24M;
	else if (ios->clock >= 16000000)
		clk = WBSD_CLK_16M;
	else if (ios->clock >= 12000000)
		clk = WBSD_CLK_12M;
	else
		clk = WBSD_CLK_375K;

	/*
	 * Only write to the clock register when
	 * there is an actual change.
	 */
	if (clk != host->clk) {
		wbsd_write_index(host, WBSD_IDX_CLK, clk);
		host->clk = clk;
	}

	/*
	 * Power up card.
	 */
	if (ios->power_mode != MMC_POWER_OFF) {
		pwr = inb(host->base + WBSD_CSR);
		pwr &= ~WBSD_POWER_N;
		outb(pwr, host->base + WBSD_CSR);
	}

	/*
	 * MMC cards need to have pin 1 high during init.
	 * It wreaks havoc with the card detection though so
	 * that needs to be disabled.
	 */
	setup = wbsd_read_index(host, WBSD_IDX_SETUP);
	if (ios->chip_select == MMC_CS_HIGH) {
		BUG_ON(ios->bus_width != MMC_BUS_WIDTH_1);
		setup |= WBSD_DAT3_H;
		host->flags |= WBSD_FIGNORE_DETECT;
	} else {
		if (setup & WBSD_DAT3_H) {
			setup &= ~WBSD_DAT3_H;

			/*
			 * We cannot resume card detection immediatly
			 * because of capacitance and delays in the chip.
			 */
			mod_timer(&host->ignore_timer, jiffies + HZ / 100);
		}
	}
	wbsd_write_index(host, WBSD_IDX_SETUP, setup);

	/*
	 * Store bus width for later. Will be used when
	 * setting up the data transfer.
	 */
	host->bus_width = ios->bus_width;

	spin_unlock_bh(&host->lock);
}

static int wbsd_get_ro(struct mmc_host *mmc)
{
	struct wbsd_host *host = mmc_priv(mmc);
	u8 csr;

	spin_lock_bh(&host->lock);

	csr = inb(host->base + WBSD_CSR);
	csr |= WBSD_MSLED;
	outb(csr, host->base + WBSD_CSR);

	mdelay(1);

	csr = inb(host->base + WBSD_CSR);
	csr &= ~WBSD_MSLED;
	outb(csr, host->base + WBSD_CSR);

	spin_unlock_bh(&host->lock);

	return csr & WBSD_WRPT;
}

static struct mmc_host_ops wbsd_ops = {
	.request	= wbsd_request,
	.set_ios	= wbsd_set_ios,
	.get_ro		= wbsd_get_ro,
};

/*****************************************************************************\
 *                                                                           *
 * Interrupt handling                                                        *
 *                                                                           *
\*****************************************************************************/

/*
 * Helper function to reset detection ignore
 */

static void wbsd_reset_ignore(unsigned long data)
{
	struct wbsd_host *host = (struct wbsd_host *)data;

	BUG_ON(host == NULL);

	DBG("Resetting card detection ignore\n");

	spin_lock_bh(&host->lock);

	host->flags &= ~WBSD_FIGNORE_DETECT;

	/*
	 * Card status might have changed during the
	 * blackout.
	 */
	tasklet_schedule(&host->card_tasklet);

	spin_unlock_bh(&host->lock);
}

/*
 * Tasklets
 */

static inline struct mmc_data *wbsd_get_data(struct wbsd_host *host)
{
	WARN_ON(!host->mrq);
	if (!host->mrq)
		return NULL;

	WARN_ON(!host->mrq->cmd);
	if (!host->mrq->cmd)
		return NULL;

	WARN_ON(!host->mrq->cmd->data);
	if (!host->mrq->cmd->data)
		return NULL;

	return host->mrq->cmd->data;
}

static void wbsd_tasklet_card(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	u8 csr;
	int delay = -1;

	spin_lock(&host->lock);

	if (host->flags & WBSD_FIGNORE_DETECT) {
		spin_unlock(&host->lock);
		return;
	}

	csr = inb(host->base + WBSD_CSR);
	WARN_ON(csr == 0xff);

	if (csr & WBSD_CARDPRESENT) {
		if (!(host->flags & WBSD_FCARD_PRESENT)) {
			DBG("Card inserted\n");
			host->flags |= WBSD_FCARD_PRESENT;

			delay = 500;
		}
	} else if (host->flags & WBSD_FCARD_PRESENT) {
		DBG("Card removed\n");
		host->flags &= ~WBSD_FCARD_PRESENT;

		if (host->mrq) {
			printk(KERN_ERR "%s: Card removed during transfer!\n",
				mmc_hostname(host->mmc));
			wbsd_reset(host);

			host->mrq->cmd->error = MMC_ERR_FAILED;
			tasklet_schedule(&host->finish_tasklet);
		}

		delay = 0;
	}

	/*
	 * Unlock first since we might get a call back.
	 */

	spin_unlock(&host->lock);

	if (delay != -1)
		mmc_detect_change(host->mmc, msecs_to_jiffies(delay));
}

static void wbsd_tasklet_fifo(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	struct mmc_data *data;

	spin_lock(&host->lock);

	if (!host->mrq)
		goto end;

	data = wbsd_get_data(host);
	if (!data)
		goto end;

	if (data->flags & MMC_DATA_WRITE)
		wbsd_fill_fifo(host);
	else
		wbsd_empty_fifo(host);

	/*
	 * Done?
	 */
	if (host->size == data->bytes_xfered) {
		wbsd_write_index(host, WBSD_IDX_FIFOEN, 0);
		tasklet_schedule(&host->finish_tasklet);
	}

end:
	spin_unlock(&host->lock);
}

static void wbsd_tasklet_crc(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	struct mmc_data *data;

	spin_lock(&host->lock);

	if (!host->mrq)
		goto end;

	data = wbsd_get_data(host);
	if (!data)
		goto end;

	DBGF("CRC error\n");

	data->error = MMC_ERR_BADCRC;

	tasklet_schedule(&host->finish_tasklet);

end:
	spin_unlock(&host->lock);
}

static void wbsd_tasklet_timeout(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	struct mmc_data *data;

	spin_lock(&host->lock);

	if (!host->mrq)
		goto end;

	data = wbsd_get_data(host);
	if (!data)
		goto end;

	DBGF("Timeout\n");

	data->error = MMC_ERR_TIMEOUT;

	tasklet_schedule(&host->finish_tasklet);

end:
	spin_unlock(&host->lock);
}

static void wbsd_tasklet_finish(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	struct mmc_data *data;

	spin_lock(&host->lock);

	WARN_ON(!host->mrq);
	if (!host->mrq)
		goto end;

	data = wbsd_get_data(host);
	if (!data)
		goto end;

	wbsd_finish_data(host, data);

end:
	spin_unlock(&host->lock);
}

static void wbsd_tasklet_block(unsigned long param)
{
	struct wbsd_host *host = (struct wbsd_host *)param;
	struct mmc_data *data;

	spin_lock(&host->lock);

	if ((wbsd_read_index(host, WBSD_IDX_CRCSTATUS) & WBSD_CRC_MASK) !=
		WBSD_CRC_OK) {
		data = wbsd_get_data(host);
		if (!data)
			goto end;

		DBGF("CRC error\n");

		data->error = MMC_ERR_BADCRC;

		tasklet_schedule(&host->finish_tasklet);
	}

end:
	spin_unlock(&host->lock);
}

/*
 * Interrupt handling
 */

static irqreturn_t wbsd_irq(int irq, void *dev_id, struct pt_regs *regs)
{
	struct wbsd_host *host = dev_id;
	int isr;

	isr = inb(host->base + WBSD_ISR);

	/*
	 * Was it actually our hardware that caused the interrupt?
	 */
	if (isr == 0xff || isr == 0x00)
		return IRQ_NONE;

	host->isr |= isr;

	/*
	 * Schedule tasklets as needed.
	 */
	if (isr & WBSD_INT_CARD)
		tasklet_schedule(&host->card_tasklet);
	if (isr & WBSD_INT_FIFO_THRE)
		tasklet_schedule(&host->fifo_tasklet);
	if (isr & WBSD_INT_CRC)
		tasklet_hi_schedule(&host->crc_tasklet);
	if (isr & WBSD_INT_TIMEOUT)
		tasklet_hi_schedule(&host->timeout_tasklet);
	if (isr & WBSD_INT_BUSYEND)
		tasklet_hi_schedule(&host->block_tasklet);
	if (isr & WBSD_INT_TC)
		tasklet_schedule(&host->finish_tasklet);

	return IRQ_HANDLED;
}

/*****************************************************************************\
 *                                                                           *
 * Device initialisation and shutdown                                        *
 *                                                                           *
\*****************************************************************************/

/*
 * Allocate/free MMC structure.
 */

static int __devinit wbsd_alloc_mmc(struct device *dev)
{
	struct mmc_host *mmc;
	struct wbsd_host *host;

	/*
	 * Allocate MMC structure.
	 */
	mmc = mmc_alloc_host(sizeof(struct wbsd_host), dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	host->mmc = mmc;

	host->dma = -1;

	/*
	 * Set host parameters.
	 */
	mmc->ops = &wbsd_ops;
	mmc->f_min = 375000;
	mmc->f_max = 24000000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA;

	spin_lock_init(&host->lock);

	/*
	 * Set up timers
	 */
	init_timer(&host->ignore_timer);
	host->ignore_timer.data = (unsigned long)host;
	host->ignore_timer.function = wbsd_reset_ignore;

	/*
	 * Maximum number of segments. Worst case is one sector per segment
	 * so this will be 64kB/512.
	 */
	mmc->max_hw_segs = 128;
	mmc->max_phys_segs = 128;

	/*
	 * Maximum number of sectors in one transfer. Also limited by 64kB
	 * buffer.
	 */
	mmc->max_sectors = 128;

	/*
	 * Maximum segment size. Could be one segment with the maximum number
	 * of segments.
	 */
	mmc->max_seg_size = mmc->max_sectors * 512;

	dev_set_drvdata(dev, mmc);

	return 0;
}

static void __devexit wbsd_free_mmc(struct device *dev)
{
	struct mmc_host *mmc;
	struct wbsd_host *host;

	mmc = dev_get_drvdata(dev);
	if (!mmc)
		return;

	host = mmc_priv(mmc);
	BUG_ON(host == NULL);

	del_timer_sync(&host->ignore_timer);

	mmc_free_host(mmc);

	dev_set_drvdata(dev, NULL);
}

/*
 * Scan for known chip id:s
 */

static int __devinit wbsd_scan(struct wbsd_host *host)
{
	int i, j, k;
	int id;

	/*
	 * Iterate through all ports, all codes to
	 * find hardware that is in our known list.
	 */
	for (i = 0; i < ARRAY_SIZE(config_ports); i++) {
		if (!request_region(config_ports[i], 2, DRIVER_NAME))
			continue;

		for (j = 0; j < ARRAY_SIZE(unlock_codes); j++) {
			id = 0xFFFF;

			host->config = config_ports[i];
			host->unlock_code = unlock_codes[j];

			wbsd_unlock_config(host);

			outb(WBSD_CONF_ID_HI, config_ports[i]);
			id = inb(config_ports[i] + 1) << 8;

			outb(WBSD_CONF_ID_LO, config_ports[i]);
			id |= inb(config_ports[i] + 1);

			wbsd_lock_config(host);

			for (k = 0; k < ARRAY_SIZE(valid_ids); k++) {
				if (id == valid_ids[k]) {
					host->chip_id = id;

					return 0;
				}
			}

			if (id != 0xFFFF) {
				DBG("Unknown hardware (id %x) found at %x\n",
					id, config_ports[i]);
			}
		}

		release_region(config_ports[i], 2);
	}

	host->config = 0;
	host->unlock_code = 0;

	return -ENODEV;
}

/*
 * Allocate/free io port ranges
 */

static int __devinit wbsd_request_region(struct wbsd_host *host, int base)
{
	if (io & 0x7)
		return -EINVAL;

	if (!request_region(base, 8, DRIVER_NAME))
		return -EIO;

	host->base = io;

	return 0;
}

static void __devexit wbsd_release_regions(struct wbsd_host *host)
{
	if (host->base)
		release_region(host->base, 8);

	host->base = 0;

	if (host->config)
		release_region(host->config, 2);

	host->config = 0;
}

/*
 * Allocate/free DMA port and buffer
 */

static void __devinit wbsd_request_dma(struct wbsd_host *host, int dma)
{
	if (dma < 0)
		return;

	if (request_dma(dma, DRIVER_NAME))
		goto err;

	/*
	 * We need to allocate a special buffer in
	 * order for ISA to be able to DMA to it.
	 */
	host->dma_buffer = kmalloc(WBSD_DMA_SIZE,
		GFP_NOIO | GFP_DMA | __GFP_REPEAT | __GFP_NOWARN);
	if (!host->dma_buffer)
		goto free;

	/*
	 * Translate the address to a physical address.
	 */
	host->dma_addr = dma_map_single(host->mmc->dev, host->dma_buffer,
		WBSD_DMA_SIZE, DMA_BIDIRECTIONAL);

	/*
	 * ISA DMA must be aligned on a 64k basis.
	 */
	if ((host->dma_addr & 0xffff) != 0)
		goto kfree;
	/*
	 * ISA cannot access memory above 16 MB.
	 */
	else if (host->dma_addr >= 0x1000000)
		goto kfree;

	host->dma = dma;

	return;

kfree:
	/*
	 * If we've gotten here then there is some kind of alignment bug
	 */
	BUG_ON(1);

	dma_unmap_single(host->mmc->dev, host->dma_addr,
		WBSD_DMA_SIZE, DMA_BIDIRECTIONAL);
	host->dma_addr = (dma_addr_t)NULL;

	kfree(host->dma_buffer);
	host->dma_buffer = NULL;

free:
	free_dma(dma);

err:
	printk(KERN_WARNING DRIVER_NAME ": Unable to allocate DMA %d. "
		"Falling back on FIFO.\n", dma);
}

static void __devexit wbsd_release_dma(struct wbsd_host *host)
{
	if (host->dma_addr) {
		dma_unmap_single(host->mmc->dev, host->dma_addr,
			WBSD_DMA_SIZE, DMA_BIDIRECTIONAL);
	}
	kfree(host->dma_buffer);
	if (host->dma >= 0)
		free_dma(host->dma);

	host->dma = -1;
	host->dma_buffer = NULL;
	host->dma_addr = (dma_addr_t)NULL;
}

/*
 * Allocate/free IRQ.
 */

static int __devinit wbsd_request_irq(struct wbsd_host *host, int irq)
{
	int ret;

	/*
	 * Allocate interrupt.
	 */

	ret = request_irq(irq, wbsd_irq, SA_SHIRQ, DRIVER_NAME, host);
	if (ret)
		return ret;

	host->irq = irq;

	/*
	 * Set up tasklets.
	 */
	tasklet_init(&host->card_tasklet, wbsd_tasklet_card,
			(unsigned long)host);
	tasklet_init(&host->fifo_tasklet, wbsd_tasklet_fifo,
			(unsigned long)host);
	tasklet_init(&host->crc_tasklet, wbsd_tasklet_crc,
			(unsigned long)host);
	tasklet_init(&host->timeout_tasklet, wbsd_tasklet_timeout,
			(unsigned long)host);
	tasklet_init(&host->finish_tasklet, wbsd_tasklet_finish,
			(unsigned long)host);
	tasklet_init(&host->block_tasklet, wbsd_tasklet_block,
			(unsigned long)host);

	return 0;
}

static void __devexit wbsd_release_irq(struct wbsd_host *host)
{
	if (!host->irq)
		return;

	free_irq(host->irq, host);

	host->irq = 0;

	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->fifo_tasklet);
	tasklet_kill(&host->crc_tasklet);
	tasklet_kill(&host->timeout_tasklet);
	tasklet_kill(&host->finish_tasklet);
	tasklet_kill(&host->block_tasklet);
}

/*
 * Allocate all resources for the host.
 */

static int __devinit wbsd_request_resources(struct wbsd_host *host,
	int base, int irq, int dma)
{
	int ret;

	/*
	 * Allocate I/O ports.
	 */
	ret = wbsd_request_region(host, base);
	if (ret)
		return ret;

	/*
	 * Allocate interrupt.
	 */
	ret = wbsd_request_irq(host, irq);
	if (ret)
		return ret;

	/*
	 * Allocate DMA.
	 */
	wbsd_request_dma(host, dma);

	return 0;
}

/*
 * Release all resources for the host.
 */

static void __devexit wbsd_release_resources(struct wbsd_host *host)
{
	wbsd_release_dma(host);
	wbsd_release_irq(host);
	wbsd_release_regions(host);
}

/*
 * Configure the resources the chip should use.
 */

static void wbsd_chip_config(struct wbsd_host *host)
{
	wbsd_unlock_config(host);

	/*
	 * Reset the chip.
	 */
	wbsd_write_config(host, WBSD_CONF_SWRST, 1);
	wbsd_write_config(host, WBSD_CONF_SWRST, 0);

	/*
	 * Select SD/MMC function.
	 */
	wbsd_write_config(host, WBSD_CONF_DEVICE, DEVICE_SD);

	/*
	 * Set up card detection.
	 */
	wbsd_write_config(host, WBSD_CONF_PINS, WBSD_PINS_DETECT_GP11);

	/*
	 * Configure chip
	 */
	wbsd_write_config(host, WBSD_CONF_PORT_HI, host->base >> 8);
	wbsd_write_config(host, WBSD_CONF_PORT_LO, host->base & 0xff);

	wbsd_write_config(host, WBSD_CONF_IRQ, host->irq);

	if (host->dma >= 0)
		wbsd_write_config(host, WBSD_CONF_DRQ, host->dma);

	/*
	 * Enable and power up chip.
	 */
	wbsd_write_config(host, WBSD_CONF_ENABLE, 1);
	wbsd_write_config(host, WBSD_CONF_POWER, 0x20);

	wbsd_lock_config(host);
}

/*
 * Check that configured resources are correct.
 */

static int wbsd_chip_validate(struct wbsd_host *host)
{
	int base, irq, dma;

	wbsd_unlock_config(host);

	/*
	 * Select SD/MMC function.
	 */
	wbsd_write_config(host, WBSD_CONF_DEVICE, DEVICE_SD);

	/*
	 * Read configuration.
	 */
	base = wbsd_read_config(host, WBSD_CONF_PORT_HI) << 8;
	base |= wbsd_read_config(host, WBSD_CONF_PORT_LO);

	irq = wbsd_read_config(host, WBSD_CONF_IRQ);

	dma = wbsd_read_config(host, WBSD_CONF_DRQ);

	wbsd_lock_config(host);

	/*
	 * Validate against given configuration.
	 */
	if (base != host->base)
		return 0;
	if (irq != host->irq)
		return 0;
	if ((dma != host->dma) && (host->dma != -1))
		return 0;

	return 1;
}

/*
 * Powers down the SD function
 */

static void wbsd_chip_poweroff(struct wbsd_host *host)
{
	wbsd_unlock_config(host);

	wbsd_write_config(host, WBSD_CONF_DEVICE, DEVICE_SD);
	wbsd_write_config(host, WBSD_CONF_ENABLE, 0);

	wbsd_lock_config(host);
}

/*****************************************************************************\
 *                                                                           *
 * Devices setup and shutdown                                                *
 *                                                                           *
\*****************************************************************************/

static int __devinit wbsd_init(struct device *dev, int base, int irq, int dma,
	int pnp)
{
	struct wbsd_host *host = NULL;
	struct mmc_host *mmc = NULL;
	int ret;

	ret = wbsd_alloc_mmc(dev);
	if (ret)
		return ret;

	mmc = dev_get_drvdata(dev);
	host = mmc_priv(mmc);

	/*
	 * Scan for hardware.
	 */
	ret = wbsd_scan(host);
	if (ret) {
		if (pnp && (ret == -ENODEV)) {
			printk(KERN_WARNING DRIVER_NAME
				": Unable to confirm device presence. You may "
				"experience lock-ups.\n");
		} else {
			wbsd_free_mmc(dev);
			return ret;
		}
	}

	/*
	 * Request resources.
	 */
	ret = wbsd_request_resources(host, io, irq, dma);
	if (ret) {
		wbsd_release_resources(host);
		wbsd_free_mmc(dev);
		return ret;
	}

	/*
	 * See if chip needs to be configured.
	 */
	if (pnp) {
		if ((host->config != 0) && !wbsd_chip_validate(host)) {
			printk(KERN_WARNING DRIVER_NAME
				": PnP active but chip not configured! "
				"You probably have a buggy BIOS. "
				"Configuring chip manually.\n");
			wbsd_chip_config(host);
		}
	} else
		wbsd_chip_config(host);

	/*
	 * Power Management stuff. No idea how this works.
	 * Not tested.
	 */
#ifdef CONFIG_PM
	if (host->config) {
		wbsd_unlock_config(host);
		wbsd_write_config(host, WBSD_CONF_PME, 0xA0);
		wbsd_lock_config(host);
	}
#endif
	/*
	 * Allow device to initialise itself properly.
	 */
	mdelay(5);

	/*
	 * Reset the chip into a known state.
	 */
	wbsd_init_device(host);

	mmc_add_host(mmc);

	printk(KERN_INFO "%s: W83L51xD", mmc_hostname(mmc));
	if (host->chip_id != 0)
		printk(" id %x", (int)host->chip_id);
	printk(" at 0x%x irq %d", (int)host->base, (int)host->irq);
	if (host->dma >= 0)
		printk(" dma %d", (int)host->dma);
	else
		printk(" FIFO");
	if (pnp)
		printk(" PnP");
	printk("\n");

	return 0;
}

static void __devexit wbsd_shutdown(struct device *dev, int pnp)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct wbsd_host *host;

	if (!mmc)
		return;

	host = mmc_priv(mmc);

	mmc_remove_host(mmc);

	/*
	 * Power down the SD/MMC function.
	 */
	if (!pnp)
		wbsd_chip_poweroff(host);

	wbsd_release_resources(host);

	wbsd_free_mmc(dev);
}

/*
 * Non-PnP
 */

static int __devinit wbsd_probe(struct platform_device *dev)
{
	return wbsd_init(&dev->dev, io, irq, dma, 0);
}

static int __devexit wbsd_remove(struct platform_device *dev)
{
	wbsd_shutdown(&dev->dev, 0);

	return 0;
}

/*
 * PnP
 */

#ifdef CONFIG_PNP

static int __devinit
wbsd_pnp_probe(struct pnp_dev *pnpdev, const struct pnp_device_id *dev_id)
{
	int io, irq, dma;

	/*
	 * Get resources from PnP layer.
	 */
	io = pnp_port_start(pnpdev, 0);
	irq = pnp_irq(pnpdev, 0);
	if (pnp_dma_valid(pnpdev, 0))
		dma = pnp_dma(pnpdev, 0);
	else
		dma = -1;

	DBGF("PnP resources: port %3x irq %d dma %d\n", io, irq, dma);

	return wbsd_init(&pnpdev->dev, io, irq, dma, 1);
}

static void __devexit wbsd_pnp_remove(struct pnp_dev *dev)
{
	wbsd_shutdown(&dev->dev, 1);
}

#endif /* CONFIG_PNP */

/*
 * Power management
 */

#ifdef CONFIG_PM

static int wbsd_suspend(struct wbsd_host *host, pm_message_t state)
{
	BUG_ON(host == NULL);

	return mmc_suspend_host(host->mmc, state);
}

static int wbsd_resume(struct wbsd_host *host)
{
	BUG_ON(host == NULL);

	wbsd_init_device(host);

	return mmc_resume_host(host->mmc);
}

static int wbsd_platform_suspend(struct platform_device *dev,
				 pm_message_t state)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	struct wbsd_host *host;
	int ret;

	if (mmc == NULL)
		return 0;

	DBGF("Suspending...\n");

	host = mmc_priv(mmc);

	ret = wbsd_suspend(host, state);
	if (ret)
		return ret;

	wbsd_chip_poweroff(host);

	return 0;
}

static int wbsd_platform_resume(struct platform_device *dev)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	struct wbsd_host *host;

	if (mmc == NULL)
		return 0;

	DBGF("Resuming...\n");

	host = mmc_priv(mmc);

	wbsd_chip_config(host);

	/*
	 * Allow device to initialise itself properly.
	 */
	mdelay(5);

	return wbsd_resume(host);
}

#ifdef CONFIG_PNP

static int wbsd_pnp_suspend(struct pnp_dev *pnp_dev, pm_message_t state)
{
	struct mmc_host *mmc = dev_get_drvdata(&pnp_dev->dev);
	struct wbsd_host *host;

	if (mmc == NULL)
		return 0;

	DBGF("Suspending...\n");

	host = mmc_priv(mmc);

	return wbsd_suspend(host, state);
}

static int wbsd_pnp_resume(struct pnp_dev *pnp_dev)
{
	struct mmc_host *mmc = dev_get_drvdata(&pnp_dev->dev);
	struct wbsd_host *host;

	if (mmc == NULL)
		return 0;

	DBGF("Resuming...\n");

	host = mmc_priv(mmc);

	/*
	 * See if chip needs to be configured.
	 */
	if (host->config != 0) {
		if (!wbsd_chip_validate(host)) {
			printk(KERN_WARNING DRIVER_NAME
				": PnP active but chip not configured! "
				"You probably have a buggy BIOS. "
				"Configuring chip manually.\n");
			wbsd_chip_config(host);
		}
	}

	/*
	 * Allow device to initialise itself properly.
	 */
	mdelay(5);

	return wbsd_resume(host);
}

#endif /* CONFIG_PNP */

#else /* CONFIG_PM */

#define wbsd_platform_suspend NULL
#define wbsd_platform_resume NULL

#define wbsd_pnp_suspend NULL
#define wbsd_pnp_resume NULL

#endif /* CONFIG_PM */

static struct platform_device *wbsd_device;

static struct platform_driver wbsd_driver = {
	.probe		= wbsd_probe,
	.remove		= __devexit_p(wbsd_remove),

	.suspend	= wbsd_platform_suspend,
	.resume		= wbsd_platform_resume,
	.driver		= {
		.name	= DRIVER_NAME,
	},
};

#ifdef CONFIG_PNP

static struct pnp_driver wbsd_pnp_driver = {
	.name		= DRIVER_NAME,
	.id_table	= pnp_dev_table,
	.probe		= wbsd_pnp_probe,
	.remove		= __devexit_p(wbsd_pnp_remove),

	.suspend	= wbsd_pnp_suspend,
	.resume		= wbsd_pnp_resume,
};

#endif /* CONFIG_PNP */

/*
 * Module loading/unloading
 */

static int __init wbsd_drv_init(void)
{
	int result;

	printk(KERN_INFO DRIVER_NAME
		": Winbond W83L51xD SD/MMC card interface driver, "
		DRIVER_VERSION "\n");
	printk(KERN_INFO DRIVER_NAME ": Copyright(c) Pierre Ossman\n");

#ifdef CONFIG_PNP

	if (!nopnp) {
		result = pnp_register_driver(&wbsd_pnp_driver);
		if (result < 0)
			return result;
	}
#endif /* CONFIG_PNP */

	if (nopnp) {
		result = platform_driver_register(&wbsd_driver);
		if (result < 0)
			return result;

		wbsd_device = platform_device_alloc(DRIVER_NAME, -1);
		if (!wbsd_device) {
			platform_driver_unregister(&wbsd_driver);
			return -ENOMEM;
		}

		result = platform_device_add(wbsd_device);
		if (result) {
			platform_device_put(wbsd_device);
			platform_driver_unregister(&wbsd_driver);
			return result;
		}
	}

	return 0;
}

static void __exit wbsd_drv_exit(void)
{
#ifdef CONFIG_PNP

	if (!nopnp)
		pnp_unregister_driver(&wbsd_pnp_driver);

#endif /* CONFIG_PNP */

	if (nopnp) {
		platform_device_unregister(wbsd_device);

		platform_driver_unregister(&wbsd_driver);
	}

	DBG("unloaded\n");
}

module_init(wbsd_drv_init);
module_exit(wbsd_drv_exit);
#ifdef CONFIG_PNP
module_param(nopnp, uint, 0444);
#endif
module_param(io, uint, 0444);
module_param(irq, uint, 0444);
module_param(dma, int, 0444);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pierre Ossman <drzeus@drzeus.cx>");
MODULE_DESCRIPTION("Winbond W83L51xD SD/MMC card interface driver");
MODULE_VERSION(DRIVER_VERSION);

#ifdef CONFIG_PNP
MODULE_PARM_DESC(nopnp, "Scan for device instead of relying on PNP. (default 0)");
#endif
MODULE_PARM_DESC(io, "I/O base to allocate. Must be 8 byte aligned. (default 0x248)");
MODULE_PARM_DESC(irq, "IRQ to allocate. (default 6)");
MODULE_PARM_DESC(dma, "DMA channel to allocate. -1 for no DMA. (default 2)");
