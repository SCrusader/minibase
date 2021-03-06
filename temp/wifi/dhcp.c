#include <bits/arp.h>
#include <bits/socket.h>
#include <bits/socket/packet.h>
#include <bits/ioctl/socket.h>
#include <bits/auxvec.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sched.h>
#include <sys/ppoll.h>
#include <sys/signal.h>

#include <errtag.h>
#include <format.h>
#include <string.h>
#include <endian.h>
#include <sigset.h>
#include <util.h>

#include "dhcp.h"
#include "dhcp_udp.h"

#define ARPHRD_NETROM  0
#define ETH_P_IP  0x0800

ERRTAG("dhcp");

/* DHCP packets are sent via raw sockets, so full ip and udp headers here. */

struct dhcpmsg {
	struct iphdr ip;
	struct udphdr udp;
	struct dhcphdr dhcp;
	char options[500];
} __attribute__((packed)) packet;

int optptr;
int sockfd;

struct ifreq ifreq;
uint32_t xid;

struct {
	uint8_t serverip[4];
	uint8_t yourip[4];
} offer;

struct {
	uint index;
	char name[IFNAMESIZ+1];
	uint8_t mac[6];
} iface;

struct sockaddr_ll sockaddr;

static void sigalarm(int sig)
{
	(void)sig;
	fail("timeout", NULL, 0);
}

static void setup_alarm(void)
{
	SIGHANDLER(sa, sigalarm, 0);

	sys_sigaction(SIGALRM, &sa, NULL);

	sys_alarm(1);
}

/* Try to come up with a somewhat random xid by pulling auxvec random
   bytes. Failure is not a big issue here, in the sense that DHCP is
   quite insecure by design and a trurly random xid hardly improves that. */

static void init_xid(char** envp)
{
	char** p = envp;

	while(*p) p++;

	struct auxvec* a = (struct auxvec*)(p + 1);

	for(; a->key; a++)
		if(a->key == AT_RANDOM)
			break;
	if(a->key)
		memcpy(&xid, (void*)a->val, 4);
	else
		memcpy(&xid, iface.mac + 2, 4); /* meh */
};

/* Socket setup and device queries (mac, ifindex) */

static void ifreq_clear(void)
{
	memset(&ifreq, 0, sizeof(ifreq));
}

static void ifreq_ioctl(int ctl, char* name)
{
	int ret;

	if((ret = sys_ioctl(sockfd, ctl, &ifreq)) < 0)
		fail("ioctl", name, ret);
}

static void get_ifindex(char* name)
{
	int nlen = strlen(name);

	if(nlen > IFNAMESIZ)
		fail("name too long:", name, 0);

	ifreq_clear();
	memcpy(ifreq.name, name, nlen);

	ifreq_ioctl(SIOCGIFINDEX, "SIOCGIFINDEX");

	iface.index = ifreq.ival;
	memcpy(iface.name, name, nlen);
	iface.name[nlen] = '\0';
}

static void get_ifname(int idx)
{
	ifreq_clear();
	ifreq.ival = idx;

	ifreq_ioctl(SIOCGIFNAME, "SIOCGIFNAME");

	iface.index = idx;
	memcpy(iface.name, ifreq.name, IFNAMESIZ);
	iface.name[IFNAMESIZ] = '\0';
	iface.name[strlen(iface.name)] = '\0';
}

static void get_ifhwaddr(void)
{
	ifreq_clear();
	memcpy(ifreq.name, iface.name, IFNAMESIZ);

	ifreq_ioctl(SIOCGIFHWADDR, "SIOCGIFHWADDR");

	if(ifreq.addr.family != ARPHRD_ETHER)
		fail("unexpected hwaddr family on", iface.name, 0);

	memcpy(iface.mac, ifreq.addr.data, 6);
}

void setup_socket(char* name)
{
	int index = 0;
	int fd, ret;
	char* p;

	if((fd = sys_socket(PF_PACKET, SOCK_DGRAM, 8)) < 0)
		fail("socket", "PF_PACKET", fd);

	if((p = parseint(name, &index)) && !*p)
		get_ifname(index);
	else
		get_ifindex(name);

	get_ifhwaddr();

	sockaddr = (struct sockaddr_ll) {
		.family = AF_PACKET,
		.ifindex = iface.index,
		.hatype = ARPHRD_NETROM,
		.pkttype = PACKET_HOST,
		.protocol = htons(ETH_P_IP),
		.halen = 6,
		.addr = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } /* broadcast */
	};

	if((ret = sys_bind(fd, &sockaddr, sizeof(sockaddr))) < 0)
		fail("bind", NULL, fd);

	sockfd = fd;
}

/* Send */

static void reset(void)
{
	memset(&packet, 0, sizeof(packet));
	optptr = 0;
}

static struct dhcpopt* add_option(int code, int len)
{
	uint alloc = len + sizeof(struct dhcpopt);

	if(optptr + alloc >= sizeof(packet.options) - 1)
		fail("out of packet space", NULL, 0);

	struct dhcpopt* opt = (struct dhcpopt*)(packet.options + optptr);

	opt->code = code;
	opt->len = len;

	optptr += alloc;

	return opt;
}

static void put_byte(int code, uint8_t val)
{
	struct dhcpopt* opt = add_option(code, 1);
	opt->payload[0] = val;
}

static void put_ip(int code, uint8_t* ip)
{
	struct dhcpopt* opt = add_option(code, 4);
	memcpy(opt->payload, ip, 4);
}

static void put_mac(int code, uint8_t* mac)
{
	struct dhcpopt* opt = add_option(code, 7);
	opt->payload[0] = 0x01;
	memcpy(opt->payload + 1, mac, 6);
}

static void put_header(int type)
{
	memset(&packet, 0, sizeof(packet));
	optptr = 0;

	packet.dhcp.xid = htonl(xid);
	packet.dhcp.op = BOOTREQUEST;
	packet.dhcp.htype = 1; /* ethernet */
	packet.dhcp.hlen = 6;
	packet.dhcp.hops = 0;

	packet.dhcp.secs = htonl(1);
	packet.dhcp.flags = 0;

	memcpy(packet.dhcp.chaddr, iface.mac, sizeof(iface.mac));

	packet.dhcp.cookie = htonl(DHCP_COOKIE);

	put_byte(DHCP_MESSAGE_TYPE, type);
}

static void put_option_end(void)
{
	packet.options[optptr++] = 0xFF;
}

static void set_udp_header(void)
{
	int udpsize = sizeof(packet.udp) + sizeof(packet.dhcp) + optptr;
	int ipsize = udpsize + sizeof(packet.ip);

	packet.ip.protocol = IPPROTO_UDP;
	memset(packet.ip.saddr, 0x00, 4);
	memset(packet.ip.daddr, 0xFF, 4);
	void* ips = &packet.ip.saddr;

	packet.udp.source = htons(BOOT_CLIENT_PORT);
	packet.udp.dest = htons(BOOT_SERVER_PORT);
	packet.udp.len = htons(udpsize);
	packet.udp.check = htons(udpchecksum(&packet.udp, udpsize, ips));

	packet.ip.tot_len = htons(ipsize);
	packet.ip.verihl = IPV4IHL5;
	packet.ip.ttl = IPDEFTTL;
	packet.ip.check = htons(ipchecksum(&packet, sizeof(packet.ip)));
}

static void send_packet(void)
{
	int ret, fd = sockfd;
	void* addr = &sockaddr;
	int alen = sizeof(sockaddr);

	put_option_end();
	set_udp_header();

	int len = ntohs(packet.ip.tot_len);

	if((ret = sys_sendto(fd, &packet, len, 0, addr, alen)) < 0)
		fail("sendto", NULL, ret);
}

/* Receive */

static int check_udp_header(void)
{
	int totlen = ntohs(packet.ip.tot_len);
	int udplen = ntohs(packet.udp.len);

	int iphdrlen = sizeof(packet.ip);

	if(packet.ip.protocol != IPPROTO_UDP)
		return 1;
	if(packet.ip.verihl != IPV4IHL5)
		return 1;
	if(packet.udp.dest != htons(BOOT_CLIENT_PORT))
		return 1;
	if(udplen != totlen - iphdrlen)
		return 1;

	return 0;
}

static int check_dhcp_header(void)
{
	if(packet.dhcp.cookie != htonl(DHCP_COOKIE))
		return 1;
	if(packet.dhcp.xid != htonl(xid))
		return 1;

	return 0;
}

/* Sadly the first request does sometimes fail unpredictably. Such failures
   are annoying, and it's difficult to handle them otherwise, so we do
   a small number of timed retries here. */

static int timedrecv(int fd, char* buf, int len)
{
	struct timespec ts = {
		.sec = 0,
		.nsec = 150*1000*1000 /* 150ms */
	};
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN
	};

	int ret = sys_ppoll(&pfd, 1, &ts, NULL);

	if(!ret)
		return -ETIMEDOUT;
	if(ret < 0)
		return ret;

	return sys_recv(fd, buf, len, 0);
}

int recv_packet(void)
{
	long rd;

	int udplen = sizeof(packet.ip) + sizeof(packet.udp);
	int hdrlen = udplen + sizeof(packet.dhcp);
	int totlen = 0;

	char* buf = (char*) &packet;
	int len = sizeof(packet);

	reset();

	while((rd = timedrecv(sockfd, buf, len)) > 0) {
		if(rd < udplen)
			continue; /* too short */
		if(rd < (totlen = ntohs(packet.ip.tot_len)))
			continue; /* incomplete */
		if(check_udp_header())
			continue; /* malformed, not udp, wrong port */
		if(totlen < hdrlen)
			continue; /* truncated DHCP header */
		if(check_dhcp_header())
			continue; /* malformed DHCP, wrong xid */
		break;
	} if(rd <= 0) {
		return rd;
	}

	optptr = totlen - hdrlen;

	return totlen;
}

struct dhcpopt* opt_at(int off)
{
	struct dhcpopt* opt;
	int hdrlen = sizeof(*opt);

	if(off < 0)
		return NULL;
	if(off > optptr - hdrlen)
		return NULL;

	opt = (struct dhcpopt*)(packet.options + off);

	if(off > optptr - hdrlen - opt->len)
		return NULL; /* truncated opt */

	return opt;
}

struct dhcpopt* first_opt(void)
{
	return opt_at(0);
}

struct dhcpopt* next_opt(struct dhcpopt* curr)
{
	char* cptr = (char*)curr;

	if(cptr < packet.options)
		return NULL;
	if(cptr > packet.options + optptr)
		return NULL;

	int pos = (cptr - packet.options);

	return opt_at(pos + sizeof(*curr) + curr->len);
}

struct dhcpopt* get_option(int code, int len)
{
	struct dhcpopt* opt;

	for(opt = first_opt(); opt; opt = next_opt(opt))
		if(opt->code != code)
			continue;
		else if(!len || opt->len == len)
			return opt;
		else
			break; /* right code but wrong size */

	return NULL;
}

static int get_message_type(void)
{
	struct dhcpopt* opt = get_option(DHCP_MESSAGE_TYPE, 1);
	return opt ? opt->payload[0] : 0;
}

static uint8_t* get_server_addr(void)
{
	struct dhcpopt* opt = get_option(DHCP_SERVER_ID, 4);
	return opt ? (uint8_t*)opt->payload : NULL;
}

void send_discover_recv_offer(void)
{
	uint8_t* srv;
	int ret, tries = 4;
again:
	put_header(DHCPDISCOVER);
	send_packet();

	while(1) {
		if((ret = recv_packet()) >= 0)
			;
		else if(ret == -ETIMEDOUT && --tries)
			goto again;
		else
			fail(NULL, "discover", ret);

		if(get_message_type() != DHCPOFFER)
			continue;
		else if(!(srv = get_server_addr()))
			continue;
		else
			break;
	}

	memcpy(offer.serverip, srv, 4);
	memcpy(offer.yourip, packet.dhcp.yiaddr, 4);
}

void send_request_recv_acknak(void)
{
	int ret, tries = 2;
again:
	put_header(DHCPREQUEST);
	put_ip(DHCP_REQUESTED_IP, offer.yourip);
	put_ip(DHCP_SERVER_ID, offer.serverip);
	put_mac(DHCP_CLIENT_ID, iface.mac);
	send_packet();

	while(1) {
		if((ret = recv_packet()) >= 0)
			;
		else if(ret == ETIMEDOUT && --tries)
			goto again;
		else
			fail(NULL, "request", ret);

		if(memcmp(&packet.ip.saddr, offer.serverip, 4))
			continue;

		int mt = get_message_type();

		if(mt == DHCPNAK)
			break;
		if(mt == DHCPACK)
			break;
	}
}

#define OPTS "ngr"
#define OPT_n (1<<0)
#define OPT_g (1<<1)
#define OPT_r (1<<2)

int main(int argc, char** argv, char** envp)
{
	int i = 1;
	int opts = 0;
	char* devname;

	if(i < argc && argv[i][0] == '-')
		opts = argbits(OPTS, argv[i++] + 1);
	if(i < argc)
		devname = argv[i++];
	else
		fail("too few arguments", NULL, 0);
	if(i < argc)
		fail("too many arguments", NULL, 0);

	setup_alarm();
	setup_socket(devname);
	init_xid(envp);

	send_discover_recv_offer();
	send_request_recv_acknak();

	uint8_t* ip = packet.dhcp.yiaddr;
	int ifi = iface.index;

	if(opts & OPT_n)
		show_config(ip);
	else
		conf_netdev(ifi, ip, (opts & OPT_g));

	if(!(opts & (OPT_r | OPT_n)))
		write_resolv_conf();

	return 0;
}
