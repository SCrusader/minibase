#include <sys/write.h>
#include <sys/brk.h>

#include <format.h>
#include <nlusctl.h>
#include <string.h>
#include <heap.h>
#include <util.h>

#include "config.h"
#include "common.h"
#include "wimon.h"

/* Formatting and sending responses to wictl has little to do with the rest
   of wimon_ctrl, so it's been split.

   Long messages (i.e. anything that's not just errno) are prepared in a heap
   buffer. Timeouts are handled in _ctrl and not here. */

static void send_reply(struct conn* cn, struct ucbuf* uc)
{
	writeall(cn->fd, uc->brk, uc->ptr - uc->brk);
}

void reply(struct conn* cn, int err)
{
	char cbuf[16];
	struct ucbuf uc;

	uc_buf_set(&uc, cbuf, sizeof(cbuf));
	uc_put_hdr(&uc, err);
	uc_put_end(&uc);

	send_reply(cn, &uc);
}

static int estimate_scalist(void)
{
	int scansp = nscans*(sizeof(struct scan) + 10*sizeof(struct ucattr));

	return scansp + 128;
}

static int estimate_status(void)
{
	int scansp = nscans*(sizeof(struct scan) + 10*sizeof(struct ucattr));
	int linksp = nlinks*(sizeof(struct link) + 10*sizeof(struct ucattr));

	return scansp + linksp + 128;
}

static void prep_heap(struct heap* hp, int size)
{
	hp->brk = (void*)sysbrk(NULL);

	size += (PAGE - size % PAGE) % PAGE;

	hp->ptr = hp->brk;
	hp->end = (void*)sysbrk(hp->brk + size);
}

static void free_heap(struct heap* hp)
{
	sysbrk(hp->brk);
}

static void put_status_wifi(struct ucbuf* uc)
{
	struct ucattr* nn;
	struct link* ls;

	if(!(ls = find_link_slot(wifi.ifi)))
		return;

	nn = uc_put_nest(uc, ATTR_WIFI);
	uc_put_int(uc, ATTR_IFI, wifi.ifi);
	uc_put_str(uc, ATTR_NAME, ls->name);
	uc_put_int(uc, ATTR_STATE, wifi.state);

	if(wifi.slen)
		uc_put_bin(uc, ATTR_SSID, wifi.ssid, wifi.slen);
	if(nonzero(wifi.bssid, sizeof(wifi.bssid)))
		uc_put_bin(uc, ATTR_BSSID, wifi.bssid, sizeof(wifi.bssid));
	if(wifi.freq)
		uc_put_int(uc, ATTR_FREQ, wifi.freq);

	uc_end_nest(uc, nn);
}

static int common_link_type(struct link* ls)
{
	if(ls->flags & S_NL80211)
		return LINK_NL80211;
	else
		return LINK_GENERIC;
}

static int common_link_state(struct link* ls)
{
	int state = ls->state;
	int flags = ls->flags;

	if(state == LS_ACTIVE)
		return LINK_ACTIVE;
	if(state == LS_STARTING)
		return LINK_STARTING;
	if(state == LS_STOPPING)
		return LINK_STOPPING;
	if(flags & S_CARRIER)
		return LINK_CARRIER;
	if(flags & S_ENABLED)
		return LINK_ENABLED;

	return LINK_OFF;
}

static void put_status_links(struct ucbuf* uc)
{
	struct link* ls;
	struct ucattr* nn;

	for(ls = links; ls < links + nlinks; ls++) {
		if(!ls->ifi) continue;

		nn = uc_put_nest(uc, ATTR_LINK);
		uc_put_int(uc, ATTR_IFI,   ls->ifi);
		uc_put_str(uc, ATTR_NAME,  ls->name);
		uc_put_int(uc, ATTR_STATE, common_link_state(ls));
		uc_put_int(uc, ATTR_TYPE,  common_link_type(ls));

		if(ls->flags & S_IPADDR) {
			uc_put_bin(uc, ATTR_IPADDR, ls->ip, sizeof(ls->ip));
			uc_put_int(uc, ATTR_IPMASK, ls->mask);
		}

		uc_end_nest(uc, nn);
	}
}

static void put_status_scans(struct ucbuf* uc)
{
	struct scan* sc;
	struct ucattr* nn;

	for(sc = scans; sc < scans + nscans; sc++) {
		if(!sc->freq) continue;
		nn = uc_put_nest(uc, ATTR_SCAN);
		uc_put_int(uc, ATTR_FREQ,   sc->freq);
		uc_put_int(uc, ATTR_TYPE,   sc->type);
		uc_put_int(uc, ATTR_SIGNAL, sc->signal);
		uc_put_int(uc, ATTR_PRIO,   sc->prio);
		uc_put_bin(uc, ATTR_BSSID, sc->bssid, sizeof(sc->bssid));
		uc_put_bin(uc, ATTR_SSID,  sc->ssid, sc->slen);
		uc_end_nest(uc, nn);
	}
}

void rep_status(struct conn* cn)
{
	struct heap hp;
	struct ucbuf uc;

	prep_heap(&hp, estimate_status());

	uc_buf_set(&uc, hp.brk, hp.end - hp.brk);
	uc_put_hdr(&uc, CMD_STATUS);
	put_status_links(&uc);
	put_status_wifi(&uc);
	put_status_scans(&uc);
	uc_put_end(&uc);

	send_reply(cn, &uc);

	free_heap(&hp);
}

void rep_scanlist(struct conn* cn)
{
	struct heap hp;
	struct ucbuf uc;

	prep_heap(&hp, estimate_scalist());

	uc_buf_set(&uc, hp.brk, hp.end - hp.brk);
	uc_put_hdr(&uc, CMD_SCAN);
	put_status_scans(&uc);
	uc_put_end(&uc);

	send_reply(cn, &uc);

	free_heap(&hp);
}
