/*
 * boothold — Write HOLD magic (and optional TFTP server IP) to DRAM
 *
 * Writes 0x484F4C44 ("HOLD") to physical address 0x01FFEFFC via /dev/mem.
 * The bootloader (V2.6+) reads this address via KSEG1 (uncached) on next
 * reset and enters download mode if it finds the magic word.  The flag
 * is one-shot: the bootloader clears it before entering download mode.
 *
 * Optional argument — boothold <A.B.C.D>:
 *   If a valid dotted IPv4 is given, boothold also writes an IP record into
 *   the same reserved DRAM page, just below the HOLD magic:
 *
 *     0x01FFEFFC : HOLD magic 0x484F4C44 ("HOLD")
 *     0x01FFEFF8 : IP   magic 0x49505634 ("IPV4")
 *     0x01FFEFF4 : IPv4 packed as (a<<24)|(b<<16)|(c<<8)|d
 *
 *   These fields grow DOWNWARD from the top of the page. The BASE of the
 *   same page (0x01FFE000 .. ~0x01FFE0F0) is used by the kernel watchdog
 *   driver for its panic post-mortem record (see rtl819x_wdt.c, WDT_REC_*).
 *   Any new boothold field must stay near the top — do not grow below
 *   0x01FFEF00, to preserve the gap.
 *
 *   The bootloader (V2.7+) honours this IP as its download-mode TFTP server
 *   address, but only when HOLD is also valid (a deliberate warm reboot from
 *   a running Linux).  Without the argument — or on an older bootloader that
 *   ignores it — the bootloader keeps its compiled default (192.168.1.6).
 *   This lets `flash_remote.sh` make the gateway's bootloader-mode IP follow
 *   its BOOT_IP without recompiling or touching the serial console.
 *
 * Does NOT reboot — the caller handles that (e.g. `boothold && reboot`).
 *
 * Why 0x01FFEFFC (high DRAM, just below btcode stack)?
 *
 *   v2.x firmware (Linux 5.10) used 0x003FFFFC at the bottom of DRAM.
 *   On Linux 6.18 (v3.0.0+) that became unreliable: ~13-27% of boots
 *   the bootloader read 0 (or kernel-code-like values) instead of HOLD.
 *   The 6.18 kernel scribbles low DRAM during early init / shutdown
 *   before the reserved-memory `no-map` declaration is honored.
 *
 *   The fix is to put HOLD high in DRAM, just below the btcode stack
 *   (which lives at the very top, 0x01FFFFFC and growing down).  The
 *   page 0x01FFE000-0x01FFEFFF is reserved-memory no-map in the device
 *   tree and is far above any address the kernel touches in early boot
 *   (kernel image is loaded at phys 0x00500000).  100% reliable.  The IP
 *   record lives in the same page, so it inherits the same guarantee.
 *
 * Build: mips-lexra-linux-musl-gcc -Os -static -o boothold boothold.c
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>

#define HOLD_PHYS       0x01FFEFFC
#define HOLD_MAGIC      0x484F4C44  /* "HOLD" */
#define IP_MAGIC_PHYS   0x01FFEFF8
#define IP_PHYS         0x01FFEFF4
#define IP_MAGIC        0x49505634  /* "IPV4" */

/* Write a 32-bit word (in DRAM byte order) and read it back to verify. */
static int poke_verify(int fd, off_t phys, uint32_t word)
{
	uint32_t val = htonl(word);
	uint32_t readback;

	if (pwrite(fd, &val, sizeof(val), phys) != sizeof(val)) {
		perror("pwrite");
		return -1;
	}
	if (pread(fd, &readback, sizeof(readback), phys) != sizeof(readback)) {
		perror("pread");
		return -1;
	}
	if (readback != val) {
		fprintf(stderr, "boothold: verify failed at 0x%08lX "
			"(wrote 0x%08X, read 0x%08X)\n",
			(unsigned long)phys, word, ntohl(readback));
		return -1;
	}
	return 0;
}

/* Parse "A.B.C.D" into a packed (a<<24)|(b<<16)|(c<<8)|d host integer. */
static int parse_ipv4(const char *s, uint32_t *out)
{
	int a, b, c, d;

	if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
		return -1;
	if (a < 0 || a > 255 || b < 0 || b > 255 ||
	    c < 0 || c > 255 || d < 0 || d > 255)
		return -1;
	*out = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
	       ((uint32_t)c << 8) | (uint32_t)d;
	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	uint32_t ip = 0;
	int have_ip = 0;

	if (argc > 1) {
		if (parse_ipv4(argv[1], &ip) == 0) {
			have_ip = 1;
		} else {
			fprintf(stderr, "boothold: ignoring invalid IP '%s' "
				"(usage: boothold [A.B.C.D])\n", argv[1]);
		}
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	if (poke_verify(fd, HOLD_PHYS, HOLD_MAGIC) != 0) {
		close(fd);
		return 1;
	}

	if (have_ip) {
		/* Write the IP value first, then its marker last, so the
		 * bootloader never sees a valid marker over a stale value. */
		if (poke_verify(fd, IP_PHYS, ip) != 0 ||
		    poke_verify(fd, IP_MAGIC_PHYS, IP_MAGIC) != 0) {
			close(fd);
			return 1;
		}
	}

	close(fd);

	if (have_ip)
		printf("Boot hold set (TFTP server IP %u.%u.%u.%u).\n",
		       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		       (ip >> 8) & 0xFF, ip & 0xFF);
	else
		printf("Boot hold set.\n");
	return 0;
}
