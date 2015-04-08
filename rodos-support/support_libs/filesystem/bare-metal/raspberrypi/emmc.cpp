/* Copyright (C) 2013 by John Cronin <jncronin@tysos.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Provides an interface to the EMMC controller and commands for interacting
 * with an sd card */

/* References:
 *
 * PLSS 	- SD Group Physical Layer Simplified Specification ver 3.00
 * HCSS		- SD Group Host Controller Simplified Specification ver 3.00
 *
 * Broadcom BCM2835 Peripherals Guide
 */

/*
 * modified by Johannes Freitag 2013, April
 */

#include "../../bare-metal-generic/fatfs/diskio.h"		/* FatFs lower layer API */
#include "rodos.h"
#include "emmc.h"

#ifndef NO_RODOS_NAMESPACE
namespace RODOS {
#endif

extern "C" {

extern void PUT32(uint32_t, uint32_t);
extern uint32_t GET32(uint32_t);

}

//Enable Debug Output
//#define EMMC_DEBUG

// Configuration options
// Enable 1.8V support
#define SD_1_8V_SUPPORT

// Enable 4-bit support
#define SD_4BIT_DATA

// Enable SDXC maximum performance mode
// Requires 150 mA power so disabled on the RPi for now
//#define SDXC_MAXIMUM_PERFORMANCE

// Enable SD write support
#define SD_WRITE_SUPPORT

// The particular SDHCI implementation
#define SDHCI_IMPLEMENTATION_GENERIC        0
#define SDHCI_IMPLEMENTATION_BCM_2708       1
#define SDHCI_IMPLEMENTATION                SDHCI_IMPLEMENTATION_BCM_2708

static char driver_name[] = "emmc";
static char device_name[] = "emmc0"; // We use a single device name as there is only
// one card slot in the RPi

static uint32_t hci_ver = 0;
static uint32_t capabilities_0 = 0;
static uint32_t capabilities_1 = 0;

struct sd_scr {
	uint32_t scr[2];
	uint32_t sd_bus_widths;
	int32_t sd_version;
};

struct fs;

struct block_device {
	char *driver_name;
	char *device_name;
	uint8_t *device_id;
	size_t dev_id_len;

	int32_t supports_multiple_block_read;
	int32_t supports_multiple_block_write;

	int32_t (*read)(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_num);
	int32_t (*write)(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_num);
	size_t block_size;

	struct fs *fs;
};

struct block_device * mmc_dev;

struct emmc_block_dev {
	struct block_device bd;
	uint32_t card_supports_sdhc;
	uint32_t card_supports_18v;
	uint32_t card_ocr;
	uint32_t card_rca;
	uint32_t last_interrupt;
	uint32_t last_error;

	struct sd_scr *scr;

	int32_t failed_voltage_switch;

	uint32_t last_cmd_reg;
	uint32_t last_cmd;
	uint32_t last_cmd_success;
	uint32_t last_r0;
	uint32_t last_r1;
	uint32_t last_r2;
	uint32_t last_r3;

	void *buf;
	int32_t blocks_to_transfer;
	size_t block_size;
	int32_t card_removal;
	uint32_t base_clock;
};

#define EMMC_BASE		0x20300000
#define	EMMC_ARG2		0
#define EMMC_BLKSIZECNT		4
#define EMMC_ARG1		8
#define EMMC_CMDTM		0xC
#define EMMC_RESP0		0x10
#define EMMC_RESP1		0x14
#define EMMC_RESP2		0x18
#define EMMC_RESP3		0x1C
#define EMMC_DATA		0x20
#define EMMC_STATUS		0x24
#define EMMC_CONTROL0		0x28
#define EMMC_CONTROL1		0x2C
#define EMMC_INTERRUPT		0x30
#define EMMC_IRPT_MASK		0x34
#define EMMC_IRPT_EN		0x38
#define EMMC_CONTROL2		0x3C
#define EMMC_CAPABILITIES_0	0x40
#define EMMC_CAPABILITIES_1	0x44
#define EMMC_FORCE_IRPT		0x50
#define EMMC_BOOT_TIMEOUT	0x70
#define EMMC_DBG_SEL		0x74
#define EMMC_EXRDFIFO_CFG	0x80
#define EMMC_EXRDFIFO_EN	0x84
#define EMMC_TUNE_STEP		0x88
#define EMMC_TUNE_STEPS_STD	0x8C
#define EMMC_TUNE_STEPS_DDR	0x90
#define EMMC_SPI_INT_SPT	0xF0
#define EMMC_SLOTISR_VER	0xFC

#define SD_CMD_INDEX(a)		((a) << 24)
#define SD_CMD_TYPE_NORMAL	0x0
#define SD_CMD_TYPE_SUSPEND	(1 << 22)
#define SD_CMD_TYPE_RESUME	(2 << 22)
#define SD_CMD_TYPE_ABORT	(3 << 22)
#define SD_CMD_TYPE_MASK    (3 << 22)
#define SD_CMD_ISDATA		(1 << 21)
#define SD_CMD_IXCHK_EN		(1 << 20)
#define SD_CMD_CRCCHK_EN	(1 << 19)
#define SD_CMD_RSPNS_TYPE_NONE	0			// For no response
#define SD_CMD_RSPNS_TYPE_136	(1 << 16)		// For response R2 (with CRC), R3,4 (no CRC)
#define SD_CMD_RSPNS_TYPE_48	(2 << 16)		// For responses R1, R5, R6, R7 (with CRC)
#define SD_CMD_RSPNS_TYPE_48B	(3 << 16)		// For responses R1b, R5b (with CRC)
#define SD_CMD_RSPNS_TYPE_MASK  (3 << 16)
#define SD_CMD_MULTI_BLOCK	(1 << 5)
#define SD_CMD_DAT_DIR_HC	0
#define SD_CMD_DAT_DIR_CH	(1 << 4)
#define SD_CMD_AUTO_CMD_EN_NONE	0
#define SD_CMD_AUTO_CMD_EN_CMD12	(1 << 2)
#define SD_CMD_AUTO_CMD_EN_CMD23	(2 << 2)
#define SD_CMD_BLKCNT_EN		(1 << 1)
#define SD_CMD_DMA          1

#define SD_ERR_CMD_TIMEOUT	0
#define SD_ERR_CMD_CRC		1
#define SD_ERR_CMD_END_BIT	2
#define SD_ERR_CMD_INDEX	3
#define SD_ERR_DATA_TIMEOUT	4
#define SD_ERR_DATA_CRC		5
#define SD_ERR_DATA_END_BIT	6
#define SD_ERR_CURRENT_LIMIT	7
#define SD_ERR_AUTO_CMD12	8
#define SD_ERR_ADMA		9
#define SD_ERR_TUNING		10
#define SD_ERR_RSVD		11

#define SD_ERR_MASK_CMD_TIMEOUT		(1 << (16 + SD_ERR_CMD_TIMEOUT))
#define SD_ERR_MASK_CMD_CRC		(1 << (16 + SD_ERR_CMD_CRC))
#define SD_ERR_MASK_CMD_END_BIT		(1 << (16 + SD_ERR_CMD_END_BIT))
#define SD_ERR_MASK_CMD_INDEX		(1 << (16 + SD_ERR_CMD_INDEX))
#define SD_ERR_MASK_DATA_TIMEOUT	(1 << (16 + SD_ERR_CMD_TIMEOUT))
#define SD_ERR_MASK_DATA_CRC		(1 << (16 + SD_ERR_CMD_CRC))
#define SD_ERR_MASK_DATA_END_BIT	(1 << (16 + SD_ERR_CMD_END_BIT))
#define SD_ERR_MASK_CURRENT_LIMIT	(1 << (16 + SD_ERR_CMD_CURRENT_LIMIT))
#define SD_ERR_MASK_AUTO_CMD12		(1 << (16 + SD_ERR_CMD_AUTO_CMD12))
#define SD_ERR_MASK_ADMA		(1 << (16 + SD_ERR_CMD_ADMA))
#define SD_ERR_MASK_TUNING		(1 << (16 + SD_ERR_CMD_TUNING))

#define SD_COMMAND_COMPLETE     1
#define SD_TRANSFER_COMPLETE    (1 << 1)
#define SD_BLOCK_GAP_EVENT      (1 << 2)
#define SD_DMA_INTERRUPT        (1 << 3)
#define SD_BUFFER_WRITE_READY   (1 << 4)
#define SD_BUFFER_READ_READY    (1 << 5)
#define SD_CARD_INSERTION       (1 << 6)
#define SD_CARD_REMOVAL         (1 << 7)
#define SD_CARD_INTERRUPT       (1 << 8)

#define SD_RESP_NONE        SD_CMD_RSPNS_TYPE_NONE
#define SD_RESP_R1          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R1b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R2          (SD_CMD_RSPNS_TYPE_136 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R3          SD_CMD_RSPNS_TYPE_48
#define SD_RESP_R4          SD_CMD_RSPNS_TYPE_136
#define SD_RESP_R5          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R5b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R6          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R7          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)

#define SD_DATA_READ        (SD_CMD_ISDATA | SD_CMD_DAT_DIR_CH)
#define SD_DATA_WRITE       (SD_CMD_ISDATA | SD_CMD_DAT_DIR_HC)

#define SD_CMD_RESERVED(a)  0xffffffff

#define SUCCESS(a)          (a->last_cmd_success)
#define FAIL(a)             (a->last_cmd_success == 0)
#define TIMEOUT(a)          (FAIL(a) && (a->last_error == 0))
#define CMD_TIMEOUT(a)      (FAIL(a) && (a->last_error & (1 << 16)))
#define CMD_CRC(a)          (FAIL(a) && (a->last_error & (1 << 17)))
#define CMD_END_BIT(a)      (FAIL(a) && (a->last_error & (1 << 18)))
#define CMD_INDEX(a)        (FAIL(a) && (a->last_error & (1 << 19)))
#define DATA_TIMEOUT(a)     (FAIL(a) && (a->last_error & (1 << 20)))
#define DATA_CRC(a)         (FAIL(a) && (a->last_error & (1 << 21)))
#define DATA_END_BIT(a)     (FAIL(a) && (a->last_error & (1 << 22)))
#define CURRENT_LIMIT(a)    (FAIL(a) && (a->last_error & (1 << 23)))
#define ACMD12_ERROR(a)     (FAIL(a) && (a->last_error & (1 << 24)))
#define ADMA_ERROR(a)       (FAIL(a) && (a->last_error & (1 << 25)))
#define TUNING_ERROR(a)     (FAIL(a) && (a->last_error & (1 << 26)))

#define SD_VER_UNKNOWN      0
#define SD_VER_1            1
#define SD_VER_1_1          2
#define SD_VER_2            3
#define SD_VER_3            4
#define SD_VER_4            5

static char *sd_versions[] = { (char *) "unknown", (char *) "1.0 and 1.01", (char *) "1.10", (char *) "2.00", (char *) "3.0x", (char *) "4.xx" };

#ifdef EMMC_DEBUG
static char *err_irpts[] = {"CMD_TIMEOUT", "CMD_CRC", "CMD_END_BIT",
	"CMD_INDEX", "DATA_TIMEOUT", "DATA_CRC", "DATA_END_BIT",
	"CURRENT_LIMIT", "AUTO_CMD12", "ADMA", "TUNING", "RSVD"};
#endif

int32_t sd_read(struct block_device *, uint8_t *, size_t buf_size, uint32_t);
int32_t sd_write(struct block_device *, uint8_t *, size_t buf_size, uint32_t);

static uint32_t sd_commands[] = { SD_CMD_INDEX(0), SD_CMD_RESERVED(1), SD_CMD_INDEX(2) | SD_RESP_R2, SD_CMD_INDEX(3) | SD_RESP_R6, SD_CMD_INDEX(4),
		SD_CMD_INDEX(5) | SD_RESP_R4, SD_CMD_INDEX(6) | SD_RESP_R1, SD_CMD_INDEX(7) | SD_RESP_R1b, SD_CMD_INDEX(8) | SD_RESP_R7, SD_CMD_INDEX(9) | SD_RESP_R2,
		SD_CMD_INDEX(10) | SD_RESP_R2, SD_CMD_INDEX(11) | SD_RESP_R1, SD_CMD_INDEX(12) | SD_RESP_R1b | SD_CMD_TYPE_ABORT, SD_CMD_INDEX(13) | SD_RESP_R1,
		SD_CMD_RESERVED(14), SD_CMD_INDEX(15), SD_CMD_INDEX(16) | SD_RESP_R1, SD_CMD_INDEX(17) | SD_RESP_R1 | SD_DATA_READ, SD_CMD_INDEX(18) | SD_RESP_R1
				| SD_DATA_READ | SD_CMD_MULTI_BLOCK, SD_CMD_INDEX(19) | SD_RESP_R1 | SD_DATA_READ, SD_CMD_INDEX(20) | SD_RESP_R1b, SD_CMD_RESERVED(21),
		SD_CMD_RESERVED(22), SD_CMD_INDEX(23) | SD_RESP_R1, SD_CMD_INDEX(24) | SD_RESP_R1 | SD_DATA_WRITE, SD_CMD_INDEX(25) | SD_RESP_R1 | SD_DATA_WRITE
				| SD_CMD_MULTI_BLOCK, SD_CMD_RESERVED(26), SD_CMD_INDEX(27) | SD_RESP_R1 | SD_DATA_WRITE, SD_CMD_INDEX(28) | SD_RESP_R1b, SD_CMD_INDEX(29)
				| SD_RESP_R1b, SD_CMD_INDEX(30) | SD_RESP_R1 | SD_DATA_READ, SD_CMD_RESERVED(31), SD_CMD_INDEX(32) | SD_RESP_R1, SD_CMD_INDEX(33) | SD_RESP_R1,
		SD_CMD_RESERVED(34), SD_CMD_RESERVED(35), SD_CMD_RESERVED(36), SD_CMD_RESERVED(37), SD_CMD_INDEX(38) | SD_RESP_R1b, SD_CMD_RESERVED(39),
		SD_CMD_RESERVED(40), SD_CMD_RESERVED(41), SD_CMD_RESERVED(42) | SD_RESP_R1, SD_CMD_RESERVED(43), SD_CMD_RESERVED(44), SD_CMD_RESERVED(45),
		SD_CMD_RESERVED(46), SD_CMD_RESERVED(47), SD_CMD_RESERVED(48), SD_CMD_RESERVED(49), SD_CMD_RESERVED(50), SD_CMD_RESERVED(51), SD_CMD_RESERVED(52),
		SD_CMD_RESERVED(53), SD_CMD_RESERVED(54), SD_CMD_INDEX(55) | SD_RESP_R1, SD_CMD_INDEX(56) | SD_RESP_R1 | SD_CMD_ISDATA, SD_CMD_RESERVED(57),
		SD_CMD_RESERVED(58), SD_CMD_RESERVED(59), SD_CMD_RESERVED(60), SD_CMD_RESERVED(61), SD_CMD_RESERVED(62), SD_CMD_RESERVED(63) };

static uint32_t sd_acommands[] = { SD_CMD_RESERVED(0), SD_CMD_RESERVED(1), SD_CMD_RESERVED(2), SD_CMD_RESERVED(3), SD_CMD_RESERVED(4), SD_CMD_RESERVED(5),
		SD_CMD_INDEX(6) | SD_RESP_R1, SD_CMD_RESERVED(7), SD_CMD_RESERVED(8), SD_CMD_RESERVED(9), SD_CMD_RESERVED(10), SD_CMD_RESERVED(11), SD_CMD_RESERVED(12),
		SD_CMD_INDEX(13) | SD_RESP_R1, SD_CMD_RESERVED(14), SD_CMD_RESERVED(15), SD_CMD_RESERVED(16), SD_CMD_RESERVED(17), SD_CMD_RESERVED(18),
		SD_CMD_RESERVED(19), SD_CMD_RESERVED(20), SD_CMD_RESERVED(21), SD_CMD_INDEX(22) | SD_RESP_R1 | SD_DATA_READ, SD_CMD_INDEX(23) | SD_RESP_R1,
		SD_CMD_RESERVED(24), SD_CMD_RESERVED(25), SD_CMD_RESERVED(26), SD_CMD_RESERVED(27), SD_CMD_RESERVED(28), SD_CMD_RESERVED(29), SD_CMD_RESERVED(30),
		SD_CMD_RESERVED(31), SD_CMD_RESERVED(32), SD_CMD_RESERVED(33), SD_CMD_RESERVED(34), SD_CMD_RESERVED(35), SD_CMD_RESERVED(36), SD_CMD_RESERVED(37),
		SD_CMD_RESERVED(38), SD_CMD_RESERVED(39), SD_CMD_RESERVED(40), SD_CMD_INDEX(41) | SD_RESP_R3, SD_CMD_INDEX(42) | SD_RESP_R1, SD_CMD_RESERVED(43),
		SD_CMD_RESERVED(44), SD_CMD_RESERVED(45), SD_CMD_RESERVED(46), SD_CMD_RESERVED(47), SD_CMD_RESERVED(48), SD_CMD_RESERVED(49), SD_CMD_RESERVED(50),
		SD_CMD_INDEX(51) | SD_RESP_R1 | SD_DATA_READ, SD_CMD_RESERVED(52), SD_CMD_RESERVED(53), SD_CMD_RESERVED(54), SD_CMD_RESERVED(55), SD_CMD_RESERVED(56),
		SD_CMD_RESERVED(57), SD_CMD_RESERVED(58), SD_CMD_RESERVED(59), SD_CMD_RESERVED(60), SD_CMD_RESERVED(61), SD_CMD_RESERVED(62), SD_CMD_RESERVED(63) };

// The actual command indices
#define GO_IDLE_STATE           0
#define ALL_SEND_CID            2
#define SEND_RELATIVE_ADDR      3
#define SET_DSR                 4
#define IO_SET_OP_COND          5
#define SWITCH_FUNC             6
#define SELECT_CARD             7
#define DESELECT_CARD           7
#define SELECT_DESELECT_CARD    7
#define SEND_IF_COND            8
#define SEND_CSD                9
#define SEND_CID                10
#define VOLTAGE_SWITCH          11
#define STOP_TRANSMISSION       12
#define SEND_STATUS             13
#define GO_INACTIVE_STATE       15
#define SET_BLOCKLEN            16
#define READ_SINGLE_BLOCK       17
#define READ_MULTIPLE_BLOCK     18
#define SEND_TUNING_BLOCK       19
#define SPEED_CLASS_CONTROL     20
#define SET_BLOCK_COUNT         23
#define WRITE_BLOCK             24
#define WRITE_MULTIPLE_BLOCK    25
#define PROGRAM_CSD             27
#define SET_WRITE_PROT          28
#define CLR_WRITE_PROT          29
#define SEND_WRITE_PROT         30
#define ERASE_WR_BLK_START      32
#define ERASE_WR_BLK_END        33
#define ERASE                   38
#define LOCK_UNLOCK             42
#define APP_CMD                 55
#define GEN_CMD                 56

#define IS_APP_CMD              0x80000000
#define ACMD(a)                 (a | IS_APP_CMD)
#define SET_BUS_WIDTH           (6 | IS_APP_CMD)
#define SD_STATUS               (13 | IS_APP_CMD)
#define SEND_NUM_WR_BLOCKS      (22 | IS_APP_CMD)
#define SET_WR_BLK_ERASE_COUNT  (23 | IS_APP_CMD)
#define SD_SEND_OP_COND         (41 | IS_APP_CMD)
#define SET_CLR_CARD_DETECT     (42 | IS_APP_CMD)
#define SEND_SCR                (51 | IS_APP_CMD)

#define SD_RESET_CMD            (1 << 25)
#define SD_RESET_DAT            (1 << 26)
#define SD_RESET_ALL            (1 << 24)

typedef uint32_t useconds_t;

#define TIMEOUT_WAIT(stop_if_true, usec) 		\
do {\
for(int64_t i = 0;i<10;i++) {\
}\
} while(!(stop_if_true));

void usleep(...) {
	for (int64_t i = 0; i < 1000; i++) {
	};
}

uint32_t byte_swap(uint32_t in) {
	uint32_t b0 = in & 0xff;
	uint32_t b1 = (in >> 8) & 0xff;
	uint32_t b2 = (in >> 16) & 0xff;
	uint32_t b3 = (in >> 24) & 0xff;
	uint32_t ret = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
	return ret;
}

// Set the clock dividers to generate a target value
static uint32_t sd_get_clock_divider(uint32_t base_clock, uint32_t target_rate) {
	return 0x00000400; //for baseclock 100000000hz
}

// Reset the CMD line
static int32_t sd_reset_cmd() {
	uint32_t control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
	control1 |= SD_RESET_CMD;
	PUT32(EMMC_BASE + EMMC_CONTROL1, control1);
	TIMEOUT_WAIT((GET32(EMMC_BASE + EMMC_CONTROL1) & SD_RESET_CMD) == 0, 1000000);
	if ((GET32(EMMC_BASE + EMMC_CONTROL1) & SD_RESET_CMD)!= 0){
		xprintf("EMMC: CMD line did not reset properly\n");
		return -1;
	}
	return 0;
}

// Reset the CMD line
static int32_t sd_reset_dat() {
	uint32_t control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
	control1 |= SD_RESET_DAT;
	PUT32(EMMC_BASE + EMMC_CONTROL1, control1);
	TIMEOUT_WAIT((GET32(EMMC_BASE + EMMC_CONTROL1) & SD_RESET_DAT) == 0, 1000000);
	if ((GET32(EMMC_BASE + EMMC_CONTROL1) & SD_RESET_DAT)!= 0){
		xprintf("EMMC: DAT line did not reset properly\n");
		return -1;
	}
	return 0;
}

static void sd_issue_command_int(struct emmc_block_dev *dev, uint32_t cmd_reg, uint32_t argument, useconds_t timeout) {
	dev->last_cmd_reg = cmd_reg;
	dev->last_cmd_success = 0;

	// This is as per HCSS 3.7.1.1/3.7.2.2

	// Check Command Inhibit
	while (GET32(EMMC_BASE + EMMC_STATUS) & 0x1)
		usleep(1000);

	// Is the command with busy?
	if ((cmd_reg & SD_CMD_RSPNS_TYPE_MASK)== SD_CMD_RSPNS_TYPE_48B) {
		// With busy

		// Is is an abort command?
		if ((cmd_reg & SD_CMD_TYPE_MASK)!= SD_CMD_TYPE_ABORT) {
			// Not an abort command

			// Wait for the data line to be free
			while (GET32(EMMC_BASE + EMMC_STATUS) & 0x2)
				usleep(1000);
		}
	}

	// Set block size and block count
	// For now, block size = 512 bytes, block count = 1,
	//  host SDMA buffer boundary = 4 kiB
	if (dev->blocks_to_transfer > 0xffff) {
		xprintf("SD: blocks_to_transfer too great (%d)\n", dev->blocks_to_transfer);
		dev->last_cmd_success = 0;
		return;
	}
	uint32_t blksizecnt = dev->block_size | (dev->blocks_to_transfer << 16);
	PUT32(EMMC_BASE + EMMC_BLKSIZECNT, blksizecnt);

	// Set argument 1 reg
	PUT32(EMMC_BASE + EMMC_ARG1, argument);

	// Set command reg
	PUT32(EMMC_BASE + EMMC_CMDTM, cmd_reg);

	usleep(2000);

	// Wait for command complete interrupt
	TIMEOUT_WAIT(GET32(EMMC_BASE + EMMC_INTERRUPT) & 0x8001, timeout);
	uint32_t irpts = GET32(EMMC_BASE + EMMC_INTERRUPT);

	// Clear command complete status
	PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffff0001);

	// Test for errors
	if ((irpts & 0xffff0001) != 0x1) {
#ifdef EMMC_DEBUG
		xprintf(
				"SD: error occured whilst waiting for command complete interrupt\n");
#endif
		dev->last_error = irpts & 0xffff0000;
		dev->last_interrupt = irpts;
		return;
	}

	// Get response data
	switch (cmd_reg & SD_CMD_RSPNS_TYPE_MASK) {
	case SD_CMD_RSPNS_TYPE_48:
	case SD_CMD_RSPNS_TYPE_48B:
		dev->last_r0 = GET32(EMMC_BASE + EMMC_RESP0);
		break;

	case SD_CMD_RSPNS_TYPE_136:
		dev->last_r0 = GET32(EMMC_BASE + EMMC_RESP0);
		dev->last_r1 = GET32(EMMC_BASE + EMMC_RESP1);
		dev->last_r2 = GET32(EMMC_BASE + EMMC_RESP2);
		dev->last_r3 = GET32(EMMC_BASE + EMMC_RESP3);
		break;
	}

	// If with data, wait for the appropriate interrupt
	if ((cmd_reg & SD_CMD_ISDATA)) {
		uint32_t wr_irpt;
		int32_t is_write = 0;
		if (cmd_reg & SD_CMD_DAT_DIR_CH)
			wr_irpt = (1 << 5);     // read
		else {
			is_write = 1;
			wr_irpt = (1 << 4);     // write
		}

		int32_t cur_block = 0;
		uint32_t *cur_buf_addr = (uint32_t *) dev->buf;
		while (cur_block < dev->blocks_to_transfer) {
			TIMEOUT_WAIT(GET32(EMMC_BASE + EMMC_INTERRUPT) & (wr_irpt | 0x8000), timeout);
			irpts = GET32(EMMC_BASE + EMMC_INTERRUPT);
			PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffff0000 | wr_irpt);

			if ((irpts & (0xffff0000 | wr_irpt)) != wr_irpt) {
#ifdef EMMC_DEBUG
				xprintf("SD: error occured whilst waiting for data ready interrupt\n");
#endif
				dev->last_error = irpts & 0xffff0000;
				dev->last_interrupt = irpts;
				return;
			}

			// Transfer the block
			size_t cur_byte_no = 0;
			while (cur_byte_no < dev->block_size) {
				if (is_write)
					PUT32(EMMC_BASE + EMMC_DATA, *cur_buf_addr);
				else
					*cur_buf_addr = GET32(EMMC_BASE + EMMC_DATA);
				cur_byte_no += 4;
				cur_buf_addr++;
			}

			cur_block++;
		}
	}

	// Wait for transfer complete (set if read/write transfer or with busy)
	if ((((cmd_reg & SD_CMD_RSPNS_TYPE_MASK)== SD_CMD_RSPNS_TYPE_48B)||
	(cmd_reg & SD_CMD_ISDATA)))
	{
		// First check command inhibit (DAT) is not already 0
		if((GET32(EMMC_BASE + EMMC_STATUS) & 0x2) == 0)
		PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffff0002);
		else
		{
			TIMEOUT_WAIT(GET32(EMMC_BASE + EMMC_INTERRUPT) & 0x8002, timeout);
			irpts = GET32(EMMC_BASE + EMMC_INTERRUPT);
			PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffff0002);

			// Handle the case where both data timeout and transfer complete
			//  are set - transfer complete overrides data timeout: HCSS 2.2.17
			if(((irpts & 0xffff0002) != 0x2) && ((irpts & 0xffff0002) != 0x100002))
			{
#ifdef EMMC_DEBUG
			xprintf("SD: error occured whilst waiting for transfer complete interrupt\n");
#endif
			dev->last_error = irpts & 0xffff0000;
			dev->last_interrupt = irpts;
			return;
		}
		PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffff0002);
	}
}

	// Return success
	dev->last_cmd_success = 1;
}

static void sd_handle_card_interrupt(struct emmc_block_dev *dev) {
	// Handle a card interrupt

#ifdef EMMC_DEBUG
	uint32_t status = GET32(EMMC_BASE + EMMC_STATUS);

	xprintf("SD: card interrupt\n");
	xprintf("SD: controller status: %08x\n", status);
#endif

	// Get the card status
	if (dev->card_rca) {
		sd_issue_command_int(dev, sd_commands[SEND_STATUS], dev->card_rca << 16, 500000);
		if (FAIL(dev)) {
#ifdef EMMC_DEBUG
			xprintf("SD: unable to get card status\n");
#endif
		} else {
#ifdef EMMC_DEBUG
			xprintf("SD: card status: %08x\n", dev->last_r0);
#endif
		}
	} else {
#ifdef EMMC_DEBUG
		xprintf("SD: no card currently selected\n");
#endif
	}
}

static void sd_handle_interrupts(struct emmc_block_dev *dev) {
	uint32_t irpts = GET32(EMMC_BASE + EMMC_INTERRUPT);
	uint32_t reset_mask = 0;

	if (irpts & SD_COMMAND_COMPLETE) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious command complete interrupt\n");
#endif
		reset_mask |= SD_COMMAND_COMPLETE;
	}

	if (irpts & SD_TRANSFER_COMPLETE) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious transfer complete interrupt\n");
#endif
		reset_mask |= SD_TRANSFER_COMPLETE;
	}

	if (irpts & SD_BLOCK_GAP_EVENT) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious block gap event interrupt\n");
#endif
		reset_mask |= SD_BLOCK_GAP_EVENT;
	}

	if (irpts & SD_DMA_INTERRUPT) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious DMA interrupt\n");
#endif
		reset_mask |= SD_DMA_INTERRUPT;
	}

	if (irpts & SD_BUFFER_WRITE_READY) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious buffer write ready interrupt\n");
#endif
		reset_mask |= SD_BUFFER_WRITE_READY;
		sd_reset_dat();
	}

	if (irpts & SD_BUFFER_READ_READY) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious buffer read ready interrupt\n");
#endif
		reset_mask |= SD_BUFFER_READ_READY;
		sd_reset_dat();
	}

	if (irpts & SD_CARD_INSERTION) {
#ifdef EMMC_DEBUG
		xprintf("SD: card insertion detected\n");
#endif
		reset_mask |= SD_CARD_INSERTION;
	}

	if (irpts & SD_CARD_REMOVAL) {
#ifdef EMMC_DEBUG
		xprintf("SD: card removal detected\n");
#endif
		reset_mask |= SD_CARD_REMOVAL;
		dev->card_removal = 1;
	}

	if (irpts & SD_CARD_INTERRUPT) {
#ifdef EMMC_DEBUG
		xprintf("SD: card interrupt detected\n");
#endif
		sd_handle_card_interrupt(dev);
		reset_mask |= SD_CARD_INTERRUPT;
	}

	if (irpts & 0x8000) {
#ifdef EMMC_DEBUG
		xprintf("SD: spurious error interrupt: %08x\n", irpts);
#endif
		reset_mask |= 0xffff0000;
	}

	PUT32(EMMC_BASE + EMMC_INTERRUPT, reset_mask);
}

static void sd_issue_command(struct emmc_block_dev *dev, uint32_t command, uint32_t argument, useconds_t timeout) {
	// First, handle any pending interrupts
	sd_handle_interrupts(dev);

	// Stop the command issue if it was the card remove interrupt that was
	//  handled
	if (dev->card_removal) {
		dev->last_cmd_success = 0;
		return;
	}

	// Now run the appropriate commands by calling sd_issue_command_int()
	if (command & IS_APP_CMD) {
		command &= 0xff;
#ifdef EMMC_DEBUG
		xprintf("SD: issuing command ACMD%d\n", command);
#endif

		if (sd_acommands[command] == SD_CMD_RESERVED(0)) {
#ifdef EMMC_DEBUG
			xprintf("SD: invalid command ACMD%d\n", command);
#endif
			dev->last_cmd_success = 0;
			return;
		}
		dev->last_cmd = APP_CMD;

		uint32_t rca = 0;
		if (dev->card_rca)
			rca = dev->card_rca << 16;
		sd_issue_command_int(dev, sd_commands[APP_CMD], rca, timeout);
		if (dev->last_cmd_success) {
			dev->last_cmd = command | IS_APP_CMD;
			sd_issue_command_int(dev, sd_acommands[command], argument, timeout);
		}
	} else {
#ifdef EMMC_DEBUG
		xprintf("SD: issuing command CMD%d\n", command);
#endif

		if (sd_commands[command] == SD_CMD_RESERVED(0)) {
#ifdef EMMC_DEBUG
			xprintf("SD: invalid command CMD%d\n", command);
#endif
			dev->last_cmd_success = 0;
			return;
		}

		dev->last_cmd = command;
		sd_issue_command_int(dev, sd_commands[command], argument, timeout);
	}

#ifdef EMMC_DEBUG
	if (FAIL(dev)) {
		xprintf("SD: error issuing command: interrupts %08x: ",
				dev->last_interrupt);
		if (dev->last_error == 0)
		xprintf("TIMEOUT");
		else {
			for (int32_t i = 0; i < SD_ERR_RSVD; i++) {
				if (dev->last_error & (1 << (i + 16))) {
					xprintf(err_irpts[i]);
					xprintf(" ");
				}
			}
		}
		xprintf("\n");
	} else
	xprintf("SD: command completed successfully\n");
#endif
}

struct emmc_block_dev g_ret1;
struct sd_scr g_sd_scr;
int32_t sd_card_init(struct block_device **dev) {
	// Check the sanity of the sd_commands and sd_acommands structures
	if (sizeof(sd_commands) != (64 * sizeof(uint32_t))) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: fatal error, sd_commands of incorrect size: %d"
				" expected %d\n", sizeof(sd_commands), 64 * sizeof(uint32_t));
#endif
		return -1;
	}
	if (sizeof(sd_acommands) != (64 * sizeof(uint32_t))) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: fatal error, sd_acommands of incorrect size: %d"
				" expected %d\n", sizeof(sd_acommands), 64 * sizeof(uint32_t));
#endif
		return -1;
	}

	// Read the controller version
	uint32_t ver = GET32(EMMC_BASE + EMMC_SLOTISR_VER);
	uint32_t sdversion = (ver >> 16) & 0xff;
#ifdef EMMC_DEBUG
	uint32_t vendor = ver >> 24;
	uint32_t slot_status = ver & 0xff;
	xprintf("EMMC: vendor %x, sdversion %x, slot_status %x\n", vendor, sdversion,
			slot_status);
#endif
	hci_ver = sdversion;

	if (hci_ver < 2) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: only SDHCI versions >= 3.0 are supported\n");
#endif
		return -1;
	}

	// Reset the controller
#ifdef EMMC_DEBUG
	xprintf("EMMC: resetting controller\n");
#endif
	uint32_t control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
	control1 |= (1 << 24);
	PUT32(EMMC_BASE + EMMC_CONTROL1, control1);
	TIMEOUT_WAIT((GET32(EMMC_BASE + EMMC_CONTROL1) & (0x7 << 24)) == 0, 1000000);
	if ((GET32(EMMC_BASE + EMMC_CONTROL1) & (0x7 << 24)) != 0) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: controller did not reset properly\n");
#endif
		return -1;
	}
#ifdef EMMC_DEBUG
	xprintf("EMMC: control0: %08x, control1: %08x, control2: %08x\n",
			GET32(EMMC_BASE + EMMC_CONTROL0),
			GET32(EMMC_BASE + EMMC_CONTROL1),
			GET32(EMMC_BASE + EMMC_CONTROL2));
#endif

	// Read the capabilities registers
	capabilities_0 = GET32(EMMC_BASE + EMMC_CAPABILITIES_0);
	capabilities_1 = GET32(EMMC_BASE + EMMC_CAPABILITIES_1);
#ifdef EMMC_DEBUG
	xprintf("EMMC: capabilities: %08x%08x\n", capabilities_1, capabilities_0);
#endif

	// Check for a valid card
#ifdef EMMC_DEBUG
	xprintf("EMMC: checking for an inserted card\n");
#endif
	TIMEOUT_WAIT(GET32(EMMC_BASE + EMMC_STATUS) & (1 << 16), 500000);
	uint32_t status_reg = GET32(EMMC_BASE + EMMC_STATUS);
	if ((status_reg & (1 << 16)) == 0) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: no card inserted\n");
#endif
		return -1;
	}
#ifdef EMMC_DEBUG
	xprintf("EMMC: status: %08x\n", status_reg);
#endif

	// Clear control2
	PUT32(EMMC_BASE + EMMC_CONTROL2, 0);

	// Get the base clock rate
	uint32_t base_clock = 100000000;

	// Set clock rate to something slow
#ifdef EMMC_DEBUG
	xprintf("EMMC: setting clock rate\n");
#endif
	control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
	control1 |= 1;			// enable clock

	// Set to 12.5 Mhz
	control1 |= sd_get_clock_divider(base_clock, 12500000);

	control1 |= (9 << 16);		// data timeout = TMCLK * 2^10
	PUT32(EMMC_BASE + EMMC_CONTROL1, control1);
	TIMEOUT_WAIT(GET32(EMMC_BASE + EMMC_CONTROL1) & 0x2, 0x1000000);
	if ((GET32(EMMC_BASE + EMMC_CONTROL1) & 0x2) == 0) {
#ifdef EMMC_DEBUG
		xprintf("EMMC: controller's clock did not stabilise within 1 second\n");
#endif
		return -1;
	}
#ifdef EMMC_DEBUG
	xprintf("EMMC: control0: %08x, control1: %08x\n",
			GET32(EMMC_BASE + EMMC_CONTROL0),
			GET32(EMMC_BASE + EMMC_CONTROL1));
#endif

	// Enable the SD clock
#ifdef EMMC_DEBUG
	xprintf("EMMC: enabling SD clock\n");
#endif
	usleep(2000);
	control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
	control1 |= 4;
	PUT32(EMMC_BASE + EMMC_CONTROL1, control1);
	usleep(2000);

	// Mask off sending interrupts to the ARM
	PUT32(EMMC_BASE + EMMC_IRPT_EN, 0);
	// Reset interrupts
	PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffffffff);
	// Have all interrupts sent to the INTERRUPT register
	uint32_t irpt_mask = 0xffffffff & (~SD_CARD_INTERRUPT);
#ifdef SD_CARD_INTERRUPTS
	irpt_mask |= SD_CARD_INTERRUPT;
#endif
	PUT32(EMMC_BASE + EMMC_IRPT_MASK, irpt_mask);

	usleep(2000);

	// Prepare the device structure
	struct emmc_block_dev *ret;

	if (*dev == 0)
		ret = &g_ret1;		//(struct emmc_block_dev *) malloc(sizeof(struct emmc_block_dev));
	else
		ret = (struct emmc_block_dev *) *dev;

	memset(ret, 0, sizeof(struct emmc_block_dev));
	ret->bd.driver_name = driver_name;
	ret->bd.device_name = device_name;
	ret->bd.block_size = 512;
	ret->bd.read = sd_read;
#ifdef SD_WRITE_SUPPORT
	ret->bd.write = sd_write;
#endif
	ret->bd.supports_multiple_block_read = 1;
	ret->bd.supports_multiple_block_write = 1;
	ret->base_clock = base_clock;

	// Send CMD0 to the card (reset to idle state)
	sd_issue_command(ret, GO_IDLE_STATE, 0, 500000);
	if (FAIL(ret)) {
		xprintf("SD: no CMD0 response\n");
		return -1;
	}

	// Send CMD8 to the card
	// Voltage supplied = 0x1 = 2.7-3.6V (standard)
	// Check pattern = 10101010b (as per PLSS 4.3.13) = 0xAA
#ifdef EMMC_DEBUG
	xprintf("SD: note a timeout error on the following command (CMD8) is normal "
			"and expected if the SD card version is less than 2.0\n");
#endif
	sd_issue_command(ret, SEND_IF_COND, 0x1aa, 500000);
	int32_t v2_later = 0;
	if (TIMEOUT(ret))
		v2_later = 0;
	else if (CMD_TIMEOUT(ret)) {
		if (sd_reset_cmd() == -1)
			return -1;
		PUT32(EMMC_BASE + EMMC_INTERRUPT, SD_ERR_MASK_CMD_TIMEOUT);
		v2_later = 0;
	} else if (FAIL(ret)) {
#ifdef EMMC_DEBUG
		xprintf("SD: failure sending CMD8\n");
#endif
		return -1;
	} else {
		if ((ret->last_r0 & 0xfff) != 0x1aa) {
			xprintf("SD: unusable card\n");
#ifdef EMMC_DEBUG
			xprintf("SD: CMD8 response %08x\n", ret->last_r0);
#endif
			return -1;
		} else
			v2_later = 1;
	}

	// Here we are supposed to check the response to CMD5 (HCSS 3.6)
	// It only returns if the card is a SDIO card
#ifdef EMMC_DEBUG
	xprintf("SD: note that a timeout error on the following command (CMD5) is "
			"normal and expected if the card is not a SDIO card.\n");
#endif
	sd_issue_command(ret, IO_SET_OP_COND, 0, 10000);
	if (!TIMEOUT(ret)) {
		if (CMD_TIMEOUT(ret)) {
			if (sd_reset_cmd() == -1)
				return -1;
			PUT32(EMMC_BASE + EMMC_INTERRUPT, SD_ERR_MASK_CMD_TIMEOUT);
		} else {
			xprintf("SD: SDIO card detected - not currently supported\n");
#ifdef EMMC_DEBUG
			xprintf("SD: CMD5 returned %08x\n", ret->last_r0);
#endif
			return -1;
		}
	}

	// Call an inquiry ACMD41 (voltage window = 0) to get the OCR
#ifdef EMMC_DEBUG
	xprintf("SD: sending inquiry ACMD41\n");
#endif
	sd_issue_command(ret, ACMD(41), 0, 500000);
	if (FAIL(ret)) {
		xprintf("SD: inquiry ACMD41 failed\n");
		return -1;
	}
#ifdef EMMC_DEBUG
	xprintf("SD: inquiry ACMD41 returned %08x\n", ret->last_r0);
#endif

	// Call initialization ACMD41
	int32_t card_is_busy = 1;
	while (card_is_busy) {
		uint32_t v2_flags = 0;
		if (v2_later) {
			// Set SDHC support
			v2_flags |= (1 << 30);

			// Set 1.8v support
#ifdef SD_1_8V_SUPPORT
			if (!ret->failed_voltage_switch)
				v2_flags |= (1 << 24);
#endif

		}

		sd_issue_command(ret, ACMD(41), 0x00ff8000 | v2_flags, 500000);
		if (FAIL(ret)) {
			xprintf("SD: error issuing ACMD41\n");
			return -1;
		}

		if ((ret->last_r0 >> 31) & 0x1) {
			// Initialization is complete
			ret->card_ocr = (ret->last_r0 >> 8) & 0xffff;
			ret->card_supports_sdhc = (ret->last_r0 >> 30) & 0x1;

#ifdef SD_1_8V_SUPPORT
			if (!ret->failed_voltage_switch)
				ret->card_supports_18v = (ret->last_r0 >> 24) & 0x1;
#endif

			card_is_busy = 0;
		} else {
			// Card is still busy
#ifdef EMMC_DEBUG
			xprintf("SD: card is busy, retrying\n");
#endif
			usleep(500000);
		}
	}

#ifdef EMMC_DEBUG
	xprintf(
			"SD: card identified: OCR: %04x, 1.8v support: %d, SDHC support: %d\n",
			ret->card_ocr, ret->card_supports_18v, ret->card_supports_sdhc);
#endif

	// Switch to 1.8V mode if possible
	if (ret->card_supports_18v) {
#ifdef EMMC_DEBUG
		xprintf("SD: switching to 1.8V mode\n");
#endif
		// As per HCSS 3.6.1

		// Send VOLTAGE_SWITCH
		sd_issue_command(ret, VOLTAGE_SWITCH, 0, 500000);
		if (FAIL(ret)) {
#ifdef EMMC_DEBUG
			xprintf("SD: error issuing VOLTAGE_SWITCH\n");
#endif
			ret->failed_voltage_switch = 1;
			return sd_card_init((struct block_device **) &ret);
		}

		// Disable SD clock
		control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
		control1 &= ~(1 << 2);
		PUT32(EMMC_BASE + EMMC_CONTROL1, control1);

		// Check DAT[3:0]
		status_reg = GET32(EMMC_BASE + EMMC_STATUS);
		uint32_t dat30 = (status_reg >> 20) & 0xf;
		if (dat30 != 0) {
#ifdef EMMC_DEBUG
			xprintf("SD: DAT[3:0] did not settle to 0\n");
#endif
			ret->failed_voltage_switch = 1;
			return sd_card_init((struct block_device **) &ret);
		}

		// Set 1.8V signal enable to 1
		uint32_t control0 = GET32(EMMC_BASE + EMMC_CONTROL0);
		control0 |= (1 << 8);
		PUT32(EMMC_BASE + EMMC_CONTROL0, control0);

		// Wait 5 ms
		usleep(5000);

		// Check the 1.8V signal enable is set
		control0 = GET32(EMMC_BASE + EMMC_CONTROL0);
		if (((control0 >> 8) & 0x1) == 0) {
#ifdef EMMC_DEBUG
			xprintf("SD: controller did not keep 1.8V signal enable high\n");
#endif
			ret->failed_voltage_switch = 1;
			return sd_card_init((struct block_device **) &ret);
		}

		// Re-enable the SD clock
		control1 = GET32(EMMC_BASE + EMMC_CONTROL1);
		control1 |= (1 << 2);
		PUT32(EMMC_BASE + EMMC_CONTROL1, control1);

		// Wait 1 ms
		usleep(1000);

		// Check DAT[3:0]
		status_reg = GET32(EMMC_BASE + EMMC_STATUS);
		dat30 = (status_reg >> 20) & 0xf;
		if (dat30 != 0xf) {
#ifdef EMMC_DEBUG
			xprintf("SD: DAT[3:0] did not settle to 1111b");
#endif
			ret->failed_voltage_switch = 1;
			return sd_card_init((struct block_device **) &ret);
		}

#ifdef EMMC_DEBUG
		xprintf("SD: voltage switch complete\n");
#endif
	}

	// Send CMD2 to get the cards CID
	sd_issue_command(ret, ALL_SEND_CID, 0, 500000);
	if (FAIL(ret)) {
		xprintf("SD: error sending ALL_SEND_CID\n");
		return -1;
	}
	uint32_t card_cid_0 = ret->last_r0;
	uint32_t card_cid_1 = ret->last_r1;
	uint32_t card_cid_2 = ret->last_r2;
	uint32_t card_cid_3 = ret->last_r3;

#ifdef EMMC_DEBUG
	xprintf("SD: card CID: %08x%08x%08x%08x\n", card_cid_3, card_cid_2,
			card_cid_1, card_cid_0);
#endif
	uint32_t dev_id[4]; // = (uint32_t *) malloc(4 * sizeof(uint32_t));
	dev_id[0] = card_cid_0;
	dev_id[1] = card_cid_1;
	dev_id[2] = card_cid_2;
	dev_id[3] = card_cid_3;
	ret->bd.device_id = (uint8_t *) dev_id;
	ret->bd.dev_id_len = 4 * sizeof(uint32_t);

	// Send CMD3 to enter the data state
	sd_issue_command(ret, SEND_RELATIVE_ADDR, 0, 500000);
	if (FAIL(ret)) {
		xprintf("SD: error sending SEND_RELATIVE_ADDR\n");
		return -1;
	}

	uint32_t cmd3_resp = ret->last_r0;
#ifdef EMMC_DEBUG
	xprintf("SD: CMD3 response: %08x\n", cmd3_resp);
#endif

	ret->card_rca = (cmd3_resp >> 16) & 0xffff;
	uint32_t crc_error = (cmd3_resp >> 15) & 0x1;
	uint32_t illegal_cmd = (cmd3_resp >> 14) & 0x1;
	uint32_t error = (cmd3_resp >> 13) & 0x1;
	uint32_t status = (cmd3_resp >> 9) & 0xf;
	uint32_t ready = (cmd3_resp >> 8) & 0x1;

	if (crc_error) {
		xprintf("SD: CRC error\n");
		return -1;
	}

	if (illegal_cmd) {
		xprintf("SD: illegal command\n");
		return -1;
	}

	if (error) {
		xprintf("SD: generic error\n");
		return -1;
	}

	if (!ready) {
		xprintf("SD: not ready for data\n");
		return -1;
	}

#ifdef EMMC_DEBUG
	xprintf("SD: RCA: %04x\n", ret->card_rca);
#endif

	// Now select the card (toggles it to transfer state)
	sd_issue_command(ret, SELECT_CARD, ret->card_rca << 16, 500000);
	if (FAIL(ret)) {
		xprintf("SD: error sending CMD7\n");
		return -1;
	}

	uint32_t cmd7_resp = ret->last_r0;
	status = (cmd7_resp >> 9) & 0xf;

	if ((status != 3) && (status != 4)) {
#ifdef EMMC_DEBUG
		xprintf("SD: invalid status (%d)\n", status);
#endif
		return -1;
	}

	// If not an SDHC card, ensure BLOCKLEN is 512 bytes
	if (!ret->card_supports_sdhc) {
		sd_issue_command(ret, SET_BLOCKLEN, 512, 500000);
		if (FAIL(ret)) {
			xprintf("SD: error sending SET_BLOCKLEN\n");
			return -1;
		}
	}
	ret->block_size = 512;
	uint32_t controller_block_size = GET32(EMMC_BASE + EMMC_BLKSIZECNT);
	controller_block_size &= (~0xfff);
	controller_block_size |= 0x200;
	PUT32(EMMC_BASE + EMMC_BLKSIZECNT, controller_block_size);

	// Get the cards SCR register
	ret->scr = &g_sd_scr; //(struct sd_scr *) malloc(sizeof(struct sd_scr));
	ret->buf = &ret->scr->scr[0];
	ret->block_size = 8;
	ret->blocks_to_transfer = 1;
	sd_issue_command(ret, SEND_SCR, 0, 500000);
	ret->block_size = 512;
	if (FAIL(ret)) {
		xprintf("SD: error sending SEND_SCR\n");
		return -1;
	}

	// Determine card version
	// Note that the SCR is big-endian
	uint32_t scr0 = byte_swap(ret->scr->scr[0]);
	ret->scr->sd_version = SD_VER_UNKNOWN;
	uint32_t sd_spec = (scr0 >> (56 - 32)) & 0xf;
	uint32_t sd_spec3 = (scr0 >> (47 - 32)) & 0x1;
	uint32_t sd_spec4 = (scr0 >> (42 - 32)) & 0x1;
	ret->scr->sd_bus_widths = (scr0 >> (48 - 32)) & 0xf;
	if (sd_spec == 0)
		ret->scr->sd_version = SD_VER_1;
	else if (sd_spec == 1)
		ret->scr->sd_version = SD_VER_1_1;
	else if (sd_spec == 2) {
		if (sd_spec3 == 0)
			ret->scr->sd_version = SD_VER_2;
		else if (sd_spec3 == 1) {
			if (sd_spec4 == 0)
				ret->scr->sd_version = SD_VER_3;
			else if (sd_spec4 == 1)
				ret->scr->sd_version = SD_VER_4;
		}
	}

#ifdef EMMC_DEBUG
	xprintf("SD: &scr: %08x\n", &ret->scr->scr[0]);
	xprintf("SD: SCR[0]: %08x, SCR[1]: %08x\n", ret->scr->scr[0],
			ret->scr->scr[1]);
	;
	xprintf("SD: SCR: %08x%08x\n", byte_swap(ret->scr->scr[0]),
			byte_swap(ret->scr->scr[1]));
	xprintf("SD: SCR: version %s, bus_widths %01x\n",
			sd_versions[ret->scr->sd_version], ret->scr->sd_bus_widths);
#endif

	if (ret->scr->sd_bus_widths & 0x4) {
		// Set 4-bit transfer mode (ACMD6)
		// See HCSS 3.4 for the algorithm
#ifdef SD_4BIT_DATA
#ifdef EMMC_DEBUG
		xprintf("SD: switching to 4-bit data mode\n");
#endif

		// Disable card interrupt in host
		uint32_t old_irpt_mask = GET32(EMMC_BASE + EMMC_IRPT_MASK);
		uint32_t new_iprt_mask = old_irpt_mask & ~(1 << 8);
		PUT32(EMMC_BASE + EMMC_IRPT_MASK, new_iprt_mask);

		// Send ACMD6 to change the card's bit mode
		sd_issue_command(ret, SET_BUS_WIDTH, 0x2, 500000);
		if (FAIL(ret))
			xprintf("SD: switch to 4-bit data mode failed\n");
		else {
			// Change bit mode for Host
			uint32_t control0 = GET32(EMMC_BASE + EMMC_CONTROL0);
			control0 |= 0x2;
			PUT32(EMMC_BASE + EMMC_CONTROL0, control0);

			// Re-enable card interrupt in host
			PUT32(EMMC_BASE + EMMC_IRPT_MASK, old_irpt_mask);

#ifdef EMMC_DEBUG
			xprintf("SD: switch to 4-bit complete\n");
#endif
		}
#endif
	}

	xprintf("SD: found a valid version %s SD card\n", sd_versions[ret->scr->sd_version]);
#ifdef EMMC_DEBUG
	xprintf("SD: setup successful (status %d)\n", status);
#endif

	// Reset interrupt register
	PUT32(EMMC_BASE + EMMC_INTERRUPT, 0xffffffff);

	*dev = (struct block_device *) ret;

	return 0;
}

static int32_t sd_ensure_data_mode(struct emmc_block_dev *edev) {
	if (edev->card_rca == 0) {
		// Try again to initialise the card
		int32_t ret = sd_card_init((struct block_device **) &edev);
		if (ret != 0)
			return ret;
	}

#ifdef EMMC_DEBUG
	xprintf("SD: ensure_data_mode() obtaining status register: ");
#endif

	sd_issue_command(edev, SEND_STATUS, edev->card_rca << 16, 500000);
	if (FAIL(edev)) {
		xprintf("SD: ensure_data_mode() error sending CMD13\n");
		edev->card_rca = 0;
		return -1;
	}

	uint32_t status = edev->last_r0;
	uint32_t cur_state = (status >> 9) & 0xf;
#ifdef EMMC_DEBUG
	xprintf("status %d\n", cur_state);
#endif
	if (cur_state == 3) {
		// Currently in the stand-by state - select it
		sd_issue_command(edev, SELECT_CARD, edev->card_rca << 16, 500000);
		if (FAIL(edev)) {
			xprintf("SD: ensure_data_mode() no response from CMD17\n");
			edev->card_rca = 0;
			return -1;
		}
	} else if (cur_state == 5) {
		// In the data transfer state - cancel the transmission
		sd_issue_command(edev, STOP_TRANSMISSION, 0, 500000);
		if (FAIL(edev)) {
			xprintf("SD: ensure_data_mode() no response from CMD12\n");
			edev->card_rca = 0;
			return -1;
		}

		// Reset the data circuit
		sd_reset_dat();
	} else if (cur_state != 4) {
		// Not in the transfer state - re-initialise
		int32_t ret = sd_card_init((struct block_device **) &edev);
		if (ret != 0)
			return ret;
	}

	// Check again that we're now in the correct mode
	if (cur_state != 4) {
#ifdef EMMC_DEBUG
		xprintf("SD: ensure_data_mode() rechecking status: ");
#endif
		sd_issue_command(edev, SEND_STATUS, edev->card_rca << 16, 500000);
		if (FAIL(edev)) {
			xprintf("SD: ensure_data_mode() no response from CMD13\n");
			edev->card_rca = 0;
			return -1;
		}
		status = edev->last_r0;
		cur_state = (status >> 9) & 0xf;

#ifdef EMMC_DEBUG
		xprintf("%d\n", cur_state);
#endif

		if (cur_state != 4) {
#ifdef EMMC_DEBUG
			xprintf("SD: unable to initialise SD card to "
					"data mode (state %d)\n", cur_state);
#endif
			edev->card_rca = 0;
			return -1;
		}
	}

	return 0;
}

static int32_t sd_do_data_command(struct emmc_block_dev *edev, int32_t is_write, uint8_t *buf, size_t buf_size, uint32_t block_no) {
	// PLSS table 4.20 - SDSC cards use byte addresses rather than block addresses
	if (!edev->card_supports_sdhc)
		block_no *= 512;

	// This is as per HCSS 3.7.2.1
	if (buf_size < edev->block_size) {
#ifdef EMMC_DEBUG
		xprintf("SD: do_data_command() called with buffer size (%d) less than "
				"block size (%d)\n", buf_size, edev->block_size);
#endif
		return -1;
	}

	edev->blocks_to_transfer = buf_size / edev->block_size;
	if (buf_size % edev->block_size) {
#ifdef EMMC_DEBUG
		xprintf("SD: do_data_command() called with buffer size (%d) not an "
				"exact multiple of block size (%d)\n", buf_size,
				edev->block_size);
#endif
		return -1;
	}
	edev->buf = buf;

	// Decide on the command to use
	int32_t command;
	if (is_write) {
		if (edev->blocks_to_transfer > 1)
			command = WRITE_MULTIPLE_BLOCK;
		else
			command = WRITE_BLOCK;
	} else {
		if (edev->blocks_to_transfer > 1)
			command = READ_MULTIPLE_BLOCK;
		else
			command = READ_SINGLE_BLOCK;
	}

	int32_t retry_count = 0;
	int32_t max_retries = 3;
	while (retry_count < max_retries) {

		sd_issue_command(edev, command, block_no, 5000000);

		if (SUCCESS(edev))
			break;
		else {
#ifdef EMMC_DEBUG
			xprintf("SD: error sending CMD%d, ", command);
			xprintf("error = %08x.  ", edev->last_error);
#endif
			retry_count++;
#ifdef EMMC_DEBUG
			if (retry_count < max_retries)
			xprintf("Retrying...\n");
			else
			xprintf("Giving up.\n");
#endif
		}
	}
	if (retry_count == max_retries) {
		edev->card_rca = 0;
		return -1;
	}

	return 0;
}

int32_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no) {
	// Check the status of the card
	struct emmc_block_dev *edev = (struct emmc_block_dev *) dev;
	if (sd_ensure_data_mode(edev) != 0)
		return -1;

#ifdef EMMC_DEBUG
	xprintf("SD: read() card ready, reading from block %u\n", block_no);
#endif

	if (sd_do_data_command(edev, 0, buf, buf_size, block_no) < 0)
		return -1;

#ifdef EMMC_DEBUG
	xprintf("SD: data read successful\n");
#endif

	return buf_size;
}

#ifdef SD_WRITE_SUPPORT
int32_t sd_write(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no) {
	// Check the status of the card
	struct emmc_block_dev *edev = (struct emmc_block_dev *) dev;
	if (sd_ensure_data_mode(edev) != 0)
		return -1;

#ifdef EMMC_DEBUG
	xprintf("SD: write() card ready, reading from block %u\n", block_no);
#endif

	if (sd_do_data_command(edev, 1, buf, buf_size, block_no) < 0)
		return -1;

#ifdef EMMC_DEBUG
	xprintf("SD: write read successful\n");
#endif

	return buf_size;
}
#endif

/************************************************************************/

int32_t emmc_init() {
	return sd_card_init(&mmc_dev);
}

int32_t emmc_read(uint8_t *buf, uint32_t buf_size, uint32_t block_no) {
	return sd_read(mmc_dev, buf, buf_size, block_no);
}

int32_t emmc_write(const uint8_t *buf, uint32_t buf_size, uint32_t block_no) {
	return sd_write(mmc_dev, (uint8_t *) buf, buf_size, block_no);
}

#ifndef NO_RODOS_NAMESPACE
}
#endif
