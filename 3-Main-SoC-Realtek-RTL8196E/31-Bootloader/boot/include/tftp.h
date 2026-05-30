/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tftp.h - Minimal TFTP/ARP state API
 */

#ifndef _BOOT_TFTP_H_
#define _BOOT_TFTP_H_

#include "etherboot.h"

extern struct arptable_t arptable_tftp[2];
extern void kick_tftpd(void);
extern void tftpd_entry(void);
extern unsigned long file_length_to_server;
extern unsigned long image_address;
extern unsigned long g_tftp_server_ip;
extern void tftp_get_server_ip(unsigned char ip[4]);
extern void tftp_set_server_ip(const unsigned char ip[4]);
extern void tftp_set_server_mac(const unsigned char mac[6]);

#endif
