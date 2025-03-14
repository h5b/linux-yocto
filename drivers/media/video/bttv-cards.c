/*

    bttv-cards.c

    this file has configuration informations - card-specific stuff
    like the big tvcards array for the most part

    Copyright (C) 1996,97,98 Ralph  Metzler (rjkm@thp.uni-koeln.de)
			   & Marcus Metzler (mocm@thp.uni-koeln.de)
    (c) 1999-2001 Gerd Knorr <kraxel@goldbach.in-berlin.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <linux/config.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>

#include <asm/io.h>

#include "bttvp.h"
#include <media/v4l2-common.h>

/* fwd decl */
static void boot_msp34xx(struct bttv *btv, int pin);
static void boot_bt832(struct bttv *btv);
static void hauppauge_eeprom(struct bttv *btv);
static void avermedia_eeprom(struct bttv *btv);
static void osprey_eeprom(struct bttv *btv);
static void modtec_eeprom(struct bttv *btv);
static void init_PXC200(struct bttv *btv);
static void init_RTV24(struct bttv *btv);

static void winview_audio(struct bttv *btv, struct video_audio *v, int set);
static void lt9415_audio(struct bttv *btv, struct video_audio *v, int set);
static void avermedia_tvphone_audio(struct bttv *btv, struct video_audio *v,
				    int set);
static void avermedia_tv_stereo_audio(struct bttv *btv, struct video_audio *v,
				      int set);
static void terratv_audio(struct bttv *btv, struct video_audio *v, int set);
static void gvbctv3pci_audio(struct bttv *btv, struct video_audio *v, int set);
static void gvbctv5pci_audio(struct bttv *btv, struct video_audio *v, int set);
static void winfast2000_audio(struct bttv *btv, struct video_audio *v, int set);
static void pvbt878p9b_audio(struct bttv *btv, struct video_audio *v, int set);
static void fv2000s_audio(struct bttv *btv, struct video_audio *v, int set);
static void windvr_audio(struct bttv *btv, struct video_audio *v, int set);
static void adtvk503_audio(struct bttv *btv, struct video_audio *v, int set);
static void rv605_muxsel(struct bttv *btv, unsigned int input);
static void eagle_muxsel(struct bttv *btv, unsigned int input);
static void xguard_muxsel(struct bttv *btv, unsigned int input);
static void ivc120_muxsel(struct bttv *btv, unsigned int input);
static void gvc1100_muxsel(struct bttv *btv, unsigned int input);

static void PXC200_muxsel(struct bttv *btv, unsigned int input);

static void picolo_tetra_muxsel(struct bttv *btv, unsigned int input);
static void picolo_tetra_init(struct bttv *btv);

static void tibetCS16_muxsel(struct bttv *btv, unsigned int input);
static void tibetCS16_init(struct bttv *btv);

static void kodicom4400r_muxsel(struct bttv *btv, unsigned int input);
static void kodicom4400r_init(struct bttv *btv);

static void sigmaSLC_muxsel(struct bttv *btv, unsigned int input);
static void sigmaSQ_muxsel(struct bttv *btv, unsigned int input);

static int terratec_active_radio_upgrade(struct bttv *btv);
static int tea5757_read(struct bttv *btv);
static int tea5757_write(struct bttv *btv, int value);
static void identify_by_eeprom(struct bttv *btv,
			       unsigned char eeprom_data[256]);
static int __devinit pvr_boot(struct bttv *btv);

/* config variables */
static unsigned int triton1=0;
static unsigned int vsfx=0;
static unsigned int latency = UNSET;
int no_overlay=-1;

static unsigned int card[BTTV_MAX]   = { [ 0 ... (BTTV_MAX-1) ] = UNSET };
static unsigned int pll[BTTV_MAX]    = { [ 0 ... (BTTV_MAX-1) ] = UNSET };
static unsigned int tuner[BTTV_MAX]  = { [ 0 ... (BTTV_MAX-1) ] = UNSET };
static unsigned int svhs[BTTV_MAX]   = { [ 0 ... (BTTV_MAX-1) ] = UNSET };
static unsigned int remote[BTTV_MAX] = { [ 0 ... (BTTV_MAX-1) ] = UNSET };
static struct bttv  *master[BTTV_MAX] = { [ 0 ... (BTTV_MAX-1) ] = NULL };
#ifdef MODULE
static unsigned int autoload = 1;
#else
static unsigned int autoload = 0;
#endif
static unsigned int gpiomask = UNSET;
static unsigned int audioall = UNSET;
static unsigned int audiomux[5] = { [ 0 ... 4 ] = UNSET };

/* insmod options */
module_param(triton1,    int, 0444);
module_param(vsfx,       int, 0444);
module_param(no_overlay, int, 0444);
module_param(latency,    int, 0444);
module_param(gpiomask,   int, 0444);
module_param(audioall,   int, 0444);
module_param(autoload,   int, 0444);

module_param_array(card,     int, NULL, 0444);
module_param_array(pll,      int, NULL, 0444);
module_param_array(tuner,    int, NULL, 0444);
module_param_array(svhs,     int, NULL, 0444);
module_param_array(remote,   int, NULL, 0444);
module_param_array(audiomux, int, NULL, 0444);

MODULE_PARM_DESC(triton1,"set ETBF pci config bit "
		 "[enable bug compatibility for triton1 + others]");
MODULE_PARM_DESC(vsfx,"set VSFX pci config bit "
		 "[yet another chipset flaw workaround]");
MODULE_PARM_DESC(latency,"pci latency timer");
MODULE_PARM_DESC(card,"specify TV/grabber card model, see CARDLIST file for a list");
MODULE_PARM_DESC(pll,"specify installed crystal (0=none, 28=28 MHz, 35=35 MHz)");
MODULE_PARM_DESC(tuner,"specify installed tuner type");
MODULE_PARM_DESC(autoload,"automatically load i2c modules like tuner.o, default is 1 (yes)");

/* ----------------------------------------------------------------------- */
/* list of card IDs for bt878+ cards                                       */

static struct CARD {
	unsigned id;
	int cardnr;
	char *name;
} cards[] __devinitdata = {
	{ 0x13eb0070, BTTV_BOARD_HAUPPAUGE878,  "Hauppauge WinTV" },
	{ 0x39000070, BTTV_BOARD_HAUPPAUGE878,  "Hauppauge WinTV-D" },
	{ 0x45000070, BTTV_BOARD_HAUPPAUGEPVR,  "Hauppauge WinTV/PVR" },
	{ 0xff000070, BTTV_BOARD_OSPREY1x0,     "Osprey-100" },
	{ 0xff010070, BTTV_BOARD_OSPREY2x0_SVID,"Osprey-200" },
	{ 0xff020070, BTTV_BOARD_OSPREY500,     "Osprey-500" },
	{ 0xff030070, BTTV_BOARD_OSPREY2000,    "Osprey-2000" },
	{ 0xff040070, BTTV_BOARD_OSPREY540,     "Osprey-540" },
	{ 0xff070070, BTTV_BOARD_OSPREY440,     "Osprey-440" },

	{ 0x00011002, BTTV_BOARD_ATI_TVWONDER,  "ATI TV Wonder" },
	{ 0x00031002, BTTV_BOARD_ATI_TVWONDERVE,"ATI TV Wonder/VE" },

	{ 0x6606107d, BTTV_BOARD_WINFAST2000,   "Leadtek WinFast TV 2000" },
	{ 0x6607107d, BTTV_BOARD_WINFASTVC100,  "Leadtek WinFast VC 100" },
	{ 0x6609107d, BTTV_BOARD_WINFAST2000,   "Leadtek TV 2000 XP" },
	{ 0x263610b4, BTTV_BOARD_STB2,          "STB TV PCI FM, Gateway P/N 6000704" },
	{ 0x264510b4, BTTV_BOARD_STB2,          "STB TV PCI FM, Gateway P/N 6000704" },
	{ 0x402010fc, BTTV_BOARD_GVBCTV3PCI,    "I-O Data Co. GV-BCTV3/PCI" },
	{ 0x405010fc, BTTV_BOARD_GVBCTV4PCI,    "I-O Data Co. GV-BCTV4/PCI" },
	{ 0x407010fc, BTTV_BOARD_GVBCTV5PCI,    "I-O Data Co. GV-BCTV5/PCI" },
	{ 0xd01810fc, BTTV_BOARD_GVBCTV5PCI,    "I-O Data Co. GV-BCTV5/PCI" },

	{ 0x001211bd, BTTV_BOARD_PINNACLE,      "Pinnacle PCTV" },
	/* some cards ship with byteswapped IDs ... */
	{ 0x1200bd11, BTTV_BOARD_PINNACLE,      "Pinnacle PCTV [bswap]" },
	{ 0xff00bd11, BTTV_BOARD_PINNACLE,      "Pinnacle PCTV [bswap]" },
	/* this seems to happen as well ... */
	{ 0xff1211bd, BTTV_BOARD_PINNACLE,      "Pinnacle PCTV" },

	{ 0x3000121a, BTTV_BOARD_VOODOOTV_FM,   "3Dfx VoodooTV FM/ VoodooTV 200" },
	{ 0x263710b4, BTTV_BOARD_VOODOOTV_FM,   "3Dfx VoodooTV FM/ VoodooTV 200" },
	{ 0x3060121a, BTTV_BOARD_STB2,	  "3Dfx VoodooTV 100/ STB OEM" },

	{ 0x3000144f, BTTV_BOARD_MAGICTVIEW063, "(Askey Magic/others) TView99 CPH06x" },
	{ 0xa005144f, BTTV_BOARD_MAGICTVIEW063, "CPH06X TView99-Card" },
	{ 0x3002144f, BTTV_BOARD_MAGICTVIEW061, "(Askey Magic/others) TView99 CPH05x" },
	{ 0x3005144f, BTTV_BOARD_MAGICTVIEW061, "(Askey Magic/others) TView99 CPH061/06L (T1/LC)" },
	{ 0x5000144f, BTTV_BOARD_MAGICTVIEW061, "Askey CPH050" },
	{ 0x300014ff, BTTV_BOARD_MAGICTVIEW061, "TView 99 (CPH061)" },
	{ 0x300214ff, BTTV_BOARD_PHOEBE_TVMAS,  "Phoebe TV Master (CPH060)" },

	{ 0x00011461, BTTV_BOARD_AVPHONE98,     "AVerMedia TVPhone98" },
	{ 0x00021461, BTTV_BOARD_AVERMEDIA98,   "AVermedia TVCapture 98" },
	{ 0x00031461, BTTV_BOARD_AVPHONE98,     "AVerMedia TVPhone98" },
	{ 0x00041461, BTTV_BOARD_AVERMEDIA98,   "AVerMedia TVCapture 98" },
	{ 0x03001461, BTTV_BOARD_AVERMEDIA98,   "VDOMATE TV TUNER CARD" },

	{ 0x1117153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue (Philips PAL B/G)" },
	{ 0x1118153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue (Temic PAL B/G)" },
	{ 0x1119153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue (Philips PAL I)" },
	{ 0x111a153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue (Temic PAL I)" },

	{ 0x1123153b, BTTV_BOARD_TERRATVRADIO,  "Terratec TV Radio+" },
	{ 0x1127153b, BTTV_BOARD_TERRATV,       "Terratec TV+ (V1.05)"    },
	/* clashes with FlyVideo
	 *{ 0x18521852, BTTV_BOARD_TERRATV,     "Terratec TV+ (V1.10)"    }, */
	{ 0x1134153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue (LR102)" },
	{ 0x1135153b, BTTV_BOARD_TERRATVALUER,  "Terratec TValue Radio" }, /* LR102 */
	{ 0x5018153b, BTTV_BOARD_TERRATVALUE,   "Terratec TValue" },       /* ?? */
	{ 0xff3b153b, BTTV_BOARD_TERRATVALUER,  "Terratec TValue Radio" }, /* ?? */

	{ 0x400015b0, BTTV_BOARD_ZOLTRIX_GENIE, "Zoltrix Genie TV" },
	{ 0x400a15b0, BTTV_BOARD_ZOLTRIX_GENIE, "Zoltrix Genie TV" },
	{ 0x400d15b0, BTTV_BOARD_ZOLTRIX_GENIE, "Zoltrix Genie TV / Radio" },
	{ 0x401015b0, BTTV_BOARD_ZOLTRIX_GENIE, "Zoltrix Genie TV / Radio" },
	{ 0x401615b0, BTTV_BOARD_ZOLTRIX_GENIE, "Zoltrix Genie TV / Radio" },

	{ 0x1430aa00, BTTV_BOARD_PV143,         "Provideo PV143A" },
	{ 0x1431aa00, BTTV_BOARD_PV143,         "Provideo PV143B" },
	{ 0x1432aa00, BTTV_BOARD_PV143,         "Provideo PV143C" },
	{ 0x1433aa00, BTTV_BOARD_PV143,         "Provideo PV143D" },
	{ 0x1433aa03, BTTV_BOARD_PV143,         "Security Eyes" },

	{ 0x1460aa00, BTTV_BOARD_PV150,         "Provideo PV150A-1" },
	{ 0x1461aa01, BTTV_BOARD_PV150,         "Provideo PV150A-2" },
	{ 0x1462aa02, BTTV_BOARD_PV150,         "Provideo PV150A-3" },
	{ 0x1463aa03, BTTV_BOARD_PV150,         "Provideo PV150A-4" },

	{ 0x1464aa04, BTTV_BOARD_PV150,         "Provideo PV150B-1" },
	{ 0x1465aa05, BTTV_BOARD_PV150,         "Provideo PV150B-2" },
	{ 0x1466aa06, BTTV_BOARD_PV150,         "Provideo PV150B-3" },
	{ 0x1467aa07, BTTV_BOARD_PV150,         "Provideo PV150B-4" },

	{ 0xa132ff00, BTTV_BOARD_IVC100,        "IVC-100"  },
	{ 0xa1550000, BTTV_BOARD_IVC200,        "IVC-200"  },
	{ 0xa1550001, BTTV_BOARD_IVC200,        "IVC-200"  },
	{ 0xa1550002, BTTV_BOARD_IVC200,        "IVC-200"  },
	{ 0xa1550003, BTTV_BOARD_IVC200,        "IVC-200"  },
	{ 0xa1550100, BTTV_BOARD_IVC200,        "IVC-200G" },
	{ 0xa1550101, BTTV_BOARD_IVC200,        "IVC-200G" },
	{ 0xa1550102, BTTV_BOARD_IVC200,        "IVC-200G" },
	{ 0xa1550103, BTTV_BOARD_IVC200,        "IVC-200G" },
	{ 0xa182ff00, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff01, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff02, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff03, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff04, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff05, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff06, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff07, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff08, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff09, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0a, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0b, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0c, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0d, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0e, BTTV_BOARD_IVC120,        "IVC-120G" },
	{ 0xa182ff0f, BTTV_BOARD_IVC120,        "IVC-120G" },

	{ 0x41424344, BTTV_BOARD_GRANDTEC,      "GrandTec Multi Capture" },
	{ 0x01020304, BTTV_BOARD_XGUARD,        "Grandtec Grand X-Guard" },

	{ 0x18501851, BTTV_BOARD_CHRONOS_VS2,   "FlyVideo 98 (LR50)/ Chronos Video Shuttle II" },
	{ 0xa0501851, BTTV_BOARD_CHRONOS_VS2,   "FlyVideo 98 (LR50)/ Chronos Video Shuttle II" },
	{ 0x18511851, BTTV_BOARD_FLYVIDEO98EZ,  "FlyVideo 98EZ (LR51)/ CyberMail AV" },
	{ 0x18521852, BTTV_BOARD_TYPHOON_TVIEW, "FlyVideo 98FM (LR50)/ Typhoon TView TV/FM Tuner" },
	{ 0x41a0a051, BTTV_BOARD_FLYVIDEO_98FM, "Lifeview FlyVideo 98 LR50 Rev Q" },
	{ 0x18501f7f, BTTV_BOARD_FLYVIDEO_98,   "Lifeview Flyvideo 98" },

    	{ 0x010115cb, BTTV_BOARD_GMV1,          "AG GMV1" },
	{ 0x010114c7, BTTV_BOARD_MODTEC_205,    "Modular Technology MM201/MM202/MM205/MM210/MM215 PCTV" },

	{ 0x10b42636, BTTV_BOARD_HAUPPAUGE878,  "STB ???" },
	{ 0x217d6606, BTTV_BOARD_WINFAST2000,   "Leadtek WinFast TV 2000" },
	{ 0xfff6f6ff, BTTV_BOARD_WINFAST2000,   "Leadtek WinFast TV 2000" },
	{ 0x03116000, BTTV_BOARD_SENSORAY311,   "Sensoray 311" },
	{ 0x00790e11, BTTV_BOARD_WINDVR,        "Canopus WinDVR PCI" },
	{ 0xa0fca1a0, BTTV_BOARD_ZOLTRIX,       "Face to Face Tvmax" },
	{ 0x20007063, BTTV_BOARD_PC_HDTV,       "pcHDTV HD-2000 TV"},
	{ 0x82b2aa6a, BTTV_BOARD_SIMUS_GVC1100, "SIMUS GVC1100" },
	{ 0x146caa0c, BTTV_BOARD_PV951,         "ituner spectra8" },
	{ 0x200a1295, BTTV_BOARD_PXC200,        "ImageNation PXC200A" },

	{ 0x40111554, BTTV_BOARD_PV_BT878P_9B,  "Prolink Pixelview PV-BT" },
	{ 0x17de0a01, BTTV_BOARD_KWORLD,        "Mecer TV/FM/Video Tuner" },

	{ 0x01051805, BTTV_BOARD_PICOLO_TETRA_CHIP, "Picolo Tetra Chip #1" },
	{ 0x01061805, BTTV_BOARD_PICOLO_TETRA_CHIP, "Picolo Tetra Chip #2" },
	{ 0x01071805, BTTV_BOARD_PICOLO_TETRA_CHIP, "Picolo Tetra Chip #3" },
	{ 0x01081805, BTTV_BOARD_PICOLO_TETRA_CHIP, "Picolo Tetra Chip #4" },

	{ 0x15409511, BTTV_BOARD_ACORP_Y878F, "Acorp Y878F" },

	/* likely broken, vendor id doesn't match the other magic views ...
	 * { 0xa0fca04f, BTTV_BOARD_MAGICTVIEW063, "Guillemot Maxi TV Video 3" }, */

	/* Duplicate PCI ID, reconfigure for this board during the eeprom read.
	* { 0x13eb0070, BTTV_BOARD_HAUPPAUGE_IMPACTVCB,  "Hauppauge ImpactVCB" }, */

	/* DVB cards (using pci function .1 for mpeg data xfer) */
	{ 0x01010071, BTTV_BOARD_NEBULA_DIGITV, "Nebula Electronics DigiTV" },
	{ 0x07611461, BTTV_BOARD_AVDVBT_761,    "AverMedia AverTV DVB-T 761" },
	{ 0x001c11bd, BTTV_BOARD_PINNACLESAT,   "Pinnacle PCTV Sat" },
	{ 0x002611bd, BTTV_BOARD_TWINHAN_DST,   "Pinnacle PCTV SAT CI" },
	{ 0x00011822, BTTV_BOARD_TWINHAN_DST,   "Twinhan VisionPlus DVB" },
	{ 0xfc00270f, BTTV_BOARD_TWINHAN_DST,   "ChainTech digitop DST-1000 DVB-S" },
	{ 0x07711461, BTTV_BOARD_AVDVBT_771,    "AVermedia AverTV DVB-T 771" },
	{ 0xdb1018ac, BTTV_BOARD_DVICO_DVBT_LITE,    "DViCO FusionHDTV DVB-T Lite" },
	{ 0xd50018ac, BTTV_BOARD_DVICO_FUSIONHDTV_5_LITE,    "DViCO FusionHDTV 5 Lite" },

	{ 0, -1, NULL }
};

/* ----------------------------------------------------------------------- */
/* array with description for bt848 / bt878 tv/grabber cards               */

struct tvcard bttv_tvcards[] = {
	/* ---- card 0x00 ---------------------------------- */
	[BTTV_BOARD_UNKNOWN] = {
		.name		= " *** UNKNOWN/GENERIC *** ",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.muxsel		= { 2, 3, 1, 0},
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MIRO] = {
		.name		= "MIRO PCTV",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 2, 0, 0, 0, 10},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_HAUPPAUGE] = {
		.name		= "Hauppauge (bt848)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 7,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 1, 2, 3, 4},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_STB] = {
		.name		= "STB, Gateway P/N 6000699 (bt848)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 7,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 4, 0, 2, 3, 1},
		.no_msp34xx	= 1,
		.needs_tvaudio	= 1,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
		.has_radio      = 1,
	},

	/* ---- card 0x04 ---------------------------------- */
	[BTTV_BOARD_INTEL] = {
		.name		= "Intel Create and Share PCI/ Smart Video Recorder III",
		.video_inputs	= 4,
		.audio_inputs	= 0,
		.tuner		= -1,
		.svhs		= 2,
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0 },
		.needs_tvaudio	= 0,
		.tuner_type	= 4,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_DIAMOND] = {
		.name		= "Diamond DTV2000",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 3,
		.muxsel		= { 2, 3, 1, 0},
		.audiomux	= { 0, 1, 0, 1, 3},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_AVERMEDIA] = {
		.name		= "AVerMedia TVPhone",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 3,
		.muxsel		= { 2, 3, 1, 1},
		.gpiomask	= 0x0f,
		.audiomux	= { 0x0c, 0x04, 0x08, 0x04, 0},
		/*                0x04 for some cards ?? */
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= avermedia_tvphone_audio,
		.has_remote     = 1,
	},
	[BTTV_BOARD_MATRIX_VISION] = {
		.name		= "MATRIX-Vision MV-Delta",
		.video_inputs	= 5,
		.audio_inputs	= 1,
		.tuner		= -1,
		.svhs		= 3,
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 0, 0},
		.audiomux	= {0 },
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x08 ---------------------------------- */
	[BTTV_BOARD_FLYVIDEO] = {
		.name		= "Lifeview FlyVideo II (Bt848) LR26 / MAXI TV Video PCI2 LR26",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xc00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0xc00, 0x800, 0x400, 0xc00, 0},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TURBOTV] = {
		.name		= "IMS/IXmicro TurboTV",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 3,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 1, 1, 2, 3, 0},
		.needs_tvaudio	= 0,
		.pll		= PLL_28,
		.tuner_type	= TUNER_TEMIC_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_HAUPPAUGE878] = {
		.name		= "Hauppauge (bt878)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x0f, /* old: 7 */
		.muxsel		= { 2, 0, 1, 1},
		.audiomux	= { 0, 1, 2, 3, 4},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MIROPRO] = {
		.name		= "MIRO PCTV pro",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x3014f,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x20001,0x10001, 0, 0,10},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x0c ---------------------------------- */
	[BTTV_BOARD_ADSTECH_TV] = {
		.name		= "ADS Technologies Channel Surfer TV (bt848)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 13, 14, 11, 7, 0, 0},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_AVERMEDIA98] = {
		.name		= "AVerMedia TVCapture 98",
		.video_inputs	= 3,
		.audio_inputs	= 4,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 13, 14, 11, 7, 0, 0},
		.needs_tvaudio	= 1,
		.msp34xx_alt    = 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook     = avermedia_tv_stereo_audio,
		.no_gpioirq     = 1,
	},
	[BTTV_BOARD_VHX] = {
		.name		= "Aimslab Video Highway Xtreme (VHX)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 7,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 2, 1, 3, 4}, /* old: { 0, 1, 2, 3, 4} */
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ZOLTRIX] = {
		.name		= "Zoltrix TV-Max",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= {0 , 0, 1 , 0, 10},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x10 ---------------------------------- */
	[BTTV_BOARD_PIXVIEWPLAYTV] = {
		.name		= "Prolink Pixelview PlayTV (bt878)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x01fe00,
		.muxsel		= { 2, 3, 1, 1},
	#if 0
		/* old */
		.audiomux	= { 0x01c000, 0, 0x018000, 0x014000, 0x002000, 0 },
	#else
		/* 2003-10-20 by "Anton A. Arapov" <arapov@mail.ru> */
		.audiomux       = { 0x001e00, 0, 0x018000, 0x014000, 0x002000, 0 },
	#endif
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
	},
	[BTTV_BOARD_WINVIEW_601] = {
		.name		= "Leadtek WinView 601",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x8300f8,
		.muxsel		= { 2, 3, 1, 1,0},
		.audiomux	= { 0x4fa007,0xcfa007,0xcfa007,0xcfa007,0xcfa007,0xcfa007},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= winview_audio,
		.has_radio	= 1,
	},
	[BTTV_BOARD_AVEC_INTERCAP] = {
		.name		= "AVEC Intercapture",
		.video_inputs	= 3,
		.audio_inputs	= 2,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0,
		.muxsel		= {2, 3, 1, 1},
		.audiomux	= {1, 0, 0, 0, 0},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_LIFE_FLYKIT] = {
		.name		= "Lifeview FlyVideo II EZ /FlyKit LR38 Bt848 (capture only)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= -1,
		.svhs		= -1,
		.gpiomask	= 0x8dff00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0 },
		.no_msp34xx	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x14 ---------------------------------- */
	[BTTV_BOARD_CEI_RAFFLES] = {
		.name		= "CEI Raffles Card",
		.video_inputs	= 3,
		.audio_inputs	= 3,
		.tuner		= 0,
		.svhs		= 2,
		.muxsel		= {2, 3, 1, 1},
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_CONFERENCETV] = {
		.name		= "Lifeview FlyVideo 98/ Lucky Star Image World ConferenceTV LR50",
		.video_inputs	= 4,
		.audio_inputs	= 2,  /* tuner, line in */
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1800,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0x800, 0x1000, 0x1000, 0x1800},
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL_I,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_PHOEBE_TVMAS] = {
		.name		= "Askey CPH050/ Phoebe Tv Master + FM",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xc00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= {0, 1, 0x800, 0x400, 0xc00, 0},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MODTEC_205] = {
		.name		= "Modular Technology MM201/MM202/MM205/MM210/MM215 PCTV, bt878",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= -1,
		.gpiomask	= 7,
		.muxsel		= { 2, 3, -1 },
		.digital_mode   = DIGITAL_MODE_CAMERA,
		.audiomux	= { 0, 0, 0, 0, 0 },
		.no_msp34xx	= 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_ALPS_TSBB5_PAL_I,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x18 ---------------------------------- */
	[BTTV_BOARD_MAGICTVIEW061] = {
		.name		= "Askey CPH05X/06X (bt878) [many vendors]",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xe00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= {0x400, 0x400, 0x400, 0x400, 0xc00},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
	},
	[BTTV_BOARD_VOBIS_BOOSTAR] = {
		.name           = "Terratec TerraTV+ Version 1.0 (Bt848)/ Terra TValue Version 1.0/ Vobis TV-Boostar",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask       = 0x1f0fff,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux       = { 0x20000, 0x30000, 0x10000, 0, 0x40000},
		.needs_tvaudio	= 0,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook     = terratv_audio,
	},
	[BTTV_BOARD_HAUPPAUG_WCAM] = {
		.name		= "Hauppauge WinCam newer (bt878)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 3,
		.gpiomask	= 7,
		.muxsel		= { 2, 0, 1, 1},
		.audiomux	= { 0, 1, 2, 3, 4},
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MAXI] = {
		.name		= "Lifeview FlyVideo 98/ MAXI TV Video PCI2 LR50",
		.video_inputs	= 4,
		.audio_inputs	= 2,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1800,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0x800, 0x1000, 0x1000, 0x1800},
		.pll            = PLL_28,
		.tuner_type	= TUNER_PHILIPS_SECAM,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x1c ---------------------------------- */
	[BTTV_BOARD_TERRATV] = {
		.name           = "Terratec TerraTV+ Version 1.1 (bt878)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1f0fff,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x20000, 0x30000, 0x10000, 0x00000, 0x40000},
		.needs_tvaudio	= 0,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= terratv_audio,
		/* GPIO wiring:
		External 20 pin connector (for Active Radio Upgrade board)
		gpio00: i2c-sda
		gpio01: i2c-scl
		gpio02: om5610-data
		gpio03: om5610-clk
		gpio04: om5610-wre
		gpio05: om5610-stereo
		gpio06: rds6588-davn
		gpio07: Pin 7 n.c.
		gpio08: nIOW
		gpio09+10: nIOR, nSEL ?? (bt878)
			gpio09: nIOR (bt848)
			gpio10: nSEL (bt848)
		Sound Routing:
		gpio16: u2-A0 (1st 4052bt)
		gpio17: u2-A1
		gpio18: u2-nEN
		gpio19: u4-A0 (2nd 4052)
		gpio20: u4-A1
			u4-nEN - GND
		Btspy:
			00000 : Cdrom (internal audio input)
			10000 : ext. Video audio input
			20000 : TV Mono
			a0000 : TV Mono/2
		1a0000 : TV Stereo
			30000 : Radio
			40000 : Mute
	*/

	},
	[BTTV_BOARD_PXC200] = {
		/* Jannik Fritsch <jannik@techfak.uni-bielefeld.de> */
		.name		= "Imagenation PXC200",
		.video_inputs	= 5,
		.audio_inputs	= 1,
		.tuner		= -1,
		.svhs		= 1, /* was: 4 */
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 0, 0},
		.audiomux	= { 0 },
		.needs_tvaudio	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.muxsel_hook    = PXC200_muxsel,

	},
	[BTTV_BOARD_FLYVIDEO_98] = {
		.name		= "Lifeview FlyVideo 98 LR50",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1800,  /* 0x8dfe00 */
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0x0800, 0x1000, 0x1000, 0x1800, 0 },
		.pll            = PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_IPROTV] = {
		.name		= "Formac iProTV, Formac ProTV I (bt848)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 3,
		.gpiomask	= 1,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 1, 0, 0, 0, 0 },
		.pll            = PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x20 ---------------------------------- */
	[BTTV_BOARD_INTEL_C_S_PCI] = {
		.name		= "Intel Create and Share PCI/ Smart Video Recorder III",
		.video_inputs	= 4,
		.audio_inputs	= 0,
		.tuner		= -1,
		.svhs		= 2,
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0 },
		.needs_tvaudio	= 0,
		.tuner_type	= 4,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TERRATVALUE] = {
		.name           = "Terratec TerraTValue Version Bt878",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xffff00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x500, 0, 0x300, 0x900, 0x900},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_WINFAST2000] = {
		.name		= "Leadtek WinFast 2000/ WinFast 2000 XP",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.muxsel		= { 2, 3, 1, 1, 0}, /* TV, CVid, SVid, CVid over SVid connector */
	#if 0
		.gpiomask	= 0xc33000,
		.audiomux	= { 0x422000,0x1000,0x0000,0x620000,0x800000 },
	#else
		/* Alexander Varakin <avarakin@hotmail.com> [stereo version] */
		.gpiomask	= 0xb33000,
		.audiomux	= { 0x122000,0x1000,0x0000,0x620000,0x800000 },
	#endif
		/* Audio Routing for "WinFast 2000 XP" (no tv stereo !)
			gpio23 -- hef4052:nEnable (0x800000)
			gpio12 -- hef4052:A1
			gpio13 -- hef4052:A0
		0x0000: external audio
		0x1000: FM
		0x2000: TV
		0x3000: n.c.
		Note: There exists another variant "Winfast 2000" with tv stereo !?
		Note: eeprom only contains FF and pci subsystem id 107d:6606
		*/
		.needs_tvaudio	= 0,
		.pll		= PLL_28,
		.has_radio	= 1,
		.tuner_type	= 5, /* default for now, gpio reads BFFF06 for Pal bg+dk */
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= winfast2000_audio,
		.has_remote     = 1,
	},
	[BTTV_BOARD_CHRONOS_VS2] = {
		.name		= "Lifeview FlyVideo 98 LR50 / Chronos Video Shuttle II",
		.video_inputs	= 4,
		.audio_inputs	= 3,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1800,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0x800, 0x1000, 0x1000, 0x1800},
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x24 ---------------------------------- */
	[BTTV_BOARD_TYPHOON_TVIEW] = {
		.name		= "Lifeview FlyVideo 98FM LR50 / Typhoon TView TV/FM Tuner",
		.video_inputs	= 4,
		.audio_inputs	= 3,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1800,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0x800, 0x1000, 0x1000, 0x1800, 0 },
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio	= 1,
	},
	[BTTV_BOARD_PXELVWPLTVPRO] = {
		.name		= "Prolink PixelView PlayTV pro",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xff,
		.muxsel		= { 2, 3, 1, 1 },
		.audiomux	= { 0x21, 0x20, 0x24, 0x2c, 0x29, 0x29 },
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MAGICTVIEW063] = {
		.name		= "Askey CPH06X TView99",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x551e00,
		.muxsel		= { 2, 3, 1, 0},
		.audiomux	= { 0x551400, 0x551200, 0, 0, 0x551c00, 0x551200 },
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= 1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
	},
	[BTTV_BOARD_PINNACLE] = {
		.name		= "Pinnacle PCTV Studio/Rave",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x03000F,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 2, 0xd0001, 0, 0, 1},
		.needs_tvaudio	= 0,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x28 ---------------------------------- */
	[BTTV_BOARD_STB2] = {
		.name		= "STB TV PCI FM, Gateway P/N 6000704 (bt878), 3Dfx VoodooTV 100",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 7,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 4, 0, 2, 3, 1},
		.no_msp34xx	= 1,
		.needs_tvaudio	= 1,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
		.has_radio      = 1,
	},
	[BTTV_BOARD_AVPHONE98] = {
		.name		= "AVerMedia TVPhone 98",
		.video_inputs	= 3,
		.audio_inputs	= 4,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 13, 4, 11, 7, 0, 0},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio	= 1,
		.audio_hook	= avermedia_tvphone_audio,
	},
	[BTTV_BOARD_PV951] = {
		.name		= "ProVideo PV951", /* pic16c54 */
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0, 0, 0, 0},
		.needs_tvaudio	= 1,
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= 1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ONAIR_TV] = {
		.name		= "Little OnAir TV",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xe00b,
		.muxsel		= {2, 3, 1, 1},
		.audiomux	= {0xff9ff6, 0xff9ff6, 0xff1ff7, 0, 0xff3ffc},
		.no_msp34xx	= 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x2c ---------------------------------- */
	[BTTV_BOARD_SIGMA_TVII_FM] = {
		.name		= "Sigma TVII-FM",
		.video_inputs	= 2,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= -1,
		.gpiomask	= 3,
		.muxsel		= {2, 3, 1, 1},
		.audiomux	= {1, 1, 0, 2, 3},
		.no_msp34xx	= 1,
		.pll		= PLL_NONE,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MATRIX_VISION2] = {
		.name		= "MATRIX-Vision MV-Delta 2",
		.video_inputs	= 5,
		.audio_inputs	= 1,
		.tuner		= -1,
		.svhs		= 3,
		.gpiomask	= 0,
		.muxsel		= { 2, 3, 1, 0, 0},
		.audiomux	= {0 },
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ZOLTRIX_GENIE] = {
		.name		= "Zoltrix Genie TV/FM",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xbcf03f,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0xbc803f, 0xbc903f, 0xbcb03f, 0, 0xbcb03f},
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= 21,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TERRATVRADIO] = {
		.name		= "Terratec TV/Radio+",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x70000,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x20000, 0x30000, 0x10000, 0, 0x40000, 0x20000 },
		.needs_tvaudio	= 1,
		.no_msp34xx	= 1,
		.pll		= PLL_35,
		.tuner_type	= 1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio	= 1,
	},

	/* ---- card 0x30 ---------------------------------- */
	[BTTV_BOARD_DYNALINK] = {
		.name		= "Askey CPH03x/ Dynalink Magic TView",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= {2,0,0,0,1},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_GVBCTV3PCI] = {
		.name		= "IODATA GV-BCTV3/PCI",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x010f00,
		.muxsel		= {2, 3, 0, 0},
		.audiomux	= {0x10000, 0, 0x10000, 0, 0, 0},
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_ALPS_TSHC6_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= gvbctv3pci_audio,
	},
	[BTTV_BOARD_PXELVWPLTVPAK] = {
		.name		= "Prolink PV-BT878P+4E / PixelView PlayTV PAK / Lenco MXTV-9578 CP",
		.video_inputs	= 5,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 3,
		.gpiomask	= 0xAA0000,
		.muxsel		= { 2,3,1,1,-1 },
		.digital_mode   = DIGITAL_MODE_CAMERA,
		.audiomux	= { 0x20000, 0, 0x80000, 0x80000, 0xa8000, 0x46000  },
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL_I,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote	= 1,
		/* GPIO wiring: (different from Rev.4C !)
			GPIO17: U4.A0 (first hef4052bt)
			GPIO19: U4.A1
			GPIO20: U5.A1 (second hef4052bt)
			GPIO21: U4.nEN
			GPIO22: BT832 Reset Line
			GPIO23: A5,A0, U5,nEN
		Note: At i2c=0x8a is a Bt832 chip, which changes to 0x88 after being reset via GPIO22
		*/
	},
	[BTTV_BOARD_EAGLE] = {
		.name           = "Eagle Wireless Capricorn2 (bt878A)",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 7,
		.muxsel         = { 2, 0, 1, 1},
		.audiomux       = { 0, 1, 2, 3, 4},
		.pll            = PLL_28,
		.tuner_type     = -1 /* TUNER_ALPS_TMDH2_NTSC */,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x34 ---------------------------------- */
	[BTTV_BOARD_PINNACLEPRO] = {
		/* David Härdeman <david@2gen.com> */
		.name           = "Pinnacle PCTV Studio Pro",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 3,
		.gpiomask       = 0x03000F,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 1, 0xd0001, 0, 0, 10},
				/* sound path (5 sources):
				MUX1 (mask 0x03), Enable Pin 0x08 (0=enable, 1=disable)
					0= ext. Audio IN
					1= from MUX2
					2= Mono TV sound from Tuner
					3= not connected
				MUX2 (mask 0x30000):
					0,2,3= from MSP34xx
					1= FM stereo Radio from Tuner */
		.needs_tvaudio  = 0,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TVIEW_RDS_FM] = {
		/* Claas Langbehn <claas@bigfoot.com>,
		Sven Grothklags <sven@upb.de> */
		.name		= "Typhoon TView RDS + FM Stereo / KNC1 TV Station RDS",
		.video_inputs	= 4,
		.audio_inputs	= 3,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x1c,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0, 0, 0x10, 8, 4 },
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio	= 1,
	},
	[BTTV_BOARD_LIFETEC_9415] = {
		/* Tim R�stermundt <rosterm@uni-muenster.de>
		in de.comp.os.unix.linux.hardware:
			options bttv card=0 pll=1 radio=1 gpiomask=0x18e0
			audiomux=0x44c71f,0x44d71f,0,0x44d71f,0x44dfff
			options tuner type=5 */
		.name		= "Lifeview FlyVideo 2000 /FlyVideo A2/ Lifetec LT 9415 TV [LR90]",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x18e0,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x0000,0x0800,0x1000,0x1000,0x18e0 },
			/* For cards with tda9820/tda9821:
				0x0000: Tuner normal stereo
				0x0080: Tuner A2 SAP (second audio program = Zweikanalton)
				0x0880: Tuner A2 stereo */
		.pll		= PLL_28,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_BESTBUY_EASYTV] = {
		/* Miguel Angel Alvarez <maacruz@navegalia.com>
		old Easy TV BT848 version (model CPH031) */
		.name           = "Askey CPH031/ BESTBUY Easy TV",
		.video_inputs	= 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0xF,
		.muxsel         = { 2, 3, 1, 0},
		.audiomux       = { 2, 0, 0, 0, 10},
		.needs_tvaudio  = 0,
		.pll		= PLL_28,
		.tuner_type	= TUNER_TEMIC_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x38 ---------------------------------- */
	[BTTV_BOARD_FLYVIDEO_98FM] = {
		/* Gordon Heydon <gjheydon@bigfoot.com ('98) */
		.name           = "Lifeview FlyVideo 98FM LR50",
		.video_inputs   = 4,
		.audio_inputs   = 3,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x1800,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 0, 0x800, 0x1000, 0x1000, 0x1800, 0 },
		.pll            = PLL_28,
		.tuner_type     = 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
		/* This is the ultimate cheapo capture card
		* just a BT848A on a small PCB!
		* Steve Hosgood <steve@equiinet.com> */
	[BTTV_BOARD_GRANDTEC] = {
		.name           = "GrandTec 'Grand Video Capture' (Bt848)",
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 1,
		.gpiomask       = 0,
		.muxsel         = { 3, 1 },
		.audiomux       = { 0 },
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_35,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ASKEY_CPH060] = {
		/* Daniel Herrington <daniel.herrington@home.com> */
		.name           = "Askey CPH060/ Phoebe TV Master Only (No FM)",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0xe00,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 0x400, 0x400, 0x400, 0x400, 0x800, 0x400 },
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_TEMIC_4036FY5_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ASKEY_CPH03X] = {
		/* Matti Mottus <mottus@physic.ut.ee> */
		.name		= "Askey CPH03x TV Capturer",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask       = 0x03000F,
		.muxsel		= { 2, 3, 1, 0},
		.audiomux       = { 2,0,0,0,1 },
		.pll            = PLL_28,
		.tuner_type	= 0,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x3c ---------------------------------- */
	[BTTV_BOARD_MM100PCTV] = {
		/* Philip Blundell <philb@gnu.org> */
		.name           = "Modular Technology MM100PCTV",
		.video_inputs   = 2,
		.audio_inputs   = 2,
		.tuner		= 0,
		.svhs		= -1,
		.gpiomask       = 11,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 2, 0, 0, 1, 8},
		.pll            = PLL_35,
		.tuner_type     = TUNER_TEMIC_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_GMV1] = {
		/* Adrian Cox <adrian@humboldt.co.uk */
		.name		= "AG Electronics GMV1",
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.tuner		= -1,
		.svhs		= 1,
		.gpiomask       = 0xF,
		.muxsel		= { 2, 2},
		.audiomux       = { },
		.no_msp34xx     = 1,
		.needs_tvaudio  = 0,
		.pll		= PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_BESTBUY_EASYTV2] = {
		/* Miguel Angel Alvarez <maacruz@navegalia.com>
		new Easy TV BT878 version (model CPH061)
		special thanks to Informatica Mieres for providing the card */
		.name           = "Askey CPH061/ BESTBUY Easy TV (bt878)",
		.video_inputs	= 3,
		.audio_inputs   = 2,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0xFF,
		.muxsel         = { 2, 3, 1, 0},
		.audiomux       = { 1, 0, 4, 4, 9},
		.needs_tvaudio  = 0,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_ATI_TVWONDER] = {
		/* Lukas Gebauer <geby@volny.cz> */
		.name		= "ATI TV-Wonder",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xf03f,
		.muxsel		= { 2, 3, 1, 0 },
		.audiomux	= { 0xbffe, 0, 0xbfff, 0, 0xbffe},
		.pll		= PLL_28,
		.tuner_type	= TUNER_TEMIC_4006FN5_MULTI_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x40 ---------------------------------- */
	[BTTV_BOARD_ATI_TVWONDERVE] = {
		/* Lukas Gebauer <geby@volny.cz> */
		.name		= "ATI TV-Wonder VE",
		.video_inputs	= 2,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= -1,
		.gpiomask	= 1,
		.muxsel		= { 2, 3, 0, 1},
		.audiomux	= { 0, 0, 1, 0, 0},
		.no_msp34xx	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_TEMIC_4006FN5_MULTI_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_FLYVIDEO2000] = {
		/* DeeJay <deejay@westel900.net (2000S) */
		.name           = "Lifeview FlyVideo 2000S LR90",
		.video_inputs   = 3,
		.audio_inputs   = 3,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask	= 0x18e0,
		.muxsel		= { 2, 3, 0, 1},
				/* Radio changed from 1e80 to 0x800 to make
				FlyVideo2000S in .hu happy (gm)*/
				/* -dk-???: set mute=0x1800 for tda9874h daughterboard */
		.audiomux	= { 0x0000,0x0800,0x1000,0x1000,0x1800, 0x1080 },
		.audio_hook	= fv2000s_audio,
		.no_msp34xx	= 1,
		.no_tda9875	= 1,
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TERRATVALUER] = {
		.name		= "Terratec TValueRadio",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0xffff00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 0x500, 0x500, 0x300, 0x900, 0x900},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio	= 1,
	},
	[BTTV_BOARD_GVBCTV4PCI] = {
		/* TANAKA Kei <peg00625@nifty.com> */
		.name           = "IODATA GV-BCTV4/PCI",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x010f00,
		.muxsel         = {2, 3, 0, 0},
		.audiomux       = {0x10000, 0, 0x10000, 0, 0, 0},
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_SHARP_2U5JF5540_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook     = gvbctv3pci_audio,
	},

	/* ---- card 0x44 ---------------------------------- */
	[BTTV_BOARD_VOODOOTV_FM] = {
		.name           = "3Dfx VoodooTV FM (Euro), VoodooTV 200 (USA)",
		/* try "insmod msp3400 simple=0" if you have
		* sound problems with this card. */
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = -1,
		.gpiomask       = 0x4f8a00,
		/* 0x100000: 1=MSP enabled (0=disable again)
		* 0x010000: Connected to "S0" on tda9880 (0=Pal/BG, 1=NTSC) */
		.audiomux       = {0x947fff, 0x987fff,0x947fff,0x947fff, 0x947fff},
		/* tvtuner, radio,   external,internal, mute,  stereo
		* tuner, Composit, SVid, Composit-on-Svid-adapter */
		.muxsel         = { 2, 3 ,0 ,1},
		.tuner_type     = TUNER_MT2032,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll		= PLL_28,
		.has_radio	= 1,
	},
	[BTTV_BOARD_AIMMS] = {
		/* Philip Blundell <pb@nexus.co.uk> */
		.name           = "Active Imaging AIMMS",
		.video_inputs   = 1,
		.audio_inputs   = 0,
		.tuner          = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
		.muxsel         = { 2 },
		.gpiomask       = 0
	},
	[BTTV_BOARD_PV_BT878P_PLUS] = {
		/* Tomasz Pyra <hellfire@sedez.iq.pl> */
		.name           = "Prolink Pixelview PV-BT878P+ (Rev.4C,8E)",
		.video_inputs   = 3,
		.audio_inputs   = 4,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 15,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 0, 0, 11, 7, 13, 0}, /* TV and Radio with same GPIO ! */
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = 25,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
		/* GPIO wiring:
			GPIO0: U4.A0 (hef4052bt)
			GPIO1: U4.A1
			GPIO2: U4.A1 (second hef4052bt)
			GPIO3: U4.nEN, U5.A0, A5.nEN
			GPIO8-15: vrd866b ?
		*/
	},
	[BTTV_BOARD_FLYVIDEO98EZ] = {
		.name		= "Lifeview FlyVideo 98EZ (capture only) LR51",
		.video_inputs	= 4,
		.audio_inputs   = 0,
		.tuner		= -1,
		.svhs		= 2,
		.muxsel		= { 2, 3, 1, 1}, /* AV1, AV2, SVHS, CVid adapter on SVHS */
		.pll		= PLL_28,
		.no_msp34xx	= 1,
		.tuner_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

	/* ---- card 0x48 ---------------------------------- */
	[BTTV_BOARD_PV_BT878P_9B] = {
		/* Dariusz Kowalewski <darekk@automex.pl> */
		.name		= "Prolink Pixelview PV-BT878P+9B (PlayTV Pro rev.9B FM+NICAM)",
		.video_inputs	= 4,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x3f,
		.muxsel		= { 2, 3, 1, 1 },
		.audiomux	= { 0x01, 0x00, 0x03, 0x03, 0x09, 0x02 },
		.needs_tvaudio  = 1,
		.no_msp34xx	= 1,
		.no_tda9875	= 1,
		.pll		= PLL_28,
		.tuner_type	= 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= pvbt878p9b_audio, /* Note: not all cards have stereo */
		.has_radio	= 1,  /* Note: not all cards have radio */
		.has_remote     = 1,
		/* GPIO wiring:
			GPIO0: A0 hef4052
			GPIO1: A1 hef4052
			GPIO3: nEN hef4052
			GPIO8-15: vrd866b
			GPIO20,22,23: R30,R29,R28
		*/
	},
	[BTTV_BOARD_SENSORAY311] = {
		/* Clay Kunz <ckunz@mail.arc.nasa.gov> */
		/* you must jumper JP5 for the card to work */
		.name           = "Sensoray 311",
		.video_inputs   = 5,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 4,
		.gpiomask       = 0,
		.muxsel         = { 2, 3, 1, 0, 0},
		.audiomux       = { 0 },
		.needs_tvaudio  = 0,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_RV605] = {
		/* Miguel Freitas <miguel@cetuc.puc-rio.br> */
		.name           = "RemoteVision MX (RV605)",
		.video_inputs   = 16,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.gpiomask       = 0x00,
		.gpiomask2      = 0x07ff,
		.muxsel         = { 0x33, 0x13, 0x23, 0x43, 0xf3, 0x73, 0xe3, 0x03,
				0xd3, 0xb3, 0xc3, 0x63, 0x93, 0x53, 0x83, 0xa3 },
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.muxsel_hook    = rv605_muxsel,
	},
	[BTTV_BOARD_POWERCLR_MTV878] = {
		.name           = "Powercolor MTV878/ MTV878R/ MTV878F",
		.video_inputs   = 3,
		.audio_inputs   = 2,
		.tuner		= 0,
		.svhs           = 2,
		.gpiomask       = 0x1C800F,  /* Bit0-2: Audio select, 8-12:remote control 14:remote valid 15:remote reset */
		.muxsel         = { 2, 1, 1, },
		.audiomux       = { 0, 1, 2, 2, 4 },
		.needs_tvaudio  = 0,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll		= PLL_28,
		.has_radio	= 1,
	},

	/* ---- card 0x4c ---------------------------------- */
	[BTTV_BOARD_WINDVR] = {
		/* Masaki Suzuki <masaki@btree.org> */
		.name           = "Canopus WinDVR PCI (COMPAQ Presario 3524JP, 5112JP)",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x140007,
		.muxsel         = { 2, 3, 1, 1 },
		.audiomux       = { 0, 1, 2, 3, 4, 0 },
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook     = windvr_audio,
	},
	[BTTV_BOARD_GRANDTEC_MULTI] = {
		.name           = "GrandTec Multi Capture Card (Bt878)",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.gpiomask       = 0,
		.muxsel         = { 2, 3, 1, 0 },
		.audiomux       = { 0 },
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_KWORLD] = {
		.name           = "Jetway TV/Capture JW-TV878-FBK, Kworld KW-TV878RF",
		.video_inputs   = 4,
		.audio_inputs   = 3,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 7,
		.muxsel         = { 2, 3, 1, 1 },   /* Tuner, SVid, SVHS, SVid to SVHS connector */
		.audiomux       = { 0 ,0 ,4, 4,4,4},/* Yes, this tuner uses the same audio output for TV and FM radio!
						* This card lacks external Audio In, so we mute it on Ext. & Int.
						* The PCB can take a sbx1637/sbx1673, wiring unknown.
						* This card lacks PCI subsystem ID, sigh.
						* audiomux=1: lower volume, 2+3: mute
						* btwincap uses 0x80000/0x80003
						*/
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		/* Samsung TCPA9095PC27A (BG+DK), philips compatible, w/FM, stereo and
		radio signal strength indicators work fine. */
		.has_radio	= 1,
		/* GPIO Info:
			GPIO0,1:   HEF4052 A0,A1
			GPIO2:     HEF4052 nENABLE
			GPIO3-7:   n.c.
			GPIO8-13:  IRDC357 data0-5 (data6 n.c. ?) [chip not present on my card]
			GPIO14,15: ??
			GPIO16-21: n.c.
			GPIO22,23: ??
			??       : mtu8b56ep microcontroller for IR (GPIO wiring unknown)*/
	},
	[BTTV_BOARD_DSP_TCVIDEO] = {
		/* Arthur Tetzlaff-Deas, DSP Design Ltd <software@dspdesign.com> */
		.name           = "DSP Design TCVIDEO",
		.video_inputs   = 4,
		.svhs           = -1,
		.muxsel         = { 2, 3, 1, 0},
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

		/* ---- card 0x50 ---------------------------------- */
	[BTTV_BOARD_HAUPPAUGEPVR] = {
		.name           = "Hauppauge WinTV PVR",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.muxsel         = { 2, 0, 1, 1},
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,

		.gpiomask       = 7,
		.audiomux       = {7},
	},
	[BTTV_BOARD_GVBCTV5PCI] = {
		.name           = "IODATA GV-BCTV5/PCI",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x0f0f80,
		.muxsel         = {2, 3, 1, 0},
		.audiomux       = {0x030000, 0x010000, 0, 0, 0x020000, 0},
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook     = gvbctv5pci_audio,
		.has_radio      = 1,
	},
	[BTTV_BOARD_OSPREY1x0] = {
		.name           = "Osprey 100/150 (878)", /* 0x1(2|3)-45C6-C1 */
		.video_inputs   = 4,                  /* id-inputs-clock */
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 3,
		.muxsel         = { 3, 2, 0, 1 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY1x0_848] = {
		.name           = "Osprey 100/150 (848)", /* 0x04-54C0-C1 & older boards */
		.video_inputs   = 3,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 2,
		.muxsel         = { 2, 3, 1 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},

		/* ---- card 0x54 ---------------------------------- */
	[BTTV_BOARD_OSPREY101_848] = {
		.name           = "Osprey 101 (848)", /* 0x05-40C0-C1 */
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 3, 1 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY1x1] = {
		.name           = "Osprey 101/151",       /* 0x1(4|5)-0004-C4 */
		.video_inputs   = 1,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.muxsel         = { 0 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY1x1_SVID] = {
		.name           = "Osprey 101/151 w/ svid",  /* 0x(16|17|20)-00C4-C1 */
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 0, 1 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY2xx] = {
		.name           = "Osprey 200/201/250/251",  /* 0x1(8|9|E|F)-0004-C4 */
		.video_inputs   = 1,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = -1,
		.muxsel         = { 0 },
		.pll            = PLL_28,
		.tuner_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},

		/* ---- card 0x58 ---------------------------------- */
	[BTTV_BOARD_OSPREY2x0_SVID] = {
		.name           = "Osprey 200/250",   /* 0x1(A|B)-00C4-C1 */
		.video_inputs   = 2,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 0, 1 },
		.pll            = PLL_28,
		.tuner_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY2x0] = {
		.name           = "Osprey 210/220",   /* 0x1(A|B)-04C0-C1 */
		.video_inputs   = 2,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 2, 3 },
		.pll            = PLL_28,
		.tuner_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY500] = {
		.name           = "Osprey 500",   /* 500 */
		.video_inputs   = 2,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 2, 3 },
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
	[BTTV_BOARD_OSPREY540] = {
		.name           = "Osprey 540",   /* 540 */
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = -1,
	#if 0 /* TODO ... */
		.svhs           = OSPREY540_SVID_ANALOG,
		.muxsel         = {       [OSPREY540_COMP_ANALOG] = 2,
					[OSPREY540_SVID_ANALOG] = 3, },
	#endif
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	#if 0 /* TODO ... */
		.muxsel_hook    = osprey_540_muxsel,
		.picture_hook   = osprey_540_set_picture,
	#endif
	},

		/* ---- card 0x5C ---------------------------------- */
	[BTTV_BOARD_OSPREY2000] = {
		.name           = "Osprey 2000",  /* 2000 */
		.video_inputs   = 2,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 2, 3 },
		.pll            = PLL_28,
		.tuner_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,      /* must avoid, conflicts with the bt860 */
	},
	[BTTV_BOARD_IDS_EAGLE] = {
		/* M G Berberich <berberic@forwiss.uni-passau.de> */
		.name           = "IDS Eagle",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs           = -1,
		.gpiomask       = 0,
		.muxsel         = { 0, 1, 2, 3 },
		.muxsel_hook    = eagle_muxsel,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.pll            = PLL_28,
	},
	[BTTV_BOARD_PINNACLESAT] = {
		.name           = "Pinnacle PCTV Sat",
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.svhs           = 1,
		.tuner          = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.muxsel         = { 3, 0, 1, 2},
		.pll            = PLL_28,
		.no_gpioirq     = 1,
		.has_dvb        = 1,
	},
	[BTTV_BOARD_FORMAC_PROTV] = {
		.name           = "Formac ProTV II (bt878)",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 3,
		.gpiomask       = 2,
		/* TV, Comp1, Composite over SVID con, SVID */
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 2, 2, 0, 0, 0 },
		.pll            = PLL_28,
		.has_radio      = 1,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	/* sound routing:
		GPIO=0x00,0x01,0x03: mute (?)
		0x02: both TV and radio (tuner: FM1216/I)
		The card has onboard audio connectors labeled "cdrom" and "board",
		not soldered here, though unknown wiring.
		Card lacks: external audio in, pci subsystem id.
	*/
	},

		/* ---- card 0x60 ---------------------------------- */
	[BTTV_BOARD_MACHTV] = {
		.name           = "MachTV",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = -1,
		.gpiomask       = 7,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 0, 1, 2, 3, 4},
		.needs_tvaudio  = 1,
		.tuner_type     = 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
	},
	[BTTV_BOARD_EURESYS_PICOLO] = {
		.name           = "Euresys Picolo",
		.video_inputs   = 3,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = 2,
		.gpiomask       = 0,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.muxsel         = { 2, 0, 1},
		.pll            = PLL_28,
		.tuner_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_PV150] = {
		/* Luc Van Hoeylandt <luc@e-magic.be> */
		.name           = "ProVideo PV150", /* 0x4f */
		.video_inputs   = 2,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.gpiomask       = 0,
		.muxsel         = { 2, 3 },
		.audiomux       = { 0 },
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_AD_TVK503] = {
		/* Hiroshi Takekawa <sian@big.or.jp> */
		/* This card lacks subsystem ID */
		.name           = "AD-TVK503", /* 0x63 */
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x001e8007,
		.muxsel         = { 2, 3, 1, 0 },
		/*                  Tuner, Radio, external, internal, off,  on */
		.audiomux       = { 0x08,  0x0f,  0x0a,     0x08,     0x0f, 0x08 },
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = 2,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.audio_hook	= adtvk503_audio,
	},

		/* ---- card 0x64 ---------------------------------- */
	[BTTV_BOARD_HERCULES_SM_TV] = {
		.name           = "Hercules Smart TV Stereo",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x00,
		.muxsel         = { 2, 3, 1, 1 },
		.needs_tvaudio  = 1,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = 5,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		/* Notes:
		- card lacks subsystem ID
		- stereo variant w/ daughter board with tda9874a @0xb0
		- Audio Routing:
			always from tda9874 independent of GPIO (?)
			external line in: unknown
		- Other chips: em78p156elp @ 0x96 (probably IR remote control)
			hef4053 (instead 4052) for unknown function
		*/
	},
	[BTTV_BOARD_PACETV] = {
		.name           = "Pace TV & Radio Card",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.muxsel         = { 2, 3, 1, 1}, /* Tuner, CVid, SVid, CVid over SVid connector */
		.gpiomask       = 0,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.tuner_type     = 1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio      = 1,
		.pll            = PLL_28,
		/* Bt878, Bt832, FI1246 tuner; no pci subsystem id
		only internal line out: (4pin header) RGGL
		Radio must be decoded by msp3410d (not routed through)*/
		/*
		.digital_mode   = DIGITAL_MODE_CAMERA,  todo!
		*/
	},
	[BTTV_BOARD_IVC200] = {
		/* Chris Willing <chris@vislab.usyd.edu.au> */
		.name           = "IVC-200",
		.video_inputs   = 1,
		.audio_inputs   = 0,
		.tuner          = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs           = -1,
		.gpiomask       = 0xdf,
		.muxsel         = { 2 },
		.pll            = PLL_28,
	},
	[BTTV_BOARD_XGUARD] = {
		.name           = "Grand X-Guard / Trust 814PCI",
		.video_inputs   = 16,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.tuner_type     = 4,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask2      = 0xff,
		.muxsel         = { 2,2,2,2, 3,3,3,3, 1,1,1,1, 0,0,0,0 },
		.muxsel_hook    = xguard_muxsel,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll            = PLL_28,
	},

		/* ---- card 0x68 ---------------------------------- */
	[BTTV_BOARD_NEBULA_DIGITV] = {
		.name           = "Nebula Electronics DigiTV",
		.video_inputs   = 1,
		.tuner          = -1,
		.svhs           = -1,
		.muxsel         = { 2, 3, 1, 0},
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_dvb        = 1,
		.has_remote	= 1,
		.gpiomask	= 0x1b,
		.no_gpioirq     = 1,
	},
	[BTTV_BOARD_PV143] = {
		/* Jorge Boncompte - DTI2 <jorge@dti2.net> */
		.name           = "ProVideo PV143",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.gpiomask       = 0,
		.muxsel         = { 2, 3, 1, 0 },
		.audiomux       = { 0 },
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_VD009X1_MINIDIN] = {
		/* M.Klahr@phytec.de */
		.name           = "PHYTEC VD-009-X1 MiniDIN (bt878)",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1, /* card has no tuner */
		.svhs           = 3,
		.gpiomask       = 0x00,
		.muxsel         = { 2, 3, 1, 0},
		.audiomux       = { 0, 0, 0, 0, 0, 0 }, /* card has no audio */
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_VD009X1_COMBI] = {
		.name           = "PHYTEC VD-009-X1 Combi (bt878)",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1, /* card has no tuner */
		.svhs           = 3,
		.gpiomask       = 0x00,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 0, 0, 0, 0, 0, 0 }, /* card has no audio */
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},

		/* ---- card 0x6c ---------------------------------- */
	[BTTV_BOARD_VD009_MINIDIN] = {
		.name           = "PHYTEC VD-009 MiniDIN (bt878)",
		.video_inputs   = 10,
		.audio_inputs   = 0,
		.tuner          = -1, /* card has no tuner */
		.svhs           = 9,
		.gpiomask       = 0x00,
		.gpiomask2      = 0x03, /* gpiomask2 defines the bits used to switch audio
					via the upper nibble of muxsel. here: used for
					xternal video-mux */
		.muxsel         = { 0x02, 0x12, 0x22, 0x32, 0x03, 0x13, 0x23, 0x33, 0x01, 0x00 },
		.audiomux       = { 0, 0, 0, 0, 0, 0 }, /* card has no audio */
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_VD009_COMBI] = {
		.name           = "PHYTEC VD-009 Combi (bt878)",
		.video_inputs   = 10,
		.audio_inputs   = 0,
		.tuner          = -1, /* card has no tuner */
		.svhs           = 9,
		.gpiomask       = 0x00,
		.gpiomask2      = 0x03, /* gpiomask2 defines the bits used to switch audio
					via the upper nibble of muxsel. here: used for
					xternal video-mux */
		.muxsel         = { 0x02, 0x12, 0x22, 0x32, 0x03, 0x13, 0x23, 0x33, 0x01, 0x01 },
		.audiomux       = { 0, 0, 0, 0, 0, 0 }, /* card has no audio */
		.needs_tvaudio  = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_IVC100] = {
		.name           = "IVC-100",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs           = -1,
		.gpiomask       = 0xdf,
		.muxsel         = { 2, 3, 1, 0 },
		.pll            = PLL_28,
	},
	[BTTV_BOARD_IVC120] = {
		/* IVC-120G - Alan Garfield <alan@fromorbit.com> */
		.name           = "IVC-120G",
		.video_inputs   = 16,
		.audio_inputs   = 0,    /* card has no audio */
		.tuner          = -1,   /* card has no tuner */
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs           = -1,   /* card has no svhs */
		.needs_tvaudio  = 0,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.gpiomask       = 0x00,
		.muxsel         = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
				0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10 },
		.muxsel_hook    = ivc120_muxsel,
		.pll            = PLL_28,
	},

		/* ---- card 0x70 ---------------------------------- */
	[BTTV_BOARD_PC_HDTV] = {
		.name           = "pcHDTV HD-2000 TV",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.muxsel         = { 2, 3, 1, 0},
		.tuner_type     = TUNER_PHILIPS_ATSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_dvb        = 1,
	},
	[BTTV_BOARD_TWINHAN_DST] = {
		.name           = "Twinhan DST + clones",
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.tuner_type     = TUNER_ABSENT,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_video       = 1,
		.has_dvb        = 1,
	},
	[BTTV_BOARD_WINFASTVC100] = {
		.name           = "Winfast VC100",
		.video_inputs   = 3,
		.audio_inputs   = 0,
		.svhs           = 1,
		.tuner          = -1,
		.muxsel         = { 3, 1, 1, 3}, /* Vid In, SVid In, Vid over SVid in connector */
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.tuner_type     = TUNER_ABSENT,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
	},
	[BTTV_BOARD_TEV560] = {
		.name           = "Teppro TEV-560/InterVision IV-560",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 3,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 1, 1, 1, 1, 0},
		.needs_tvaudio  = 1,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_35,
	},

		/* ---- card 0x74 ---------------------------------- */
	[BTTV_BOARD_SIMUS_GVC1100] = {
		.name           = "SIMUS GVC1100",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
		.muxsel         = { 2, 2, 2, 2},
		.gpiomask       = 0x3F,
		.muxsel_hook    = gvc1100_muxsel,
	},
	[BTTV_BOARD_NGSTV_PLUS] = {
		/* Carlos Silva r3pek@r3pek.homelinux.org || card 0x75 */
		.name           = "NGS NGSTV+",
		.video_inputs   = 3,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x008007,
		.muxsel         = {2, 3, 0, 0},
		.audiomux       = {0, 0, 0, 0, 0x000003, 0},
		.pll            = PLL_28,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
	},
	[BTTV_BOARD_LMLBT4] = {
		/* http://linuxmedialabs.com */
		.name           = "LMLBT4",
		.video_inputs   = 4, /* IN1,IN2,IN3,IN4 */
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.muxsel         = { 2, 3, 1, 0 },
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.needs_tvaudio  = 0,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_TEKRAM_M205] = {
		/* Helmroos Harri <harri.helmroos@pp.inet.fi> */
		.name           = "Tekram M205 PRO",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs           = 2,
		.needs_tvaudio  = 0,
		.gpiomask       = 0x68,
		.muxsel         = { 2, 3, 1},
		.audiomux       = { 0x68, 0x68, 0x61, 0x61, 0x00 },
		.pll            = PLL_28,
	},

		/* ---- card 0x78 ---------------------------------- */
	[BTTV_BOARD_CONTVFMI] = {
		/* Javier Cendan Ares <jcendan@lycos.es> */
		/* bt878 TV + FM without subsystem ID */
		.name           = "Conceptronic CONTVFMi",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x008007,
		.muxsel         = { 2, 3, 1, 1 },
		.audiomux       = { 0, 1, 2, 2, 3 },
		.needs_tvaudio  = 0,
		.pll            = PLL_28,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
		.has_radio      = 1,
	},
	[BTTV_BOARD_PICOLO_TETRA_CHIP] = {
		/*Eric DEBIEF <debief@telemsa.com>*/
		/*EURESYS Picolo Tetra : 4 Conexant Fusion 878A, no audio, video input set with analog multiplexers GPIO controled*/
		/* adds picolo_tetra_muxsel(), picolo_tetra_init(), the folowing declaration strucure, and #define BTTV_BOARD_PICOLO_TETRA_CHIP*/
		/*0x79 in bttv.h*/
		.name           = "Euresys Picolo Tetra",
		.video_inputs   = 4,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.gpiomask       = 0,
		.gpiomask2      = 0x3C<<16,/*Set the GPIO[18]->GPIO[21] as output pin.==> drive the video inputs through analog multiplexers*/
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.muxsel         = {2,2,2,2},/*878A input is always MUX0, see above.*/
		.audiomux       = { 0, 0, 0, 0, 0, 0 }, /* card has no audio */
		.pll            = PLL_28,
		.needs_tvaudio  = 0,
		.muxsel_hook    = picolo_tetra_muxsel,/*Required as it doesn't follow the classic input selection policy*/
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_SPIRIT_TV] = {
		/* Spirit TV Tuner from http://spiritmodems.com.au */
		/* Stafford Goodsell <surge@goliath.homeunix.org> */
		.name           = "Spirit TV Tuner",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x0000000f,
		.muxsel         = { 2, 1, 1 },
		.audiomux       = { 0x02, 0x00, 0x00, 0x00, 0x00},
		.tuner_type     = TUNER_TEMIC_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
	},
	[BTTV_BOARD_AVDVBT_771] = {
		/* Wolfram Joost <wojo@frokaschwei.de> */
		.name           = "AVerMedia AVerTV DVB-T 771",
		.video_inputs   = 2,
		.svhs           = 1,
		.tuner          = -1,
		.tuner_type     = TUNER_ABSENT,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.muxsel         = { 3 , 3 },
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll            = PLL_28,
		.has_dvb        = 1,
		.no_gpioirq     = 1,
		.has_remote     = 1,
	},
		/* ---- card 0x7c ---------------------------------- */
	[BTTV_BOARD_AVDVBT_761] = {
		/* Matt Jesson <dvb@jesson.eclipse.co.uk> */
		/* Based on the Nebula card data - added remote and new card number - BTTV_BOARD_AVDVBT_761, see also ir-kbd-gpio.c */
		.name           = "AverMedia AverTV DVB-T 761",
		.video_inputs   = 2,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 3, 1, 2, 0}, /* Comp0, S-Video, ?, ? */
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll            = PLL_28,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_dvb        = 1,
		.no_gpioirq     = 1,
		.has_remote     = 1,
	},
	[BTTV_BOARD_MATRIX_VISIONSQ] = {
		/* andre.schwarz@matrix-vision.de */
		.name             = "MATRIX Vision Sigma-SQ",
		.video_inputs     = 16,
		.audio_inputs     = 0,
		.tuner            = -1,
		.svhs             = -1,
		.gpiomask         = 0x0,
		.muxsel           = { 2, 2, 2, 2, 2, 2, 2, 2,
				3, 3, 3, 3, 3, 3, 3, 3 },
		.muxsel_hook      = sigmaSQ_muxsel,
		.audiomux         = { 0 },
		.no_msp34xx       = 1,
		.pll              = PLL_28,
		.tuner_type       = -1,
		.tuner_addr	  = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_MATRIX_VISIONSLC] = {
		/* andre.schwarz@matrix-vision.de */
		.name             = "MATRIX Vision Sigma-SLC",
		.video_inputs     = 4,
		.audio_inputs     = 0,
		.tuner            = -1,
		.svhs             = -1,
		.gpiomask         = 0x0,
		.muxsel           = { 2, 2, 2, 2 },
		.muxsel_hook      = sigmaSLC_muxsel,
		.audiomux         = { 0 },
		.no_msp34xx       = 1,
		.pll              = PLL_28,
		.tuner_type       = -1,
		.tuner_addr	  = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
		/* BTTV_BOARD_APAC_VIEWCOMP */
	[BTTV_BOARD_APAC_VIEWCOMP] = {
		/* Attila Kondoros <attila.kondoros@chello.hu> */
		/* bt878 TV + FM 0x00000000 subsystem ID */
		.name           = "APAC Viewcomp 878(AMAX)",
		.video_inputs   = 2,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = -1,
		.gpiomask       = 0xFF,
		.muxsel         = { 2, 3, 1, 1},
		.audiomux       = { 2, 0, 0, 0, 10},
		.needs_tvaudio  = 0,
		.pll            = PLL_28,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,   /* miniremote works, see ir-kbd-gpio.c */
		.has_radio      = 1,   /* not every card has radio */
	},

		/* ---- card 0x80 ---------------------------------- */
	[BTTV_BOARD_DVICO_DVBT_LITE] = {
		/* Chris Pascoe <c.pascoe@itee.uq.edu.au> */
		.name           = "DViCO FusionHDTV DVB-T Lite",
		.tuner          = -1,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll            = PLL_28,
		.no_video       = 1,
		.has_dvb        = 1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
	[BTTV_BOARD_VGEAR_MYVCD] = {
		/* Steven <photon38@pchome.com.tw> */
		.name           = "V-Gear MyVCD",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x3f,
		.muxsel         = {2, 3, 1, 0},
		.audiomux       = {0x31, 0x31, 0x31, 0x31, 0x31, 0x31},
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio      = 0,
	#if 0
		.has_remote     = 1,
	#endif
	},
	[BTTV_BOARD_SUPER_TV] = {
		/* Rick C <cryptdragoon@gmail.com> */
		.name           = "Super TV Tuner",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.muxsel         = { 2, 3, 1, 0},
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask       = 0x008007,
		.audiomux       = { 0, 0x000001,0,0, 0},
		.needs_tvaudio  = 1,
		.has_radio      = 1,
	},
	[BTTV_BOARD_TIBET_CS16] = {
		/* Chris Fanning <video4linux@haydon.net> */
		.name           = "Tibet Systems 'Progress DVR' CS16",
		.video_inputs   = 16,
		.audio_inputs   = 0,
		.tuner          = -1,
		.svhs           = -1,
		.muxsel         = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
		.pll		= PLL_28,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432	= 1,
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.muxsel_hook    = tibetCS16_muxsel,
	},
	[BTTV_BOARD_KODICOM_4400R] = {
		/* Bill Brack <wbrack@mmm.com.hk> */
		/*
		* Note that, because of the card's wiring, the "master"
		* BT878A chip (i.e. the one which controls the analog switch
		* and must use this card type) is the 2nd one detected.  The
		* other 3 chips should use card type 0x85, whose description
		* follows this one.  There is a EEPROM on the card (which is
		* connected to the I2C of one of those other chips), but is
		* not currently handled.  There is also a facility for a
		* "monitor", which is also not currently implemented.
		*/
		.name           = "Kodicom 4400R (master)",
		.video_inputs	= 16,
		.audio_inputs	= 0,
		.tuner		= -1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs		= -1,
		/* GPIO bits 0-9 used for analog switch:
		*   00 - 03:	camera selector
		*   04 - 06:	channel (controller) selector
		*   07:	data (1->on, 0->off)
		*   08:	strobe
		*   09:	reset
		* bit 16 is input from sync separator for the channel
		*/
		.gpiomask	= 0x0003ff,
		.no_gpioirq     = 1,
		.muxsel		= { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 },
		.pll		= PLL_28,
		.no_msp34xx	= 1,
		.no_tda7432	= 1,
		.no_tda9875	= 1,
		.muxsel_hook	= kodicom4400r_muxsel,
	},
	[BTTV_BOARD_KODICOM_4400R_SL] = {
		/* Bill Brack <wbrack@mmm.com.hk> */
		/* Note that, for reasons unknown, the "master" BT878A chip (i.e. the
		* one which controls the analog switch, and must use the card type)
		* is the 2nd one detected.  The other 3 chips should use this card
		* type
		*/
		.name		= "Kodicom 4400R (slave)",
		.video_inputs	= 16,
		.audio_inputs	= 0,
		.tuner		= -1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.svhs		= -1,
		.gpiomask	= 0x010000,
		.no_gpioirq     = 1,
		.muxsel		= { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 },
		.pll		= PLL_28,
		.no_msp34xx	= 1,
		.no_tda7432	= 1,
		.no_tda9875	= 1,
		.muxsel_hook	= kodicom4400r_muxsel,
	},
		/* ---- card 0x86---------------------------------- */
	[BTTV_BOARD_ADLINK_RTV24] = {
		/* Michael Henson <mhenson@clarityvi.com> */
		/* Adlink RTV24 with special unlock codes */
		.name           = "Adlink RTV24",
		.video_inputs   = 4,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.muxsel         = { 2, 3, 1, 0},
		.tuner_type     = -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.pll            = PLL_28,
	},
		/* ---- card 0x87---------------------------------- */
	[BTTV_BOARD_DVICO_FUSIONHDTV_5_LITE] = {
		/* Michael Krufky <mkrufky@m1k.net> */
		.name           = "DViCO FusionHDTV 5 Lite",
		.tuner          = 0,
		.tuner_type     = TUNER_LG_TDVS_H062F,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.svhs           = 2,
		.muxsel		= { 2, 3, 1 },
		.gpiomask       = 0x00e00007,
		.audiomux       = { 0x00400005, 0, 0x00000001, 0, 0x00c00007, 0 },
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.has_dvb        = 1,
	},
		/* ---- card 0x88---------------------------------- */
	[BTTV_BOARD_ACORP_Y878F] = {
		/* Mauro Carvalho Chehab <mchehab@brturbo.com.br> */
		.name		= "Acorp Y878F",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x01fe00,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux       = { 0x001e00, 0, 0x018000, 0x014000, 0x002000, 0 },
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_YMEC_TVF66T5_B_DFF,
		.tuner_addr	= 0xc1 >>1,
		.radio_addr     = 0xc1 >>1,
		.has_radio	= 1,
	},
		/* ---- card 0x89 ---------------------------------- */
	[BTTV_BOARD_CONCEPTRONIC_CTVFMI2] = {
		.name           = "Conceptronic CTVFMi v2",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x001c0007,
		.muxsel         = { 2, 3, 1, 1 },
		.audiomux       = { 0, 1, 2, 2, 3 },
		.needs_tvaudio  = 0,
		.pll            = PLL_28,
		.tuner_type     = TUNER_TENA_9533_DI,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote     = 1,
		.has_radio      = 1,
	},
		/* ---- card 0x8a ---------------------------------- */
	[BTTV_BOARD_PV_BT878P_2E] = {
		.name          = "Prolink Pixelview PV-BT878P+ (Rev.2E)",
		.video_inputs  = 5,
		.audio_inputs  = 1,
		.tuner         = 0,
		.svhs          = 3,
		.gpiomask      = 0x01fe00,
		.muxsel        = { 2,3,1,1,-1 },
		.digital_mode  = DIGITAL_MODE_CAMERA,
		.audiomux      = { 0x00400, 0x10400, 0x04400, 0x80000, 0x12400, 0x46000  },
		.no_msp34xx    = 1,
		.pll           = PLL_28,
		.tuner_type    = TUNER_LG_PAL_FM,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_remote    = 1,
	},
		/* ---- card 0x8b ---------------------------------- */
	[BTTV_BOARD_PV_M4900] = {
		/* S�rgio Fortier <sergiofortier@yahoo.com.br> */
		.name           = "Prolink PixelView PlayTV MPEG2 PV-M4900",
		.video_inputs   = 3,
		.audio_inputs   = 1,
		.tuner          = 0,
		.svhs           = 2,
		.gpiomask       = 0x3f,
		.muxsel         = { 2, 3, 1, 1 },
		.audiomux       = { 0x21, 0x20, 0x24, 0x2c, 0x29, 0x29 },
		.no_msp34xx     = 1,
		.pll            = PLL_28,
		.tuner_type     = TUNER_YMEC_TVF_5533MF,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.has_radio      = 1,
		.has_remote     = 1,
	},
		/* ---- card 0x8c ---------------------------------- */
	[BTTV_BOARD_OSPREY440]  = {
		.name           = "Osprey 440",
		.video_inputs   = 1,
		.audio_inputs   = 1,
		.tuner          = -1,
		.svhs           = 1,
		.muxsel         = { 2 },
		.pll            = PLL_28,
		.tuner_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.no_msp34xx     = 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
	},
		/* ---- card 0x8d ---------------------------------- */
	[BTTV_BOARD_ASOUND_SKYEYE] = {
		.name		= "Asound Skyeye PCTV",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 15,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= {2,0,0,0,1},
		.needs_tvaudio	= 1,
		.pll		= PLL_28,
		.tuner_type	= 2,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
		/* ---- card 0x8e ---------------------------------- */
	[BTTV_BOARD_SABRENT_TVFM] = {
		.name		= "Sabrent TV-FM (bttv version)",
		.video_inputs	= 3,
		.audio_inputs	= 1,
		.tuner		= 0,
		.svhs		= 2,
		.gpiomask	= 0x108007,
		.muxsel		= { 2, 3, 1, 1},
		.audiomux	= { 100000, 100002, 100002, 100000},
		.no_msp34xx	= 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.pll		= PLL_28,
		.tuner_type	= TUNER_TNF_5335MF,
		.tuner_addr	= ADDR_UNSET,
		.has_radio      = 1,
	},
	/* ---- card 0x8f ---------------------------------- */
	[BTTV_BOARD_HAUPPAUGE_IMPACTVCB] = {
		.name		= "Hauppauge ImpactVCB (bt878)",
		.video_inputs	= 4,
		.audio_inputs	= 0,
		.tuner		= -1,
		.svhs		= -1,
		.gpiomask	= 0x0f, /* old: 7 */
		.muxsel		= { 0, 1, 3, 2}, /* Composite 0-3 */
		.no_msp34xx	= 1,
		.no_tda9875     = 1,
		.no_tda7432     = 1,
		.tuner_type	= -1,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
	},
};

static const unsigned int bttv_num_tvcards = ARRAY_SIZE(bttv_tvcards);

/* ----------------------------------------------------------------------- */

static unsigned char eeprom_data[256];

/*
 * identify card
 */
void __devinit bttv_idcard(struct bttv *btv)
{
	unsigned int gpiobits;
	int i,type;
	unsigned short tmp;

	/* read PCI subsystem ID */
	pci_read_config_word(btv->c.pci, PCI_SUBSYSTEM_ID, &tmp);
	btv->cardid = tmp << 16;
	pci_read_config_word(btv->c.pci, PCI_SUBSYSTEM_VENDOR_ID, &tmp);
	btv->cardid |= tmp;

	if (0 != btv->cardid && 0xffffffff != btv->cardid) {
		/* look for the card */
		for (type = -1, i = 0; cards[i].id != 0; i++)
			if (cards[i].id  == btv->cardid)
				type = i;

		if (type != -1) {
			/* found it */
			printk(KERN_INFO "bttv%d: detected: %s [card=%d], "
			       "PCI subsystem ID is %04x:%04x\n",
			       btv->c.nr,cards[type].name,cards[type].cardnr,
			       btv->cardid & 0xffff,
			       (btv->cardid >> 16) & 0xffff);
			btv->c.type = cards[type].cardnr;
		} else {
			/* 404 */
			printk(KERN_INFO "bttv%d: subsystem: %04x:%04x (UNKNOWN)\n",
			       btv->c.nr, btv->cardid & 0xffff,
			       (btv->cardid >> 16) & 0xffff);
			printk(KERN_DEBUG "please mail id, board name and "
			       "the correct card= insmod option to video4linux-list@redhat.com\n");
		}
	}

	/* let the user override the autodetected type */
	if (card[btv->c.nr] < bttv_num_tvcards)
		btv->c.type=card[btv->c.nr];

	/* print which card config we are using */
	printk(KERN_INFO "bttv%d: using: %s [card=%d,%s]\n",btv->c.nr,
	       bttv_tvcards[btv->c.type].name, btv->c.type,
	       card[btv->c.nr] < bttv_num_tvcards
	       ? "insmod option" : "autodetected");

	/* overwrite gpio stuff ?? */
	if (UNSET == audioall && UNSET == audiomux[0])
		return;

	if (UNSET != audiomux[0]) {
		gpiobits = 0;
		for (i = 0; i < 5; i++) {
			bttv_tvcards[btv->c.type].audiomux[i] = audiomux[i];
			gpiobits |= audiomux[i];
		}
	} else {
		gpiobits = audioall;
		for (i = 0; i < 5; i++) {
			bttv_tvcards[btv->c.type].audiomux[i] = audioall;
		}
	}
	bttv_tvcards[btv->c.type].gpiomask = (UNSET != gpiomask) ? gpiomask : gpiobits;
	printk(KERN_INFO "bttv%d: gpio config override: mask=0x%x, mux=",
	       btv->c.nr,bttv_tvcards[btv->c.type].gpiomask);
	for (i = 0; i < 5; i++) {
		printk("%s0x%x", i ? "," : "", bttv_tvcards[btv->c.type].audiomux[i]);
	}
	printk("\n");
}

/*
 * (most) board specific initialisations goes here
 */

/* Some Modular Technology cards have an eeprom, but no subsystem ID */
static void identify_by_eeprom(struct bttv *btv, unsigned char eeprom_data[256])
{
	int type = -1;

	if (0 == strncmp(eeprom_data,"GET MM20xPCTV",13))
		type = BTTV_BOARD_MODTEC_205;
	else if (0 == strncmp(eeprom_data+20,"Picolo",7))
		type = BTTV_BOARD_EURESYS_PICOLO;
	else if (eeprom_data[0] == 0x84 && eeprom_data[2]== 0)
		type = BTTV_BOARD_HAUPPAUGE; /* old bt848 */

	if (-1 != type) {
		btv->c.type = type;
		printk("bttv%d: detected by eeprom: %s [card=%d]\n",
		       btv->c.nr, bttv_tvcards[btv->c.type].name, btv->c.type);
	}
}

static void flyvideo_gpio(struct bttv *btv)
{
	int gpio,has_remote,has_radio,is_capture_only,is_lr90,has_tda9820_tda9821;
	int tuner=-1,ttype;

	gpio_inout(0xffffff, 0);
	udelay(8);  /* without this we would see the 0x1800 mask */
	gpio = gpio_read();
	/* FIXME: must restore OUR_EN ??? */

	/* all cards provide GPIO info, some have an additional eeprom
	 * LR50: GPIO coding can be found lower right CP1 .. CP9
	 *       CP9=GPIO23 .. CP1=GPIO15; when OPEN, the corresponding GPIO reads 1.
	 *       GPIO14-12: n.c.
	 * LR90: GP9=GPIO23 .. GP1=GPIO15 (right above the bt878)

	 * lowest 3 bytes are remote control codes (no handshake needed)
	 * xxxFFF: No remote control chip soldered
	 * xxxF00(LR26/LR50), xxxFE0(LR90): Remote control chip (LVA001 or CF45) soldered
	 * Note: Some bits are Audio_Mask !
	 */
	ttype=(gpio&0x0f0000)>>16;
	switch(ttype) {
	case 0x0: tuner=2; /* NTSC, e.g. TPI8NSR11P */
		break;
	case 0x2: tuner=39;/* LG NTSC (newer TAPC series) TAPC-H701P */
		break;
	case 0x4: tuner=5; /* Philips PAL TPI8PSB02P, TPI8PSB12P, TPI8PSB12D or FI1216, FM1216 */
		break;
	case 0x6: tuner=37;/* LG PAL (newer TAPC series) TAPC-G702P */
		break;
		case 0xC: tuner=3; /* Philips SECAM(+PAL) FQ1216ME or FI1216MF */
		break;
	default:
		printk(KERN_INFO "bttv%d: FlyVideo_gpio: unknown tuner type.\n", btv->c.nr);
	}

	has_remote          =   gpio & 0x800000;
	has_radio	    =   gpio & 0x400000;
	/*   unknown                   0x200000;
	 *   unknown2                  0x100000; */
	is_capture_only     = !(gpio & 0x008000); /* GPIO15 */
	has_tda9820_tda9821 = !(gpio & 0x004000);
	is_lr90             = !(gpio & 0x002000); /* else LR26/LR50 (LR38/LR51 f. capture only) */
	/*
	 * gpio & 0x001000    output bit for audio routing */

	if(is_capture_only)
		tuner=4; /* No tuner present */

	printk(KERN_INFO "bttv%d: FlyVideo Radio=%s RemoteControl=%s Tuner=%d gpio=0x%06x\n",
	       btv->c.nr, has_radio? "yes":"no ", has_remote? "yes":"no ", tuner, gpio);
	printk(KERN_INFO "bttv%d: FlyVideo  LR90=%s tda9821/tda9820=%s capture_only=%s\n",
		btv->c.nr, is_lr90?"yes":"no ", has_tda9820_tda9821?"yes":"no ",
		is_capture_only?"yes":"no ");

	if(tuner!= -1) /* only set if known tuner autodetected, else let insmod option through */
		btv->tuner_type = tuner;
	btv->has_radio = has_radio;

	/* LR90 Audio Routing is done by 2 hef4052, so Audio_Mask has 4 bits: 0x001c80
	 * LR26/LR50 only has 1 hef4052, Audio_Mask 0x000c00
	 * Audio options: from tuner, from tda9821/tda9821(mono,stereo,sap), from tda9874, ext., mute */
	if(has_tda9820_tda9821) btv->audio_hook = lt9415_audio;
	/* todo: if(has_tda9874) btv->audio_hook = fv2000s_audio; */
}

static int miro_tunermap[] = { 0,6,2,3,   4,5,6,0,  3,0,4,5,  5,2,16,1,
			       14,2,17,1, 4,1,4,3,  1,2,16,1, 4,4,4,4 };
static int miro_fmtuner[]  = { 0,0,0,0,   0,0,0,0,  0,0,0,0,  0,0,0,1,
			       1,1,1,1,   1,1,1,0,  0,0,0,0,  0,1,0,0 };

static void miro_pinnacle_gpio(struct bttv *btv)
{
	int id,msp,gpio;
	char *info;

	gpio_inout(0xffffff, 0);
	gpio = gpio_read();
	id   = ((gpio>>10) & 63) -1;
	msp  = bttv_I2CRead(btv, I2C_MSP3400, "MSP34xx");
	if (id < 32) {
		btv->tuner_type = miro_tunermap[id];
		if (0 == (gpio & 0x20)) {
			btv->has_radio = 1;
			if (!miro_fmtuner[id]) {
				btv->has_matchbox = 1;
				btv->mbox_we    = (1<<6);
				btv->mbox_most  = (1<<7);
				btv->mbox_clk   = (1<<8);
				btv->mbox_data  = (1<<9);
				btv->mbox_mask  = (1<<6)|(1<<7)|(1<<8)|(1<<9);
			}
		} else {
			btv->has_radio = 0;
		}
		if (-1 != msp) {
			if (btv->c.type == BTTV_BOARD_MIRO)
				btv->c.type = BTTV_BOARD_MIROPRO;
			if (btv->c.type == BTTV_BOARD_PINNACLE)
				btv->c.type = BTTV_BOARD_PINNACLEPRO;
		}
		printk(KERN_INFO
		       "bttv%d: miro: id=%d tuner=%d radio=%s stereo=%s\n",
		       btv->c.nr, id+1, btv->tuner_type,
		       !btv->has_radio ? "no" :
		       (btv->has_matchbox ? "matchbox" : "fmtuner"),
		       (-1 == msp) ? "no" : "yes");
	} else {
		/* new cards with microtune tuner */
		id = 63 - id;
		btv->has_radio = 0;
		switch (id) {
		case 1:
			info = "PAL / mono";
			btv->tda9887_conf = TDA9887_INTERCARRIER;
			break;
		case 2:
			info = "PAL+SECAM / stereo";
			btv->has_radio = 1;
			btv->tda9887_conf = TDA9887_QSS;
			break;
		case 3:
			info = "NTSC / stereo";
			btv->has_radio = 1;
			btv->tda9887_conf = TDA9887_QSS;
			break;
		case 4:
			info = "PAL+SECAM / mono";
			btv->tda9887_conf = TDA9887_QSS;
			break;
		case 5:
			info = "NTSC / mono";
			btv->tda9887_conf = TDA9887_INTERCARRIER;
			break;
		case 6:
			info = "NTSC / stereo";
			btv->tda9887_conf = TDA9887_INTERCARRIER;
			break;
		case 7:
			info = "PAL / stereo";
			btv->tda9887_conf = TDA9887_INTERCARRIER;
			break;
		default:
			info = "oops: unknown card";
			break;
		}
		if (-1 != msp)
			btv->c.type = BTTV_BOARD_PINNACLEPRO;
		printk(KERN_INFO
		       "bttv%d: pinnacle/mt: id=%d info=\"%s\" radio=%s\n",
		       btv->c.nr, id, info, btv->has_radio ? "yes" : "no");
		btv->tuner_type = TUNER_MT2032;
	}
}

/* GPIO21   L: Buffer aktiv, H: Buffer inaktiv */
#define LM1882_SYNC_DRIVE     0x200000L

static void init_ids_eagle(struct bttv *btv)
{
	gpio_inout(0xffffff,0xFFFF37);
	gpio_write(0x200020);

	/* flash strobe inverter ?! */
	gpio_write(0x200024);

	/* switch sync drive off */
	gpio_bits(LM1882_SYNC_DRIVE,LM1882_SYNC_DRIVE);

	/* set BT848 muxel to 2 */
	btaor((2)<<5, ~(2<<5), BT848_IFORM);
}

/* Muxsel helper for the IDS Eagle.
 * the eagles does not use the standard muxsel-bits but
 * has its own multiplexer */
static void eagle_muxsel(struct bttv *btv, unsigned int input)
{
	btaor((2)<<5, ~(3<<5), BT848_IFORM);
	gpio_bits(3,bttv_tvcards[btv->c.type].muxsel[input&7]);

       /* composite */
       /* set chroma ADC to sleep */
       btor(BT848_ADC_C_SLEEP, BT848_ADC);
       /* set to composite video */
       btand(~BT848_CONTROL_COMP, BT848_E_CONTROL);
       btand(~BT848_CONTROL_COMP, BT848_O_CONTROL);

       /* switch sync drive off */
       gpio_bits(LM1882_SYNC_DRIVE,LM1882_SYNC_DRIVE);
}

static void gvc1100_muxsel(struct bttv *btv, unsigned int input)
{
	static const int masks[] = {0x30, 0x01, 0x12, 0x23};
	gpio_write(masks[input%4]);
}

/* LMLBT4x initialization - to allow access to GPIO bits for sensors input and
   alarms output

   GPIObit    | 10 | 9 | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
   assignment | TI | O3|INx| O2| O1|IN4|IN3|IN2|IN1|   |   |

   IN - sensor inputs, INx - sensor inputs and TI XORed together
   O1,O2,O3 - alarm outputs (relays)

   OUT ENABLE   1    1   0  . 1  1   0   0 . 0   0   0    0   = 0x6C0

*/

static void init_lmlbt4x(struct bttv *btv)
{
	printk(KERN_DEBUG "LMLBT4x init\n");
	btwrite(0x000000, BT848_GPIO_REG_INP);
	gpio_inout(0xffffff, 0x0006C0);
	gpio_write(0x000000);
}

static void sigmaSQ_muxsel(struct bttv *btv, unsigned int input)
{
	unsigned int inmux = input % 8;
	gpio_inout( 0xf, 0xf );
	gpio_bits( 0xf, inmux );
}

static void sigmaSLC_muxsel(struct bttv *btv, unsigned int input)
{
	unsigned int inmux = input % 4;
	gpio_inout( 3<<9, 3<<9 );
	gpio_bits( 3<<9, inmux<<9 );
}

/* ----------------------------------------------------------------------- */

static void bttv_reset_audio(struct bttv *btv)
{
	/*
	 * BT878A has a audio-reset register.
	 * 1. This register is an audio reset function but it is in
	 *    function-0 (video capture) address space.
	 * 2. It is enough to do this once per power-up of the card.
	 * 3. There is a typo in the Conexant doc -- it is not at
	 *    0x5B, but at 0x058. (B is an odd-number, obviously a typo!).
	 * --//Shrikumar 030609
	 */
	if (btv->id != 878)
		return;

	if (bttv_debug)
		printk("bttv%d: BT878A ARESET\n",btv->c.nr);
	btwrite((1<<7), 0x058);
	udelay(10);
	btwrite(     0, 0x058);
}

/* initialization part one -- before registering i2c bus */
void __devinit bttv_init_card1(struct bttv *btv)
{
	switch (btv->c.type) {
	case BTTV_BOARD_HAUPPAUGE:
	case BTTV_BOARD_HAUPPAUGE878:
		boot_msp34xx(btv,5);
		break;
	case BTTV_BOARD_VOODOOTV_FM:
		boot_msp34xx(btv,20);
		break;
	case BTTV_BOARD_AVERMEDIA98:
		boot_msp34xx(btv,11);
		break;
	case BTTV_BOARD_HAUPPAUGEPVR:
		pvr_boot(btv);
		break;
	case BTTV_BOARD_TWINHAN_DST:
	case BTTV_BOARD_AVDVBT_771:
	case BTTV_BOARD_PINNACLESAT:
		btv->use_i2c_hw = 1;
		break;
	case BTTV_BOARD_ADLINK_RTV24:
		init_RTV24( btv );
		break;

	}
	if (!bttv_tvcards[btv->c.type].has_dvb)
		bttv_reset_audio(btv);
}

/* initialization part two -- after registering i2c bus */
void __devinit bttv_init_card2(struct bttv *btv)
{
	int tda9887;
	int addr=ADDR_UNSET;

	btv->tuner_type = -1;

	if (BTTV_BOARD_UNKNOWN == btv->c.type) {
		bttv_readee(btv,eeprom_data,0xa0);
		identify_by_eeprom(btv,eeprom_data);
	}

	switch (btv->c.type) {
	case BTTV_BOARD_MIRO:
	case BTTV_BOARD_MIROPRO:
	case BTTV_BOARD_PINNACLE:
	case BTTV_BOARD_PINNACLEPRO:
		/* miro/pinnacle */
		miro_pinnacle_gpio(btv);
		break;
	case BTTV_BOARD_FLYVIDEO_98:
	case BTTV_BOARD_MAXI:
	case BTTV_BOARD_LIFE_FLYKIT:
	case BTTV_BOARD_FLYVIDEO:
	case BTTV_BOARD_TYPHOON_TVIEW:
	case BTTV_BOARD_CHRONOS_VS2:
	case BTTV_BOARD_FLYVIDEO_98FM:
	case BTTV_BOARD_FLYVIDEO2000:
	case BTTV_BOARD_FLYVIDEO98EZ:
	case BTTV_BOARD_CONFERENCETV:
	case BTTV_BOARD_LIFETEC_9415:
		flyvideo_gpio(btv);
		break;
	case BTTV_BOARD_HAUPPAUGE:
	case BTTV_BOARD_HAUPPAUGE878:
	case BTTV_BOARD_HAUPPAUGEPVR:
		/* pick up some config infos from the eeprom */
		bttv_readee(btv,eeprom_data,0xa0);
		hauppauge_eeprom(btv);
		break;
	case BTTV_BOARD_AVERMEDIA98:
	case BTTV_BOARD_AVPHONE98:
		bttv_readee(btv,eeprom_data,0xa0);
		avermedia_eeprom(btv);
		break;
	case BTTV_BOARD_PXC200:
		init_PXC200(btv);
		break;
	case BTTV_BOARD_PICOLO_TETRA_CHIP:
		picolo_tetra_init(btv);
		break;
	case BTTV_BOARD_VHX:
		btv->has_radio    = 1;
		btv->has_matchbox = 1;
		btv->mbox_we      = 0x20;
		btv->mbox_most    = 0;
		btv->mbox_clk     = 0x08;
		btv->mbox_data    = 0x10;
		btv->mbox_mask    = 0x38;
		break;
	case BTTV_BOARD_VOBIS_BOOSTAR:
	case BTTV_BOARD_TERRATV:
		terratec_active_radio_upgrade(btv);
		break;
	case BTTV_BOARD_MAGICTVIEW061:
		if (btv->cardid == 0x3002144f) {
			btv->has_radio=1;
			printk("bttv%d: radio detected by subsystem id (CPH05x)\n",btv->c.nr);
		}
		break;
       case BTTV_BOARD_STB2:
		if (btv->cardid == 0x3060121a) {
			/* Fix up entry for 3DFX VoodooTV 100,
			   which is an OEM STB card variant. */
			btv->has_radio=0;
			btv->tuner_type=TUNER_TEMIC_NTSC;
		}
		break;
	case BTTV_BOARD_OSPREY1x0:
	case BTTV_BOARD_OSPREY1x0_848:
	case BTTV_BOARD_OSPREY101_848:
	case BTTV_BOARD_OSPREY1x1:
	case BTTV_BOARD_OSPREY1x1_SVID:
	case BTTV_BOARD_OSPREY2xx:
	case BTTV_BOARD_OSPREY2x0_SVID:
	case BTTV_BOARD_OSPREY2x0:
	case BTTV_BOARD_OSPREY500:
	case BTTV_BOARD_OSPREY540:
	case BTTV_BOARD_OSPREY2000:
		bttv_readee(btv,eeprom_data,0xa0);
		osprey_eeprom(btv);
		break;
	case BTTV_BOARD_IDS_EAGLE:
		init_ids_eagle(btv);
		break;
	case BTTV_BOARD_MODTEC_205:
		bttv_readee(btv,eeprom_data,0xa0);
		modtec_eeprom(btv);
		break;
	case BTTV_BOARD_LMLBT4:
		init_lmlbt4x(btv);
		break;
	case BTTV_BOARD_TIBET_CS16:
		tibetCS16_init(btv);
		break;
	case BTTV_BOARD_KODICOM_4400R:
		kodicom4400r_init(btv);
		break;
	}

	/* pll configuration */
	if (!(btv->id==848 && btv->revision==0x11)) {
		/* defaults from card list */
		if (PLL_28 == bttv_tvcards[btv->c.type].pll) {
			btv->pll.pll_ifreq=28636363;
			btv->pll.pll_crystal=BT848_IFORM_XT0;
		}
		if (PLL_35 == bttv_tvcards[btv->c.type].pll) {
			btv->pll.pll_ifreq=35468950;
			btv->pll.pll_crystal=BT848_IFORM_XT1;
		}
		/* insmod options can override */
		switch (pll[btv->c.nr]) {
		case 0: /* none */
			btv->pll.pll_crystal = 0;
			btv->pll.pll_ifreq   = 0;
			btv->pll.pll_ofreq   = 0;
			break;
		case 1: /* 28 MHz */
		case 28:
			btv->pll.pll_ifreq   = 28636363;
			btv->pll.pll_ofreq   = 0;
			btv->pll.pll_crystal = BT848_IFORM_XT0;
			break;
		case 2: /* 35 MHz */
		case 35:
			btv->pll.pll_ifreq   = 35468950;
			btv->pll.pll_ofreq   = 0;
			btv->pll.pll_crystal = BT848_IFORM_XT1;
			break;
		}
	}
	btv->pll.pll_current = -1;

	/* tuner configuration (from card list / autodetect / insmod option) */
	if (ADDR_UNSET != bttv_tvcards[btv->c.type].tuner_addr)
		addr = bttv_tvcards[btv->c.type].tuner_addr;

	if (UNSET != bttv_tvcards[btv->c.type].tuner_type)
		if(UNSET == btv->tuner_type)
			btv->tuner_type = bttv_tvcards[btv->c.type].tuner_type;
	if (UNSET != tuner[btv->c.nr])
		btv->tuner_type = tuner[btv->c.nr];
	printk("bttv%d: using tuner=%d\n",btv->c.nr,btv->tuner_type);

	if (btv->tuner_type != UNSET) {
		struct tuner_setup tun_setup;

		tun_setup.mode_mask = T_ANALOG_TV | T_DIGITAL_TV;
		tun_setup.type = btv->tuner_type;
		tun_setup.addr = addr;

		bttv_call_i2c_clients(btv, TUNER_SET_TYPE_ADDR, &tun_setup);
	}

	if (btv->tda9887_conf) {
		bttv_call_i2c_clients(btv, TDA9887_SET_CONFIG,
							&btv->tda9887_conf);
	}

	btv->svhs = bttv_tvcards[btv->c.type].svhs;
	if (svhs[btv->c.nr] != UNSET)
		btv->svhs = svhs[btv->c.nr];
	if (remote[btv->c.nr] != UNSET)
		btv->has_remote = remote[btv->c.nr];

	if (bttv_tvcards[btv->c.type].has_radio)
		btv->has_radio=1;
	if (bttv_tvcards[btv->c.type].has_remote)
		btv->has_remote=1;
	if (!bttv_tvcards[btv->c.type].no_gpioirq)
		btv->gpioirq=1;
	if (bttv_tvcards[btv->c.type].audio_hook)
		btv->audio_hook=bttv_tvcards[btv->c.type].audio_hook;

	if (bttv_tvcards[btv->c.type].digital_mode == DIGITAL_MODE_CAMERA) {
		/* detect Bt832 chip for quartzsight digital camera */
		if ((bttv_I2CRead(btv, I2C_BT832_ALT1, "Bt832") >=0) ||
		    (bttv_I2CRead(btv, I2C_BT832_ALT2, "Bt832") >=0))
			boot_bt832(btv);
	}

	if (!autoload)
		return;

	/* try to detect audio/fader chips */
	if (!bttv_tvcards[btv->c.type].no_msp34xx &&
	    bttv_I2CRead(btv, I2C_MSP3400, "MSP34xx") >=0)
		request_module("msp3400");

	if (bttv_tvcards[btv->c.type].msp34xx_alt &&
	    bttv_I2CRead(btv, I2C_MSP3400_ALT, "MSP34xx (alternate address)") >=0)
		request_module("msp3400");

	if (!bttv_tvcards[btv->c.type].no_tda9875 &&
	    bttv_I2CRead(btv, I2C_TDA9875, "TDA9875") >=0)
		request_module("tda9875");

	if (!bttv_tvcards[btv->c.type].no_tda7432 &&
	    bttv_I2CRead(btv, I2C_TDA7432, "TDA7432") >=0)
		request_module("tda7432");

	if (bttv_tvcards[btv->c.type].needs_tvaudio)
		request_module("tvaudio");

	/* tuner modules */
	tda9887 = 0;
	if (btv->tda9887_conf)
		tda9887 = 1;
	if (0 == tda9887 && 0 == bttv_tvcards[btv->c.type].has_dvb &&
	    bttv_I2CRead(btv, I2C_TDA9887, "TDA9887") >=0)
		tda9887 = 1;
	/* Hybrid DVB card, DOES have a tda9887 */
	if (btv->c.type == BTTV_BOARD_DVICO_FUSIONHDTV_5_LITE)
		tda9887 = 1;
	if((btv->tuner_type == TUNER_PHILIPS_FM1216ME_MK3) ||
	   (btv->tuner_type == TUNER_PHILIPS_FM1236_MK3) ||
	   (btv->tuner_type == TUNER_PHILIPS_FM1256_IH3) ||
	    tda9887)
		request_module("tda9887");
	if (btv->tuner_type != UNSET)
		request_module("tuner");
}


/* ----------------------------------------------------------------------- */

static void modtec_eeprom(struct bttv *btv)
{
	if( strncmp(&(eeprom_data[0x1e]),"Temic 4066 FY5",14) ==0) {
		btv->tuner_type=TUNER_TEMIC_4066FY5_PAL_I;
		printk("bttv%d: Modtec: Tuner autodetected by eeprom: %s\n",
		       btv->c.nr,&eeprom_data[0x1e]);
	} else if (strncmp(&(eeprom_data[0x1e]),"Alps TSBB5",10) ==0) {
		btv->tuner_type=TUNER_ALPS_TSBB5_PAL_I;
		printk("bttv%d: Modtec: Tuner autodetected by eeprom: %s\n",
		       btv->c.nr,&eeprom_data[0x1e]);
	} else if (strncmp(&(eeprom_data[0x1e]),"Philips FM1246",14) ==0) {
		btv->tuner_type=TUNER_PHILIPS_NTSC;
		printk("bttv%d: Modtec: Tuner autodetected by eeprom: %s\n",
		       btv->c.nr,&eeprom_data[0x1e]);
	} else {
		printk("bttv%d: Modtec: Unknown TunerString: %s\n",
		       btv->c.nr,&eeprom_data[0x1e]);
	}
}

static void __devinit hauppauge_eeprom(struct bttv *btv)
{
	struct tveeprom tv;

	tveeprom_hauppauge_analog(&btv->i2c_client, &tv, eeprom_data);
	btv->tuner_type = tv.tuner_type;
	btv->has_radio  = tv.has_radio;

	printk("bttv%d: Hauppauge eeprom indicates model#%d\n",
		btv->c.nr, tv.model);

	/*
	 * Some of the 878 boards have duplicate PCI IDs. Switch the board
	 * type based on model #.
	 */
	if(tv.model == 64900) {
		printk("bttv%d: Switching board type from %s to %s\n",
			btv->c.nr,
			bttv_tvcards[btv->c.type].name,
			bttv_tvcards[BTTV_BOARD_HAUPPAUGE_IMPACTVCB].name);
		btv->c.type = BTTV_BOARD_HAUPPAUGE_IMPACTVCB;
	}
}

static int terratec_active_radio_upgrade(struct bttv *btv)
{
	int freq;

	btv->has_radio    = 1;
	btv->has_matchbox = 1;
	btv->mbox_we      = 0x10;
	btv->mbox_most    = 0x20;
	btv->mbox_clk     = 0x08;
	btv->mbox_data    = 0x04;
	btv->mbox_mask    = 0x3c;

	btv->mbox_iow     = 1 <<  8;
	btv->mbox_ior     = 1 <<  9;
	btv->mbox_csel    = 1 << 10;

	freq=88000/62.5;
	tea5757_write(btv, 5 * freq + 0x358); /* write 0x1ed8 */
	if (0x1ed8 == tea5757_read(btv)) {
		printk("bttv%d: Terratec Active Radio Upgrade found.\n",
		       btv->c.nr);
		btv->has_radio    = 1;
		btv->has_matchbox = 1;
	} else {
		btv->has_radio    = 0;
		btv->has_matchbox = 0;
	}
	return 0;
}


/* ----------------------------------------------------------------------- */

/*
 * minimal bootstrap for the WinTV/PVR -- upload altera firmware.
 *
 * The hcwamc.rbf firmware file is on the Hauppauge driver CD.  Have
 * a look at Pvr/pvr45xxx.EXE (self-extracting zip archive, can be
 * unpacked with unzip).
 */
#define PVR_GPIO_DELAY		10

#define BTTV_ALT_DATA		0x000001
#define BTTV_ALT_DCLK		0x100000
#define BTTV_ALT_NCONFIG	0x800000

static int __devinit pvr_altera_load(struct bttv *btv, u8 *micro, u32 microlen)
{
	u32 n;
	u8 bits;
	int i;

	gpio_inout(0xffffff,BTTV_ALT_DATA|BTTV_ALT_DCLK|BTTV_ALT_NCONFIG);
	gpio_write(0);
	udelay(PVR_GPIO_DELAY);

	gpio_write(BTTV_ALT_NCONFIG);
	udelay(PVR_GPIO_DELAY);

	for (n = 0; n < microlen; n++) {
		bits = micro[n];
		for ( i = 0 ; i < 8 ; i++ ) {
			gpio_bits(BTTV_ALT_DCLK,0);
			if (bits & 0x01)
				gpio_bits(BTTV_ALT_DATA,BTTV_ALT_DATA);
			else
				gpio_bits(BTTV_ALT_DATA,0);
			gpio_bits(BTTV_ALT_DCLK,BTTV_ALT_DCLK);
			bits >>= 1;
		}
	}
	gpio_bits(BTTV_ALT_DCLK,0);
	udelay(PVR_GPIO_DELAY);

	/* begin Altera init loop (Not necessary,but doesn't hurt) */
	for (i = 0 ; i < 30 ; i++) {
		gpio_bits(BTTV_ALT_DCLK,0);
		gpio_bits(BTTV_ALT_DCLK,BTTV_ALT_DCLK);
	}
	gpio_bits(BTTV_ALT_DCLK,0);
	return 0;
}

static int __devinit pvr_boot(struct bttv *btv)
{
	const struct firmware *fw_entry;
	int rc;

	rc = request_firmware(&fw_entry, "hcwamc.rbf", &btv->c.pci->dev);
	if (rc != 0) {
		printk(KERN_WARNING "bttv%d: no altera firmware [via hotplug]\n",
		       btv->c.nr);
		return rc;
	}
	rc = pvr_altera_load(btv, fw_entry->data, fw_entry->size);
	printk(KERN_INFO "bttv%d: altera firmware upload %s\n",
	       btv->c.nr, (rc < 0) ? "failed" : "ok");
	release_firmware(fw_entry);
	return rc;
}

/* ----------------------------------------------------------------------- */
/* some osprey specific stuff                                              */

static void __devinit osprey_eeprom(struct bttv *btv)
{
       int i = 0;
       unsigned char *ee = eeprom_data;
       unsigned long serial = 0;

       if (btv->c.type == 0) {
	       /* this might be an antique... check for MMAC label in eeprom */
	       if ((ee[0]=='M') && (ee[1]=='M') && (ee[2]=='A') && (ee[3]=='C')) {
		       unsigned char checksum = 0;
		       for (i =0; i<21; i++)
			       checksum += ee[i];
		       if (checksum != ee[21])
			       return;
		       btv->c.type = BTTV_BOARD_OSPREY1x0_848;
		       for (i = 12; i < 21; i++)
			       serial *= 10, serial += ee[i] - '0';
	       }
       } else {
	       unsigned short type;
	       int offset = 4*16;

	       for(; offset < 8*16; offset += 16) {
		       unsigned short checksum = 0;
		       /* verify the checksum */
		       for(i = 0; i<14; i++) checksum += ee[i+offset];
			       checksum = ~checksum;  /* no idea why */
			       if ((((checksum>>8)&0x0FF) == ee[offset+14]) &&
				   ((checksum & 0x0FF) == ee[offset+15])) {
			       break;
		       }
	       }

	       if (offset >= 8*16)
		       return;

	       /* found a valid descriptor */
	       type = (ee[offset+4]<<8) | (ee[offset+5]);

	       switch(type) {

	       /* 848 based */
	       case 0x0004:
		       btv->c.type = BTTV_BOARD_OSPREY1x0_848;
		       break;
	       case 0x0005:
		       btv->c.type = BTTV_BOARD_OSPREY101_848;
		       break;

	       /* 878 based */
	       case 0x0012:
	       case 0x0013:
		       btv->c.type = BTTV_BOARD_OSPREY1x0;
		       break;
	       case 0x0014:
	       case 0x0015:
		       btv->c.type = BTTV_BOARD_OSPREY1x1;
		       break;
	       case 0x0016:
	       case 0x0017:
	       case 0x0020:
		       btv->c.type = BTTV_BOARD_OSPREY1x1_SVID;
		       break;
	       case 0x0018:
	       case 0x0019:
	       case 0x001E:
	       case 0x001F:
		       btv->c.type = BTTV_BOARD_OSPREY2xx;
		       break;
	       case 0x001A:
	       case 0x001B:
		       btv->c.type = BTTV_BOARD_OSPREY2x0_SVID;
		       break;
	       case 0x0040:
		       btv->c.type = BTTV_BOARD_OSPREY500;
		       break;
	       case 0x0050:
	       case 0x0056:
		       btv->c.type = BTTV_BOARD_OSPREY540;
		       /* bttv_osprey_540_init(btv); */
		       break;
	       case 0x0060:
	       case 0x0070:
		       btv->c.type = BTTV_BOARD_OSPREY2x0;
		       /* enable output on select control lines */
		       gpio_inout(0xffffff,0x000303);
		       break;
	       default:
		       /* unknown...leave generic, but get serial # */
		       break;
	       }
	       serial =  (ee[offset+6] << 24)
		       | (ee[offset+7] << 16)
		       | (ee[offset+8] <<  8)
		       | (ee[offset+9]);
       }

       printk(KERN_INFO "bttv%d: osprey eeprom: card=%d name=%s serial=%ld\n",
	      btv->c.nr, btv->c.type, bttv_tvcards[btv->c.type].name,serial);
}

/* ----------------------------------------------------------------------- */
/* AVermedia specific stuff, from  bktr_card.c                             */

static int tuner_0_table[] = {
	TUNER_PHILIPS_NTSC,  TUNER_PHILIPS_PAL /* PAL-BG*/,
	TUNER_PHILIPS_PAL,   TUNER_PHILIPS_PAL /* PAL-I*/,
	TUNER_PHILIPS_PAL,   TUNER_PHILIPS_PAL,
	TUNER_PHILIPS_SECAM, TUNER_PHILIPS_SECAM,
	TUNER_PHILIPS_SECAM, TUNER_PHILIPS_PAL,
	TUNER_PHILIPS_FM1216ME_MK3 };

static int tuner_1_table[] = {
	TUNER_TEMIC_NTSC,  TUNER_TEMIC_PAL,
	TUNER_TEMIC_PAL,   TUNER_TEMIC_PAL,
	TUNER_TEMIC_PAL,   TUNER_TEMIC_PAL,
	TUNER_TEMIC_4012FY5, TUNER_TEMIC_4012FY5, /* TUNER_TEMIC_SECAM */
	TUNER_TEMIC_4012FY5, TUNER_TEMIC_PAL};

static void __devinit avermedia_eeprom(struct bttv *btv)
{
	int tuner_make,tuner_tv_fm,tuner_format,tuner=0;

	tuner_make      = (eeprom_data[0x41] & 0x7);
	tuner_tv_fm     = (eeprom_data[0x41] & 0x18) >> 3;
	tuner_format    = (eeprom_data[0x42] & 0xf0) >> 4;
	btv->has_remote = (eeprom_data[0x42] & 0x01);

	if (tuner_make == 0 || tuner_make == 2)
		if(tuner_format <=0x0a)
			tuner = tuner_0_table[tuner_format];
	if (tuner_make == 1)
		if(tuner_format <=9)
			tuner = tuner_1_table[tuner_format];

	if (tuner_make == 4)
		if(tuner_format == 0x09)
			tuner = TUNER_LG_NTSC_NEW_TAPC; /* TAPC-G702P */

	printk(KERN_INFO "bttv%d: Avermedia eeprom[0x%02x%02x]: tuner=",
		btv->c.nr,eeprom_data[0x41],eeprom_data[0x42]);
	if(tuner) {
		btv->tuner_type=tuner;
		printk("%d",tuner);
	} else
		printk("Unknown type");
	printk(" radio:%s remote control:%s\n",
	       tuner_tv_fm     ? "yes" : "no",
	       btv->has_remote ? "yes" : "no");
}

/* used on Voodoo TV/FM (Voodoo 200), S0 wired to 0x10000 */
void bttv_tda9880_setnorm(struct bttv *btv, int norm)
{
	/* fix up our card entry */
	if(norm==VIDEO_MODE_NTSC) {
		bttv_tvcards[BTTV_BOARD_VOODOOTV_FM].audiomux[0]=0x957fff;
		bttv_tvcards[BTTV_BOARD_VOODOOTV_FM].audiomux[4]=0x957fff;
		dprintk("bttv_tda9880_setnorm to NTSC\n");
	}
	else {
		bttv_tvcards[BTTV_BOARD_VOODOOTV_FM].audiomux[0]=0x947fff;
		bttv_tvcards[BTTV_BOARD_VOODOOTV_FM].audiomux[4]=0x947fff;
		dprintk("bttv_tda9880_setnorm to PAL\n");
	}
	/* set GPIO according */
	gpio_bits(bttv_tvcards[btv->c.type].gpiomask,
		  bttv_tvcards[btv->c.type].audiomux[btv->audio]);
}


/*
 * reset/enable the MSP on some Hauppauge cards
 * Thanks to Kyösti Mälkki (kmalkki@cc.hut.fi)!
 *
 * Hauppauge:  pin  5
 * Voodoo:     pin 20
 */
static void __devinit boot_msp34xx(struct bttv *btv, int pin)
{
	int mask = (1 << pin);

	gpio_inout(mask,mask);
	gpio_bits(mask,0);
	udelay(2500);
	gpio_bits(mask,mask);

	if (bttv_gpio)
		bttv_gpio_tracking(btv,"msp34xx");
	if (bttv_verbose)
		printk(KERN_INFO "bttv%d: Hauppauge/Voodoo msp34xx: reset line "
		       "init [%d]\n", btv->c.nr, pin);
}

static void __devinit boot_bt832(struct bttv *btv)
{
}

/* ----------------------------------------------------------------------- */
/*  Imagenation L-Model PXC200 Framegrabber */
/*  This is basically the same procedure as
 *  used by Alessandro Rubini in his pxc200
 *  driver, but using BTTV functions */

static void __devinit init_PXC200(struct bttv *btv)
{
	static int vals[] __devinitdata = { 0x08, 0x09, 0x0a, 0x0b, 0x0d, 0x0d,
					    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
					    0x00 };
	unsigned int i;
	int tmp;
	u32 val;

	/* Initialise GPIO-connevted stuff */
	gpio_inout(0xffffff, (1<<13));
	gpio_write(0);
	udelay(3);
	gpio_write(1<<13);
	/* GPIO inputs are pulled up, so no need to drive
	 * reset pin any longer */
	gpio_bits(0xffffff, 0);
	if (bttv_gpio)
		bttv_gpio_tracking(btv,"pxc200");

	/*  we could/should try and reset/control the AD pots? but
	    right now  we simply  turned off the crushing.  Without
	    this the AGC drifts drifts
	    remember the EN is reverse logic -->
	    setting BT848_ADC_AGC_EN disable the AGC
	    tboult@eecs.lehigh.edu
	*/

	btwrite(BT848_ADC_RESERVED|BT848_ADC_AGC_EN, BT848_ADC);

	/*	Initialise MAX517 DAC */
	printk(KERN_INFO "Setting DAC reference voltage level ...\n");
	bttv_I2CWrite(btv,0x5E,0,0x80,1);

	/*	Initialise 12C508 PIC */
	/*	The I2CWrite and I2CRead commmands are actually to the
	 *	same chips - but the R/W bit is included in the address
	 *	argument so the numbers are different */


	printk(KERN_INFO "Initialising 12C508 PIC chip ...\n");

	/* First of all, enable the clock line. This is used in the PXC200-F */
	val = btread(BT848_GPIO_DMA_CTL);
	val |= BT848_GPIO_DMA_CTL_GPCLKMODE;
	btwrite(val, BT848_GPIO_DMA_CTL);

	/* Then, push to 0 the reset pin long enough to reset the *
	 * device same as above for the reset line, but not the same
	 * value sent to the GPIO-connected stuff
	 * which one is the good one? */
	gpio_inout(0xffffff,(1<<2));
	gpio_write(0);
	udelay(10);
	gpio_write(1<<2);

	for (i = 0; i < ARRAY_SIZE(vals); i++) {
		tmp=bttv_I2CWrite(btv,0x1E,0,vals[i],1);
		if (tmp != -1) {
			printk(KERN_INFO
			       "I2C Write(%2.2x) = %i\nI2C Read () = %2.2x\n\n",
			       vals[i],tmp,bttv_I2CRead(btv,0x1F,NULL));
		}
	}

	printk(KERN_INFO "PXC200 Initialised.\n");
}



/* ----------------------------------------------------------------------- */
/*
 *  The Adlink RTV-24 (aka Angelo) has some special initialisation to unlock
 *  it. This apparently involves the following procedure for each 878 chip:
 *
 *  1) write 0x00C3FEFF to the GPIO_OUT_EN register
 *
 *  2)  write to GPIO_DATA
 *      - 0x0E
 *      - sleep 1ms
 *      - 0x10 + 0x0E
 *      - sleep 10ms
 *      - 0x0E
 *     read from GPIO_DATA into buf (uint_32)
 *      - if ( data>>18 & 0x01 != 0) || ( buf>>19 & 0x01 != 1 )
 *                 error. ERROR_CPLD_Check_Failed stop.
 *
 *  3) write to GPIO_DATA
 *      - write 0x4400 + 0x0E
 *      - sleep 10ms
 *      - write 0x4410 + 0x0E
 *      - sleep 1ms
 *      - write 0x0E
 *     read from GPIO_DATA into buf (uint_32)
 *      - if ( buf>>18 & 0x01 ) || ( buf>>19 && 0x01 != 0 )
 *                error. ERROR_CPLD_Check_Failed.
 */
/* ----------------------------------------------------------------------- */
static void
init_RTV24 (struct bttv *btv)
{
	uint32_t dataRead = 0;
	long watchdog_value = 0x0E;

	printk (KERN_INFO
		"bttv%d: Adlink RTV-24 initialisation in progress ...\n",
		btv->c.nr);

	btwrite (0x00c3feff, BT848_GPIO_OUT_EN);

	btwrite (0 + watchdog_value, BT848_GPIO_DATA);
	msleep (1);
	btwrite (0x10 + watchdog_value, BT848_GPIO_DATA);
	msleep (10);
	btwrite (0 + watchdog_value, BT848_GPIO_DATA);

	dataRead = btread (BT848_GPIO_DATA);

	if ((((dataRead >> 18) & 0x01) != 0) || (((dataRead >> 19) & 0x01) != 1)) {
		printk (KERN_INFO
			"bttv%d: Adlink RTV-24 initialisation(1) ERROR_CPLD_Check_Failed (read %d)\n",
			btv->c.nr, dataRead);
	}

	btwrite (0x4400 + watchdog_value, BT848_GPIO_DATA);
	msleep (10);
	btwrite (0x4410 + watchdog_value, BT848_GPIO_DATA);
	msleep (1);
	btwrite (watchdog_value, BT848_GPIO_DATA);
	msleep (1);
	dataRead = btread (BT848_GPIO_DATA);

	if ((((dataRead >> 18) & 0x01) != 0) || (((dataRead >> 19) & 0x01) != 0)) {
		printk (KERN_INFO
			"bttv%d: Adlink RTV-24 initialisation(2) ERROR_CPLD_Check_Failed (read %d)\n",
			btv->c.nr, dataRead);

		return;
	}

	printk (KERN_INFO
		"bttv%d: Adlink RTV-24 initialisation complete.\n", btv->c.nr);
}



/* ----------------------------------------------------------------------- */
/* Miro Pro radio stuff -- the tea5757 is connected to some GPIO ports     */
/*
 * Copyright (c) 1999 Csaba Halasz <qgehali@uni-miskolc.hu>
 * This code is placed under the terms of the GNU General Public License
 *
 * Brutally hacked by Dan Sheridan <dan.sheridan@contact.org.uk> djs52 8/3/00
 */

static void bus_low(struct bttv *btv, int bit)
{
	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_ior | btv->mbox_iow | btv->mbox_csel,
			  btv->mbox_ior | btv->mbox_iow | btv->mbox_csel);
		udelay(5);
	}

	gpio_bits(bit,0);
	udelay(5);

	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_iow | btv->mbox_csel, 0);
		udelay(5);
	}
}

static void bus_high(struct bttv *btv, int bit)
{
	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_ior | btv->mbox_iow | btv->mbox_csel,
			  btv->mbox_ior | btv->mbox_iow | btv->mbox_csel);
		udelay(5);
	}

	gpio_bits(bit,bit);
	udelay(5);

	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_iow | btv->mbox_csel, 0);
		udelay(5);
	}
}

static int bus_in(struct bttv *btv, int bit)
{
	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_ior | btv->mbox_iow | btv->mbox_csel,
			  btv->mbox_ior | btv->mbox_iow | btv->mbox_csel);
		udelay(5);

		gpio_bits(btv->mbox_iow | btv->mbox_csel, 0);
		udelay(5);
	}
	return gpio_read() & (bit);
}

/* TEA5757 register bits */
#define TEA_FREQ		0:14
#define TEA_BUFFER		15:15

#define TEA_SIGNAL_STRENGTH	16:17

#define TEA_PORT1		18:18
#define TEA_PORT0		19:19

#define TEA_BAND		20:21
#define TEA_BAND_FM		0
#define TEA_BAND_MW		1
#define TEA_BAND_LW		2
#define TEA_BAND_SW		3

#define TEA_MONO		22:22
#define TEA_ALLOW_STEREO	0
#define TEA_FORCE_MONO		1

#define TEA_SEARCH_DIRECTION	23:23
#define TEA_SEARCH_DOWN		0
#define TEA_SEARCH_UP		1

#define TEA_STATUS		24:24
#define TEA_STATUS_TUNED	0
#define TEA_STATUS_SEARCHING	1

/* Low-level stuff */
static int tea5757_read(struct bttv *btv)
{
	unsigned long timeout;
	int value = 0;
	int i;

	/* better safe than sorry */
	gpio_inout(btv->mbox_mask, btv->mbox_clk | btv->mbox_we);

	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_ior | btv->mbox_iow | btv->mbox_csel,
			  btv->mbox_ior | btv->mbox_iow | btv->mbox_csel);
		udelay(5);
	}

	if (bttv_gpio)
		bttv_gpio_tracking(btv,"tea5757 read");

	bus_low(btv,btv->mbox_we);
	bus_low(btv,btv->mbox_clk);

	udelay(10);
	timeout= jiffies + HZ;

	/* wait for DATA line to go low; error if it doesn't */
	while (bus_in(btv,btv->mbox_data) && time_before(jiffies, timeout))
		schedule();
	if (bus_in(btv,btv->mbox_data)) {
		printk(KERN_WARNING "bttv%d: tea5757: read timeout\n",btv->c.nr);
		return -1;
	}

	dprintk("bttv%d: tea5757:",btv->c.nr);
	for(i = 0; i < 24; i++)
	{
		udelay(5);
		bus_high(btv,btv->mbox_clk);
		udelay(5);
		dprintk("%c",(bus_in(btv,btv->mbox_most) == 0)?'T':'-');
		bus_low(btv,btv->mbox_clk);
		value <<= 1;
		value |= (bus_in(btv,btv->mbox_data) == 0)?0:1;  /* MSB first */
		dprintk("%c", (bus_in(btv,btv->mbox_most) == 0)?'S':'M');
	}
	dprintk("\nbttv%d: tea5757: read 0x%X\n", btv->c.nr, value);
	return value;
}

static int tea5757_write(struct bttv *btv, int value)
{
	int i;
	int reg = value;

	gpio_inout(btv->mbox_mask, btv->mbox_clk | btv->mbox_we | btv->mbox_data);

	if (btv->mbox_ior) {
		gpio_bits(btv->mbox_ior | btv->mbox_iow | btv->mbox_csel,
			  btv->mbox_ior | btv->mbox_iow | btv->mbox_csel);
		udelay(5);
	}
	if (bttv_gpio)
		bttv_gpio_tracking(btv,"tea5757 write");

	dprintk("bttv%d: tea5757: write 0x%X\n", btv->c.nr, value);
	bus_low(btv,btv->mbox_clk);
	bus_high(btv,btv->mbox_we);
	for(i = 0; i < 25; i++)
	{
		if (reg & 0x1000000)
			bus_high(btv,btv->mbox_data);
		else
			bus_low(btv,btv->mbox_data);
		reg <<= 1;
		bus_high(btv,btv->mbox_clk);
		udelay(10);
		bus_low(btv,btv->mbox_clk);
		udelay(10);
	}
	bus_low(btv,btv->mbox_we);  /* unmute !!! */
	return 0;
}

void tea5757_set_freq(struct bttv *btv, unsigned short freq)
{
	dprintk("tea5757_set_freq %d\n",freq);
	tea5757_write(btv, 5 * freq + 0x358); /* add 10.7MHz (see docs) */
}


/* ----------------------------------------------------------------------- */
/* winview                                                                 */

static void winview_audio(struct bttv *btv, struct video_audio *v, int set)
{
	/* PT2254A programming Jon Tombs, jon@gte.esi.us.es */
	int bits_out, loops, vol, data;

	if (!set) {
		/* Fixed by Leandro Lucarella <luca@linuxmendoza.org.ar (07/31/01) */
		v->flags |= VIDEO_AUDIO_VOLUME;
		return;
	}

	/* 32 levels logarithmic */
	vol = 32 - ((v->volume>>11));
	/* units */
	bits_out = (PT2254_DBS_IN_2>>(vol%5));
	/* tens */
	bits_out |= (PT2254_DBS_IN_10>>(vol/5));
	bits_out |= PT2254_L_CHANNEL | PT2254_R_CHANNEL;
	data = gpio_read();
	data &= ~(WINVIEW_PT2254_CLK| WINVIEW_PT2254_DATA|
		  WINVIEW_PT2254_STROBE);
	for (loops = 17; loops >= 0 ; loops--) {
		if (bits_out & (1<<loops))
			data |=  WINVIEW_PT2254_DATA;
		else
			data &= ~WINVIEW_PT2254_DATA;
		gpio_write(data);
		udelay(5);
		data |= WINVIEW_PT2254_CLK;
		gpio_write(data);
		udelay(5);
		data &= ~WINVIEW_PT2254_CLK;
		gpio_write(data);
	}
	data |=  WINVIEW_PT2254_STROBE;
	data &= ~WINVIEW_PT2254_DATA;
	gpio_write(data);
	udelay(10);
	data &= ~WINVIEW_PT2254_STROBE;
	gpio_write(data);
}

/* ----------------------------------------------------------------------- */
/* mono/stereo control for various cards (which don't use i2c chips but    */
/* connect something to the GPIO pins                                      */

static void
gvbctv3pci_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int con = 0;

	if (set) {
		gpio_inout(0x300, 0x300);
		if (v->mode & VIDEO_SOUND_LANG1)
			con = 0x000;
		if (v->mode & VIDEO_SOUND_LANG2)
			con = 0x300;
		if (v->mode & VIDEO_SOUND_STEREO)
			con = 0x200;
/*		if (v->mode & VIDEO_SOUND_MONO)
 *			con = 0x100; */
		gpio_bits(0x300, con);
	} else {
		v->mode = VIDEO_SOUND_STEREO |
			  VIDEO_SOUND_LANG1  | VIDEO_SOUND_LANG2;
	}
}

static void
gvbctv5pci_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int val, con;

	if (btv->radio_user)
		return;

	val = gpio_read();
	if (set) {
		con = 0x000;
		if (v->mode & VIDEO_SOUND_LANG2) {
			if (v->mode & VIDEO_SOUND_LANG1) {
				/* LANG1 + LANG2 */
				con = 0x100;
			}
			else {
				/* LANG2 */
				con = 0x300;
			}
		}
		if (con != (val & 0x300)) {
			gpio_bits(0x300, con);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"gvbctv5pci");
		}
	} else {
		switch (val & 0x70) {
		  case 0x10:
			v->mode = VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
			break;
		  case 0x30:
			v->mode = VIDEO_SOUND_LANG2;
			break;
		  case 0x50:
			v->mode = VIDEO_SOUND_LANG1;
			break;
		  case 0x60:
			v->mode = VIDEO_SOUND_STEREO;
			break;
		  case 0x70:
			v->mode = VIDEO_SOUND_MONO;
			break;
		  default:
			v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
				  VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
		}
	}
}

/*
 * Mario Medina Nussbaum <medisoft@alohabbs.org.mx>
 *  I discover that on BT848_GPIO_DATA address a byte 0xcce enable stereo,
 *  0xdde enables mono and 0xccd enables sap
 *
 * Petr Vandrovec <VANDROVE@vc.cvut.cz>
 *  P.S.: At least mask in line above is wrong - GPIO pins 3,2 select
 *  input/output sound connection, so both must be set for output mode.
 *
 * Looks like it's needed only for the "tvphone", the "tvphone 98"
 * handles this with a tda9840
 *
 */
static void
avermedia_tvphone_audio(struct bttv *btv, struct video_audio *v, int set)
{
	int val = 0;

	if (set) {
		if (v->mode & VIDEO_SOUND_LANG2)   /* SAP */
			val = 0x02;
		if (v->mode & VIDEO_SOUND_STEREO)
			val = 0x01;
		if (val) {
			gpio_bits(0x03,val);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"avermedia");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1;
		return;
	}
}

static void
avermedia_tv_stereo_audio(struct bttv *btv, struct video_audio *v, int set)
{
	int val = 0;

	if (set) {
		if (v->mode & VIDEO_SOUND_LANG2)   /* SAP */
			val = 0x01;
		if (v->mode & VIDEO_SOUND_STEREO)  /* STEREO */
			val = 0x02;
		btaor(val, ~0x03, BT848_GPIO_DATA);
		if (bttv_gpio)
			bttv_gpio_tracking(btv,"avermedia");
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
		return;
	}
}

/* Lifetec 9415 handling */
static void
lt9415_audio(struct bttv *btv, struct video_audio *v, int set)
{
	int val = 0;

	if (gpio_read() & 0x4000) {
		v->mode = VIDEO_SOUND_MONO;
		return;
	}

	if (set) {
		if (v->mode & VIDEO_SOUND_LANG2)  /* A2 SAP */
			val = 0x0080;
		if (v->mode & VIDEO_SOUND_STEREO) /* A2 stereo */
			val = 0x0880;
		if ((v->mode & VIDEO_SOUND_LANG1) ||
		    (v->mode & VIDEO_SOUND_MONO))
			val = 0;
		gpio_bits(0x0880, val);
		if (bttv_gpio)
			bttv_gpio_tracking(btv,"lt9415");
	} else {
		/* autodetect doesn't work with this card :-( */
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
		return;
	}
}

/* TDA9821 on TerraTV+ Bt848, Bt878 */
static void
terratv_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int con = 0;

	if (set) {
		gpio_inout(0x180000,0x180000);
		if (v->mode & VIDEO_SOUND_LANG2)
			con = 0x080000;
		if (v->mode & VIDEO_SOUND_STEREO)
			con = 0x180000;
		gpio_bits(0x180000, con);
		if (bttv_gpio)
			bttv_gpio_tracking(btv,"terratv");
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	}
}

static void
winfast2000_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned long val = 0;

	if (set) {
		/*btor (0xc32000, BT848_GPIO_OUT_EN);*/
		if (v->mode & VIDEO_SOUND_MONO)		/* Mono */
			val = 0x420000;
		if (v->mode & VIDEO_SOUND_LANG1)	/* Mono */
			val = 0x420000;
		if (v->mode & VIDEO_SOUND_LANG2)	/* SAP */
			val = 0x410000;
		if (v->mode & VIDEO_SOUND_STEREO)	/* Stereo */
			val = 0x020000;
		if (val) {
			gpio_bits(0x430000, val);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"winfast2000");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			  VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	}
}

/*
 * Dariusz Kowalewski <darekk@automex.pl>
 * sound control for Prolink PV-BT878P+9B (PixelView PlayTV Pro FM+NICAM
 * revision 9B has on-board TDA9874A sound decoder).
 *
 * Note: There are card variants without tda9874a. Forcing the "stereo sound route"
 *       will mute this cards.
 */
static void
pvbt878p9b_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int val = 0;

	if (btv->radio_user)
		return;

	if (set) {
		if (v->mode & VIDEO_SOUND_MONO)	{
			val = 0x01;
		}
		if ((v->mode & (VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2))
		    || (v->mode & VIDEO_SOUND_STEREO)) {
			val = 0x02;
		}
		if (val) {
			gpio_bits(0x03,val);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"pvbt878p9b");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	}
}

/*
 * Dariusz Kowalewski <darekk@automex.pl>
 * sound control for FlyVideo 2000S (with tda9874 decoder)
 * based on pvbt878p9b_audio() - this is not tested, please fix!!!
 */
static void
fv2000s_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int val = 0xffff;

	if (btv->radio_user)
		return;
	if (set) {
		if (v->mode & VIDEO_SOUND_MONO)	{
			val = 0x0000;
		}
		if ((v->mode & (VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2))
		    || (v->mode & VIDEO_SOUND_STEREO)) {
			val = 0x1080; /*-dk-???: 0x0880, 0x0080, 0x1800 ... */
		}
		if (val != 0xffff) {
			gpio_bits(0x1800, val);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"fv2000s");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	}
}

/*
 * sound control for Canopus WinDVR PCI
 * Masaki Suzuki <masaki@btree.org>
 */
static void
windvr_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned long val = 0;

	if (set) {
		if (v->mode & VIDEO_SOUND_MONO)
			val = 0x040000;
		if (v->mode & VIDEO_SOUND_LANG1)
			val = 0;
		if (v->mode & VIDEO_SOUND_LANG2)
			val = 0x100000;
		if (v->mode & VIDEO_SOUND_STEREO)
			val = 0;
		if (val) {
			gpio_bits(0x140000, val);
			if (bttv_gpio)
				bttv_gpio_tracking(btv,"windvr");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			  VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	}
}

/*
 * sound control for AD-TVK503
 * Hiroshi Takekawa <sian@big.or.jp>
 */
static void
adtvk503_audio(struct bttv *btv, struct video_audio *v, int set)
{
	unsigned int con = 0xffffff;

	/* btaor(0x1e0000, ~0x1e0000, BT848_GPIO_OUT_EN); */

	if (set) {
		/* btor(***, BT848_GPIO_OUT_EN); */
		if (v->mode & VIDEO_SOUND_LANG1)
			con = 0x00000000;
		if (v->mode & VIDEO_SOUND_LANG2)
			con = 0x00180000;
		if (v->mode & VIDEO_SOUND_STEREO)
			con = 0x00000000;
		if (v->mode & VIDEO_SOUND_MONO)
			con = 0x00060000;
		if (con != 0xffffff) {
			gpio_bits(0x1e0000,con);
			if (bttv_gpio)
				bttv_gpio_tracking(btv, "adtvk503");
		}
	} else {
		v->mode = VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
			  VIDEO_SOUND_LANG1  | VIDEO_SOUND_LANG2;
	}
}

/* RemoteVision MX (rv605) muxsel helper [Miguel Freitas]
 *
 * This is needed because rv605 don't use a normal multiplex, but a crosspoint
 * switch instead (CD22M3494E). This IC can have multiple active connections
 * between Xn (input) and Yn (output) pins. We need to clear any existing
 * connection prior to establish a new one, pulsing the STROBE pin.
 *
 * The board hardwire Y0 (xpoint) to MUX1 and MUXOUT to Yin.
 * GPIO pins are wired as:
 *  GPIO[0:3] - AX[0:3] (xpoint) - P1[0:3] (microcontroler)
 *  GPIO[4:6] - AY[0:2] (xpoint) - P1[4:6] (microcontroler)
 *  GPIO[7]   - DATA (xpoint)    - P1[7] (microcontroler)
 *  GPIO[8]   -                  - P3[5] (microcontroler)
 *  GPIO[9]   - RESET (xpoint)   - P3[6] (microcontroler)
 *  GPIO[10]  - STROBE (xpoint)  - P3[7] (microcontroler)
 *  GPINTR    -                  - P3[4] (microcontroler)
 *
 * The microcontroler is a 80C32 like. It should be possible to change xpoint
 * configuration either directly (as we are doing) or using the microcontroler
 * which is also wired to I2C interface. I have no further info on the
 * microcontroler features, one would need to disassembly the firmware.
 * note: the vendor refused to give any information on this product, all
 *       that stuff was found using a multimeter! :)
 */
static void rv605_muxsel(struct bttv *btv, unsigned int input)
{
	/* reset all conections */
	gpio_bits(0x200,0x200);
	mdelay(1);
	gpio_bits(0x200,0x000);
	mdelay(1);

	/* create a new conection */
	gpio_bits(0x480,0x080);
	gpio_bits(0x480,0x480);
	mdelay(1);
	gpio_bits(0x480,0x080);
	mdelay(1);
}

/* Tibet Systems 'Progress DVR' CS16 muxsel helper [Chris Fanning]
 *
 * The CS16 (available on eBay cheap) is a PCI board with four Fusion
 * 878A chips, a PCI bridge, an Atmel microcontroller, four sync seperator
 * chips, ten eight input analog multiplexors, a not chip and a few
 * other components.
 *
 * 16 inputs on a secondary bracket are provided and can be selected
 * from each of the four capture chips.  Two of the eight input
 * multiplexors are used to select from any of the 16 input signals.
 *
 * Unsupported hardware capabilities:
 *  . A video output monitor on the secondary bracket can be selected from
 *    one of the 878A chips.
 *  . Another passthrough but I haven't spent any time investigating it.
 *  . Digital I/O (logic level connected to GPIO) is available from an
 *    onboard header.
 *
 * The on chip input mux should always be set to 2.
 * GPIO[16:19] - Video input selection
 * GPIO[0:3]   - Video output monitor select (only available from one 878A)
 * GPIO[?:?]   - Digital I/O.
 *
 * There is an ATMEL microcontroller with an 8031 core on board.  I have not
 * determined what function (if any) it provides.  With the microcontroller
 * and sync seperator chips a guess is that it might have to do with video
 * switching and maybe some digital I/O.
 */
static void tibetCS16_muxsel(struct bttv *btv, unsigned int input)
{
	/* video mux */
	gpio_bits(0x0f0000, input << 16);
}

static void tibetCS16_init(struct bttv *btv)
{
	/* enable gpio bits, mask obtained via btSpy */
	gpio_inout(0xffffff, 0x0f7fff);
	gpio_write(0x0f7fff);
}

/*
 * The following routines for the Kodicom-4400r get a little mind-twisting.
 * There is a "master" controller and three "slave" controllers, together
 * an analog switch which connects any of 16 cameras to any of the BT87A's.
 * The analog switch is controlled by the "master", but the detection order
 * of the four BT878A chips is in an order which I just don't understand.
 * The "master" is actually the second controller to be detected.  The
 * logic on the board uses logical numbers for the 4 controlers, but
 * those numbers are different from the detection sequence.  When working
 * with the analog switch, we need to "map" from the detection sequence
 * over to the board's logical controller number.  This mapping sequence
 * is {3, 0, 2, 1}, i.e. the first controller to be detected is logical
 * unit 3, the second (which is the master) is logical unit 0, etc.
 * We need to maintain the status of the analog switch (which of the 16
 * cameras is connected to which of the 4 controllers).  Rather than
 * add to the bttv structure for this, we use the data reserved for
 * the mbox (unused for this card type).
 */

/*
 * First a routine to set the analog switch, which controls which camera
 * is routed to which controller.  The switch comprises an X-address
 * (gpio bits 0-3, representing the camera, ranging from 0-15), and a
 * Y-address (gpio bits 4-6, representing the controller, ranging from 0-3).
 * A data value (gpio bit 7) of '1' enables the switch, and '0' disables
 * the switch.  A STROBE bit (gpio bit 8) latches the data value into the
 * specified address.  The idea is to set the address and data, then bring
 * STROBE high, and finally bring STROBE back to low.
 */
static void kodicom4400r_write(struct bttv *btv,
			       unsigned char xaddr,
			       unsigned char yaddr,
			       unsigned char data) {
	unsigned int udata;

	udata = (data << 7) | ((yaddr&3) << 4) | (xaddr&0xf);
	gpio_bits(0x1ff, udata);		/* write ADDR and DAT */
	gpio_bits(0x1ff, udata | (1 << 8));	/* strobe high */
	gpio_bits(0x1ff, udata);		/* strobe low */
}

/*
 * Next the mux select.  Both the "master" and "slave" 'cards' (controllers)
 * use this routine.  The routine finds the "master" for the card, maps
 * the controller number from the detected position over to the logical
 * number, writes the appropriate data to the analog switch, and housekeeps
 * the local copy of the switch information.  The parameter 'input' is the
 * requested camera number (0 - 15).
 */
static void kodicom4400r_muxsel(struct bttv *btv, unsigned int input)
{
	char *sw_status;
	int xaddr, yaddr;
	struct bttv *mctlr;
	static unsigned char map[4] = {3, 0, 2, 1};

	mctlr = master[btv->c.nr];
	if (mctlr == NULL) {	/* ignore if master not yet detected */
		return;
	}
	yaddr = (btv->c.nr - mctlr->c.nr + 1) & 3; /* the '&' is for safety */
	yaddr = map[yaddr];
	sw_status = (char *)(&mctlr->mbox_we);
	xaddr = input & 0xf;
	/* Check if the controller/camera pair has changed, else ignore */
	if (sw_status[yaddr] != xaddr)
	{
		/* "open" the old switch, "close" the new one, save the new */
		kodicom4400r_write(mctlr, sw_status[yaddr], yaddr, 0);
		sw_status[yaddr] = xaddr;
		kodicom4400r_write(mctlr, xaddr, yaddr, 1);
	}
}

/*
 * During initialisation, we need to reset the analog switch.  We
 * also preset the switch to map the 4 connectors on the card to the
 * *user's* (see above description of kodicom4400r_muxsel) channels
 * 0 through 3
 */
static void kodicom4400r_init(struct bttv *btv)
{
	char *sw_status = (char *)(&btv->mbox_we);
	int ix;

	gpio_inout(0x0003ff, 0x0003ff);
	gpio_write(1 << 9);	/* reset MUX */
	gpio_write(0);
	/* Preset camera 0 to the 4 controllers */
	for (ix=0; ix<4; ix++) {
		sw_status[ix] = ix;
		kodicom4400r_write(btv, ix, ix, 1);
	}
	/*
	 * Since this is the "master", we need to set up the
	 * other three controller chips' pointers to this structure
	 * for later use in the muxsel routine.
	 */
	if ((btv->c.nr<1) || (btv->c.nr>BTTV_MAX-3))
	    return;
	master[btv->c.nr-1] = btv;
	master[btv->c.nr]   = btv;
	master[btv->c.nr+1] = btv;
	master[btv->c.nr+2] = btv;
}

/* The Grandtec X-Guard framegrabber card uses two Dual 4-channel
 * video multiplexers to provide up to 16 video inputs. These
 * multiplexers are controlled by the lower 8 GPIO pins of the
 * bt878. The multiplexers probably Pericom PI5V331Q or similar.

 * xxx0 is pin xxx of multiplexer U5,
 * yyy1 is pin yyy of multiplexer U2
 */
#define ENA0    0x01
#define ENB0    0x02
#define ENA1    0x04
#define ENB1    0x08

#define IN10    0x10
#define IN00    0x20
#define IN11    0x40
#define IN01    0x80

static void xguard_muxsel(struct bttv *btv, unsigned int input)
{
	static const int masks[] = {
		ENB0, ENB0|IN00, ENB0|IN10, ENB0|IN00|IN10,
		ENA0, ENA0|IN00, ENA0|IN10, ENA0|IN00|IN10,
		ENB1, ENB1|IN01, ENB1|IN11, ENB1|IN01|IN11,
		ENA1, ENA1|IN01, ENA1|IN11, ENA1|IN01|IN11,
	};
	gpio_write(masks[input%16]);
}
static void picolo_tetra_init(struct bttv *btv)
{
	/*This is the video input redirection fonctionality : I DID NOT USED IT. */
	btwrite (0x08<<16,BT848_GPIO_DATA);/*GPIO[19] [==> 4053 B+C] set to 1 */
	btwrite (0x04<<16,BT848_GPIO_DATA);/*GPIO[18] [==> 4053 A]  set to 1*/
}
static void picolo_tetra_muxsel (struct bttv* btv, unsigned int input)
{

	dprintk (KERN_DEBUG "bttv%d : picolo_tetra_muxsel =>  input = %d\n",btv->c.nr,input);
	/*Just set the right path in the analog multiplexers : channel 1 -> 4 ==> Analog Mux ==> MUX0*/
	/*GPIO[20]&GPIO[21] used to choose the right input*/
	btwrite (input<<20,BT848_GPIO_DATA);

}

/*
 * ivc120_muxsel [Added by Alan Garfield <alan@fromorbit.com>]
 *
 * The IVC120G security card has 4 i2c controlled TDA8540 matrix
 * swichers to provide 16 channels to MUX0. The TDA8540's have
 * 4 indepedant outputs and as such the IVC120G also has the
 * optional "Monitor Out" bus. This allows the card to be looking
 * at one input while the monitor is looking at another.
 *
 * Since I've couldn't be bothered figuring out how to add an
 * independant muxsel for the monitor bus, I've just set it to
 * whatever the card is looking at.
 *
 *  OUT0 of the TDA8540's is connected to MUX0         (0x03)
 *  OUT1 of the TDA8540's is connected to "Monitor Out"        (0x0C)
 *
 *  TDA8540_ALT3 IN0-3 = Channel 13 - 16       (0x03)
 *  TDA8540_ALT4 IN0-3 = Channel 1 - 4         (0x03)
 *  TDA8540_ALT5 IN0-3 = Channel 5 - 8         (0x03)
 *  TDA8540_ALT6 IN0-3 = Channel 9 - 12                (0x03)
 *
 */

/* All 7 possible sub-ids for the TDA8540 Matrix Switcher */
#define I2C_TDA8540        0x90
#define I2C_TDA8540_ALT1   0x92
#define I2C_TDA8540_ALT2   0x94
#define I2C_TDA8540_ALT3   0x96
#define I2C_TDA8540_ALT4   0x98
#define I2C_TDA8540_ALT5   0x9a
#define I2C_TDA8540_ALT6   0x9c

static void ivc120_muxsel(struct bttv *btv, unsigned int input)
{
	/* Simple maths */
	int key = input % 4;
	int matrix = input / 4;

	dprintk("bttv%d: ivc120_muxsel: Input - %02d | TDA - %02d | In - %02d\n",
		btv->c.nr, input, matrix, key);

	/* Handles the input selection on the TDA8540's */
	bttv_I2CWrite(btv, I2C_TDA8540_ALT3, 0x00,
		      ((matrix == 3) ? (key | key << 2) : 0x00), 1);
	bttv_I2CWrite(btv, I2C_TDA8540_ALT4, 0x00,
		      ((matrix == 0) ? (key | key << 2) : 0x00), 1);
	bttv_I2CWrite(btv, I2C_TDA8540_ALT5, 0x00,
		      ((matrix == 1) ? (key | key << 2) : 0x00), 1);
	bttv_I2CWrite(btv, I2C_TDA8540_ALT6, 0x00,
		      ((matrix == 2) ? (key | key << 2) : 0x00), 1);

	/* Handles the output enables on the TDA8540's */
	bttv_I2CWrite(btv, I2C_TDA8540_ALT3, 0x02,
		      ((matrix == 3) ? 0x03 : 0x00), 1);  /* 13 - 16 */
	bttv_I2CWrite(btv, I2C_TDA8540_ALT4, 0x02,
		      ((matrix == 0) ? 0x03 : 0x00), 1);  /* 1-4 */
	bttv_I2CWrite(btv, I2C_TDA8540_ALT5, 0x02,
		      ((matrix == 1) ? 0x03 : 0x00), 1);  /* 5-8 */
	bttv_I2CWrite(btv, I2C_TDA8540_ALT6, 0x02,
		      ((matrix == 2) ? 0x03 : 0x00), 1);  /* 9-12 */

	/* Selects MUX0 for input on the 878 */
	btaor((0)<<5, ~(3<<5), BT848_IFORM);
}


/* PXC200 muxsel helper
 * luke@syseng.anu.edu.au
 * another transplant
 * from Alessandro Rubini (rubini@linux.it)
 *
 * There are 4 kinds of cards:
 * PXC200L which is bt848
 * PXC200F which is bt848 with PIC controlling mux
 * PXC200AL which is bt878
 * PXC200AF which is bt878 with PIC controlling mux
 */
#define PX_CFG_PXC200F 0x01
#define PX_FLAG_PXC200A  0x00001000 /* a pxc200A is bt-878 based */
#define PX_I2C_PIC       0x0f
#define PX_PXC200A_CARDID 0x200a1295
#define PX_I2C_CMD_CFG   0x00

static void PXC200_muxsel(struct bttv *btv, unsigned int input)
{
	int rc;
	long mux;
	int bitmask;
	unsigned char buf[2];

	/* Read PIC config to determine if this is a PXC200F */
	/* PX_I2C_CMD_CFG*/
	buf[0]=0;
	buf[1]=0;
	rc=bttv_I2CWrite(btv,(PX_I2C_PIC<<1),buf[0],buf[1],1);
	if (rc) {
	  printk(KERN_DEBUG "bttv%d: PXC200_muxsel: pic cfg write failed:%d\n", btv->c.nr,rc);
	  /* not PXC ? do nothing */
	  return;
	}

	rc=bttv_I2CRead(btv,(PX_I2C_PIC<<1),NULL);
	if (!(rc & PX_CFG_PXC200F)) {
	  printk(KERN_DEBUG "bttv%d: PXC200_muxsel: not PXC200F rc:%d \n", btv->c.nr,rc);
	  return;
	}


	/* The multiplexer in the 200F is handled by the GPIO port */
	/* get correct mapping between inputs  */
	/*  mux = bttv_tvcards[btv->type].muxsel[input] & 3; */
	/* ** not needed!?   */
	mux = input;

	/* make sure output pins are enabled */
	/* bitmask=0x30f; */
	bitmask=0x302;
	/* check whether we have a PXC200A */
	if (btv->cardid == PX_PXC200A_CARDID)  {
	   bitmask ^= 0x180; /* use 7 and 9, not 8 and 9 */
	   bitmask |= 7<<4; /* the DAC */
	}
	btwrite(bitmask, BT848_GPIO_OUT_EN);

	bitmask = btread(BT848_GPIO_DATA);
	if (btv->cardid == PX_PXC200A_CARDID)
	  bitmask = (bitmask & ~0x280) | ((mux & 2) << 8) | ((mux & 1) << 7);
	else /* older device */
	  bitmask = (bitmask & ~0x300) | ((mux & 3) << 8);
	btwrite(bitmask,BT848_GPIO_DATA);

	/*
	 * Was "to be safe, set the bt848 to input 0"
	 * Actually, since it's ok at load time, better not messing
	 * with these bits (on PXC200AF you need to set mux 2 here)
	 *
	 * needed because bttv-driver sets mux before calling this function
	 */
	if (btv->cardid == PX_PXC200A_CARDID)
	  btaor(2<<5, ~BT848_IFORM_MUXSEL, BT848_IFORM);
	else /* older device */
	  btand(~BT848_IFORM_MUXSEL,BT848_IFORM);

	printk(KERN_DEBUG "bttv%d: setting input channel to:%d\n", btv->c.nr,(int)mux);
}

/* ----------------------------------------------------------------------- */
/* motherboard chipset specific stuff                                      */

void __devinit bttv_check_chipset(void)
{
	int pcipci_fail = 0;
	struct pci_dev *dev = NULL;

	if (pci_pci_problems & PCIPCI_FAIL)
		pcipci_fail = 1;
	if (pci_pci_problems & (PCIPCI_TRITON|PCIPCI_NATOMA|PCIPCI_VIAETBF))
		triton1 = 1;
	if (pci_pci_problems & PCIPCI_VSFX)
		vsfx = 1;
#ifdef PCIPCI_ALIMAGIK
	if (pci_pci_problems & PCIPCI_ALIMAGIK)
		latency = 0x0A;
#endif


	/* print warnings about any quirks found */
	if (triton1)
		printk(KERN_INFO "bttv: Host bridge needs ETBF enabled.\n");
	if (vsfx)
		printk(KERN_INFO "bttv: Host bridge needs VSFX enabled.\n");
	if (pcipci_fail) {
		printk(KERN_WARNING "bttv: BT848 and your chipset may not work together.\n");
		if (!no_overlay) {
			printk(KERN_WARNING "bttv: overlay will be disabled.\n");
			no_overlay = 1;
		} else {
			printk(KERN_WARNING "bttv: overlay forced. Use this option at your own risk.\n");
		}
	}
	if (UNSET != latency)
		printk(KERN_INFO "bttv: pci latency fixup [%d]\n",latency);
	while ((dev = pci_get_device(PCI_VENDOR_ID_INTEL,
				      PCI_DEVICE_ID_INTEL_82441, dev))) {
		unsigned char b;
		pci_read_config_byte(dev, 0x53, &b);
		if (bttv_debug)
			printk(KERN_INFO "bttv: Host bridge: 82441FX Natoma, "
			       "bufcon=0x%02x\n",b);
	}
}

int __devinit bttv_handle_chipset(struct bttv *btv)
{
	unsigned char command;

	if (!triton1 && !vsfx && UNSET == latency)
		return 0;

	if (bttv_verbose) {
		if (triton1)
			printk(KERN_INFO "bttv%d: enabling ETBF (430FX/VP3 compatibilty)\n",btv->c.nr);
		if (vsfx && btv->id >= 878)
			printk(KERN_INFO "bttv%d: enabling VSFX\n",btv->c.nr);
		if (UNSET != latency)
			printk(KERN_INFO "bttv%d: setting pci timer to %d\n",
			       btv->c.nr,latency);
	}

	if (btv->id < 878) {
		/* bt848 (mis)uses a bit in the irq mask for etbf */
		if (triton1)
			btv->triton1 = BT848_INT_ETBF;
	} else {
		/* bt878 has a bit in the pci config space for it */
		pci_read_config_byte(btv->c.pci, BT878_DEVCTRL, &command);
		if (triton1)
			command |= BT878_EN_TBFX;
		if (vsfx)
			command |= BT878_EN_VSFX;
		pci_write_config_byte(btv->c.pci, BT878_DEVCTRL, command);
	}
	if (UNSET != latency)
		pci_write_config_byte(btv->c.pci, PCI_LATENCY_TIMER, latency);
	return 0;
}


/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
