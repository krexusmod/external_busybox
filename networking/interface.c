/*
 * stolen from net-tools-1.59 and stripped down for busybox by
 *			Erik Andersen <andersen@codepoet.org>
 *
 * Heavily modified by Manuel Novoa III       Mar 12, 2001
 *
 * Added print_bytes_scaled function to reduce code size.
 * Added some (potentially) missing defines.
 * Improved display support for -a and for a named interface.
 *
 * -----------------------------------------------------------
 *
 * ifconfig   This file contains an implementation of the command
 *              that either displays or sets the characteristics of
 *              one or more of the system's networking interfaces.
 *
 * Version:     $Id: interface.c,v 1.25 2004/08/26 21:45:21 andersen Exp $
 *
 * Author:      Fred N. van Kempen, <waltje@uwalt.nl.mugnet.org>
 *              and others.  Copyright 1993 MicroWalt Corporation
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 *
 * Patched to support 'add' and 'del' keywords for INET(4) addresses
 * by Mrs. Brisby <mrs.brisby@nimh.org>
 *
 * {1.34} - 19980630 - Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *                     - gettext instead of catgets for i18n
 *          10/1998  - Andi Kleen. Use interface list primitives.
 *	    20001008 - Bernd Eckenfels, Patch from RH for setting mtu
 *			(default AF was wrong)
 */

#include "inet_common.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <net/if.h>
#include <net/if_arp.h>
#include "busybox.h"

#ifdef CONFIG_FEATURE_IPV6
# define HAVE_AFINET6 1
#else
# undef HAVE_AFINET6
#endif

#define _PATH_PROCNET_DEV               "/proc/net/dev"
#define _PATH_PROCNET_IFINET6           "/proc/net/if_inet6"

#if HAVE_AFINET6

#ifndef _LINUX_IN6_H
/*
 *    This is in linux/include/net/ipv6.h.
 */

struct in6_ifreq {
	struct in6_addr ifr6_addr;
	uint32_t ifr6_prefixlen;
	unsigned int ifr6_ifindex;
};

#endif

#endif							/* HAVE_AFINET6 */

/* Defines for glibc2.0 users. */
#ifndef SIOCSIFTXQLEN
#define SIOCSIFTXQLEN      0x8943
#define SIOCGIFTXQLEN      0x8942
#endif

/* ifr_qlen is ifru_ivalue, but it isn't present in 2.0 kernel headers */
#ifndef ifr_qlen
#define ifr_qlen        ifr_ifru.ifru_mtu
#endif

#ifndef HAVE_TXQUEUELEN
#define HAVE_TXQUEUELEN 1
#endif

#ifndef IFF_DYNAMIC
#define IFF_DYNAMIC     0x8000	/* dialup device with changing addresses */
#endif

/* This structure defines protocol families and their handlers. */
struct aftype {
	const char *name;
	const char *title;
	int af;
	int alen;
	char *(*print) (unsigned char *);
	char *(*sprint) (struct sockaddr *, int numeric);
	int (*input) (int type, char *bufp, struct sockaddr *);
	void (*herror) (char *text);
	int (*rprint) (int options);
	int (*rinput) (int typ, int ext, char **argv);

	/* may modify src */
	int (*getmask) (char *src, struct sockaddr * mask, char *name);

	int fd;
	char *flag_file;
};

/* Display an Internet socket address. */
static char *INET_sprint(struct sockaddr *sap, int numeric)
{
	static char buff[128];

	if (sap->sa_family == 0xFFFF || sap->sa_family == 0)
		return safe_strncpy(buff, "[NONE SET]", sizeof(buff));

	if (INET_rresolve(buff, sizeof(buff), (struct sockaddr_in *) sap,
					  numeric, 0xffffff00) != 0)
		return (NULL);

	return (buff);
}

static struct aftype inet_aftype = {
	"inet", "DARPA Internet", AF_INET, sizeof(unsigned long),
	NULL /* UNUSED INET_print */ , INET_sprint,
	NULL /* UNUSED INET_input */ , NULL /* UNUSED INET_reserror */ ,
	NULL /*INET_rprint */ , NULL /*INET_rinput */ ,
	NULL /* UNUSED INET_getnetmask */ ,
	-1,
	NULL
};

#if HAVE_AFINET6

/* Display an Internet socket address. */
/* dirty! struct sockaddr usually doesn't suffer for inet6 addresses, fst. */
static char *INET6_sprint(struct sockaddr *sap, int numeric)
{
	static char buff[128];

	if (sap->sa_family == 0xFFFF || sap->sa_family == 0)
		return safe_strncpy(buff, "[NONE SET]", sizeof(buff));
	if (INET6_rresolve
		(buff, sizeof(buff), (struct sockaddr_in6 *) sap, numeric) != 0)
		return safe_strncpy(buff, "[UNKNOWN]", sizeof(buff));
	return (buff);
}

static struct aftype inet6_aftype = {
	"inet6", "IPv6", AF_INET6, sizeof(struct in6_addr),
	NULL /* UNUSED INET6_print */ , INET6_sprint,
	NULL /* UNUSED INET6_input */ , NULL /* UNUSED INET6_reserror */ ,
	NULL /*INET6_rprint */ , NULL /*INET6_rinput */ ,
	NULL /* UNUSED INET6_getnetmask */ ,
	-1,
	NULL
};

#endif							/* HAVE_AFINET6 */

/* Display an UNSPEC address. */
static char *UNSPEC_print(unsigned char *ptr)
{
	static char buff[sizeof(struct sockaddr) * 3 + 1];
	char *pos;
	unsigned int i;

	pos = buff;
	for (i = 0; i < sizeof(struct sockaddr); i++) {
		/* careful -- not every libc's sprintf returns # bytes written */
		sprintf(pos, "%02X-", (*ptr++ & 0377));
		pos += 3;
	}
	/* Erase trailing "-".  Works as long as sizeof(struct sockaddr) != 0 */
	*--pos = '\0';
	return (buff);
}

/* Display an UNSPEC socket address. */
static char *UNSPEC_sprint(struct sockaddr *sap, int numeric)
{
	static char buf[64];

	if (sap->sa_family == 0xFFFF || sap->sa_family == 0)
		return safe_strncpy(buf, "[NONE SET]", sizeof(buf));
	return (UNSPEC_print((unsigned char *)sap->sa_data));
}

static struct aftype unspec_aftype = {
	"unspec", "UNSPEC", AF_UNSPEC, 0,
	UNSPEC_print, UNSPEC_sprint, NULL, NULL,
	NULL,
};

static struct aftype * const aftypes[] = {
	&inet_aftype,
#if HAVE_AFINET6
	&inet6_aftype,
#endif
	&unspec_aftype,
	NULL
};

/* Check our protocol family table for this family. */
static struct aftype *get_afntype(int af)
{
	struct aftype * const *afp;

	afp = aftypes;
	while (*afp != NULL) {
		if ((*afp)->af == af)
			return (*afp);
		afp++;
	}
	return (NULL);
}

/* Check our protocol family table for this family and return its socket */
static int get_socket_for_af(int af)
{
	struct aftype * const *afp;

	afp = aftypes;
	while (*afp != NULL) {
		if ((*afp)->af == af)
			return (*afp)->fd;
		afp++;
	}
	return -1;
}

struct user_net_device_stats {
	unsigned long long rx_packets;	/* total packets received       */
	unsigned long long tx_packets;	/* total packets transmitted    */
	unsigned long long rx_bytes;	/* total bytes received         */
	unsigned long long tx_bytes;	/* total bytes transmitted      */
	unsigned long rx_errors;	/* bad packets received         */
	unsigned long tx_errors;	/* packet transmit problems     */
	unsigned long rx_dropped;	/* no space in linux buffers    */
	unsigned long tx_dropped;	/* no space available in linux  */
	unsigned long rx_multicast;	/* multicast packets received   */
	unsigned long rx_compressed;
	unsigned long tx_compressed;
	unsigned long collisions;

	/* detailed rx_errors: */
	unsigned long rx_length_errors;
	unsigned long rx_over_errors;	/* receiver ring buff overflow  */
	unsigned long rx_crc_errors;	/* recved pkt with crc error    */
	unsigned long rx_frame_errors;	/* recv'd frame alignment error */
	unsigned long rx_fifo_errors;	/* recv'r fifo overrun          */
	unsigned long rx_missed_errors;	/* receiver missed packet     */
	/* detailed tx_errors */
	unsigned long tx_aborted_errors;
	unsigned long tx_carrier_errors;
	unsigned long tx_fifo_errors;
	unsigned long tx_heartbeat_errors;
	unsigned long tx_window_errors;
};

struct interface {
	struct interface *next, *prev;
	char name[IFNAMSIZ];	/* interface name        */
	short type;			/* if type               */
	short flags;		/* various flags         */
	int metric;			/* routing metric        */
	int mtu;			/* MTU value             */
	int tx_queue_len;	/* transmit queue length */
	struct ifmap map;	/* hardware setup        */
	struct sockaddr addr;	/* IP address            */
	struct sockaddr dstaddr;	/* P-P IP address        */
	struct sockaddr broadaddr;	/* IP broadcast address  */
	struct sockaddr netmask;	/* IP network mask       */
	int has_ip;
	char hwaddr[32];	/* HW address            */
	int statistics_valid;
	struct user_net_device_stats stats;	/* statistics            */
	int keepalive;		/* keepalive value for SLIP */
	int outfill;		/* outfill value for SLIP */
};


int interface_opt_a = 0;	/* show all interfaces          */

static struct interface *int_list, *int_last;
static int skfd = -1;	/* generic raw socket desc.     */


static int sockets_open(int family)
{
	struct aftype * const *aft;
	int sfd = -1;
	static int force = -1;

	if (force < 0) {
		force = 0;
		if (get_linux_version_code() < KERNEL_VERSION(2,1,0))
			force = 1;
		if (access("/proc/net", R_OK))
			force = 1;
	}
	for (aft = aftypes; *aft; aft++) {
		struct aftype *af = *aft;
		int type = SOCK_DGRAM;

		if (af->af == AF_UNSPEC)
			continue;
		if (family && family != af->af)
			continue;
		if (af->fd != -1) {
			sfd = af->fd;
			continue;
		}
		/* Check some /proc file first to not stress kmod */
		if (!family && !force && af->flag_file) {
			if (access(af->flag_file, R_OK))
				continue;
		}
		af->fd = socket(af->af, type, 0);
		if (af->fd >= 0)
			sfd = af->fd;
	}
	if (sfd < 0) {
		bb_error_msg("No usable address families found.");
	}
	return sfd;
}

#ifdef CONFIG_FEATURE_CLEAN_UP
static void sockets_close(void)
{
	struct aftype * const *aft;
	for (aft = aftypes; *aft != NULL; aft++) {
		struct aftype *af = *aft;
		if( af->fd != -1 ) {
			close(af->fd);
			af->fd = -1;
		}
	}
}
#endif

/* like strcmp(), but knows about numbers */
static int nstrcmp(const char *a, const char *b)
{
	const char *a_ptr = a;
	const char *b_ptr = b;

	while (*a == *b) {
		if (*a == '\0') {
			return 0;
		}
		if (!isdigit(*a) && isdigit(*(a+1))) {
			a_ptr = a+1;
			b_ptr = b+1;
		}
		a++;
		b++;
	}

	if (isdigit(*a) && isdigit(*b)) {
		return atoi(a_ptr) > atoi(b_ptr) ? 1 : -1;
	}
	return *a - *b;
}

static struct interface *add_interface(char *name)
{
	struct interface *ife, **nextp, *new;

	for (ife = int_last; ife; ife = ife->prev) {
		int n = nstrcmp(ife->name, name);

		if (n == 0)
			return ife;
		if (n < 0)
			break;
	}

	new = xzalloc(sizeof(*new));
	safe_strncpy(new->name, name, IFNAMSIZ);
	nextp = ife ? &ife->next : &int_list;
	new->prev = ife;
	new->next = *nextp;
	if (new->next)
		new->next->prev = new;
	else
		int_last = new;
	*nextp = new;
	return new;
}


static int if_readconf(void)
{
	int numreqs = 30;
	struct ifconf ifc;
	struct ifreq *ifr;
	int n, err = -1;
	int skfd2;

	/* SIOCGIFCONF currently seems to only work properly on AF_INET sockets
	   (as of 2.1.128) */
	skfd2 = get_socket_for_af(AF_INET);
	if (skfd2 < 0) {
		bb_perror_msg(("warning: no inet socket available"));
		/* Try to soldier on with whatever socket we can get hold of.  */
		skfd2 = sockets_open(0);
		if (skfd2 < 0)
			return -1;
	}

	ifc.ifc_buf = NULL;
	for (;;) {
		ifc.ifc_len = sizeof(struct ifreq) * numreqs;
		ifc.ifc_buf = xrealloc(ifc.ifc_buf, ifc.ifc_len);

		if (ioctl(skfd2, SIOCGIFCONF, &ifc) < 0) {
			perror("SIOCGIFCONF");
			goto out;
		}
		if (ifc.ifc_len == sizeof(struct ifreq) * numreqs) {
			/* assume it overflowed and try again */
			numreqs += 10;
			continue;
		}
		break;
	}

	ifr = ifc.ifc_req;
	for (n = 0; n < ifc.ifc_len; n += sizeof(struct ifreq)) {
		add_interface(ifr->ifr_name);
		ifr++;
	}
	err = 0;

  out:
	free(ifc.ifc_buf);
	return err;
}

static char *get_name(char *name, char *p)
{
	/* Extract <name>[:<alias>] from nul-terminated p where p matches
	   <name>[:<alias>]: after leading whitespace.
	   If match is not made, set name empty and return unchanged p */
	int namestart=0, nameend=0, aliasend;
	while (isspace(p[namestart]))
		namestart++;
	nameend=namestart;
	while (p[nameend] && p[nameend]!=':' && !isspace(p[nameend]))
		nameend++;
	if (p[nameend]==':') {
		aliasend=nameend+1;
		while (p[aliasend] && isdigit(p[aliasend]))
			aliasend++;
		if (p[aliasend]==':') {
			nameend=aliasend;
		}
		if ((nameend-namestart)<IFNAMSIZ) {
			memcpy(name,&p[namestart],nameend-namestart);
			name[nameend-namestart]='\0';
			p=&p[nameend];
		} else {
			/* Interface name too large */
			name[0]='\0';
		}
	} else {
		/* first ':' not found - return empty */
		name[0]='\0';
	}
	return p + 1;
}

/* If scanf supports size qualifiers for %n conversions, then we can
 * use a modified fmt that simply stores the position in the fields
 * having no associated fields in the proc string.  Of course, we need
 * to zero them again when we're done.  But that is smaller than the
 * old approach of multiple scanf occurrences with large numbers of
 * args. */

/* static const char * const ss_fmt[] = { */
/*	"%lln%llu%lu%lu%lu%lu%ln%ln%lln%llu%lu%lu%lu%lu%lu", */
/*	"%llu%llu%lu%lu%lu%lu%ln%ln%llu%llu%lu%lu%lu%lu%lu", */
/*	"%llu%llu%lu%lu%lu%lu%lu%lu%llu%llu%lu%lu%lu%lu%lu%lu" */
/* }; */

	/* Lie about the size of the int pointed to for %n. */
#if INT_MAX == LONG_MAX
static const char * const ss_fmt[] = {
	"%n%llu%u%u%u%u%n%n%n%llu%u%u%u%u%u",
	"%llu%llu%u%u%u%u%n%n%llu%llu%u%u%u%u%u",
	"%llu%llu%u%u%u%u%u%u%llu%llu%u%u%u%u%u%u"
};
#else
static const char * const ss_fmt[] = {
	"%n%llu%lu%lu%lu%lu%n%n%n%llu%lu%lu%lu%lu%lu",
	"%llu%llu%lu%lu%lu%lu%n%n%llu%llu%lu%lu%lu%lu%lu",
	"%llu%llu%lu%lu%lu%lu%lu%lu%llu%llu%lu%lu%lu%lu%lu%lu"
};

#endif

static void get_dev_fields(char *bp, struct interface *ife, int procnetdev_vsn)
{
	memset(&ife->stats, 0, sizeof(struct user_net_device_stats));

	sscanf(bp, ss_fmt[procnetdev_vsn],
		   &ife->stats.rx_bytes, /* missing for 0 */
		   &ife->stats.rx_packets,
		   &ife->stats.rx_errors,
		   &ife->stats.rx_dropped,
		   &ife->stats.rx_fifo_errors,
		   &ife->stats.rx_frame_errors,
		   &ife->stats.rx_compressed, /* missing for <= 1 */
		   &ife->stats.rx_multicast, /* missing for <= 1 */
		   &ife->stats.tx_bytes, /* missing for 0 */
		   &ife->stats.tx_packets,
		   &ife->stats.tx_errors,
		   &ife->stats.tx_dropped,
		   &ife->stats.tx_fifo_errors,
		   &ife->stats.collisions,
		   &ife->stats.tx_carrier_errors,
		   &ife->stats.tx_compressed /* missing for <= 1 */
		   );

	if (procnetdev_vsn <= 1) {
		if (procnetdev_vsn == 0) {
			ife->stats.rx_bytes = 0;
			ife->stats.tx_bytes = 0;
		}
		ife->stats.rx_multicast = 0;
		ife->stats.rx_compressed = 0;
		ife->stats.tx_compressed = 0;
	}
}

static inline int procnetdev_version(char *buf)
{
	if (strstr(buf, "compressed"))
		return 2;
	if (strstr(buf, "bytes"))
		return 1;
	return 0;
}

static int if_readlist_proc(char *target)
{
	static int proc_read;
	FILE *fh;
	char buf[512];
	struct interface *ife;
	int err, procnetdev_vsn;

	if (proc_read)
		return 0;
	if (!target)
		proc_read = 1;

	fh = fopen(_PATH_PROCNET_DEV, "r");
	if (!fh) {
		bb_perror_msg("Warning: cannot open %s. Limited output.", _PATH_PROCNET_DEV);
		return if_readconf();
	}
	fgets(buf, sizeof buf, fh);	/* eat line */
	fgets(buf, sizeof buf, fh);

	procnetdev_vsn = procnetdev_version(buf);

	err = 0;
	while (fgets(buf, sizeof buf, fh)) {
		char *s, name[128];

		s = get_name(name, buf);
		ife = add_interface(name);
		get_dev_fields(s, ife, procnetdev_vsn);
		ife->statistics_valid = 1;
		if (target && !strcmp(target, name))
			break;
	}
	if (ferror(fh)) {
		perror(_PATH_PROCNET_DEV);
		err = -1;
		proc_read = 0;
	}
	fclose(fh);
	return err;
}

static int if_readlist(void)
{
	int err = if_readlist_proc(NULL);

	if (!err)
		err = if_readconf();
	return err;
}

static int for_all_interfaces(int (*doit) (struct interface *, void *),
							  void *cookie)
{
	struct interface *ife;

	if (!int_list && (if_readlist() < 0))
		return -1;
	for (ife = int_list; ife; ife = ife->next) {
		int err = doit(ife, cookie);

		if (err)
			return err;
	}
	return 0;
}

/* Fetch the interface configuration from the kernel. */
static int if_fetch(struct interface *ife)
{
	struct ifreq ifr;
	int fd;
	char *ifname = ife->name;

	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFFLAGS, &ifr) < 0)
		return (-1);
	ife->flags = ifr.ifr_flags;

	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFHWADDR, &ifr) < 0)
		memset(ife->hwaddr, 0, 32);
	else
		memcpy(ife->hwaddr, ifr.ifr_hwaddr.sa_data, 8);

	ife->type = ifr.ifr_hwaddr.sa_family;

	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFMETRIC, &ifr) < 0)
		ife->metric = 0;
	else
		ife->metric = ifr.ifr_metric;

	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFMTU, &ifr) < 0)
		ife->mtu = 0;
	else
		ife->mtu = ifr.ifr_mtu;

#ifdef SIOCGIFMAP
	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFMAP, &ifr) == 0)
		ife->map = ifr.ifr_map;
	else
#endif
		memset(&ife->map, 0, sizeof(struct ifmap));

#ifdef HAVE_TXQUEUELEN
	strcpy(ifr.ifr_name, ifname);
	if (ioctl(skfd, SIOCGIFTXQLEN, &ifr) < 0)
		ife->tx_queue_len = -1;	/* unknown value */
	else
		ife->tx_queue_len = ifr.ifr_qlen;
#else
	ife->tx_queue_len = -1;	/* unknown value */
#endif

	/* IPv4 address? */
	fd = get_socket_for_af(AF_INET);
	if (fd >= 0) {
		strcpy(ifr.ifr_name, ifname);
		ifr.ifr_addr.sa_family = AF_INET;
		if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
			ife->has_ip = 1;
			ife->addr = ifr.ifr_addr;
			strcpy(ifr.ifr_name, ifname);
			if (ioctl(fd, SIOCGIFDSTADDR, &ifr) < 0)
				memset(&ife->dstaddr, 0, sizeof(struct sockaddr));
			else
				ife->dstaddr = ifr.ifr_dstaddr;

			strcpy(ifr.ifr_name, ifname);
			if (ioctl(fd, SIOCGIFBRDADDR, &ifr) < 0)
				memset(&ife->broadaddr, 0, sizeof(struct sockaddr));
			else
				ife->broadaddr = ifr.ifr_broadaddr;

			strcpy(ifr.ifr_name, ifname);
			if (ioctl(fd, SIOCGIFNETMASK, &ifr) < 0)
				memset(&ife->netmask, 0, sizeof(struct sockaddr));
			else
				ife->netmask = ifr.ifr_netmask;
		} else
			memset(&ife->addr, 0, sizeof(struct sockaddr));
	}

	return 0;
}


static int do_if_fetch(struct interface *ife)
{
	if (if_fetch(ife) < 0) {
		char *errmsg;

		if (errno == ENODEV) {
			/* Give better error message for this case. */
			errmsg = "Device not found";
		} else {
			errmsg = strerror(errno);
		}
		bb_error_msg("%s: error fetching interface information: %s",
				ife->name, errmsg);
		return -1;
	}
	return 0;
}

/* This structure defines hardware protocols and their handlers. */
struct hwtype {
	const char * const name;
	const char *title;
	int type;
	int alen;
	char *(*print) (unsigned char *);
	int (*input) (char *, struct sockaddr *);
	int (*activate) (int fd);
	int suppress_null_addr;
};

static const struct hwtype unspec_hwtype = {
	"unspec", "UNSPEC", -1, 0,
	UNSPEC_print, NULL, NULL
};

static const struct hwtype loop_hwtype = {
	"loop", "Local Loopback", ARPHRD_LOOPBACK, 0,
	NULL, NULL, NULL
};

#include <net/if_arp.h>

#if (__GLIBC__ >=2 && __GLIBC_MINOR >= 1) || defined(_NEWLIB_VERSION)
#include <net/ethernet.h>
#else
#include <linux/if_ether.h>
#endif

/* Display an Ethernet address in readable format. */
static char *pr_ether(unsigned char *ptr)
{
	static char buff[64];

	snprintf(buff, sizeof(buff), "%02X:%02X:%02X:%02X:%02X:%02X",
			 (ptr[0] & 0377), (ptr[1] & 0377), (ptr[2] & 0377),
			 (ptr[3] & 0377), (ptr[4] & 0377), (ptr[5] & 0377)
		);
	return (buff);
}

static const struct hwtype ether_hwtype = {
	"ether", "Ethernet", ARPHRD_ETHER, ETH_ALEN,
	pr_ether, NULL /* UNUSED in_ether */ , NULL
};

#include <net/if_arp.h>

static const struct hwtype ppp_hwtype = {
	"ppp", "Point-Point Protocol", ARPHRD_PPP, 0,
	NULL, NULL, NULL /* UNUSED do_ppp */ , 0
};

static const struct hwtype * const hwtypes[] = {
	&loop_hwtype,
	&ether_hwtype,
	&ppp_hwtype,
	&unspec_hwtype,
	NULL
};

#ifdef IFF_PORTSEL
static const char * const if_port_text[] = {
	/* Keep in step with <linux/netdevice.h> */
	"unknown",
	"10base2",
	"10baseT",
	"AUI",
	"100baseT",
	"100baseTX",
	"100baseFX",
	NULL
};
#endif

/* Check our hardware type table for this type. */
static const struct hwtype *get_hwntype(int type)
{
	const struct hwtype * const *hwp;

	hwp = hwtypes;
	while (*hwp != NULL) {
		if ((*hwp)->type == type)
			return (*hwp);
		hwp++;
	}
	return (NULL);
}

/* return 1 if address is all zeros */
static int hw_null_address(const struct hwtype *hw, void *ap)
{
	unsigned int i;
	unsigned char *address = (unsigned char *) ap;

	for (i = 0; i < hw->alen; i++)
		if (address[i])
			return 0;
	return 1;
}

static const char TRext[] = "\0\0\0Ki\0Mi\0Gi\0Ti";

static void print_bytes_scaled(unsigned long long ull, const char *end)
{
	unsigned long long int_part;
	const char *ext;
	unsigned int frac_part;
	int i;

	frac_part = 0;
	ext = TRext;
	int_part = ull;
	i = 4;
	do {
		if (int_part >= 1024) {
			frac_part = ((((unsigned int) int_part) & (1024-1)) * 10) / 1024;
			int_part /= 1024;
			ext += 3;	/* KiB, MiB, GiB, TiB */
		}
		--i;
	} while (i);

	printf("X bytes:%llu (%llu.%u %sB)%s", ull, int_part, frac_part, ext, end);
}

static const char * const ife_print_flags_strs[] = {
	"UP ",
	"BROADCAST ",
	"DEBUG ",
	"LOOPBACK ",
	"POINTOPOINT ",
	"NOTRAILERS ",
	"RUNNING ",
	"NOARP ",
	"PROMISC ",
	"ALLMULTI ",
	"SLAVE ",
	"MASTER ",
	"MULTICAST ",
#ifdef HAVE_DYNAMIC
	"DYNAMIC "
#endif
};

static const unsigned short ife_print_flags_mask[] = {
	IFF_UP,
	IFF_BROADCAST,
	IFF_DEBUG,
	IFF_LOOPBACK,
	IFF_POINTOPOINT,
	IFF_NOTRAILERS,
	IFF_RUNNING,
	IFF_NOARP,
	IFF_PROMISC,
	IFF_ALLMULTI,
	IFF_SLAVE,
	IFF_MASTER,
	IFF_MULTICAST,
#ifdef HAVE_DYNAMIC
	IFF_DYNAMIC
#endif
	0
};

static void ife_print(struct interface *ptr)
{
	struct aftype *ap;
	const struct hwtype *hw;
	int hf;
	int can_compress = 0;

#if HAVE_AFINET6
	FILE *f;
	char addr6[40], devname[20];
	struct sockaddr_in6 sap;
	int plen, scope, dad_status, if_idx;
	char addr6p[8][5];
#endif

	ap = get_afntype(ptr->addr.sa_family);
	if (ap == NULL)
		ap = get_afntype(0);

	hf = ptr->type;

	if (hf == ARPHRD_CSLIP || hf == ARPHRD_CSLIP6)
		can_compress = 1;

	hw = get_hwntype(hf);
	if (hw == NULL)
		hw = get_hwntype(-1);

	printf("%-9.9s Link encap:%s  ", ptr->name, hw->title);
	/* For some hardware types (eg Ash, ATM) we don't print the
	   hardware address if it's null.  */
	if (hw->print != NULL && (!(hw_null_address(hw, ptr->hwaddr) &&
								hw->suppress_null_addr)))
		printf("HWaddr %s  ", hw->print((unsigned char *)ptr->hwaddr));
#ifdef IFF_PORTSEL
	if (ptr->flags & IFF_PORTSEL) {
		printf("Media:%s", if_port_text[ptr->map.port] /* [0] */);
		if (ptr->flags & IFF_AUTOMEDIA)
			printf("(auto)");
	}
#endif
	printf("\n");

	if (ptr->has_ip) {
		printf("          %s addr:%s ", ap->name,
			   ap->sprint(&ptr->addr, 1));
		if (ptr->flags & IFF_POINTOPOINT) {
			printf(" P-t-P:%s ", ap->sprint(&ptr->dstaddr, 1));
		}
		if (ptr->flags & IFF_BROADCAST) {
			printf(" Bcast:%s ", ap->sprint(&ptr->broadaddr, 1));
		}
		printf(" Mask:%s\n", ap->sprint(&ptr->netmask, 1));
	}

#if HAVE_AFINET6

#define IPV6_ADDR_ANY           0x0000U

#define IPV6_ADDR_UNICAST       0x0001U
#define IPV6_ADDR_MULTICAST     0x0002U
#define IPV6_ADDR_ANYCAST       0x0004U

#define IPV6_ADDR_LOOPBACK      0x0010U
#define IPV6_ADDR_LINKLOCAL     0x0020U
#define IPV6_ADDR_SITELOCAL     0x0040U

#define IPV6_ADDR_COMPATv4      0x0080U

#define IPV6_ADDR_SCOPE_MASK    0x00f0U

#define IPV6_ADDR_MAPPED        0x1000U
#define IPV6_ADDR_RESERVED      0x2000U	/* reserved address space */

	if ((f = fopen(_PATH_PROCNET_IFINET6, "r")) != NULL) {
		while (fscanf
			   (f, "%4s%4s%4s%4s%4s%4s%4s%4s %02x %02x %02x %02x %20s\n",
				addr6p[0], addr6p[1], addr6p[2], addr6p[3], addr6p[4],
				addr6p[5], addr6p[6], addr6p[7], &if_idx, &plen, &scope,
				&dad_status, devname) != EOF) {
			if (!strcmp(devname, ptr->name)) {
				sprintf(addr6, "%s:%s:%s:%s:%s:%s:%s:%s",
						addr6p[0], addr6p[1], addr6p[2], addr6p[3],
						addr6p[4], addr6p[5], addr6p[6], addr6p[7]);
				inet_pton(AF_INET6, addr6,
						  (struct sockaddr *) &sap.sin6_addr);
				sap.sin6_family = AF_INET6;
				printf("          inet6 addr: %s/%d",
					   inet6_aftype.sprint((struct sockaddr *) &sap, 1),
					   plen);
				printf(" Scope:");
				switch (scope & IPV6_ADDR_SCOPE_MASK) {
				case 0:
					printf("Global");
					break;
				case IPV6_ADDR_LINKLOCAL:
					printf("Link");
					break;
				case IPV6_ADDR_SITELOCAL:
					printf("Site");
					break;
				case IPV6_ADDR_COMPATv4:
					printf("Compat");
					break;
				case IPV6_ADDR_LOOPBACK:
					printf("Host");
					break;
				default:
					printf("Unknown");
				}
				printf("\n");
			}
		}
		fclose(f);
	}
#endif

	printf("          ");
	/* DONT FORGET TO ADD THE FLAGS IN ife_print_short, too */

	if (ptr->flags == 0) {
		printf("[NO FLAGS] ");
	} else {
		int i = 0;
		do {
			if (ptr->flags & ife_print_flags_mask[i]) {
				printf(ife_print_flags_strs[i]);
			}
		} while (ife_print_flags_mask[++i]);
	}

	/* DONT FORGET TO ADD THE FLAGS IN ife_print_short */
	printf(" MTU:%d  Metric:%d", ptr->mtu, ptr->metric ? ptr->metric : 1);
#ifdef SIOCSKEEPALIVE
	if (ptr->outfill || ptr->keepalive)
		printf("  Outfill:%d  Keepalive:%d", ptr->outfill, ptr->keepalive);
#endif
	printf("\n");

	/* If needed, display the interface statistics. */

	if (ptr->statistics_valid) {
		/* XXX: statistics are currently only printed for the primary address,
		 *      not for the aliases, although strictly speaking they're shared
		 *      by all addresses.
		 */
		printf("          ");

		printf("RX packets:%llu errors:%lu dropped:%lu overruns:%lu frame:%lu\n",
			   ptr->stats.rx_packets, ptr->stats.rx_errors,
			   ptr->stats.rx_dropped, ptr->stats.rx_fifo_errors,
			   ptr->stats.rx_frame_errors);
		if (can_compress)
			printf("             compressed:%lu\n",
				   ptr->stats.rx_compressed);
		printf("          ");
		printf("TX packets:%llu errors:%lu dropped:%lu overruns:%lu carrier:%lu\n",
			   ptr->stats.tx_packets, ptr->stats.tx_errors,
			   ptr->stats.tx_dropped, ptr->stats.tx_fifo_errors,
			   ptr->stats.tx_carrier_errors);
		printf("          collisions:%lu ", ptr->stats.collisions);
		if (can_compress)
			printf("compressed:%lu ", ptr->stats.tx_compressed);
		if (ptr->tx_queue_len != -1)
			printf("txqueuelen:%d ", ptr->tx_queue_len);
		printf("\n          R");
		print_bytes_scaled(ptr->stats.rx_bytes, "  T");
		print_bytes_scaled(ptr->stats.tx_bytes, "\n");

	}

	if ((ptr->map.irq || ptr->map.mem_start || ptr->map.dma ||
		 ptr->map.base_addr)) {
		printf("          ");
		if (ptr->map.irq)
			printf("Interrupt:%d ", ptr->map.irq);
		if (ptr->map.base_addr >= 0x100)	/* Only print devices using it for
											   I/O maps */
			printf("Base address:0x%lx ",
				   (unsigned long) ptr->map.base_addr);
		if (ptr->map.mem_start) {
			printf("Memory:%lx-%lx ", ptr->map.mem_start,
				   ptr->map.mem_end);
		}
		if (ptr->map.dma)
			printf("DMA chan:%x ", ptr->map.dma);
		printf("\n");
	}
	printf("\n");
}


static int do_if_print(struct interface *ife, void *cookie)
{
	int *opt_a = (int *) cookie;
	int res;

	res = do_if_fetch(ife);
	if (res >= 0) {
		if ((ife->flags & IFF_UP) || *opt_a)
			ife_print(ife);
	}
	return res;
}

static struct interface *lookup_interface(char *name)
{
	struct interface *ife = NULL;

	if (if_readlist_proc(name) < 0)
		return NULL;
	ife = add_interface(name);
	return ife;
}

/* for ipv4 add/del modes */
static int if_print(char *ifname)
{
	int res;

	if (!ifname) {
		res = for_all_interfaces(do_if_print, &interface_opt_a);
	} else {
		struct interface *ife;

		ife = lookup_interface(ifname);
		res = do_if_fetch(ife);
		if (res >= 0)
			ife_print(ife);
	}
	return res;
}

int display_interfaces(char *ifname);
int display_interfaces(char *ifname)
{
	int status;

	/* Create a channel to the NET kernel. */
	if ((skfd = sockets_open(0)) < 0) {
		bb_perror_msg_and_die("socket");
	}

	/* Do we have to show the current setup? */
	status = if_print(ifname);
#ifdef CONFIG_FEATURE_CLEAN_UP
	sockets_close();
#endif
	exit(status < 0);
}
