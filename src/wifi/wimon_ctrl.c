#include <bits/socket.h>
#include <bits/socket/unix.h>

#include <sys/accept.h>
#include <sys/bind.h>
#include <sys/close.h>
#include <sys/listen.h>
#include <sys/recv.h>
#include <sys/socket.h>
#include <sys/unlink.h>
#include <sys/setitimer.h>

#include <nlusctl.h>
#include <string.h>
#include <fail.h>

#include "config.h"
#include "common.h"
#include "wimon.h"

/* Control socket code and client (wictl) communication.

   Most state-changing commands are kept synchronous -- wictl waits for
   the action to complete. Within wimon, this is implemented in a form
   of "latches". The request is accepted, and if it needs to wait for
   something to happen, it gets latched. Other parts of the code that
   may observe the action then call unlatch(), which checks for any
   connections latched on this particular action, and sends replies
   if necessary. */

#define TIMEOUT 1

#define NOERROR 0
#define REPLIED 1
#define LATCHED 2

static void release_latch(struct conn* cn, int err)
{
	struct itimerval old, new = {
		.interval = { 0, 0 },
		.value = { TIMEOUT, 0 }
	};

	syssetitimer(0, &new, &old);

	if(err)
		reply(cn, err);
	else if(cn->evt == CONF)
		rep_linkconf(cn);
	else if(cn->evt == SCAN)
		rep_scanlist(cn);
	else
		reply(cn, 0);

	syssetitimer(0, &old, NULL);

	cn->evt = 0;
	cn->ifi = 0;
}

void unlatch(int ifi, int evt, int err)
{
	struct conn* cn;

	for(cn = conns; cn < conns + nconns; cn++) {
		if(!cn->fd || !cn->evt)
			continue;
		if(ifi != cn->ifi)
			continue;
		if(evt != cn->evt && evt != ANY)
			continue;

		release_latch(cn, err);
	}
}

static int setlatch(struct conn* cn, int ifi, int evt)
{
	if(cn->evt)
		return -EBUSY;

	cn->evt = evt;
	cn->ifi = ifi;

	return LATCHED;
}

static int get_ifi(struct ucmsg* msg)
{
	int* iv = uc_get_int(msg, ATTR_IFI);
	return iv ? *iv : 0;
}

static int switch_to_wifi(int rifi)
{
	int ifi;

	if((ifi = grab_wifi_device(rifi)) < 0)
		return ifi;

	stop_links_except(ifi);

	return 0;
}

static int find_wired_link(int rifi)
{
	int ifi = 0;
	struct link* ls;

	if(rifi && (ls = find_link_slot(rifi)))
		return rifi;

	for(ls = links; ls < links + nlinks; ls++) {
		if(!ls->ifi)
			continue;
		if(ls->flags & S_NL80211)
			continue;
		if(ifi)
			return -EMFILE;
		ifi = ls->ifi;
	}

	return ifi;
}

static int cmd_scan(struct conn* cn, struct ucmsg* msg)
{
	int ifi, rifi = get_ifi(msg);

	if((ifi = grab_wifi_device(rifi)) < 0)
		return ifi;

	start_wifi_scan();

	return setlatch(cn, WIFI, SCAN);
}

static int cmd_roaming(struct conn* cn, struct ucmsg* msg)
{
	int ret, rifi = get_ifi(msg);

	if((ret = switch_to_wifi(rifi)) < 0)
		return ret;
	if((ret = wifi_mode_roaming()))
		return ret;

	return setlatch(cn, WIFI, CONF);
}

static int cmd_fixedap(struct conn* cn, struct ucmsg* msg)
{
	int ret, rifi = get_ifi(msg);
	struct ucattr* ap;

	if(!(ap = uc_get(msg, ATTR_SSID)))
		return -EINVAL;

	uint8_t* ssid = uc_payload(ap);
	int slen = uc_paylen(ap);
	char* psk = uc_get_str(msg, ATTR_PSK);

	if((ret = switch_to_wifi(rifi)) < 0)
		return ret;
	if((ret = wifi_mode_fixedap(ssid, slen, psk)))
		return ret;

	return setlatch(cn, WIFI, CONF);
}

static int cmd_neutral(struct conn* cn, struct ucmsg* msg)
{
	wifi_mode_disabled();
	stop_links_except(0);

	if(!any_stopping_links())
		return 0;

	return setlatch(cn, NONE, DOWN);
}

static int cmd_wired(struct conn* cn, struct ucmsg* msg)
{
	int ifi, ret;
	int rifi = get_ifi(msg);
	struct link* ls;

	if((ifi = find_wired_link(rifi)) < 0)
		return ifi;
	if(!(ls = find_link_slot(ifi)))
		return -ENODEV;
	if((ret = start_wired_link(ls)) < 0)
		return ret;

	wifi_mode_disabled();
	stop_links_except(ls->ifi);

	return setlatch(cn, ifi, CONF);
}

static int cmd_status(struct conn* cn, struct ucmsg* msg)
{
	rep_status(cn);
	return REPLIED;
}

static const struct cmd {
	int cmd;
	int (*call)(struct conn* cn, struct ucmsg* msg);
} commands[] = {
	{ CMD_STATUS,  cmd_status  },
	{ CMD_NEUTRAL, cmd_neutral },
	{ CMD_ROAMING, cmd_roaming },
	{ CMD_FIXEDAP, cmd_fixedap },
	{ CMD_WIRED,   cmd_wired   },
	{ CMD_SCAN,    cmd_scan    },
	{ 0,           NULL        }
};

static void dispatch_cmd(struct conn* cn, struct ucmsg* msg)
{
	const struct cmd* cd;
	int cmd = msg->cmd;
	int ret;

	if(cn->evt)
		release_latch(cn, -EINTR);

	for(cd = commands; cd->cmd; cd++)
		if(cd->cmd == cmd)
			break;
	if(!cd->cmd)
		reply(cn, -ENOSYS);
	else if((ret = cd->call(cn, msg)) <= 0)
		reply(cn, ret);
}

static void shutdown_conn(struct conn* cn)
{
	sysclose(cn->fd);
	memzero(cn, sizeof(*cn));
}

/* The code below allows for persistent connections, and can handle multiple
   commands per connection. This feature was crucial at some point, but that's
   no longer so. It is kept mostly as a sanity feature for foreign clients
   (anything that's not wictl), and also because it relaxes timing requirements
   for wictl connections. */

void handle_conn(struct conn* cn)
{
	int fd = cn->fd;
	char rbuf[1024];
	char* buf = rbuf;
	char* ptr = rbuf;
	char* end = rbuf + sizeof(rbuf);
	int flags = MSG_DONTWAIT;
	int rb;
	struct itimerval old, itv = {
		.interval = { 0, 0 },
		.value = { TIMEOUT, 0 }
	};

	struct ucmsg* msg;

	syssetitimer(0, &itv, &old);

	while((rb = sysrecv(fd, ptr, end - ptr, flags)) > 0) {
		ptr += rb;

		while((msg = uc_msg(buf, ptr - buf))) {
			dispatch_cmd(cn, msg);
			buf += msg->len;
		}

		if(buf >= ptr) { /* no unparsed bytes left */
			buf = rbuf;
			ptr = buf;
			flags = MSG_DONTWAIT;
		} else if(buf > rbuf) { /* incomplete msg */
			memmove(rbuf, buf, ptr - buf);
			ptr = rbuf + (ptr - buf);
			buf = rbuf;
			flags = 0;
		} if(ptr >= end) { /* incomplete msg and no more space */
			rb = -ENOBUFS;
			reply(cn, rb);
			break;
		}
	} if((rb < 0 && rb != -EAGAIN) || cn->hup) {
		shutdown_conn(cn);
	}

	syssetitimer(0, &old, NULL);
	save_config();
}

void accept_ctrl(int sfd)
{
	int cfd;
	struct sockaddr addr;
	int addr_len = sizeof(addr);
	struct conn *cn;

	while((cfd = sysaccept(sfd, &addr, &addr_len)) > 0)
		if((cn = grab_conn_slot()))
			cn->fd = cfd;
		else
			sysclose(cfd);
}

void unlink_ctrl(void)
{
	sysunlink(WICTL);
}

void setup_ctrl(void)
{
	int fd;
	struct sockaddr_un addr = {
		.family = AF_UNIX,
		.path = WICTL
	};

	const int flags = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	if((fd = syssocket(AF_UNIX, flags, 0)) < 0)
		fail("socket", "AF_UNIX", fd);

	long ret;
	char* name = WICTL;

	ctrlfd = fd;

	if((ret = sysbind(fd, &addr, sizeof(addr))) < 0)
		fail("bind", name, ret);
	else if((ret = syslisten(fd, 1)))
		fail("listen", name, ret);
	else
		return;

	ctrlfd = -1;
}
