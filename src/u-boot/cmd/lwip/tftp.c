// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2024-2025 Linaro Ltd. */

#include <command.h>
#include <net.h>

U_BOOT_CMD(tftpboot, 3, 0, do_tftpb,
	   "boot image via network using TFTP protocol",
	   "[loadAddress] [[hostIPaddr:]bootfilename]");

#ifdef CONFIG_CMD_TFTPPUT
U_BOOT_CMD(tftpput, 4, 0, do_tftpput,
	   "send RAM bytes to a TFTP server",
	   "<address> <size-hex> <server-ip>:[port:]<filename>");
#endif
