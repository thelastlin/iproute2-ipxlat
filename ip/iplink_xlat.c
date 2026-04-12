/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * iplink_xlat.c	ipxlat device support
 *
 * ipxlat is a SIIT (Stateless IP/ICMP Translation) virtual device.
 * Device configuration is handled via generic netlink (ip xlat command),
 * not via rtnl link attributes.
 */

#include <stdio.h>

#include "utils.h"
#include "ip_common.h"

static void xlat_print_help(struct link_util *lu, int argc, char **argv,
			    FILE *f)
{
	fprintf(f,
		"Usage: ip link add ... type ipxlat\n"
		"\n"
		"ipxlat is a SIIT (Stateless IP/ICMP Translation) device.\n"
		"Use 'ip xlat' command to configure translation settings.\n");
}

struct link_util ipxlat_link_util = {
	.id = "ipxlat",
	.parse_opt = NULL,
	.print_opt = NULL,
	.print_help = xlat_print_help,
};
