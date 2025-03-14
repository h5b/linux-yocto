/*
 * include/asm-arm/arch-ixp2000/ixp2000-regs.h
 *
 * Chipset register definitions for IXP2400/2800 based systems.
 *
 * Original Author: Naeem Afzal <naeem.m.afzal@intel.com>
 *
 * Maintainer: Deepak Saxena <dsaxena@plexity.net>
 *
 * Copyright (C) 2002 Intel Corp.
 * Copyright (C) 2003-2004 MontaVista Software, Inc.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */
#ifndef _IXP2000_REGS_H_
#define _IXP2000_REGS_H_

/*
 * IXP2000 linux memory map:
 *
 * virt		phys		size
 * fb000000	db000000	16M		PCI CFG1
 * fc000000	da000000	16M		PCI CFG0
 * fd000000	d8000000	16M		PCI I/O
 * fe[0-7]00000			8M		per-platform mappings
 * feb00000	c8000000	1M		MSF
 * fec00000	df000000	1M		PCI CSRs
 * fed00000	de000000	1M		PCI CREG
 * fee00000	d6000000	1M		INTCTL
 * fef00000	c0000000	1M		CAP
 */

/* 
 * Static I/O regions.
 *
 * Most of the registers are clumped in 4K regions spread throughout
 * the 0xc0000000 -> 0xc0100000 address range, but we just map in
 * the whole range using a single 1 MB section instead of small
 * 4K pages.  This has two advantages for us:
 *
 * 1) We use only one TLB entry for large number of on-chip I/O devices.
 *
 * 2) We can easily set the Section attributes to XCB=101 on the IXP2400
 *    as required per erratum #66.  We accomplish this by using a
 *    new MT_IXP2000_DEVICE memory type with the bits set as required.
 *
 * CAP stands for CSR Access Proxy.
 *
 * If you change the virtual address of this mapping, please propagate
 * the change to arch/arm/kernel/debug.S, which hardcodes the virtual
 * address of the UART located in this region.
 */

#define	IXP2000_CAP_PHYS_BASE		0xc0000000
#define	IXP2000_CAP_VIRT_BASE		0xfef00000
#define	IXP2000_CAP_SIZE		0x00100000

/*
 * Addresses for specific on-chip peripherals.
 */
#define	IXP2000_SLOWPORT_CSR_VIRT_BASE	0xfef80000
#define	IXP2000_GLOBAL_REG_VIRT_BASE	0xfef04000
#define	IXP2000_UART_PHYS_BASE		0xc0030000
#define	IXP2000_UART_VIRT_BASE		0xfef30000
#define	IXP2000_TIMER_VIRT_BASE		0xfef20000
#define	IXP2000_UENGINE_CSR_VIRT_BASE	0xfef18000
#define	IXP2000_GPIO_VIRT_BASE		0xfef10000

/*
 * Devices outside of the 0xc0000000 -> 0xc0100000 range.  The virtual
 * addresses of the INTCTL and PCI_CSR mappings are hardcoded in
 * entry-macro.S, so if you ever change these please propagate
 * the change.
 */
#define IXP2000_INTCTL_PHYS_BASE	0xd6000000
#define	IXP2000_INTCTL_VIRT_BASE	0xfee00000
#define	IXP2000_INTCTL_SIZE		0x00100000

#define IXP2000_PCI_CREG_PHYS_BASE	0xde000000
#define	IXP2000_PCI_CREG_VIRT_BASE	0xfed00000
#define	IXP2000_PCI_CREG_SIZE		0x00100000

#define IXP2000_PCI_CSR_PHYS_BASE	0xdf000000
#define	IXP2000_PCI_CSR_VIRT_BASE	0xfec00000
#define	IXP2000_PCI_CSR_SIZE		0x00100000

#define IXP2000_MSF_PHYS_BASE		0xc8000000
#define IXP2000_MSF_VIRT_BASE		0xfeb00000
#define IXP2000_MSF_SIZE		0x00100000

#define IXP2000_PCI_IO_PHYS_BASE	0xd8000000
#define	IXP2000_PCI_IO_VIRT_BASE	0xfd000000
#define IXP2000_PCI_IO_SIZE     	0x01000000

#define IXP2000_PCI_CFG0_PHYS_BASE	0xda000000
#define IXP2000_PCI_CFG0_VIRT_BASE	0xfc000000
#define IXP2000_PCI_CFG0_SIZE   	0x01000000

#define IXP2000_PCI_CFG1_PHYS_BASE	0xdb000000
#define IXP2000_PCI_CFG1_VIRT_BASE	0xfb000000
#define IXP2000_PCI_CFG1_SIZE		0x01000000

/* 
 * Timers
 */
#define	IXP2000_TIMER_REG(x)		((volatile unsigned long*)(IXP2000_TIMER_VIRT_BASE | (x)))
/* Timer control */
#define	IXP2000_T1_CTL			IXP2000_TIMER_REG(0x00)
#define	IXP2000_T2_CTL			IXP2000_TIMER_REG(0x04)
#define	IXP2000_T3_CTL			IXP2000_TIMER_REG(0x08)
#define	IXP2000_T4_CTL			IXP2000_TIMER_REG(0x0c)
/* Store initial value */
#define	IXP2000_T1_CLD			IXP2000_TIMER_REG(0x10)
#define	IXP2000_T2_CLD			IXP2000_TIMER_REG(0x14)
#define	IXP2000_T3_CLD			IXP2000_TIMER_REG(0x18)
#define	IXP2000_T4_CLD			IXP2000_TIMER_REG(0x1c)
/* Read current value */
#define	IXP2000_T1_CSR			IXP2000_TIMER_REG(0x20)
#define	IXP2000_T2_CSR			IXP2000_TIMER_REG(0x24)
#define	IXP2000_T3_CSR			IXP2000_TIMER_REG(0x28)
#define	IXP2000_T4_CSR			IXP2000_TIMER_REG(0x2c)
/* Clear associated timer interrupt */
#define	IXP2000_T1_CLR			IXP2000_TIMER_REG(0x30)
#define	IXP2000_T2_CLR			IXP2000_TIMER_REG(0x34)
#define	IXP2000_T3_CLR			IXP2000_TIMER_REG(0x38)
#define	IXP2000_T4_CLR			IXP2000_TIMER_REG(0x3c)
/* Timer watchdog enable for T4 */
#define	IXP2000_TWDE			IXP2000_TIMER_REG(0x40)

#define	WDT_ENABLE			0x00000001
#define	TIMER_DIVIDER_256		0x00000008
#define	TIMER_ENABLE			0x00000080
#define	IRQ_MASK_TIMER1         	(1 << 4)

/*
 * Interrupt controller registers
 */
#define IXP2000_INTCTL_REG(x)		(volatile unsigned long*)(IXP2000_INTCTL_VIRT_BASE | (x))
#define IXP2000_IRQ_STATUS		IXP2000_INTCTL_REG(0x08)
#define IXP2000_IRQ_ENABLE		IXP2000_INTCTL_REG(0x10)
#define IXP2000_IRQ_ENABLE_SET		IXP2000_INTCTL_REG(0x10)
#define IXP2000_IRQ_ENABLE_CLR		IXP2000_INTCTL_REG(0x18)
#define IXP2000_FIQ_ENABLE_CLR		IXP2000_INTCTL_REG(0x14)
#define IXP2000_IRQ_ERR_STATUS		IXP2000_INTCTL_REG(0x24)
#define IXP2000_IRQ_ERR_ENABLE_SET	IXP2000_INTCTL_REG(0x2c)
#define IXP2000_FIQ_ERR_ENABLE_CLR	IXP2000_INTCTL_REG(0x30)
#define IXP2000_IRQ_ERR_ENABLE_CLR	IXP2000_INTCTL_REG(0x34)
#define IXP2000_IRQ_THD_RAW_STATUS_A_0	IXP2000_INTCTL_REG(0x60)
#define IXP2000_IRQ_THD_RAW_STATUS_A_1	IXP2000_INTCTL_REG(0x64)
#define IXP2000_IRQ_THD_RAW_STATUS_A_2	IXP2000_INTCTL_REG(0x68)
#define IXP2000_IRQ_THD_RAW_STATUS_A_3	IXP2000_INTCTL_REG(0x6c)
#define IXP2000_IRQ_THD_RAW_STATUS_B_0	IXP2000_INTCTL_REG(0x80)
#define IXP2000_IRQ_THD_RAW_STATUS_B_1	IXP2000_INTCTL_REG(0x84)
#define IXP2000_IRQ_THD_RAW_STATUS_B_2	IXP2000_INTCTL_REG(0x88)
#define IXP2000_IRQ_THD_RAW_STATUS_B_3	IXP2000_INTCTL_REG(0x8c)
#define IXP2000_IRQ_THD_STATUS_A_0	IXP2000_INTCTL_REG(0xe0)
#define IXP2000_IRQ_THD_STATUS_A_1	IXP2000_INTCTL_REG(0xe4)
#define IXP2000_IRQ_THD_STATUS_A_2	IXP2000_INTCTL_REG(0xe8)
#define IXP2000_IRQ_THD_STATUS_A_3	IXP2000_INTCTL_REG(0xec)
#define IXP2000_IRQ_THD_STATUS_B_0	IXP2000_INTCTL_REG(0x100)
#define IXP2000_IRQ_THD_STATUS_B_1	IXP2000_INTCTL_REG(0x104)
#define IXP2000_IRQ_THD_STATUS_B_2	IXP2000_INTCTL_REG(0x108)
#define IXP2000_IRQ_THD_STATUS_B_3	IXP2000_INTCTL_REG(0x10c)
#define IXP2000_IRQ_THD_ENABLE_SET_A_0	IXP2000_INTCTL_REG(0x160)
#define IXP2000_IRQ_THD_ENABLE_SET_A_1	IXP2000_INTCTL_REG(0x164)
#define IXP2000_IRQ_THD_ENABLE_SET_A_2	IXP2000_INTCTL_REG(0x168)
#define IXP2000_IRQ_THD_ENABLE_SET_A_3	IXP2000_INTCTL_REG(0x16c)
#define IXP2000_IRQ_THD_ENABLE_SET_B_0	IXP2000_INTCTL_REG(0x180)
#define IXP2000_IRQ_THD_ENABLE_SET_B_1	IXP2000_INTCTL_REG(0x184)
#define IXP2000_IRQ_THD_ENABLE_SET_B_2	IXP2000_INTCTL_REG(0x188)
#define IXP2000_IRQ_THD_ENABLE_SET_B_3	IXP2000_INTCTL_REG(0x18c)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_A_0	IXP2000_INTCTL_REG(0x1e0)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_A_1	IXP2000_INTCTL_REG(0x1e4)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_A_2	IXP2000_INTCTL_REG(0x1e8)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_A_3	IXP2000_INTCTL_REG(0x1ec)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_B_0	IXP2000_INTCTL_REG(0x200)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_B_1	IXP2000_INTCTL_REG(0x204)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_B_2	IXP2000_INTCTL_REG(0x208)
#define IXP2000_IRQ_THD_ENABLE_CLEAR_B_3	IXP2000_INTCTL_REG(0x20c)

/*
 * Mask of valid IRQs in the 32-bit IRQ register. We use
 * this to mark certain IRQs as being invalid.
 */
#define	IXP2000_VALID_IRQ_MASK	0x0f0fffff

/*
 * PCI config register access from core
 */
#define IXP2000_PCI_CREG(x)		(volatile unsigned long*)(IXP2000_PCI_CREG_VIRT_BASE | (x))
#define IXP2000_PCI_CMDSTAT 		IXP2000_PCI_CREG(0x04)
#define IXP2000_PCI_CSR_BAR		IXP2000_PCI_CREG(0x10)
#define IXP2000_PCI_SRAM_BAR		IXP2000_PCI_CREG(0x14)
#define IXP2000_PCI_SDRAM_BAR		IXP2000_PCI_CREG(0x18)

/*
 * PCI CSRs
 */
#define IXP2000_PCI_CSR(x)		(volatile unsigned long*)(IXP2000_PCI_CSR_VIRT_BASE | (x))

/*
 * PCI outbound interrupts
 */
#define IXP2000_PCI_OUT_INT_STATUS	IXP2000_PCI_CSR(0x30)
#define IXP2000_PCI_OUT_INT_MASK	IXP2000_PCI_CSR(0x34)
/*
 * PCI communications
 */
#define IXP2000_PCI_MAILBOX0		IXP2000_PCI_CSR(0x50)
#define IXP2000_PCI_MAILBOX1		IXP2000_PCI_CSR(0x54)
#define IXP2000_PCI_MAILBOX2		IXP2000_PCI_CSR(0x58)
#define IXP2000_PCI_MAILBOX3		IXP2000_PCI_CSR(0x5C)
#define IXP2000_XSCALE_DOORBELL		IXP2000_PCI_CSR(0x60)
#define IXP2000_XSCALE_DOORBELL_SETUP	IXP2000_PCI_CSR(0x64)
#define IXP2000_PCI_DOORBELL		IXP2000_PCI_CSR(0x70)
#define IXP2000_PCI_DOORBELL_SETUP	IXP2000_PCI_CSR(0x74)

/*
 * DMA engines
 */
#define IXP2000_PCI_CH1_BYTE_CNT	IXP2000_PCI_CSR(0x80)
#define IXP2000_PCI_CH1_ADDR		IXP2000_PCI_CSR(0x84)
#define IXP2000_PCI_CH1_DRAM_ADDR	IXP2000_PCI_CSR(0x88)
#define IXP2000_PCI_CH1_DESC_PTR	IXP2000_PCI_CSR(0x8C)
#define IXP2000_PCI_CH1_CNTRL		IXP2000_PCI_CSR(0x90)
#define IXP2000_PCI_CH1_ME_PARAM	IXP2000_PCI_CSR(0x94)
#define IXP2000_PCI_CH2_BYTE_CNT	IXP2000_PCI_CSR(0xA0)
#define IXP2000_PCI_CH2_ADDR		IXP2000_PCI_CSR(0xA4)
#define IXP2000_PCI_CH2_DRAM_ADDR	IXP2000_PCI_CSR(0xA8)
#define IXP2000_PCI_CH2_DESC_PTR	IXP2000_PCI_CSR(0xAC)
#define IXP2000_PCI_CH2_CNTRL		IXP2000_PCI_CSR(0xB0)
#define IXP2000_PCI_CH2_ME_PARAM	IXP2000_PCI_CSR(0xB4)
#define IXP2000_PCI_CH3_BYTE_CNT	IXP2000_PCI_CSR(0xC0)
#define IXP2000_PCI_CH3_ADDR		IXP2000_PCI_CSR(0xC4)
#define IXP2000_PCI_CH3_DRAM_ADDR	IXP2000_PCI_CSR(0xC8)
#define IXP2000_PCI_CH3_DESC_PTR	IXP2000_PCI_CSR(0xCC)
#define IXP2000_PCI_CH3_CNTRL		IXP2000_PCI_CSR(0xD0)
#define IXP2000_PCI_CH3_ME_PARAM	IXP2000_PCI_CSR(0xD4)
#define IXP2000_DMA_INF_MODE		IXP2000_PCI_CSR(0xE0)
/*
 * Size masks for BARs
 */
#define IXP2000_PCI_SRAM_BASE_ADDR_MASK	IXP2000_PCI_CSR(0xFC)
#define IXP2000_PCI_DRAM_BASE_ADDR_MASK	IXP2000_PCI_CSR(0x100)
/*
 * Control and uEngine related
 */
#define IXP2000_PCI_CONTROL		IXP2000_PCI_CSR(0x13C)
#define IXP2000_PCI_ADDR_EXT		IXP2000_PCI_CSR(0x140)
#define IXP2000_PCI_ME_PUSH_STATUS	IXP2000_PCI_CSR(0x148)
#define IXP2000_PCI_ME_PUSH_EN		IXP2000_PCI_CSR(0x14C)
#define IXP2000_PCI_ERR_STATUS		IXP2000_PCI_CSR(0x150)
#define IXP2000_PCI_ERR_ENABLE		IXP2000_PCI_CSR(0x154)
/*
 * Inbound PCI interrupt control
 */
#define IXP2000_PCI_XSCALE_INT_STATUS	IXP2000_PCI_CSR(0x158)
#define IXP2000_PCI_XSCALE_INT_ENABLE	IXP2000_PCI_CSR(0x15C)

#define IXP2000_PCICNTL_PNR		(1<<17)	/* PCI not Reset bit of PCI_CONTROL */
#define IXP2000_PCICNTL_PCF		(1<<28)	/* PCI Central function bit */
#define IXP2000_XSCALE_INT		(1<<1)	/* Interrupt from XScale to PCI */

/* These are from the IRQ register in the PCI ISR register */
#define PCI_CONTROL_BE_DEO		(1 << 22)	/* Big Endian Data Enable Out */
#define PCI_CONTROL_BE_DEI		(1 << 21)	/* Big Endian Data Enable In  */
#define PCI_CONTROL_BE_BEO		(1 << 20)	/* Big Endian Byte Enable Out */
#define PCI_CONTROL_BE_BEI		(1 << 19)	/* Big Endian Byte Enable In  */
#define PCI_CONTROL_IEE			(1 << 17)	/* I/O cycle Endian swap Enable */

#define IXP2000_PCI_RST_REL		(1 << 2)
#define CFG_RST_DIR			(*IXP2000_PCI_CONTROL & IXP2000_PCICNTL_PCF)
#define CFG_PCI_BOOT_HOST		(1 << 2)
#define CFG_BOOT_PROM			(1 << 1)

/*
 * SlowPort CSRs
 *
 * The slowport is used to access things like flash, SONET framer control
 * ports, slave microprocessors, CPLDs, and others of chip memory mapped
 * peripherals.
 */
#define	SLOWPORT_CSR(x)		(volatile unsigned long*)(IXP2000_SLOWPORT_CSR_VIRT_BASE | (x))

#define	IXP2000_SLOWPORT_CCR		SLOWPORT_CSR(0x00)
#define	IXP2000_SLOWPORT_WTC1		SLOWPORT_CSR(0x04)
#define	IXP2000_SLOWPORT_WTC2		SLOWPORT_CSR(0x08)
#define	IXP2000_SLOWPORT_RTC1		SLOWPORT_CSR(0x0c)
#define	IXP2000_SLOWPORT_RTC2		SLOWPORT_CSR(0x10)
#define	IXP2000_SLOWPORT_FSR		SLOWPORT_CSR(0x14)
#define	IXP2000_SLOWPORT_PCR		SLOWPORT_CSR(0x18)
#define	IXP2000_SLOWPORT_ADC		SLOWPORT_CSR(0x1C)
#define	IXP2000_SLOWPORT_FAC		SLOWPORT_CSR(0x20)
#define	IXP2000_SLOWPORT_FRM		SLOWPORT_CSR(0x24)
#define	IXP2000_SLOWPORT_FIN		SLOWPORT_CSR(0x28)

/*
 * CCR values.  
 * The CCR configures the clock division for the slowport interface.
 */
#define	SLOWPORT_CCR_DIV_1		0x00
#define	SLOWPORT_CCR_DIV_2		0x01
#define	SLOWPORT_CCR_DIV_4		0x02
#define	SLOWPORT_CCR_DIV_6		0x03
#define	SLOWPORT_CCR_DIV_8		0x04
#define	SLOWPORT_CCR_DIV_10		0x05
#define	SLOWPORT_CCR_DIV_12		0x06
#define	SLOWPORT_CCR_DIV_14		0x07
#define	SLOWPORT_CCR_DIV_16		0x08
#define	SLOWPORT_CCR_DIV_18		0x09
#define	SLOWPORT_CCR_DIV_20		0x0a
#define	SLOWPORT_CCR_DIV_22		0x0b
#define	SLOWPORT_CCR_DIV_24		0x0c
#define	SLOWPORT_CCR_DIV_26		0x0d
#define	SLOWPORT_CCR_DIV_28		0x0e
#define	SLOWPORT_CCR_DIV_30		0x0f

/*
 * PCR values.  PCR configure the mode of the interface.
 */
#define	SLOWPORT_MODE_FLASH		0x00
#define	SLOWPORT_MODE_LUCENT		0x01
#define	SLOWPORT_MODE_PMC_SIERRA	0x02
#define	SLOWPORT_MODE_INTEL_UP		0x03
#define	SLOWPORT_MODE_MOTOROLA_UP	0x04

/*
 * ADC values.  Defines data and address bus widths.
 */
#define	SLOWPORT_ADDR_WIDTH_8		0x00
#define	SLOWPORT_ADDR_WIDTH_16		0x01
#define	SLOWPORT_ADDR_WIDTH_24		0x02
#define	SLOWPORT_ADDR_WIDTH_32		0x03
#define	SLOWPORT_DATA_WIDTH_8		0x00
#define	SLOWPORT_DATA_WIDTH_16		0x10
#define	SLOWPORT_DATA_WIDTH_24		0x20
#define	SLOWPORT_DATA_WIDTH_32		0x30

/*
 * Masks and shifts for various fields in the WTC and RTC registers.
 */
#define	SLOWPORT_WRTC_MASK_HD		0x0003
#define	SLOWPORT_WRTC_MASK_SU		0x003c
#define	SLOWPORT_WRTC_MASK_PW		0x03c0

#define	SLOWPORT_WRTC_SHIFT_HD		0x00
#define	SLOWPORT_WRTC_SHIFT_SU		0x02
#define	SLOWPORT_WRTC_SHFIT_PW		0x06


/*
 * GPIO registers & GPIO interface.
 */
#define IXP2000_GPIO_REG(x)		((volatile unsigned long*)(IXP2000_GPIO_VIRT_BASE+(x)))
#define IXP2000_GPIO_PLR		IXP2000_GPIO_REG(0x00)
#define IXP2000_GPIO_PDPR		IXP2000_GPIO_REG(0x04)
#define IXP2000_GPIO_PDSR		IXP2000_GPIO_REG(0x08)
#define IXP2000_GPIO_PDCR		IXP2000_GPIO_REG(0x0c)
#define IXP2000_GPIO_POPR		IXP2000_GPIO_REG(0x10)
#define IXP2000_GPIO_POSR		IXP2000_GPIO_REG(0x14)
#define IXP2000_GPIO_POCR		IXP2000_GPIO_REG(0x18)
#define IXP2000_GPIO_REDR		IXP2000_GPIO_REG(0x1c)
#define IXP2000_GPIO_FEDR		IXP2000_GPIO_REG(0x20)
#define IXP2000_GPIO_EDSR		IXP2000_GPIO_REG(0x24)
#define IXP2000_GPIO_LSHR		IXP2000_GPIO_REG(0x28)
#define IXP2000_GPIO_LSLR		IXP2000_GPIO_REG(0x2c)
#define IXP2000_GPIO_LDSR		IXP2000_GPIO_REG(0x30)
#define IXP2000_GPIO_INER		IXP2000_GPIO_REG(0x34)
#define IXP2000_GPIO_INSR		IXP2000_GPIO_REG(0x38)
#define IXP2000_GPIO_INCR		IXP2000_GPIO_REG(0x3c)
#define IXP2000_GPIO_INST		IXP2000_GPIO_REG(0x40)

/*
 * "Global" registers...whatever that's supposed to mean.
 */
#define GLOBAL_REG_BASE			(IXP2000_GLOBAL_REG_VIRT_BASE + 0x0a00)
#define GLOBAL_REG(x)			(volatile unsigned long*)(GLOBAL_REG_BASE | (x))

#define IXP2000_MAJ_PROD_TYPE_MASK	0x001F0000
#define IXP2000_MAJ_PROD_TYPE_IXP2000	0x00000000
#define IXP2000_MIN_PROD_TYPE_MASK 	0x0000FF00
#define IXP2000_MIN_PROD_TYPE_IXP2400	0x00000200
#define IXP2000_MIN_PROD_TYPE_IXP2850	0x00000100
#define IXP2000_MIN_PROD_TYPE_IXP2800	0x00000000
#define IXP2000_MAJ_REV_MASK	      	0x000000F0
#define IXP2000_MIN_REV_MASK	      	0x0000000F
#define IXP2000_PROD_ID_MASK		0xFFFFFFFF

#define IXP2000_PRODUCT_ID		GLOBAL_REG(0x00)
#define IXP2000_MISC_CONTROL		GLOBAL_REG(0x04)
#define IXP2000_MSF_CLK_CNTRL  		GLOBAL_REG(0x08)
#define IXP2000_RESET0      		GLOBAL_REG(0x0c)
#define IXP2000_RESET1      		GLOBAL_REG(0x10)
#define IXP2000_CCR            		GLOBAL_REG(0x14)
#define	IXP2000_STRAP_OPTIONS  		GLOBAL_REG(0x18)

#define	RSTALL				(1 << 16)
#define	WDT_RESET_ENABLE		0x01000000


/*
 * MSF registers.  The IXP2400 and IXP2800 have somewhat different MSF
 * units, but the registers that differ between the two don't overlap,
 * so we can have one register list for both.
 */
#define IXP2000_MSF_REG(x)			((volatile unsigned long*)(IXP2000_MSF_VIRT_BASE + (x)))
#define IXP2000_MSF_RX_CONTROL			IXP2000_MSF_REG(0x0000)
#define IXP2000_MSF_TX_CONTROL			IXP2000_MSF_REG(0x0004)
#define IXP2000_MSF_INTERRUPT_STATUS		IXP2000_MSF_REG(0x0008)
#define IXP2000_MSF_INTERRUPT_ENABLE		IXP2000_MSF_REG(0x000c)
#define IXP2000_MSF_CSIX_TYPE_MAP		IXP2000_MSF_REG(0x0010)
#define IXP2000_MSF_FC_EGRESS_STATUS		IXP2000_MSF_REG(0x0014)
#define IXP2000_MSF_FC_INGRESS_STATUS		IXP2000_MSF_REG(0x0018)
#define IXP2000_MSF_HWM_CONTROL			IXP2000_MSF_REG(0x0024)
#define IXP2000_MSF_FC_STATUS_OVERRIDE		IXP2000_MSF_REG(0x0028)
#define IXP2000_MSF_CLOCK_CONTROL		IXP2000_MSF_REG(0x002c)
#define IXP2000_MSF_RX_PORT_MAP			IXP2000_MSF_REG(0x0040)
#define IXP2000_MSF_RBUF_ELEMENT_DONE		IXP2000_MSF_REG(0x0044)
#define IXP2000_MSF_RX_MPHY_POLL_LIMIT		IXP2000_MSF_REG(0x0048)
#define IXP2000_MSF_RX_CALENDAR_LENGTH		IXP2000_MSF_REG(0x0048)
#define IXP2000_MSF_RX_THREAD_FREELIST_TIMEOUT_0	IXP2000_MSF_REG(0x0050)
#define IXP2000_MSF_RX_THREAD_FREELIST_TIMEOUT_1	IXP2000_MSF_REG(0x0054)
#define IXP2000_MSF_RX_THREAD_FREELIST_TIMEOUT_2	IXP2000_MSF_REG(0x0058)
#define IXP2000_MSF_TX_SEQUENCE_0		IXP2000_MSF_REG(0x0060)
#define IXP2000_MSF_TX_SEQUENCE_1		IXP2000_MSF_REG(0x0064)
#define IXP2000_MSF_TX_SEQUENCE_2		IXP2000_MSF_REG(0x0068)
#define IXP2000_MSF_TX_MPHY_POLL_LIMIT		IXP2000_MSF_REG(0x0070)
#define IXP2000_MSF_TX_CALENDAR_LENGTH		IXP2000_MSF_REG(0x0070)
#define IXP2000_MSF_RX_UP_CONTROL_0		IXP2000_MSF_REG(0x0080)
#define IXP2000_MSF_RX_UP_CONTROL_1		IXP2000_MSF_REG(0x0084)
#define IXP2000_MSF_RX_UP_CONTROL_2		IXP2000_MSF_REG(0x0088)
#define IXP2000_MSF_RX_UP_CONTROL_3		IXP2000_MSF_REG(0x008c)
#define IXP2000_MSF_TX_UP_CONTROL_0		IXP2000_MSF_REG(0x0090)
#define IXP2000_MSF_TX_UP_CONTROL_1		IXP2000_MSF_REG(0x0094)
#define IXP2000_MSF_TX_UP_CONTROL_2		IXP2000_MSF_REG(0x0098)
#define IXP2000_MSF_TX_UP_CONTROL_3		IXP2000_MSF_REG(0x009c)
#define IXP2000_MSF_TRAIN_DATA			IXP2000_MSF_REG(0x00a0)
#define IXP2000_MSF_TRAIN_CALENDAR		IXP2000_MSF_REG(0x00a4)
#define IXP2000_MSF_TRAIN_FLOW_CONTROL		IXP2000_MSF_REG(0x00a8)
#define IXP2000_MSF_TX_CALENDAR_0		IXP2000_MSF_REG(0x1000)
#define IXP2000_MSF_RX_PORT_CALENDAR_STATUS	IXP2000_MSF_REG(0x1400)


#endif				/* _IXP2000_H_ */
