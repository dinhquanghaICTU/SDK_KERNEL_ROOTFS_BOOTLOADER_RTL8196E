// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * monitor.c - Debug console commands and CPU speed calibration
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include <linux/interrupt.h>
#include "boot_common.h"
#include "boot_soc.h"
#include "monitor.h"
#include "boot_net.h"
#include "nic.h"
#include "spi_flash.h"
#include "uart.h"
#include "cache.h"

#define MAIN_PROMPT "<RealTek>"
#define putchar(x) serial_outc(x)
#define IPTOUL(a, b, c, d) ((a << 24) | (b << 16) | (c << 8) | d)

extern unsigned int _end;
extern void ddump(unsigned char *pData, int len);
extern int rtl8651_getAsicEthernetPHYReg(unsigned int phyId, unsigned int regId,
					 unsigned int *rData);
extern int rtl8651_setAsicEthernetPHYReg(unsigned int phyId, unsigned int regId,
					 unsigned int wData);
extern void GetLine(char *buffer, const unsigned int size, int EchoFlag);
extern int GetArgc(const char *string);
extern char **GetArgv(const char *string);
extern char *StrUpr(char *string);
extern int Hex2Val(char *HexStr, unsigned long *PVal);

static int require_args(int argc, int min, const char *usage)
{
	if (argc < min) {
		if (usage && *usage)
			printf("Usage: %s\n", usage);
		else
			printf("Usage: <command> <args>\n");
		return 0;
	}
	return 1;
}

static int parse_hex_arg(const char *arg, unsigned long *out,
			 const char *label)
{
	const char *p = arg;

	if (!p || !*p) {
		printf("Invalid hex value.\n");
		return 0;
	}
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	if (!Hex2Val((char *)p, out)) {
		if (label && *label)
			printf("Invalid hex %s.\n", label);
		else
			printf("Invalid hex value.\n");
		return 0;
	}
	return 1;
}

int YesOrNo(void);
int CmdHelp(int argc, char *argv[]);
int CmdDumpWord(int argc, char *argv[]);
int CmdDumpByte(int argc, char *argv[]); // wei add
int CmdWriteWord(int argc, char *argv[]);
int CmdWriteByte(int argc, char *argv[]);
int CmdWriteHword(int argc, char *argv[]);
int CmdWriteAll(int argc, char *argv[]);
int CmdCmp(int argc, char *argv[]);
int CmdIp(int argc, char *argv[]);
int CmdAuto(int argc, char *argv[]);
int CmdLoad(int argc, char *argv[]);
int CmdCfn(int argc, char *argv[]);
int CmdSFlw(int argc, char *argv[]);
int CmdFlr(int argc, char *argv[]);
int TestCmd_MDIOR(int argc, char *argv[]); // wei add
int TestCmd_MDIOW(int argc, char *argv[]); // wei add
int CmdPHYregR(int argc, char *argv[]);
int CmdPHYregW(int argc, char *argv[]);

extern int write_data(unsigned long dst, unsigned long length,
		      unsigned char *target);
extern int read_data(unsigned long src, unsigned long length,
		     unsigned char *target);

COMMAND_TABLE MainCmdTable[] = {
    {"HELP", 0, CmdHelp, "HELP: Print this help message"},
    {"?", 0, CmdHelp,
     "HELP (?)				    : Print this help message"},
    {"DB", 2, CmdDumpByte, "DB <Address> <Len>"}, // wei add
    {"DW", 2, CmdDumpWord,
     "DW <Address> <Len>"}, // same command with ICE, easy use
    {"EB", 2, CmdWriteByte, "EB <Address> <Value1> <Value2>..."},
    {"EW", 2, CmdWriteWord, "EW <Address> <Value1> <Value2>..."},
    {"CMP", 3, CmdCmp, "CMP: CMP <dst><src><length>"},
    {"IPCONFIG", 2, CmdIp, "IPCONFIG:<TargetAddress>"},
    {"AUTOBURN", 1, CmdAuto, "AUTOBURN: 0/1"},
    {"LOADADDR", 1, CmdLoad, "LOADADDR: <Load Address>"},
    {"J", 1, CmdCfn, "J: Jump to <TargetAddress>"},
    {"FLR", 3, CmdFlr, "FLR: FLR <dst><src><length>"},
    {"FLW", 3, CmdSFlw,
     "FLW <dst_ROM_offset> <src_RAM_addr> <length_Byte>: Write "
     "offset-data to SPI from RAM"},
    {"MDIOR", 0, TestCmd_MDIOR, "MDIOR:  MDIOR <phyid> <reg>"}, // wei add,
    {"MDIOW", 0, TestCmd_MDIOW,
     "MDIOW:  MDIOW <phyid> <reg> <data>"}, // wei add,
    {"PHYR", 2, CmdPHYregR, "PHYR: PHYR <PHYID><reg>"},
    {"PHYW", 3, CmdPHYregW, "PHYW: PHYW <PHYID><reg><data>"},
};

/********   caculate CPU clock   ************/
int check_cpu_speed(void);
void timer_init(unsigned long lexra_clock);
static void timer_interrupt(int num, void *ptr, struct pt_regs *reg);
struct irqaction irq_timer = {timer_interrupt, 0, 8, "timer", NULL, NULL};
static volatile unsigned int jiffies = 0;
static void timer_interrupt(int num, void *ptr, struct pt_regs *reg)
{
	REG32(TCIR_REG) = (1 << 31) | (1 << 29); /* TC0IE + TC0IP(W1C) */
	jiffies++;
}
volatile int get_timer_jiffies(void) { return jiffies; };

/**
 * timer_init - Initialize the hardware timer for periodic interrupts
 * @lexra_clock: CPU clock frequency in Hz
 *
 * Configures Timer0 for 10ms (100 Hz) periodic interrupts and
 * sets up the interrupt routing.
 */
void timer_init(unsigned long lexra_clock)
{
	/* Stop timer and clear any pending interrupt (needed for ramtest
	   where the timer is already running from the flash bootcode). */
	REG32(TCCNR_REG) = 0;
	REG32(TCIR_REG) = (1 << 31) | (1 << 29); /* W1C: TC0IE + TC0IP */
	jiffies = 0;

#define DIVISOR 0xE
#define DIVF_OFFSET 16
	REG32(CDBR_REG) = (DIVISOR) << DIVF_OFFSET;
	int SysClkRate = lexra_clock;
#define TICK_10MS_FREQ 100   /* 100 Hz */
#define TICK_100MS_FREQ 1000 /* 1000 Hz */
#define TICK_FREQ TICK_10MS_FREQ
	REG32(TC0DATA_REG) = (((SysClkRate / DIVISOR) / TICK_FREQ) + 1) << 4;
	/* Enable timer */
	REG32(TCCNR_REG) = (1 << 31) | (1 << 30);
	/* Wait n cycles for timer to re-latch the new value of TC0DATA. */
	int c;
	for (c = 0; c < DIVISOR; c++)
		;
	/* Set interrupt routing register */
	REG32(IRR1_REG) = 0x00050004; // uart:IRQ5,  time0:IRQ4
	/* Enable timer interrupt */
	REG32(TCIR_REG) = (1 << 31);
}

unsigned long loops_per_jiffy = (1 << 12);
#define LPS_PREC 8
#define HZ 100
unsigned long loops_per_sec =
    2490368 * HZ; // @CPU 500MHz (this will be update in check_cpu_speed())

/**
 * check_cpu_speed - Measure CPU clock speed using timer calibration
 *
 * Initializes the hardware timer, then uses a binary search to
 * calibrate loops_per_jiffy against the 10ms timer tick.
 *
 * Return: CPU speed in MHz
 */
int check_cpu_speed(void)
{
	unsigned long ticks, loopbit;
	int lps_precision = LPS_PREC;

	extern long glexra_clock;

	timer_init(glexra_clock);
	request_IRQ(8, &irq_timer, NULL);

	loops_per_jiffy = (1 << 12);
	while (loops_per_jiffy <<= 1) {
		/* wait for "start of" clock tick */
		ticks = jiffies;
		while (ticks == jiffies)
			/* nothing */;
		/* Go .. */
		ticks = jiffies;
		__delay(loops_per_jiffy);
		ticks = jiffies - ticks;
		if (ticks)
			break;
	}

	/* Do a binary approximation to get loops_per_jiffy set to equal one
	   clock (up to lps_precision bits) */
	loops_per_jiffy >>= 1;
	loopbit = loops_per_jiffy;
	while (lps_precision-- && (loopbit >>= 1)) {
		loops_per_jiffy |= loopbit;
		ticks = jiffies;
		while (ticks == jiffies)
			;
		ticks = jiffies;
		__delay(loops_per_jiffy);
		if (jiffies != ticks) /* longer than 1 tick */
			loops_per_jiffy &= ~loopbit;
	}

	return ((loops_per_jiffy / (500000 / HZ)) + 1);
}
/**
 * monitor - Interactive command-line monitor loop
 *
 * Drains stale UART input, then loops: prints the prompt, reads a
 * command line, parses it, and dispatches to the matching handler
 * in MainCmdTable.
 */
/*
---------------------------------------------------------------------------
;				Monitor
---------------------------------------------------------------------------
*/
void monitor(void)
{
	char buffer[MAX_MONITOR_BUFFER + 1];
	int argc, i, retval;
	char **argv;

	/*
	 * Drain stale bytes from the UART RX FIFO before entering the
	 * command loop.  When the user holds ESC to enter download mode,
	 * keyboard repeat fills the FIFO with 0x1b bytes that would
	 * otherwise be consumed as input by GetLine().  GetLine() also
	 * ignores ESC characters for late arrivals during key repeat.
	 */
	g_uart_peek = -1;
	while (uart_data_ready())
		(void)uart_getc_nowait();

	while (1) {
		printf("%s", MAIN_PROMPT);
		memset(buffer, 0, MAX_MONITOR_BUFFER);
		GetLine(buffer, MAX_MONITOR_BUFFER, 1);
		printf("\n");

		argc = GetArgc((const char *)buffer);
		if (argc < 1)
			continue;

		argv = GetArgv((const char *)buffer);
		StrUpr(argv[0]);

		for (i = 0; i < (sizeof(MainCmdTable) / sizeof(COMMAND_TABLE));
		     i++) {
			if (!strcmp(argv[0], MainCmdTable[i].cmd)) {
				retval =
				    MainCmdTable[i].func(argc - 1, argv + 1);
				break;
			}
		}
		if (i == sizeof(MainCmdTable) / sizeof(COMMAND_TABLE))
			printf("Unknown command !\r\n");
	}
}

/*
---------------------------------------------------------------------------
; Ethernet Download
---------------------------------------------------------------------------
*/
int CmdCfn(int argc, char *argv[])
{
	unsigned long Address;
	void (*jump)(void);
	if (argc < 1) {
		printf("Usage: J <TargetAddress>\n");
		return FALSE;
	}
	if (!parse_hex_arg(argv[0], &Address, "Address")) {
		printf("Usage: J <TargetAddress>\n");
		return FALSE;
	}

	dprintf("---Jump to address=%X\n", Address);
	jump = (void *)(Address);
	outl(0, GIMR0); // mask all interrupt
	cli();
	/* if the jump-Address is BFC00000, then do watchdog reset */
	if (Address == 0xBFC00000) {
		*(volatile unsigned long *)(0xB800311c) =
		    0; /*this is to enable 865xc watch dog reset*/
		for (;;)
			;
	} else /*else disable PHY to prevent from ethernet disturb Linux kernel
		  booting */
	{
		WRITE_MEM32(PCRP0, (READ_MEM32(PCRP0) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP1, (READ_MEM32(PCRP1) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP2, (READ_MEM32(PCRP2) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP3, (READ_MEM32(PCRP3) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP4, (READ_MEM32(PCRP4) & (~EnablePHYIf)));
		flush_cache();
	}
	jump();
}

/* This command can be used to configure host ip and target ip	*/

int CmdIp(int argc, char *argv[])
{
	unsigned char *ptr;
	unsigned int i;
	int ip[4];
	unsigned char ip_u8[4];

	if (argc == 0) {
		unsigned char ip[4];
		tftp_get_server_ip(ip);
		printf(" Target Address=%d.%d.%d.%d\n",
		       ip[0], ip[1], ip[2], ip[3]);
		return 0;
	}

	ptr = argv[0];

	for (i = 0; i < 4; i++) {
		ip[i] = strtol((const char *)ptr, (char **)NULL, 10);
		if (ip[i] < 0 || ip[i] > 255) {
			printf("Invalid IP format.\n");
			printf("Usage: IPCONFIG <A.B.C.D>\n");
			return 0;
		}
		if (i < 3) {
			ptr = strchr(ptr, '.');
			if (!ptr) {
				printf("Invalid IP format.\n");
				printf("Usage: IPCONFIG <A.B.C.D>\n");
				return 0;
			}
			ptr++;
		}
	}
	ip_u8[0] = (unsigned char)ip[0];
	ip_u8[1] = (unsigned char)ip[1];
	ip_u8[2] = (unsigned char)ip[2];
	ip_u8[3] = (unsigned char)ip[3];
	tftp_set_server_ip(ip_u8);
	/* Keep the shared server-IP global in sync (the boothold DRAM handoff
	 * sets the same variable — see boot/main.c, boot/net/tftpd.c). */
	g_tftp_server_ip = ((unsigned long)ip[0] << 24) |
			   ((unsigned long)ip[1] << 16) |
			   ((unsigned long)ip[2] << 8) | (unsigned long)ip[3];
	/*replace the MAC address middle 4 bytes.*/
	eth0_mac[1] = ip[0];
	eth0_mac[2] = ip[1];
	eth0_mac[3] = ip[2];
	eth0_mac[4] = ip[3];
	tftp_set_server_mac((const unsigned char *)eth0_mac);
	prom_printf("Now your Target IP is %d.%d.%d.%d\n", ip[0], ip[1], ip[2],
		    ip[3]);
}
int CmdDumpWord(int argc, char *argv[])
{
	unsigned long src;
	unsigned int len, i;

	if (!require_args(argc, 1, "DW <Address> <Len>"))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	if (src < 0x80000000)
		src |= 0x80000000;

	if (!argv[1])
		len = 1;
	else
		len = strtoul((const char *)(argv[1]), (char **)NULL, 10);
	while ((src) & 0x03)
		src++;

	for (i = 0; i < len; i += 4, src += 16) {
		dprintf("%08X:	%08X	%08X	%08X	%08X\n", src,
			*(unsigned long *)(src), *(unsigned long *)(src + 4),
			*(unsigned long *)(src + 8),
			*(unsigned long *)(src + 12));
	}
}

int CmdDumpByte(int argc, char *argv[])
{

	unsigned long src;
	unsigned int len, i;

	if (!require_args(argc, 1, "DB <Address> <Len>"))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	if (src < 0x80000000)
		src |= 0x80000000;
	if (!argv[1])
		len = 16;
	else
		len = strtoul((const char *)(argv[1]), (char **)NULL, 10);

	ddump((unsigned char *)src, len);
}

int CmdWriteWord(int argc, char *argv[])
{

unsigned long src;
unsigned long value;
unsigned int i;

	if (!require_args(argc, 2, "EW <Address> <Value1> <Value2>..."))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	while ((src) & 0x03)
		src++;

	for (i = 0; i < argc - 1; i++, src += 4) {
		if (!parse_hex_arg(argv[i + 1], &value, "Value"))
			return 0;
		*(volatile unsigned int *)(src) = (unsigned int)value;
	}
}

int CmdWriteHword(int argc, char *argv[])
{

	unsigned long src;
	unsigned long value;
	unsigned short i;

	if (!require_args(argc, 2, "EH <Address> <Value1> <Value2>..."))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;

	src &= 0xfffffffe;

	for (i = 0; i < argc - 1; i++, src += 2) {
		if (!parse_hex_arg(argv[i + 1], &value, "Value"))
			return 0;
		*(volatile unsigned short *)(src) = (unsigned short)value;
	}
}

int CmdWriteByte(int argc, char *argv[])
{

	unsigned long src;
	unsigned long value;
	unsigned char i;

	if (!require_args(argc, 2, "EB <Address> <Value1> <Value2>..."))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;

	for (i = 0; i < argc - 1; i++, src++) {
		if (!parse_hex_arg(argv[i + 1], &value, "Value"))
			return 0;
		*(volatile unsigned char *)(src) = (unsigned char)value;
	}
}

int CmdCmp(int argc, char *argv[])
{
	int i;
	unsigned long dst, src;
	unsigned long dst_value, src_value;
	unsigned int length;
	unsigned long error;

	if (!require_args(argc, 3, "CMP <dst> <src> <length>"))
		return 1;
	if (!parse_hex_arg(argv[0], &dst, "Dst"))
		return 1;
	if (!parse_hex_arg(argv[1], &src, "Src"))
		return 1;
	if (!parse_hex_arg(argv[2], (unsigned long *)&length, "Length"))
		return 1;
	error = 0;
	for (i = 0; i < length; i += 4) {
		dst_value = *(volatile unsigned int *)(dst + i);
		src_value = *(volatile unsigned int *)(src + i);
		if (dst_value != src_value) {
			printf("%dth data(%x %x) error\n", i, dst_value,
			       src_value);
			error = 1;
		}
	}
	if (!error)
		printf("No error found\n");
}

extern int autoBurn;
int CmdAuto(int argc, char *argv[])
{
	if (argc < 1) {
		printf("AutoBurning=%d\n", autoBurn);
		return 0;
	}

	if (argv[0][0] == '0' && argv[0][1] == '\0')
		autoBurn = 0;
	else if (argv[0][0] == '1' && argv[0][1] == '\0')
		autoBurn = 1;
	else {
		printf("AutoBurning=%d\n", autoBurn);
		printf("Usage: AUTOBURN 0|1\n");
		return 0;
	}
	printf("AutoBurning=%d\n", autoBurn);
	return 0;
}

int CmdLoad(int argc, char *argv[])
{
	unsigned long addr;

	if (argc < 1) {
		printf("TFTP Load Addr: 0x%x\n", image_address);
		return 0;
	}

	if (!parse_hex_arg(argv[0], &addr, "Address")) {
		printf("Usage: LOADADDR <HexAddress>\n");
		return 0;
	}
	image_address = addr;
	printf("Set TFTP Load Addr 0x%x\n", image_address);
	return 0;
}

/*
--------------------------------------------------------------------------
Flash Utility
--------------------------------------------------------------------------
*/
int CmdFlr(int argc, char *argv[])
{
	unsigned long dst, src;
	unsigned int length;

	if (!require_args(argc, 3, "FLR <dst> <src> <length>"))
		return 0;

	if (!parse_hex_arg(argv[0], &dst, "Dst"))
		return 0;
	if (!parse_hex_arg(argv[1], &src, "Src"))
		return 0;
	if (!parse_hex_arg(argv[2], (unsigned long *)&length, "Length"))
		return 0;

	printf("Flash read from %X to %X with %X bytes	?\n", src, dst, length);
	printf("(Y)es , (N)o ? --> ");

	if (YesOrNo()) {
		if (flashread(dst, src, length)) {
			printf("Flash Read Succeeded!\n");
			file_length_to_server = length;
			image_address = dst;
		} else
			printf("Flash Read Failed!\n");
	} else
		printf("Abort!\n");
}

/* Setting image header */

/*
------------------------------------------  ---------------------------------
; Command Help
---------------------------------------------------------------------------
*/

int CmdHelp(int argc, char *argv[])
{
	int i;

	printf("----------------- COMMAND MODE HELP ------------------\n");
	for (i = 0; i < (sizeof(MainCmdTable) / sizeof(COMMAND_TABLE)); i++) {
		if (MainCmdTable[i].msg) {
			printf("%s\n", MainCmdTable[i].msg);
		}
	}

	return TRUE;
}

int YesOrNo(void)
{
	unsigned char iChar[2];

	GetLine(iChar, 2, 1);
	printf("\n"); // vicadd
	if ((iChar[0] == 'Y') || (iChar[0] == 'y'))
		return 1;
	else
		return 0;
}

int CmdSFlw(int argc, char *argv[])
{
	if (!require_args(argc, 3, "FLW <dst> <src> <length>"))
		return 1;

	unsigned long dst_flash_addr_offset = 0;
	unsigned long src_RAM_addr = 0;
	unsigned long length = 0;

	if (!parse_hex_arg(argv[0], &dst_flash_addr_offset, "Dst"))
		return 1;
	if (!parse_hex_arg(argv[1], &src_RAM_addr, "Src"))
		return 1;
	if (!parse_hex_arg(argv[2], &length, "Length"))
		return 1;

	unsigned int end_of_RAM_addr = src_RAM_addr + length;
	printf("Write 0x%x Bytes to SPI flash, offset 0x%x<0x%x>, from RAM "
	       "0x%x to 0x%x\n",
	       (unsigned int)length,
	       (unsigned int)dst_flash_addr_offset,
	       (unsigned int)dst_flash_addr_offset + 0xbd000000,
	       (unsigned int)src_RAM_addr,
	       end_of_RAM_addr);
	printf("(Y)es, (N)o->");
	if (YesOrNo()) {
		spi_pio_init();
		spi_flw_image_mio_8198(0,
				       (unsigned int)dst_flash_addr_offset,
				       (unsigned char *)src_RAM_addr, length);
	} // end if YES
	else
		printf("Abort!\n");
}

int TestCmd_MDIOR(int argc, char *argv[])
{
	if (!require_args(argc, 1, "MDIOR <phyid> <reg>"))
		return 1;

	unsigned int reg = strtoul((const char *)(argv[0]), (char **)NULL, 10);
	unsigned int data;
	int i, phyid;
	for (i = 0; i < 32; i++) {
		phyid = i;
		rtl8651_getAsicEthernetPHYReg(phyid, reg, &data);
		dprintf("PHYID=0x%02x regID=0x%02x data=0x%04x\r\n", phyid, reg,
			data);
	}
}

int TestCmd_MDIOW(int argc, char *argv[])
{
	if (!require_args(argc, 3, "MDIOW <phyid> <reg> <data>"))
		return 1;
	unsigned int phyid =
	    strtoul((const char *)(argv[0]), (char **)NULL, 16);
	unsigned int reg = strtoul((const char *)(argv[1]), (char **)NULL, 10);
	unsigned int data = strtoul((const char *)(argv[2]), (char **)NULL, 16);
	dprintf("Write PHYID=0x%x regID=0x%x data=0x%x\r\n", phyid, reg, data);
	rtl8651_setAsicEthernetPHYReg(phyid, reg, data);
	unsigned int tmp;
	rtl8651_getAsicEthernetPHYReg(phyid, reg, &tmp);
	dprintf("Readback PHYID=0x%x regID=0x%x data=0x%x\r\n", phyid, reg, tmp);
}

int CmdPHYregR(int argc, char *argv[])
{
	if (!require_args(argc, 2, "PHYR <phyid> <reg>"))
		return 1;
	unsigned long phyid, regnum;
	unsigned int uid, tmp;
	phyid = strtoul((const char *)(argv[0]), (char **)NULL, 16);
	regnum = strtoul((const char *)(argv[1]), (char **)NULL, 16);
	rtl8651_getAsicEthernetPHYReg(phyid, regnum, &tmp);
	uid = tmp;
	prom_printf("PHYID=0x%x regID=0x%x data=0x%x\r\n",
		    phyid, regnum, uid);
}

int CmdPHYregW(int argc, char *argv[])
{
	if (!require_args(argc, 3, "PHYW <phyid> <reg> <data>"))
		return 1;
	unsigned long phyid, regnum;
	unsigned long data;
	unsigned int uid, tmp;
	phyid = strtoul((const char *)(argv[0]), (char **)NULL, 16);
	regnum = strtoul((const char *)(argv[1]), (char **)NULL, 16);
	data = strtoul((const char *)(argv[2]), (char **)NULL, 16);
	prom_printf("Write PHYID=0x%x regID=0x%x data=0x%x\r\n",
		    phyid, regnum, data);
	rtl8651_setAsicEthernetPHYReg(phyid, regnum, data);
	rtl8651_getAsicEthernetPHYReg(phyid, regnum, &tmp);
	uid = tmp;
	prom_printf("Readback PHYID=0x%x regID=0x%x data=0x%x\r\n",
		    phyid, regnum, uid);
}

#define END_ADDR 0x02000000 // 32MB

/* override strap/system definitions for this board */
#undef SYS_BASE
#undef SYS_INT_STATUS
#undef SYS_HW_STRAP
#undef SYS_BIST_CTRL
#undef SYS_DRF_BIST_CTRL
#undef SYS_BIST_OUT
#undef SYS_BIST_DONE
#undef SYS_BIST_FAIL
#undef SYS_DRF_BIST_DONE
#undef SYS_DRF_BIST_FAIL
#undef SYS_PLL_REG
#undef CK_M2X_FREQ_SEL
#undef ST_CPU_FREQ_SEL
#undef ST_FW_CPU_FREQDIV_SEL
#undef ST_CK_CPU_FREQDIV_SEL
#undef ST_CLKLX_FROM_CLKM
#undef ST_CLKLX_FROM_HALFOC
#undef ST_CLKOC_FROM_CLKM
#undef CK_M2X_FREQ_SEL_OFFSET
#undef ST_CPU_FREQ_SEL_OFFSET
#undef ST_CPU_FREQDIV_SEL_OFFSET
#undef ST_CLKLX_FROM_CLKM_OFFSET

// System register Table
#define SYS_BASE 0xb8000000
#define SYS_INT_STATUS (SYS_BASE + 0x04)
#define SYS_HW_STRAP (SYS_BASE + 0x08)
#define SYS_BIST_CTRL (SYS_BASE + 0x14)
#define SYS_DRF_BIST_CTRL (SYS_BASE + 0x18)
#define SYS_BIST_OUT (SYS_BASE + 0x1c)
#define SYS_BIST_DONE (SYS_BASE + 0x20)
#define SYS_BIST_FAIL (SYS_BASE + 0x24)
#define SYS_DRF_BIST_DONE (SYS_BASE + 0x28)
#define SYS_DRF_BIST_FAIL (SYS_BASE + 0x2c)
#define SYS_PLL_REG (SYS_BASE + 0x30)
// hw strap register
#define CK_M2X_FREQ_SEL (0x7 << 10)
#define ST_CPU_FREQ_SEL (0xf << 13)
#define ST_FW_CPU_FREQDIV_SEL (0x1 << 18) // new
#define ST_CK_CPU_FREQDIV_SEL (0x1 << 19) // new
#define ST_CLKLX_FROM_CLKM (1 << 21)
#define ST_CLKLX_FROM_HALFOC (1 << 22)
#define ST_CLKOC_FROM_CLKM (1 << 24)
#define CK_M2X_FREQ_SEL_OFFSET 10
#define ST_CPU_FREQ_SEL_OFFSET 13
#define ST_CPU_FREQDIV_SEL_OFFSET 18
#define ST_CLKLX_FROM_CLKM_OFFSET 21
#define SPEED_IRQ_NO 27						 // PA0
#define SPEED_IRR_NO (SPEED_IRQ_NO / 8)				 // IRR3
#define SPEED_IRR_OFFSET ((SPEED_IRQ_NO - SPEED_IRR_NO * 8) * 4) // 12

#define GICR_BASE 0xB8003000
#define GIMR_REG (0x000 + GICR_BASE) /* Global interrupt mask */
#define GISR_REG (0x004 + GICR_BASE) /* Global interrupt status */
#define IRR_REG (0x008 + GICR_BASE)  /* Interrupt routing */
#define IRR1_REG (0x00C + GICR_BASE) /* Interrupt routing */
#define IRR2_REG (0x010 + GICR_BASE) /* Interrupt routing */
#define IRR3_REG (0x014 + GICR_BASE) /* Interrupt routing */

static void SPEED_isr(int irq, void *dev_id, struct pt_regs *regs)
{

	unsigned int isr = REG32(GISR_REG);
	unsigned int cpu_status = REG32(SYS_INT_STATUS);

	// dprintf("=>CPU Wake-up interrupt happen! GISR=%08x \n", isr);

	if ((isr & (1 << SPEED_IRQ_NO)) == 0) // check isr==1
	{
		dprintf("Fail, ISR=%x bit %d is not 1\n", isr, SPEED_IRQ_NO);
		while (1)
			;
	}

	if ((cpu_status & (1 << 1)) == 0) // check source==1
	{ // dprintf("Fail, Source=%x bit %d is not 1 \n", cpu_status, 1);
		while (1)
			;
	}

	REG32(SYS_INT_STATUS) = (1 << 1); // enable cpu wakeup interrupt mask
	//	REG32(GISR_REG)=1<<SPEED_IRQ_NO;	//write to clear, but
	//cannot clear

	REG32(GIMR_REG) =
	    REG32(GIMR_REG) & ~(1 << SPEED_IRQ_NO); // so, disable interrupt
}

struct irqaction irq_SPEED = {
    SPEED_isr, (unsigned long)NULL, (unsigned long)SPEED_IRQ_NO,
    "SPEED",   (void *)NULL,	    (struct irqaction *)NULL};

/**
 * SettingCPUClk - Change CPU clock frequency at runtime
 * @clk_sel: clock multiplier selection (4-bit field)
 * @clk_div: clock divider selection (2-bit field)
 * @sync_oc: OC sync mode (unused)
 *
 * Modifies the hardware strap register, then puts the CPU to sleep
 * while the PLL relocks.  The CPU wakes on the speed-change interrupt.
 *
 * Return: 0 on success
 */
int SettingCPUClk(int clk_sel, int clk_div, int sync_oc)
{
	int clk_curr, clk_exp;
	unsigned int old_clk_sel;
	unsigned int mask;
	unsigned int sysreg;
	REG32(SYS_INT_STATUS) = (1 << 1); // enable cpu wakeup interrupt mask
	while (REG32(GISR_REG) & (1 << SPEED_IRQ_NO))
		; // wait speed bit to low.
	mask = REG32(GIMR_REG);
	// open speed irq
	int irraddr = IRR_REG + SPEED_IRR_NO * 4;
	REG32(irraddr) = (REG32(irraddr) & ~(0x0f << SPEED_IRR_OFFSET)) |
			 (3 << SPEED_IRR_OFFSET);
	request_IRQ(SPEED_IRQ_NO, &irq_SPEED, NULL);
	// be seure open interrupt first.
	REG32(GIMR_REG) = (1 << SPEED_IRQ_NO); // accept speed interrupt
	sysreg = REG32(SYS_HW_STRAP);
	old_clk_sel = (sysreg & ST_CPU_FREQ_SEL) >> ST_CPU_FREQ_SEL_OFFSET;
	sysreg &= ~(ST_FW_CPU_FREQDIV_SEL);
	sysreg &= ~(ST_CK_CPU_FREQDIV_SEL);
	sysreg &= ~(ST_CPU_FREQ_SEL);
	sysreg |= (clk_div & 0x03) << ST_CPU_FREQDIV_SEL_OFFSET;
	sysreg |= (clk_sel & 0x0f) << ST_CPU_FREQ_SEL_OFFSET;
	REG32(SYS_HW_STRAP) = sysreg;
	if (old_clk_sel != clk_sel) {

		REG32(GISR_REG) = 0xffffffff;
		REG32(SYS_BIST_CTRL) |= (1 << 2); // lock bus arb2
		while ((REG32(SYS_BIST_DONE) & (1 << 0)) == 0)
			; // wait bit to 1, is mean lock ok

		__asm__ volatile("sleep");
		__asm__ volatile("nop");

		REG32(SYS_BIST_CTRL) &= ~(1 << 2); // unlock
		while ((REG32(SYS_BIST_DONE) & (1 << 0)) == (1 << 0))
			; // wait bit to 0  unlock
	}
	REG32(GIMR_REG) = mask;
}
