/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Implementation of Inter-Asterisk eXchange
 * 
 * Copyright (C) 2003, Digium
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <asterisk/frame.h>
#include <asterisk/utils.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "iax2.h"
#include "iax2-parser.h"
#include "iax2-provision.h"


static int frames = 0;
static int iframes = 0;
static int oframes = 0;

static void internaloutput(const char *str)
{
	printf(str);
}

static void internalerror(const char *str)
{
	fprintf(stderr, "WARNING: %s", str);
}

static void (*outputf)(const char *str) = internaloutput;
static void (*errorf)(const char *str) = internalerror;

static void dump_addr(char *output, int maxlen, void *value, int len)
{
	struct sockaddr_in sin;
	char iabuf[INET_ADDRSTRLEN];
	if (len == (int)sizeof(sin)) {
		memcpy(&sin, value, len);
		snprintf(output, maxlen, "IPV4 %s:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
	} else {
		snprintf(output, maxlen, "Invalid Address");
	}
}

static void dump_string(char *output, int maxlen, void *value, int len)
{
	maxlen--;
	if (maxlen > len)
		maxlen = len;
	strncpy(output,value, maxlen);
	output[maxlen] = '\0';
}

static void dump_int(char *output, int maxlen, void *value, int len)
{
	if (len == (int)sizeof(unsigned int))
		snprintf(output, maxlen, "%lu", (unsigned long)ntohl(*((unsigned int *)value)));
	else
		snprintf(output, maxlen, "Invalid INT");
}

static void dump_short(char *output, int maxlen, void *value, int len)
{
	if (len == (int)sizeof(unsigned short))
		snprintf(output, maxlen, "%d", ntohs(*((unsigned short *)value)));
	else
		snprintf(output, maxlen, "Invalid SHORT");
}

static void dump_byte(char *output, int maxlen, void *value, int len)
{
	if (len == (int)sizeof(unsigned char))
		snprintf(output, maxlen, "%d", *((unsigned char *)value));
	else
		snprintf(output, maxlen, "Invalid BYTE");
}

static void dump_ipaddr(char *output, int maxlen, void *value, int len)
{
	struct sockaddr_in sin;
	char iabuf[INET_ADDRSTRLEN];
	if (len == (int)sizeof(unsigned int)) {
		memcpy(&sin.sin_addr, value, len);
		ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr);
		snprintf(output, maxlen, "%s", iabuf);
	} else
		snprintf(output, maxlen, "Invalid IPADDR");
}


static void dump_prov_flags(char *output, int maxlen, void *value, int len)
{
	char buf[256] = "";
	if (len == (int)sizeof(unsigned int))
		snprintf(output, maxlen, "%lu (%s)", (unsigned long)ntohl(*((unsigned int *)value)),
			iax_provflags2str(buf, sizeof(buf), ntohl(*((unsigned int *)value))));
	else
		snprintf(output, maxlen, "Invalid INT");
}

static void dump_prov_ies(char *output, int maxlen, unsigned char *iedata, int len);
static void dump_prov(char *output, int maxlen, void *value, int len)
{
	dump_prov_ies(output, maxlen, value, len);
}

static struct iax2_ie {
	int ie;
	char *name;
	void (*dump)(char *output, int maxlen, void *value, int len);
} ies[] = {
	{ IAX_IE_CALLED_NUMBER, "CALLED NUMBER", dump_string },
	{ IAX_IE_CALLING_NUMBER, "CALLING NUMBER", dump_string },
	{ IAX_IE_CALLING_ANI, "ANI", dump_string },
	{ IAX_IE_CALLING_NAME, "CALLING NAME", dump_string },
	{ IAX_IE_CALLED_CONTEXT, "CALLED CONTEXT", dump_string },
	{ IAX_IE_USERNAME, "USERNAME", dump_string },
	{ IAX_IE_PASSWORD, "PASSWORD", dump_string },
	{ IAX_IE_CAPABILITY, "CAPABILITY", dump_int },
	{ IAX_IE_FORMAT, "FORMAT", dump_int },
	{ IAX_IE_LANGUAGE, "LANGUAGE", dump_string },
	{ IAX_IE_VERSION, "VERSION", dump_short },
	{ IAX_IE_ADSICPE, "ADSICPE", dump_short },
	{ IAX_IE_DNID, "DNID", dump_string },
	{ IAX_IE_AUTHMETHODS, "AUTHMETHODS", dump_short },
	{ IAX_IE_CHALLENGE, "CHALLENGE", dump_string },
	{ IAX_IE_MD5_RESULT, "MD5 RESULT", dump_string },
	{ IAX_IE_RSA_RESULT, "RSA RESULT", dump_string },
	{ IAX_IE_APPARENT_ADDR, "APPARENT ADDRESS", dump_addr },
	{ IAX_IE_REFRESH, "REFRESH", dump_short },
	{ IAX_IE_DPSTATUS, "DIALPLAN STATUS", dump_short },
	{ IAX_IE_CALLNO, "CALL NUMBER", dump_short },
	{ IAX_IE_CAUSE, "CAUSE", dump_string },
	{ IAX_IE_IAX_UNKNOWN, "UNKNOWN IAX CMD", dump_byte },
	{ IAX_IE_MSGCOUNT, "MESSAGE COUNT", dump_short },
	{ IAX_IE_AUTOANSWER, "AUTO ANSWER REQ" },
	{ IAX_IE_TRANSFERID, "TRANSFER ID", dump_int },
	{ IAX_IE_RDNIS, "REFERRING DNIS", dump_string },
	{ IAX_IE_PROVISIONING, "PROVISIONING", dump_prov },
	{ IAX_IE_AESPROVISIONING, "AES PROVISIONG" },
	{ IAX_IE_DATETIME, "DATE TIME", dump_int },
	{ IAX_IE_DEVICETYPE, "DEVICE TYPE", dump_string },
	{ IAX_IE_SERVICEIDENT, "SERVICE IDENT", dump_string },
	{ IAX_IE_FIRMWAREVER, "FIRMWARE VER", dump_short },
	{ IAX_IE_FWBLOCKDESC, "FW BLOCK DESC", dump_int },
	{ IAX_IE_FWBLOCKDATA, "FW BLOCK DATA" },
	{ IAX_IE_PROVVER, "PROVISIONG VER", dump_int },
};

static struct iax2_ie prov_ies[] = {
	{ PROV_IE_USEDHCP, "USEDHCP" },
	{ PROV_IE_IPADDR, "IPADDR", dump_ipaddr },
	{ PROV_IE_SUBNET, "SUBNET", dump_ipaddr },
	{ PROV_IE_GATEWAY, "GATEWAY", dump_ipaddr },
	{ PROV_IE_PORTNO, "BINDPORT", dump_short },
	{ PROV_IE_USER, "USERNAME", dump_string },
	{ PROV_IE_PASS, "PASSWORD", dump_string },
	{ PROV_IE_LANG, "LANGUAGE", dump_string },
	{ PROV_IE_TOS, "TYPEOFSERVICE", dump_byte },
	{ PROV_IE_FLAGS, "FLAGS", dump_prov_flags },
	{ PROV_IE_FORMAT, "FORMAT", dump_int },
	{ PROV_IE_AESKEY, "AESKEY" },
	{ PROV_IE_SERVERIP, "SERVERIP", dump_ipaddr },
	{ PROV_IE_SERVERPORT, "SERVERPORT", dump_short },
	{ PROV_IE_NEWAESKEY, "NEWAESKEY" },
	{ PROV_IE_PROVVER, "PROV VERSION", dump_int },
	{ PROV_IE_ALTSERVER, "ALTSERVERIP", dump_ipaddr },
};

const char *iax_ie2str(int ie)
{
	int x;
	for (x=0;x<(int)sizeof(ies) / (int)sizeof(ies[0]); x++) {
		if (ies[x].ie == ie)
			return ies[x].name;
	}
	return "Unknown IE";
}


static void dump_prov_ies(char *output, int maxlen, unsigned char *iedata, int len)
{
	int ielen;
	int ie;
	int x;
	int found;
	char interp[80];
	char tmp[256];
	if (len < 2)
		return;
	strcpy(output, "\n"); 
	maxlen -= strlen(output); output += strlen(output);
	while(len > 2) {
		ie = iedata[0];
		ielen = iedata[1];
		if (ielen + 2> len) {
			snprintf(tmp, (int)sizeof(tmp), "Total Prov IE length of %d bytes exceeds remaining prov frame length of %d bytes\n", ielen + 2, len);
			strncpy(output, tmp, maxlen - 1);
			maxlen -= strlen(output); output += strlen(output);
			return;
		}
		found = 0;
		for (x=0;x<(int)sizeof(prov_ies) / (int)sizeof(prov_ies[0]); x++) {
			if (prov_ies[x].ie == ie) {
				if (prov_ies[x].dump) {
					prov_ies[x].dump(interp, (int)sizeof(interp), iedata + 2, ielen);
					snprintf(tmp, (int)sizeof(tmp), "       %-15.15s : %s\n", prov_ies[x].name, interp);
					strncpy(output, tmp, maxlen - 1);
					maxlen -= strlen(output); output += strlen(output);
				} else {
					if (ielen)
						snprintf(interp, (int)sizeof(interp), "%d bytes", ielen);
					else
						strcpy(interp, "Present");
					snprintf(tmp, (int)sizeof(tmp), "       %-15.15s : %s\n", prov_ies[x].name, interp);
					strncpy(output, tmp, maxlen - 1);
					maxlen -= strlen(output); output += strlen(output);
				}
				found++;
			}
		}
		if (!found) {
			snprintf(tmp, (int)sizeof(tmp), "       Unknown Prov IE %03d  : Present\n", ie);
			strncpy(output, tmp, maxlen - 1);
			maxlen -= strlen(output); output += strlen(output);
		}
		iedata += (2 + ielen);
		len -= (2 + ielen);
	}
}

static void dump_ies(unsigned char *iedata, int len)
{
	int ielen;
	int ie;
	int x;
	int found;
	char interp[1024];
	char tmp[1024];
	if (len < 2)
		return;
	while(len > 2) {
		ie = iedata[0];
		ielen = iedata[1];
		if (ielen + 2> len) {
			snprintf(tmp, (int)sizeof(tmp), "Total IE length of %d bytes exceeds remaining frame length of %d bytes\n", ielen + 2, len);
			outputf(tmp);
			return;
		}
		found = 0;
		for (x=0;x<(int)sizeof(ies) / (int)sizeof(ies[0]); x++) {
			if (ies[x].ie == ie) {
				if (ies[x].dump) {
					ies[x].dump(interp, (int)sizeof(interp), iedata + 2, ielen);
					snprintf(tmp, (int)sizeof(tmp), "   %-15.15s : %s\n", ies[x].name, interp);
					outputf(tmp);
				} else {
					if (ielen)
						snprintf(interp, (int)sizeof(interp), "%d bytes", ielen);
					else
						strcpy(interp, "Present");
					snprintf(tmp, (int)sizeof(tmp), "   %-15.15s : %s\n", ies[x].name, interp);
					outputf(tmp);
				}
				found++;
			}
		}
		if (!found) {
			snprintf(tmp, (int)sizeof(tmp), "   Unknown IE %03d  : Present\n", ie);
			outputf(tmp);
		}
		iedata += (2 + ielen);
		len -= (2 + ielen);
	}
	outputf("\n");
}

void iax_showframe(struct iax_frame *f, struct ast_iax2_full_hdr *fhi, int rx, struct sockaddr_in *sin, int datalen)
{
	char *frames[] = {
		"(0?)",
		"DTMF   ",
		"VOICE  ",
		"VIDEO  ",
		"CONTROL",
		"NULL   ",
		"IAX    ",
		"TEXT   ",
		"IMAGE  " };
	char *iaxs[] = {
		"(0?)",
		"NEW    ",
		"PING   ",
		"PONG   ",
		"ACK    ",
		"HANGUP ",
		"REJECT ",
		"ACCEPT ",
		"AUTHREQ",
		"AUTHREP",
		"INVAL  ",
		"LAGRQ  ",
		"LAGRP  ",
		"REGREQ ",
		"REGAUTH",
		"REGACK ",
		"REGREJ ",
		"REGREL ",
		"VNAK   ",
		"DPREQ  ",
		"DPREP  ",
		"DIAL   ",
		"TXREQ  ",
		"TXCNT  ",
		"TXACC  ",
		"TXREADY",
		"TXREL  ",
		"TXREJ  ",
		"QUELCH ",
		"UNQULCH",
		"POKE",
		"PAGE",
		"MWI",
		"UNSUPPORTED",
		"TRANSFER",
		"PROVISION",
		"FWDOWNLD",
		"FWDATA"
	};
	char *cmds[] = {
		"(0?)",
		"HANGUP ",
		"RING   ",
		"RINGING",
		"ANSWER ",
		"BUSY   ",
		"TKOFFHK ",
		"OFFHOOK" };
	struct ast_iax2_full_hdr *fh;
	char retries[20];
	char class2[20];
	char subclass2[20];
	char *class;
	char *subclass;
	char tmp[256];
	char iabuf[INET_ADDRSTRLEN];
	if (f) {
		fh = f->data;
		snprintf(retries, (int)sizeof(retries), "%03d", f->retries);
	} else {
		fh = fhi;
		if (ntohs(fh->dcallno) & IAX_FLAG_RETRANS)
			strcpy(retries, "Yes");
		else
			strcpy(retries, " No");
	}
	if (!(ntohs(fh->scallno) & IAX_FLAG_FULL)) {
		/* Don't mess with mini-frames */
		return;
	}
	if (fh->type > (int)sizeof(frames)/(int)sizeof(char *)) {
		snprintf(class2, (int)sizeof(class2), "(%d?)", fh->type);
		class = class2;
	} else {
		class = frames[(int)fh->type];
	}
	if (fh->type == AST_FRAME_DTMF) {
		sprintf(subclass2, "%c", fh->csub);
		subclass = subclass2;
	} else if (fh->type == AST_FRAME_IAX) {
		if (fh->csub >= (int)sizeof(iaxs)/(int)sizeof(iaxs[0])) {
			snprintf(subclass2, (int)sizeof(subclass2), "(%d?)", fh->csub);
			subclass = subclass2;
		} else {
			subclass = iaxs[(int)fh->csub];
		}
	} else if (fh->type == AST_FRAME_CONTROL) {
		if (fh->csub > (int)sizeof(cmds)/(int)sizeof(char *)) {
			snprintf(subclass2, (int)sizeof(subclass2), "(%d?)", fh->csub);
			subclass = subclass2;
		} else {
			subclass = cmds[(int)fh->csub];
		}
	} else {
		snprintf(subclass2, (int)sizeof(subclass2), "%d", fh->csub);
		subclass = subclass2;
	}
snprintf(tmp, (int)sizeof(tmp), 
"%s-Frame Retry[%s] -- OSeqno: %3.3d ISeqno: %3.3d Type: %s Subclass: %s\n",
	(rx ? "Rx" : "Tx"),
	retries, fh->oseqno, fh->iseqno, class, subclass);
	outputf(tmp);
snprintf(tmp, (int)sizeof(tmp), 
"   Timestamp: %05lums  SCall: %5.5d  DCall: %5.5d [%s:%d]\n",
	(unsigned long)ntohl(fh->ts),
	ntohs(fh->scallno) & ~IAX_FLAG_FULL, ntohs(fh->dcallno) & ~IAX_FLAG_RETRANS,
		ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ntohs(sin->sin_port));
	outputf(tmp);
	if (fh->type == AST_FRAME_IAX)
		dump_ies(fh->iedata, datalen);
}

int iax_ie_append_raw(struct iax_ie_data *ied, unsigned char ie, void *data, int datalen)
{
	char tmp[256];
	if (datalen > ((int)sizeof(ied->buf) - ied->pos)) {
		snprintf(tmp, (int)sizeof(tmp), "Out of space for ie '%s' (%d), need %d have %d\n", iax_ie2str(ie), ie, datalen, (int)sizeof(ied->buf) - ied->pos);
		errorf(tmp);
		return -1;
	}
	ied->buf[ied->pos++] = ie;
	ied->buf[ied->pos++] = datalen;
	memcpy(ied->buf + ied->pos, data, datalen);
	ied->pos += datalen;
	return 0;
}

int iax_ie_append_addr(struct iax_ie_data *ied, unsigned char ie, struct sockaddr_in *sin)
{
	return iax_ie_append_raw(ied, ie, sin, (int)sizeof(struct sockaddr_in));
}

int iax_ie_append_int(struct iax_ie_data *ied, unsigned char ie, unsigned int value) 
{
	unsigned int newval;
	newval = htonl(value);
	return iax_ie_append_raw(ied, ie, &newval, (int)sizeof(newval));
}

int iax_ie_append_short(struct iax_ie_data *ied, unsigned char ie, unsigned short value) 
{
	unsigned short newval;
	newval = htons(value);
	return iax_ie_append_raw(ied, ie, &newval, (int)sizeof(newval));
}

int iax_ie_append_str(struct iax_ie_data *ied, unsigned char ie, unsigned char *str)
{
	return iax_ie_append_raw(ied, ie, str, strlen(str));
}

int iax_ie_append_byte(struct iax_ie_data *ied, unsigned char ie, unsigned char dat)
{
	return iax_ie_append_raw(ied, ie, &dat, 1);
}

int iax_ie_append(struct iax_ie_data *ied, unsigned char ie) 
{
	return iax_ie_append_raw(ied, ie, NULL, 0);
}

void iax_set_output(void (*func)(const char *))
{
	outputf = func;
}

void iax_set_error(void (*func)(const char *))
{
	errorf = func;
}

int iax_parse_ies(struct iax_ies *ies, unsigned char *data, int datalen)
{
	/* Parse data into information elements */
	int len;
	int ie;
	char tmp[256];
	memset(ies, 0, (int)sizeof(struct iax_ies));
	ies->msgcount = -1;
	ies->firmwarever = -1;
	while(datalen >= 2) {
		ie = data[0];
		len = data[1];
		if (len > datalen - 2) {
			errorf("Information element length exceeds message size\n");
			return -1;
		}
		switch(ie) {
		case IAX_IE_CALLED_NUMBER:
			ies->called_number = data + 2;
			break;
		case IAX_IE_CALLING_NUMBER:
			ies->calling_number = data + 2;
			break;
		case IAX_IE_CALLING_ANI:
			ies->calling_ani = data + 2;
			break;
		case IAX_IE_CALLING_NAME:
			ies->calling_name = data + 2;
			break;
		case IAX_IE_CALLED_CONTEXT:
			ies->called_context = data + 2;
			break;
		case IAX_IE_USERNAME:
			ies->username = data + 2;
			break;
		case IAX_IE_PASSWORD:
			ies->password = data + 2;
			break;
		case IAX_IE_CAPABILITY:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting capability to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else
				ies->capability = ntohl(*((unsigned int *)(data + 2)));
			break;
		case IAX_IE_FORMAT:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting format to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else
				ies->format = ntohl(*((unsigned int *)(data + 2)));
			break;
		case IAX_IE_LANGUAGE:
			ies->language = data + 2;
			break;
		case IAX_IE_VERSION:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp),  "Expecting version to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->version = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_ADSICPE:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting adsicpe to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->adsicpe = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_DNID:
			ies->dnid = data + 2;
			break;
		case IAX_IE_RDNIS:
			ies->rdnis = data + 2;
			break;
		case IAX_IE_AUTHMETHODS:
			if (len != (int)sizeof(unsigned short))  {
				snprintf(tmp, (int)sizeof(tmp), "Expecting authmethods to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->authmethods = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_CHALLENGE:
			ies->challenge = data + 2;
			break;
		case IAX_IE_MD5_RESULT:
			ies->md5_result = data + 2;
			break;
		case IAX_IE_RSA_RESULT:
			ies->rsa_result = data + 2;
			break;
		case IAX_IE_APPARENT_ADDR:
			ies->apparent_addr = ((struct sockaddr_in *)(data + 2));
			break;
		case IAX_IE_REFRESH:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp),  "Expecting refresh to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->refresh = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_DPSTATUS:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp),  "Expecting dpstatus to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->dpstatus = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_CALLNO:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp),  "Expecting callno to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->callno = ntohs(*((unsigned short *)(data + 2)));
			break;
		case IAX_IE_CAUSE:
			ies->cause = data + 2;
			break;
		case IAX_IE_IAX_UNKNOWN:
			if (len == 1)
				ies->iax_unknown = data[2];
			else {
				snprintf(tmp, (int)sizeof(tmp), "Expected single byte Unknown command, but was %d long\n", len);
				errorf(tmp);
			}
			break;
		case IAX_IE_MSGCOUNT:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting msgcount to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->msgcount = ntohs(*((unsigned short *)(data + 2)));	
			break;
		case IAX_IE_AUTOANSWER:
			ies->autoanswer = 1;
			break;
		case IAX_IE_MUSICONHOLD:
			ies->musiconhold = 1;
			break;
		case IAX_IE_TRANSFERID:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting transferid to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else
				ies->transferid = ntohl(*((unsigned int *)(data + 2)));
			break;
		case IAX_IE_DATETIME:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting date/time to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else
				ies->datetime = ntohl(*((unsigned int *)(data + 2)));
			break;
		case IAX_IE_FIRMWAREVER:
			if (len != (int)sizeof(unsigned short)) {
				snprintf(tmp, (int)sizeof(tmp), "Expecting firmwarever to be %d bytes long but was %d\n", (int)sizeof(unsigned short), len);
				errorf(tmp);
			} else
				ies->firmwarever = ntohs(*((unsigned short *)(data + 2)));	
			break;
		case IAX_IE_DEVICETYPE:
			ies->devicetype = data + 2;
			break;
		case IAX_IE_SERVICEIDENT:
			ies->serviceident = data + 2;
			break;
		case IAX_IE_FWBLOCKDESC:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expected block desc to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else
				ies->fwdesc = ntohl(*((unsigned int *)(data + 2)));
			break;
		case IAX_IE_FWBLOCKDATA:
			ies->fwdata = data + 2;
			ies->fwdatalen = len;
			break;
		case IAX_IE_PROVVER:
			if (len != (int)sizeof(unsigned int)) {
				snprintf(tmp, (int)sizeof(tmp), "Expected provisioning version to be %d bytes long but was %d\n", (int)sizeof(unsigned int), len);
				errorf(tmp);
			} else {
				ies->provverpres = 1;
				ies->provver = ntohl(*((unsigned int *)(data + 2)));
			}
			break;
		default:
			snprintf(tmp, (int)sizeof(tmp), "Ignoring unknown information element '%s' (%d) of length %d\n", iax_ie2str(ie), ie, len);
			outputf(tmp);
		}
		/* Overwrite information element with 0, to null terminate previous portion */
		data[0] = 0;
		datalen -= (len + 2);
		data += (len + 2);
	}
	/* Null-terminate last field */
	*data = '\0';
	if (datalen) {
		errorf("Invalid information element contents, strange boundary\n");
		return -1;
	}
	return 0;
}

void iax_frame_wrap(struct iax_frame *fr, struct ast_frame *f)
{
	fr->af.frametype = f->frametype;
	fr->af.subclass = f->subclass;
	fr->af.mallocd = 0;				/* Our frame is static relative to the container */
	fr->af.datalen = f->datalen;
	fr->af.samples = f->samples;
	fr->af.offset = AST_FRIENDLY_OFFSET;
	fr->af.src = f->src;
	fr->af.delivery.tv_sec = 0;
	fr->af.delivery.tv_usec = 0;
	fr->af.data = fr->afdata;
	if (fr->af.datalen) 
		memcpy(fr->af.data, f->data, fr->af.datalen);
}

struct iax_frame *iax_frame_new(int direction, int datalen)
{
	struct iax_frame *fr;
	fr = malloc((int)sizeof(struct iax_frame) + datalen);
	if (fr) {
		fr->direction = direction;
		fr->retrans = -1;
		frames++;
		if (fr->direction == DIRECTION_INGRESS)
			iframes++;
		else
			oframes++;
	}
	return fr;
}

void iax_frame_free(struct iax_frame *fr)
{
	/* Note: does not remove from scheduler! */
	if (fr->direction == DIRECTION_INGRESS)
		iframes--;
	else if (fr->direction == DIRECTION_OUTGRESS)
		oframes--;
	else {
		errorf("Attempt to double free frame detected\n");
		return;
	}
	fr->direction = 0;
	free(fr);
	frames--;
}

int iax_get_frames(void) { return frames; }
int iax_get_iframes(void) { return iframes; }
int iax_get_oframes(void) { return oframes; }
