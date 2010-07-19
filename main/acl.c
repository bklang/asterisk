/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Various sorts of access control
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/network.h"

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__Darwin__)
#include <fcntl.h>
#include <net/route.h>
#endif

#if defined(SOLARIS)
#include <sys/sockio.h>
#include <net/if.h>
#elif defined(HAVE_GETIFADDRS)
#include <ifaddrs.h>
#endif

#include "asterisk/acl.h"
#include "asterisk/channel.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/srv.h"

#if (!defined(SOLARIS) && !defined(HAVE_GETIFADDRS))
static int get_local_address(struct ast_sockaddr *ourip)
{
	return -1;
}
#else
static void score_address(const struct sockaddr_in *sin, struct in_addr *best_addr, int *best_score)
{
	const char *address;
	int score;

	address = ast_inet_ntoa(sin->sin_addr);

	/* RFC 1700 alias for the local network */
	if (address[0] == '0') {
		score = -25;
	/* RFC 1700 localnet */
	} else if (strncmp(address, "127", 3) == 0) {
		score = -20;
	/* RFC 1918 non-public address space */
	} else if (strncmp(address, "10.", 3) == 0) {
		score = -5;
	/* RFC 1918 non-public address space */
	} else if (strncmp(address, "172", 3) == 0) {
		/* 172.16.0.0 - 172.19.255.255, but not 172.160.0.0 - 172.169.255.255 */
		if (address[4] == '1' && address[5] >= '6' && address[6] == '.') {
			score = -5;
		/* 172.20.0.0 - 172.29.255.255, but not 172.200.0.0 - 172.255.255.255 nor 172.2.0.0 - 172.2.255.255 */
		} else if (address[4] == '2' && address[6] == '.') {
			score = -5;
		/* 172.30.0.0 - 172.31.255.255 */
		} else if (address[4] == '3' && address[5] <= '1') {
			score = -5;
		/* All other 172 addresses are public */
		} else {
			score = 0;
		}
	/* RFC 2544 Benchmark test range (198.18.0.0 - 198.19.255.255, but not 198.180.0.0 - 198.199.255.255) */
	} else if (strncmp(address, "198.1", 5) == 0 && address[5] >= '8' && address[6] == '.') {
		score = -10;
	/* RFC 1918 non-public address space */
	} else if (strncmp(address, "192.168", 7) == 0) {
		score = -5;
	/* RFC 3330 Zeroconf network */
	} else if (strncmp(address, "169.254", 7) == 0) {
		/*!\note Better score than a test network, but not quite as good as RFC 1918
		 * address space.  The reason is that some Linux distributions automatically
		 * configure a Zeroconf address before trying DHCP, so we want to prefer a
		 * DHCP lease to a Zeroconf address.
		 */
		score = -10;
	/* RFC 3330 Test network */
	} else if (strncmp(address, "192.0.2.", 8) == 0) {
		score = -15;
	/* Every other address should be publically routable */
	} else {
		score = 0;
	}

	if (score > *best_score) {
		*best_score = score;
		memcpy(best_addr, &sin->sin_addr, sizeof(*best_addr));
	}
}

static int get_local_address(struct ast_sockaddr *ourip)
{
	int s, res = -1;
#ifdef SOLARIS
	struct lifreq *ifr = NULL;
	struct lifnum ifn;
	struct lifconf ifc;
	struct sockaddr_in *sa;
	char *buf = NULL;
	int bufsz, x;
#endif /* SOLARIS */
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__linux__) || defined(__Darwin__)
	struct ifaddrs *ifap, *ifaphead;
	int rtnerr;
	const struct sockaddr_in *sin;
#endif /* BSD_OR_LINUX */
	struct in_addr best_addr;
	int best_score = -100;
	memset(&best_addr, 0, sizeof(best_addr));

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__linux__) || defined(__Darwin__)
	rtnerr = getifaddrs(&ifaphead);
	if (rtnerr) {
		perror(NULL);
		return -1;
	}
#endif /* BSD_OR_LINUX */

	s = socket(AF_INET, SOCK_STREAM, 0);

	if (s > 0) {
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__linux__) || defined(__Darwin__)
		for (ifap = ifaphead; ifap; ifap = ifap->ifa_next) {

			if (ifap->ifa_addr && ifap->ifa_addr->sa_family == AF_INET) {
				sin = (const struct sockaddr_in *) ifap->ifa_addr;
				score_address(sin, &best_addr, &best_score);
				res = 0;

				if (best_score == 0) {
					break;
				}
			}
		}
#endif /* BSD_OR_LINUX */

		/* There is no reason whatsoever that this shouldn't work on Linux or BSD also. */
#ifdef SOLARIS
		/* Get a count of interfaces on the machine */
		ifn.lifn_family = AF_INET;
		ifn.lifn_flags = 0;
		ifn.lifn_count = 0;
		if (ioctl(s, SIOCGLIFNUM, &ifn) < 0) {
			close(s);
			return -1;
		}

		bufsz = ifn.lifn_count * sizeof(struct lifreq);
		if (!(buf = malloc(bufsz))) {
			close(s);
			return -1;
		}
		memset(buf, 0, bufsz);

		/* Get a list of interfaces on the machine */
		ifc.lifc_len = bufsz;
		ifc.lifc_buf = buf;
		ifc.lifc_family = AF_INET;
		ifc.lifc_flags = 0;
		if (ioctl(s, SIOCGLIFCONF, &ifc) < 0) {
			close(s);
			free(buf);
			return -1;
		}

		for (ifr = ifc.lifc_req, x = 0; x < ifn.lifn_count; ifr++, x++) {
			sa = (struct sockaddr_in *)&(ifr->lifr_addr);
			score_address(sa, &best_addr, &best_score);
			res = 0;

			if (best_score == 0) {
				break;
			}
		}

		free(buf);
#endif /* SOLARIS */

		close(s);
	}
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__linux__) || defined(__Darwin__)
	freeifaddrs(ifaphead);
#endif /* BSD_OR_LINUX */

	if (res == 0 && ourip) {
		ast_sockaddr_setnull(ourip);
		ourip->ss.ss_family = AF_INET;
		((struct sockaddr_in *)&ourip->ss)->sin_addr = best_addr;
	}
	return res;
}
#endif /* HAVE_GETIFADDRS */

/* Free HA structure */
void ast_free_ha(struct ast_ha *ha)
{
	struct ast_ha *hal;
	while (ha) {
		hal = ha;
		ha = ha->next;
		ast_free(hal);
	}
}

/* Copy HA structure */
void ast_copy_ha(const struct ast_ha *from, struct ast_ha *to)
{
	ast_sockaddr_copy(&to->addr, &from->addr);
	ast_sockaddr_copy(&to->netmask, &from->netmask);
	to->sense = from->sense;
}

/* Create duplicate of ha structure */
static struct ast_ha *ast_duplicate_ha(struct ast_ha *original)
{
	struct ast_ha *new_ha;

	if ((new_ha = ast_calloc(1, sizeof(*new_ha)))) {
		/* Copy from original to new object */
		ast_copy_ha(original, new_ha);
	}

	return new_ha;
}

/* Create duplicate HA link list */
/*  Used in chan_sip2 templates */
struct ast_ha *ast_duplicate_ha_list(struct ast_ha *original)
{
	struct ast_ha *start = original;
	struct ast_ha *ret = NULL;
	struct ast_ha *current, *prev = NULL;

	while (start) {
		current = ast_duplicate_ha(start);  /* Create copy of this object */
		if (prev) {
			prev->next = current;           /* Link previous to this object */
		}

		if (!ret) {
			ret = current;                  /* Save starting point */
		}

		start = start->next;                /* Go to next object */
		prev = current;                     /* Save pointer to this object */
	}
	return ret;                             /* Return start of list */
}

/*!
 * \brief
 * Isolate a 32-bit section of an IPv6 address
 *
 * An IPv6 address can be divided into 4 32-bit chunks. This gives
 * easy access to one of these chunks.
 *
 * \param sin6 A pointer to a struct sockaddr_in6
 * \param index Which 32-bit chunk to operate on. Must be in the range 0-3.
 */
#define V6_WORD(sin6, index) ((uint32_t *)&((sin6)->sin6_addr))[(index)]

/*!
 * \brief
 * Apply a netmask to an address and store the result in a separate structure.
 *
 * When dealing with IPv6 addresses, one cannot apply a netmask with a simple
 * logical and operation. Furthermore, the incoming address may be an IPv4 address
 * and need to be mapped properly before attempting to apply a rule.
 *
 * \param addr The IP address to apply the mask to.
 * \param netmask The netmask configured in the host access rule.
 * \param result The resultant address after applying the netmask to the given address
 * \retval 0 Successfully applied netmask
 * \reval -1 Failed to apply netmask
 */
static int apply_netmask(const struct ast_sockaddr *addr, const struct ast_sockaddr *netmask,
		struct ast_sockaddr *result)
{
	int res = 0;

	if (ast_sockaddr_is_ipv4(addr)) {
		struct sockaddr_in result4 = { 0, };
		struct sockaddr_in *addr4 = (struct sockaddr_in *) &addr->ss;
		struct sockaddr_in *mask4 = (struct sockaddr_in *) &netmask->ss;
		result4.sin_family = AF_INET;
		result4.sin_addr.s_addr = addr4->sin_addr.s_addr & mask4->sin_addr.s_addr;
		ast_sockaddr_from_sin(result, &result4);
	} else if (ast_sockaddr_is_ipv6(addr)) {
		struct sockaddr_in6 result6 = { 0, };
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) &addr->ss;
		struct sockaddr_in6 *mask6 = (struct sockaddr_in6 *) &netmask->ss;
		int i;
		result6.sin6_family = AF_INET6;
		for (i = 0; i < 4; ++i) {
			V6_WORD(&result6, i) = V6_WORD(addr6, i) & V6_WORD(mask6, i);
		}
		memcpy(&result->ss, &result6, sizeof(result6));
		result->len = sizeof(result6);
	} else {
		/* Unsupported address scheme */
		res = -1;
	}

	return res;
}

/*!
 * \brief
 * Parse a netmask in CIDR notation
 *
 * \details
 * For a mask of an IPv4 address, this should be a number between 0 and 32. For
 * a mask of an IPv6 address, this should be a number between 0 and 128. This
 * function creates an IPv6 ast_sockaddr from the given netmask. For masks of
 * IPv4 addresses, this is accomplished by adding 96 to the original netmask.
 *
 * \param[out] addr The ast_sockaddr produced from the CIDR netmask
 * \param is_v4 Tells if the address we are masking is IPv4.
 * \param mask_str The CIDR mask to convert
 * \retval -1 Failure
 * \retval 0 Success
 */
static int parse_cidr_mask(struct ast_sockaddr *addr, int is_v4, const char *mask_str)
{
	int mask;

	if (sscanf(mask_str, "%30d", &mask) != 1) {
		return -1;
	}

	if (is_v4) {
		struct sockaddr_in sin;
		if (mask < 0 || mask > 32) {
			return -1;
		}
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		/* If mask is 0, then we already have the
		 * appropriate all 0s address in sin from
		 * the above memset.
		 */
		if (mask != 0) {
			sin.sin_addr.s_addr = htonl(0xFFFFFFFF << (32 - mask));
		}
		ast_sockaddr_from_sin(addr, &sin);
	} else {
		struct sockaddr_in6 sin6;
		int i;
		if (mask < 0 || mask > 128) {
			return -1;
		}
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		for (i = 0; i < 4; ++i) {
			/* Once mask reaches 0, we don't have
			 * to explicitly set anything anymore
			 * since sin6 was zeroed out already
			 */
			if (mask > 0) {
				V6_WORD(&sin6, i) = htonl(0xFFFFFFFF << (mask < 32 ? (32 - mask) : 0));
				mask -= mask < 32 ? mask : 32;
			}
		}
		memcpy(&addr->ss, &sin6, sizeof(sin6));
		addr->len = sizeof(sin6);
	}

	return 0;
}

struct ast_ha *ast_append_ha(const char *sense, const char *stuff, struct ast_ha *path, int *error)
{
	struct ast_ha *ha;
	struct ast_ha *prev = NULL;
	struct ast_ha *ret;
	char *tmp = ast_strdupa(stuff);
	char *address = NULL, *mask = NULL;
	int addr_is_v4;

	ret = path;
	while (path) {
		prev = path;
		path = path->next;
	}

	if (!(ha = ast_calloc(1, sizeof(*ha)))) {
		return ret;
	}

	address = strsep(&tmp, "/");
	if (!address) {
		address = tmp;
	} else {
		mask = tmp;
	}

	if (!ast_sockaddr_parse(&ha->addr, address, PARSE_PORT_FORBID)) {
		ast_log(LOG_WARNING, "Invalid IP address: %s\n", address);
		ast_free_ha(ha);
		*error = 1;
		return ret;
	}

	/* If someone specifies an IPv4-mapped IPv6 address,
	 * we just convert this to an IPv4 ACL
	 */
	if (ast_sockaddr_ipv4_mapped(&ha->addr, &ha->addr)) {
		ast_log(LOG_NOTICE, "IPv4-mapped ACL network address specified. "
				"Converting to an IPv4 ACL network address.\n");
	}

	addr_is_v4 = ast_sockaddr_is_ipv4(&ha->addr);

	if (!mask) {
		parse_cidr_mask(&ha->netmask, addr_is_v4, addr_is_v4 ? "32" : "128");
	} else if (strchr(mask, ':') || strchr(mask, '.')) {
		int mask_is_v4;
		/* Mask is of x.x.x.x or x:x:x:x:x:x:x:x variety */
		if (!ast_sockaddr_parse(&ha->netmask, mask, PARSE_PORT_FORBID)) {
			ast_log(LOG_WARNING, "Invalid netmask: %s\n", mask);
			ast_free_ha(ha);
			*error = 1;
			return ret;
		}
		/* If someone specifies an IPv4-mapped IPv6 netmask,
		 * we just convert this to an IPv4 ACL
		 */
		if (ast_sockaddr_ipv4_mapped(&ha->netmask, &ha->netmask)) {
			ast_log(LOG_NOTICE, "IPv4-mapped ACL netmask specified. "
					"Converting to an IPv4 ACL netmask.\n");
		}
		mask_is_v4 = ast_sockaddr_is_ipv4(&ha->netmask);
		if (addr_is_v4 ^ mask_is_v4) {
			ast_log(LOG_WARNING, "Address and mask are not using same address scheme.\n");
			ast_free_ha(ha);
			*error = 1;
			return ret;
		}
	} else if (parse_cidr_mask(&ha->netmask, addr_is_v4, mask)) {
		ast_log(LOG_WARNING, "Invalid CIDR netmask: %s\n", mask);
		ast_free_ha(ha);
		*error = 1;
		return ret;
	}

	if (apply_netmask(&ha->addr, &ha->netmask, &ha->addr)) {
		/* This shouldn't happen because ast_sockaddr_parse would
		 * have failed much earlier on an unsupported address scheme
		 */
		char *failmask = ast_strdupa(ast_sockaddr_stringify(&ha->netmask));
		char *failaddr = ast_strdupa(ast_sockaddr_stringify(&ha->addr));
		ast_log(LOG_WARNING, "Unable to apply netmask %s to address %s\n", failmask, failaddr);
		ast_free_ha(ha);
		*error = 1;
		return ret;
	}

	ha->sense = strncasecmp(sense, "p", 1) ? AST_SENSE_DENY : AST_SENSE_ALLOW;

	ha->next = NULL;
	if (prev) {
		prev->next = ha;
	} else {
		ret = ha;
	}

	ast_debug(1, "%s/%s sense %d appended to acl for peer\n", ast_strdupa(ast_sockaddr_stringify(&ha->addr)), ast_strdupa(ast_sockaddr_stringify(&ha->netmask)), ha->sense);

	return ret;
}

int ast_apply_ha(const struct ast_ha *ha, const struct ast_sockaddr *addr)
{
	/* Start optimistic */
	int res = AST_SENSE_ALLOW;
	const struct ast_ha *current_ha;

	for (current_ha = ha; current_ha; current_ha = current_ha->next) {
		struct ast_sockaddr result;
		struct ast_sockaddr mapped_addr;
		const struct ast_sockaddr *addr_to_use;
#if 0	/* debugging code */
		char iabuf[INET_ADDRSTRLEN];
		char iabuf2[INET_ADDRSTRLEN];
		/* DEBUG */
		ast_copy_string(iabuf, ast_inet_ntoa(sin->sin_addr), sizeof(iabuf));
		ast_copy_string(iabuf2, ast_inet_ntoa(ha->netaddr), sizeof(iabuf2));
		ast_debug(1, "##### Testing %s with %s\n", iabuf, iabuf2);
#endif
		if (ast_sockaddr_is_ipv4(&ha->addr)) {
			if (ast_sockaddr_is_ipv6(addr)) {
				if (ast_sockaddr_is_ipv4_mapped(addr)) {
					/* IPv4 ACLs apply to IPv4-mapped addresses */
					ast_sockaddr_ipv4_mapped(addr, &mapped_addr);
					addr_to_use = &mapped_addr;
				} else {
					/* An IPv4 ACL does not apply to an IPv6 address */
					continue;
				}
			} else {
				/* Address is IPv4 and ACL is IPv4. No biggie */
				addr_to_use = addr;
			}
		} else {
			if (ast_sockaddr_is_ipv6(addr) && !ast_sockaddr_is_ipv4_mapped(addr)) {
				addr_to_use = addr;
			} else {
				/* Address is IPv4 or IPv4 mapped but ACL is IPv6. Skip */
				continue;
			}
		}

		/* For each rule, if this address and the netmask = the net address
		   apply the current rule */
		if (apply_netmask(addr_to_use, &current_ha->netmask, &result)) {
			/* Unlikely to happen since we know the address to be IPv4 or IPv6 */
			continue;
		}
		if (!ast_sockaddr_cmp_addr(&result, &current_ha->addr)) {
			res = current_ha->sense;
		}
	}
	return res;
}

static int resolve_first(struct ast_sockaddr *addr, const char *name, int flag,
			 int family)
{
	struct ast_sockaddr *addrs;
	int addrs_cnt;

	addrs_cnt = ast_sockaddr_resolve(&addrs, name, flag, family);
	if (addrs_cnt > 0) {
		if (addrs_cnt > 1) {
			ast_debug(1, "Multiple addresses. Using the first only\n");
		}
		ast_sockaddr_copy(addr, &addrs[0]);
		ast_free(addrs);
	} else {
		ast_log(LOG_WARNING, "Unable to lookup '%s'\n", name);
		return -1;
	}

	return 0;
}

int ast_get_ip_or_srv(struct ast_sockaddr *addr, const char *value, const char *service)
{
	char srv[256];
	char host[256];
	int srv_ret = 0;
	int tportno;

	if (service) {
		snprintf(srv, sizeof(srv), "%s.%s", service, value);
		if ((srv_ret = ast_get_srv(NULL, host, sizeof(host), &tportno, srv)) > 0) {
			value = host;
		}
	}

	if (resolve_first(addr, value, PARSE_PORT_FORBID, addr->ss.ss_family) != 0) {
		return -1;
	}

	if (srv_ret > 0) {
		ast_sockaddr_set_port(addr, tportno);
	}

	return 0;
}

struct dscp_codepoint {
	char *name;
	unsigned int space;
};

/* IANA registered DSCP codepoints */

static const struct dscp_codepoint dscp_pool1[] = {
	{ "CS0", 0x00 },
	{ "CS1", 0x08 },
	{ "CS2", 0x10 },
	{ "CS3", 0x18 },
	{ "CS4", 0x20 },
	{ "CS5", 0x28 },
	{ "CS6", 0x30 },
	{ "CS7", 0x38 },
	{ "AF11", 0x0A },
	{ "AF12", 0x0C },
	{ "AF13", 0x0E },
	{ "AF21", 0x12 },
	{ "AF22", 0x14 },
	{ "AF23", 0x16 },
	{ "AF31", 0x1A },
	{ "AF32", 0x1C },
	{ "AF33", 0x1E },
	{ "AF41", 0x22 },
	{ "AF42", 0x24 },
	{ "AF43", 0x26 },
	{ "EF", 0x2E },
};

int ast_str2cos(const char *value, unsigned int *cos)
{
	int fval;

	if (sscanf(value, "%30d", &fval) == 1) {
		if (fval < 8) {
		    *cos = fval;
		    return 0;
		}
	}

	return -1;
}

int ast_str2tos(const char *value, unsigned int *tos)
{
	int fval;
	unsigned int x;

	if (sscanf(value, "%30i", &fval) == 1) {
		*tos = fval & 0xFF;
		return 0;
	}

	for (x = 0; x < ARRAY_LEN(dscp_pool1); x++) {
		if (!strcasecmp(value, dscp_pool1[x].name)) {
			*tos = dscp_pool1[x].space << 2;
			return 0;
		}
	}

	return -1;
}

const char *ast_tos2str(unsigned int tos)
{
	unsigned int x;

	for (x = 0; x < ARRAY_LEN(dscp_pool1); x++) {
		if (dscp_pool1[x].space == (tos >> 2)) {
			return dscp_pool1[x].name;
		}
	}

	return "unknown";
}

int ast_get_ip(struct ast_sockaddr *addr, const char *value)
{
	return ast_get_ip_or_srv(addr, value, NULL);
}

int ast_ouraddrfor(const struct ast_sockaddr *them, struct ast_sockaddr *us)
{
	int port;
	int s;

	port = ast_sockaddr_port(us);

	if ((s = socket(ast_sockaddr_is_ipv6(them) ? AF_INET6 : AF_INET,
			SOCK_DGRAM, 0)) < 0) {
		ast_log(LOG_ERROR, "Cannot create socket\n");
		return -1;
	}

	if (ast_connect(s, them)) {
		ast_log(LOG_WARNING, "Cannot connect\n");
		close(s);
		return -1;
	}
	if (ast_getsockname(s, us)) {

		ast_log(LOG_WARNING, "Cannot get socket name\n");
		close(s);
		return -1;
	}
	close(s);
	ast_debug(3, "For destination '%s', our source address is '%s'.\n",
		  ast_strdupa(ast_sockaddr_stringify_addr(them)),
		  ast_strdupa(ast_sockaddr_stringify_addr(us)));

	ast_sockaddr_set_port(us, port);

	return 0;
}

int ast_find_ourip(struct ast_sockaddr *ourip, const struct ast_sockaddr *bindaddr)
{
	char ourhost[MAXHOSTNAMELEN] = "";
	struct ast_sockaddr root;

	/* just use the bind address if it is nonzero */
	if (!ast_sockaddr_is_any(bindaddr)) {
		ast_sockaddr_copy(ourip, bindaddr);
		ast_debug(3, "Attached to given IP address\n");
		return 0;
	}
	/* try to use our hostname */
	if (gethostname(ourhost, sizeof(ourhost) - 1)) {
		ast_log(LOG_WARNING, "Unable to get hostname\n");
	} else {
		if (resolve_first(ourip, ourhost, PARSE_PORT_FORBID, 0) == 0) {
			return 0;
		}
	}
	ast_debug(3, "Trying to check A.ROOT-SERVERS.NET and get our IP address for that connection\n");
	/* A.ROOT-SERVERS.NET. */
	if (!resolve_first(&root, "A.ROOT-SERVERS.NET", PARSE_PORT_FORBID, 0) &&
	    !ast_ouraddrfor(&root, ourip)) {
		return 0;
	}
	return get_local_address(ourip);
}

