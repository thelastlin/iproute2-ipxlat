/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ipxlat.c		"ip xlat"
 *
 * Configuration tool for ipxlat SIIT devices using generic netlink.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/genetlink.h>
#include <linux/ipxlat.h>

#include "utils.h"
#include "ip_common.h"
#include "json_print.h"
#include "ll_map.h"
#include "libgenl.h"

#define IPXLAT_BUFLEN 1024

static struct rtnl_handle genl_rth;
static int genl_family = -1;

#define IPXLAT_GENL_REQ(_req, _cmd, _flags) \
	GENL_REQUEST(_req, IPXLAT_BUFLEN, genl_family, 0, \
		     IPXLAT_FAMILY_VERSION, _cmd, _flags)

static void usage(void)
{
	fprintf(stderr,
		"Usage: ip xlat set dev NAME [ xlat-prefix6 ADDR/LEN ] [ lowest-ipv6-mtu MTU ]\n"
		"       ip xlat show [ dev NAME ]\n");
	exit(-1);
}

static int do_set(int argc, char **argv)
{
	inet_prefix prefix6;
	__u32 lowest_mtu = 0;
	int ifindex;
	bool prefix6_set = false, mtu_set = false;

	inet_prefix_reset(&prefix6);

	if (argc == 0)
		missarg("dev");

	if (strcmp(*argv, "dev") != 0)
		invarg("expected dev", *argv);
	NEXT_ARG();

	ifindex = ll_name_to_index(*argv);
	if (!ifindex)
		return nodev(*argv);
	NEXT_ARG_FWD();

	while (argc > 0) {
		if (strcmp(*argv, "xlat-prefix6") == 0) {
			NEXT_ARG();
			if (get_prefix(&prefix6, *argv, AF_INET6))
				invarg("invalid xlat-prefix6", *argv);
			if (prefix6.bitlen == 0 ||
			    prefix6.bitlen > IPXLAT_XLAT_PREFIX6_MAX_PREFIX_LEN)
				invarg("prefix length out of range", *argv);
			prefix6_set = true;
			argc--; argv++;
		} else if (strcmp(*argv, "lowest-ipv6-mtu") == 0) {
			NEXT_ARG();
			if (get_u32(&lowest_mtu, *argv, 0))
				invarg("invalid MTU", *argv);
			mtu_set = true;
			argc--; argv++;
		} else {
			invarg("unknown argument", *argv);
		}
	}

	if (!prefix6_set && !mtu_set) {
		fprintf(stderr, "Nothing to set.\n");
		return -1;
	}

	IPXLAT_GENL_REQ(req, IPXLAT_CMD_DEV_SET, NLM_F_REQUEST);

	addattr32(&req.n, IPXLAT_BUFLEN, IPXLAT_A_DEV_IFINDEX, ifindex);

	if (prefix6_set || mtu_set) {
		struct rtattr *nest;

		nest = addattr_nest(&req.n, IPXLAT_BUFLEN, IPXLAT_A_DEV_CONFIG | NLA_F_NESTED);

		if (prefix6_set) {
			struct rtattr *prefix_nest;

			prefix_nest = addattr_nest(&req.n, IPXLAT_BUFLEN,
						   IPXLAT_A_CFG_XLAT_PREFIX6);
			prefix_nest->rta_type |= NLA_F_NESTED;
			addattr_l(&req.n, IPXLAT_BUFLEN, IPXLAT_A_POOL_PREFIX,
				  prefix6.data, prefix6.bytelen);
			addattr8(&req.n, IPXLAT_BUFLEN, IPXLAT_A_POOL_PREFIX_LEN,
				 prefix6.bitlen);
			addattr_nest_end(&req.n, prefix_nest);
		}

		if (mtu_set)
			addattr32(&req.n, IPXLAT_BUFLEN,
				  IPXLAT_A_CFG_LOWEST_IPV6_MTU, lowest_mtu);

		addattr_nest_end(&req.n, nest);
	}

	if (rtnl_talk(&genl_rth, &req.n, NULL) < 0)
		return -2;

	return 0;
}

static void print_prefix6(struct rtattr *attr)
{
	struct rtattr *tb[IPXLAT_A_POOL_MAX + 1];
	__u8 plen;
	char addr_buf[INET6_ADDRSTRLEN];

	parse_rtattr_nested(tb, IPXLAT_A_POOL_MAX, attr);

	if (!tb[IPXLAT_A_POOL_PREFIX] || !tb[IPXLAT_A_POOL_PREFIX_LEN])
		return;

	if (RTA_PAYLOAD(tb[IPXLAT_A_POOL_PREFIX]) < sizeof(struct in6_addr))
		return;

	plen = rta_getattr_u8(tb[IPXLAT_A_POOL_PREFIX_LEN]);
	format_host_rta_r(AF_INET6, tb[IPXLAT_A_POOL_PREFIX], addr_buf, sizeof(addr_buf));

	open_json_object("xlat-prefix6");
	print_string(PRINT_ANY, "prefix", "xlat-prefix6 %s", addr_buf);
	print_uint(PRINT_ANY, "prefix-len", "/%u", plen);
	close_json_object();
}

static int print_config(struct nlmsghdr *n, void *arg)
{
	struct genlmsghdr *ghdr;
	struct rtattr *tb[IPXLAT_A_DEV_MAX + 1];
	struct rtattr *cfg_tb[IPXLAT_A_CFG_MAX + 1];
	int len = n->nlmsg_len;
	int ifindex;
	int *filter_ifindex = arg;

	if (n->nlmsg_type != genl_family)
		return -1;

	len -= NLMSG_LENGTH(GENL_HDRLEN);
	if (len < 0)
		return -1;

	ghdr = NLMSG_DATA(n);
	if (ghdr->cmd != IPXLAT_CMD_DEV_GET)
		return 0;

	parse_rtattr_flags(tb, IPXLAT_A_DEV_MAX, (void *)ghdr + GENL_HDRLEN, len,
			   NLA_F_NESTED);

	if (!tb[IPXLAT_A_DEV_IFINDEX])
		return -1;

	ifindex = rta_getattr_u32(tb[IPXLAT_A_DEV_IFINDEX]);

	/* Filter by ifindex if specified */
	if (filter_ifindex && *filter_ifindex && ifindex != *filter_ifindex)
		return 0;

	open_json_object(NULL);
	print_uint(PRINT_ANY, "ifindex", "%u: ", ifindex);
	print_color_string(PRINT_ANY, COLOR_IFNAME, "ifname",
			   "%s:", ll_index_to_name(ifindex));
	print_nl();

	if (tb[IPXLAT_A_DEV_CONFIG]) {
		parse_rtattr_nested(cfg_tb, IPXLAT_A_CFG_MAX,
				    tb[IPXLAT_A_DEV_CONFIG]);

		if (cfg_tb[IPXLAT_A_CFG_XLAT_PREFIX6]) {
			print_string(PRINT_FP, NULL, "    ", NULL);
			print_prefix6(cfg_tb[IPXLAT_A_CFG_XLAT_PREFIX6]);
			print_nl();
		}

		if (cfg_tb[IPXLAT_A_CFG_LOWEST_IPV6_MTU]) {
			__u32 mtu = rta_getattr_u32(cfg_tb[IPXLAT_A_CFG_LOWEST_IPV6_MTU]);
			print_string(PRINT_FP, NULL, "    ", NULL);
			print_uint(PRINT_ANY, "lowest-ipv6-mtu",
				   "lowest-ipv6-mtu %u", mtu);
			print_nl();
		}
	}

	close_json_object();

	return 0;
}

static int do_show(int argc, char **argv)
{
	int filter_ifindex = 0;

	if (argc > 0 && strcmp(*argv, "dev") == 0) {
		NEXT_ARG();
		filter_ifindex = ll_name_to_index(*argv);
		if (!filter_ifindex)
			return nodev(*argv);
	}

	IPXLAT_GENL_REQ(req, IPXLAT_CMD_DEV_GET,
			NLM_F_REQUEST | NLM_F_DUMP);

	req.n.nlmsg_seq = genl_rth.dump = ++genl_rth.seq;
	if (rtnl_send(&genl_rth, &req, req.n.nlmsg_len) < 0) {
		perror("Failed to send request");
		return -1;
	}

	new_json_obj(json);
	if (rtnl_dump_filter(&genl_rth, print_config, &filter_ifindex) < 0) {
		delete_json_obj();
		fprintf(stderr, "Dump terminated\n");
		return -1;
	}
	delete_json_obj();

	return 0;
}

int do_ipxlat(int argc, char **argv)
{
	if (argc < 1)
		usage();

	if (strcmp(*argv, "help") == 0)
		usage();

	if (genl_init_handle(&genl_rth, IPXLAT_FAMILY_NAME, &genl_family))
		exit(1);

	if (strcmp(*argv, "set") == 0)
		return do_set(argc - 1, argv + 1);
	if (strcmp(*argv, "show") == 0)
		return do_show(argc - 1, argv + 1);

	fprintf(stderr, "Command \"%s\" is unknown, try \"ip xlat help\".\n",
		*argv);
	return -1;
}
