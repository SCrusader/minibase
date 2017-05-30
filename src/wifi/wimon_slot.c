#include <string.h>
#include <format.h>
#include <null.h>

#include "wimon.h"

struct link links[NLINKS];
struct scan scans[NSCANS];
int nlinks;
int nscans;
struct child children[NCHILDREN];
int nchildren;

struct uplink uplink; /* single one for now */

struct link* find_link_slot(int ifi)
{
	struct link* ls;

	for(ls = links; ls < links + nlinks; ls++)
		if(ls->ifi == ifi)
			return ls;

	return NULL;
}

struct link* grab_link_slot(int ifi)
{
	struct link* ls;
	
	if((ls = find_link_slot(ifi)))
		return ls;

	for(ls = links; ls < links + nlinks; ls++)
		if(!ls->ifi)
			return ls;
	if(ls >= links + NLINKS)
		return NULL;

	nlinks++;
	return ls;
}

void free_link_slot(struct link* ls)
{
	memset(ls, 0, sizeof(*ls));
}

struct scan* find_scan_slot(uint8_t* bssid)
{
	struct scan* sc;

	for(sc = scans; sc < scans + nscans; sc++)
		if(!sc->freq)
			continue;
		else if(!memcmp(sc->bssid, bssid, 6))
			return sc;

	return NULL;
}

struct scan* grab_scan_slot(uint8_t* bssid)
{
	struct scan* sc;

	if((sc = find_scan_slot(bssid)))
		return sc;

	for(sc = scans; sc < scans + nscans; sc++)
		if(!sc->freq)
			break;
	if(sc >= scans + NSCANS)
		return NULL;
	if(sc == scans + nscans)
		nscans++;

	return sc;
}

void free_scan_slot(struct scan* sc)
{
	eprintf("%s %s\n", __FUNCTION__, sc->ssid);

	memset(sc, 0, sizeof(*sc));

	if(sc == &scans[nscans-1])
		nscans--;
}

void drop_scan_slots()
{
	eprintf("%s\n", __FUNCTION__);
	nscans = 0;
}

struct child* grab_child_slot(void)
{
	struct child* ch;

	for(ch = children; ch < children + nchildren; ch++)
		if(!ch->ifi)
			break;
	if(ch >= children + NCHILDREN)
		return NULL;
	if(ch >= children + nchildren)
		nchildren++;

	return ch;
}

struct child* find_child_slot(int pid)
{
	struct child* ch;

	for(ch = children; ch < children + nchildren; ch++)
		if(ch->pid == pid)
			return ch;
	
	return NULL;
}

void free_child_slot(struct child* ch)
{
	memzero(ch, sizeof(*ch));
	if(ch == children + nchildren - 1)
		nchildren--;
}
