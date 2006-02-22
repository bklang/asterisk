/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Provide Open Settlement Protocol capability
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \arg See also: \ref chan_sip.c
 */

#include <sys/types.h>
#include <osp.h>
#include <openssl/err.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/say.h"
#include "asterisk/module.h"
#include "asterisk/options.h"
#include "asterisk/crypto.h"
#include "asterisk/md5.h"
#include "asterisk/cli.h"
#include "asterisk/io.h"
#include "asterisk/lock.h"
#include "asterisk/astosp.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/causes.h"
#include "asterisk/callerid.h"
#include "asterisk/pbx.h"

#define MAX_CERTS 10
#define MAX_SERVICEPOINTS 10
#define OSP_MAX 256

#define OSP_DEFAULT_MAX_CONNECTIONS	20
#define OSP_DEFAULT_RETRY_DELAY		0
#define OSP_DEFAULT_RETRY_LIMIT		2
#define OSP_DEFAULT_TIMEOUT			500

static int loadPemCert(unsigned char *FileName, unsigned char *buffer, int *len);
static int loadPemPrivateKey(unsigned char *FileName, unsigned char *buffer, int *len);

AST_MUTEX_DEFINE_STATIC(osplock);

static int initialized = 0;
static int hardware = 0;
static unsigned tokenformat = TOKEN_ALGO_SIGNED;

struct osp_provider {
	char name[OSP_MAX];
	char localpvtkey[OSP_MAX];
	char localcert[OSP_MAX];
	char cacerts[MAX_CERTS][OSP_MAX]; 
	int cacount;
	char servicepoints[MAX_SERVICEPOINTS][OSP_MAX];
	char source[OSP_MAX];
	int spcount;
	int dead;
	int maxconnections;
	int retrydelay;
	int retrylimit;
	int timeout;
	OSPTPROVHANDLE handle;
	struct osp_provider *next;
};
static struct osp_provider *providers;

static int osp_build(struct ast_config *cfg, char *cat)
{
	OSPTCERT TheAuthCert[MAX_CERTS];
	unsigned char Reqbuf[4096],LocalBuf[4096],AuthBuf[MAX_CERTS][4096];
	struct ast_variable *v;
	struct osp_provider *osp;
	int x,length,errorcode=0;
	int mallocd=0,i;
	char *cacerts[MAX_CERTS];
	const char *servicepoints[MAX_SERVICEPOINTS];
	OSPTPRIVATEKEY privatekey;
	OSPTCERT localcert;
	OSPTCERT *authCerts[MAX_CERTS];

	
	
	ast_mutex_lock(&osplock);
	osp = providers;
	while(osp) {
		if (!strcasecmp(osp->name, cat))
			break;
		osp = osp->next;
	}
	ast_mutex_unlock(&osplock);
	if (!osp) {
		mallocd = 1;
		if (!(osp = ast_calloc(1, sizeof(*osp)))) {
			return -1;
		}
		osp->handle = -1;
	}
	ast_copy_string(osp->name, cat, sizeof(osp->name));
	snprintf(osp->localpvtkey, sizeof(osp->localpvtkey) ,"%s/%s-privatekey.pem", ast_config_AST_KEY_DIR, cat);
	snprintf(osp->localcert, sizeof(osp->localpvtkey), "%s/%s-localcert.pem", ast_config_AST_KEY_DIR, cat);
	osp->maxconnections=OSP_DEFAULT_MAX_CONNECTIONS;
	osp->retrydelay = OSP_DEFAULT_RETRY_DELAY;
	osp->retrylimit = OSP_DEFAULT_RETRY_LIMIT;
	osp->timeout = OSP_DEFAULT_TIMEOUT;
	osp->source[0] = '\0';
	ast_log(LOG_DEBUG, "Building OSP Provider '%s'\n", cat);
	v = ast_variable_browse(cfg, cat);
	while(v) {
		if (!strcasecmp(v->name, "privatekey")) {
			if (v->value[0] == '/')
				ast_copy_string(osp->localpvtkey, v->value, sizeof(osp->localpvtkey));
			else
				snprintf(osp->localpvtkey, sizeof(osp->localpvtkey), "%s/%s", ast_config_AST_KEY_DIR , v->value);
		} else if (!strcasecmp(v->name, "localcert")) {
			if (v->value[0] == '/')
				ast_copy_string(osp->localcert, v->value, sizeof(osp->localcert));
			else
				snprintf(osp->localcert, sizeof(osp->localcert), "%s/%s", ast_config_AST_KEY_DIR, v->value);
		} else if (!strcasecmp(v->name, "cacert")) {
			if (osp->cacount < MAX_CERTS) {
				if (v->value[0] == '/')
					ast_copy_string(osp->cacerts[osp->cacount], v->value, sizeof(osp->cacerts[0]));
				else
					snprintf(osp->cacerts[osp->cacount], sizeof(osp->cacerts[0]), "%s/%s", ast_config_AST_KEY_DIR, v->value);
				osp->cacount++;
			} else
				ast_log(LOG_WARNING, "Too many CA Certificates at line %d\n", v->lineno);
		} else if (!strcasecmp(v->name, "servicepoint")) {
			if (osp->spcount < MAX_SERVICEPOINTS) {
				ast_copy_string(osp->servicepoints[osp->spcount], v->value, sizeof(osp->servicepoints[0]));
				osp->spcount++;
			} else
				ast_log(LOG_WARNING, "Too many Service points at line %d\n", v->lineno);
		} else if (!strcasecmp(v->name, "maxconnections")) {
			if ((sscanf(v->value, "%d", &x) == 1) && (x > 0) && (x <= 1000)) {
				osp->maxconnections = x;
			} else
				ast_log(LOG_WARNING, "maxconnections should be an integer from 1 to 1000, not '%s' at line %d\n", v->value, v->lineno);
		} else if (!strcasecmp(v->name, "retrydelay")) {
			if ((sscanf(v->value, "%d", &x) == 1) && (x >= 0) && (x <= 10)) {
				osp->retrydelay = x;
			} else
				ast_log(LOG_WARNING, "retrydelay should be an integer from 0 to 10, not '%s' at line %d\n", v->value, v->lineno);
		} else if (!strcasecmp(v->name, "retrylimit")) {
			if ((sscanf(v->value, "%d", &x) == 1) && (x >= 0) && (x <= 100)) {
				osp->retrylimit = x;
			} else
				ast_log(LOG_WARNING, "retrylimit should be an integer from 0 to 100, not '%s' at line %d\n", v->value, v->lineno);
		} else if (!strcasecmp(v->name, "timeout")) {
			if ((sscanf(v->value, "%d", &x) == 1) && (x >= 200) && (x <= 10000)) {
				osp->timeout = x;
			} else
				ast_log(LOG_WARNING, "timeout should be an integer from 200 to 10000, not '%s' at line %d\n", v->value, v->lineno);
		} else if (!strcasecmp(v->name, "source")) {
			ast_copy_string(osp->source, v->value, sizeof(osp->source));
		}
		v = v->next;
	}
	if (osp->cacount < 1) {
		snprintf(osp->cacerts[osp->cacount], sizeof(osp->cacerts[0]), "%s/%s-cacert.pem", ast_config_AST_KEY_DIR, cat);
		osp->cacount++;
	}
	for (x=0;x<osp->cacount;x++)
		cacerts[x] = osp->cacerts[x];
	for (x=0;x<osp->spcount;x++)
		servicepoints[x] = osp->servicepoints[x];
	
	ast_mutex_lock(&osplock);
	osp->dead = 0;
	if (osp->handle > -1) {
		ast_log(LOG_DEBUG, "Deleting old handle for '%s'\n", osp->name);
		OSPPProviderDelete(osp->handle, 0);
	}
		

    length = 0;
	ast_log(LOG_DEBUG, "Loading private key for '%s' (%s)\n", osp->name, osp->localpvtkey);
    errorcode = loadPemPrivateKey(osp->localpvtkey,Reqbuf,&length);
    if (errorcode == 0)
    {
        privatekey.PrivateKeyData = Reqbuf;
        privatekey.PrivateKeyLength = length;
    }
    else
    {
         return -1;
    }

    length = 0;
	ast_log(LOG_DEBUG, "Loading local cert for '%s' (%s)\n", osp->name, osp->localcert);
    errorcode = loadPemCert(osp->localcert,LocalBuf,&length);
    if (errorcode == 0)
    {
        localcert.CertData = LocalBuf;
        localcert.CertDataLength = length;
    }
    else
    {
         return -1;
    }

    for (i=0;i<osp->cacount;i++)
    {
        length = 0;
		ast_log(LOG_DEBUG, "Loading CA cert %d for '%s' (%s)\n", i + 1, osp->name, osp->cacerts[i]);
        errorcode = loadPemCert(osp->cacerts[i],AuthBuf[i],&length);
        if (errorcode == 0)
        {
            TheAuthCert[i].CertData = AuthBuf[i];
            TheAuthCert[i].CertDataLength = length;
            authCerts[i] = &(TheAuthCert[i]);
        }
        else
        {
			return -1;        
		}
    }
	
	ast_log(LOG_DEBUG, "Creating provider handle for '%s'\n", osp->name);
	
	ast_log(LOG_DEBUG, "Service point '%s %d'\n", servicepoints[0], osp->spcount);
	
	if (OSPPProviderNew(osp->spcount, 
					    servicepoints, 
					   NULL, 
					   "localhost", 
					   &privatekey, 
					   &localcert, 
					   osp->cacount, 
					   (const OSPTCERT **)authCerts, 
					   1, 
					   300, 
					   osp->maxconnections, 
					   1, 
					   osp->retrydelay, 
					   osp->retrylimit, 
					   osp->timeout, 
					   "", 
					   "", 
					   &osp->handle)) {
		ast_log(LOG_WARNING, "Unable to initialize provider '%s'\n", cat);
		osp->dead = 1;
	}
	
	if (mallocd) {
		osp->next = providers;
		providers = osp;
	}
	ast_mutex_unlock(&osplock);	
	return 0;
}

static int show_osp(int fd, int argc, char *argv[])
{
	struct osp_provider *osp;
	char *search = NULL;
	int x;
	int found = 0;
	char *tokenalgo;

	if ((argc < 2) || (argc > 3))
		return RESULT_SHOWUSAGE;
	if (argc > 2)
		search = argv[2];
	if (!search) {
		switch (tokenformat) {
			case TOKEN_ALGO_BOTH:
				tokenalgo = "Both";
				break;
			case TOKEN_ALGO_UNSIGNED:
				tokenalgo = "Unsigned";
				break;
			case TOKEN_ALGO_SIGNED:
			default:
				tokenalgo = "Signed";
				break;
		}
		ast_cli(fd, "OSP: %s %s %s\n", initialized ? "Initialized" : "Uninitialized", hardware ? "Accelerated" : "Normal", tokenalgo);
	}

	ast_mutex_lock(&osplock);
	osp = providers;
	while(osp) {
		if (!search || !strcasecmp(osp->name, search)) {
			if (found)
				ast_cli(fd, "\n");
			ast_cli(fd, " == OSP Provider '%s' ==\n", osp->name);
			ast_cli(fd, "Local Private Key: %s\n", osp->localpvtkey);
			ast_cli(fd, "Local Certificate: %s\n", osp->localcert);
			for (x=0;x<osp->cacount;x++)
				ast_cli(fd, "CA Certificate %d:  %s\n", x + 1, osp->cacerts[x]);
			for (x=0;x<osp->spcount;x++)
				ast_cli(fd, "Service Point %d:   %s\n", x + 1, osp->servicepoints[x]);
			ast_cli(fd, "Max Connections:   %d\n", osp->maxconnections);
			ast_cli(fd, "Retry Delay:       %d seconds\n", osp->retrydelay);
			ast_cli(fd, "Retry Limit:       %d\n", osp->retrylimit);
			ast_cli(fd, "Timeout:           %d milliseconds\n", osp->timeout);
			ast_cli(fd, "Source:            %s\n", strlen(osp->source) ? osp->source : "<unspecified>");
			ast_cli(fd, "OSP Handle:        %d\n", osp->handle);
			found++;
		}
		osp = osp->next;
	}
	ast_mutex_unlock(&osplock);
	if (!found) {
		if (search) 
			ast_cli(fd, "Unable to find OSP provider '%s'\n", search);
		else
			ast_cli(fd, "No OSP providers configured\n");
	}
	return RESULT_SUCCESS;
}


/*----------------------------------------------*
 *               Loads the Certificate          *
 *----------------------------------------------*/
static int loadPemCert(unsigned char *FileName, unsigned char *buffer, int *len)
{
    int length = 0;
    unsigned char *temp;
    BIO *bioIn = NULL;
    X509 *cert=NULL;
    int retVal = OSPC_ERR_NO_ERROR;

    temp = buffer;
    bioIn = BIO_new_file((const char*)FileName,"r");
    if (bioIn == NULL)
    {
		ast_log(LOG_WARNING,"Failed to find the File - %s \n",FileName);
		return -1;
    }
    else
    {
        cert = PEM_read_bio_X509(bioIn,NULL,NULL,NULL);
        if (cert == NULL)
        {
			ast_log(LOG_WARNING,"Failed to parse the Certificate from the File - %s \n",FileName);
			return -1;
        }
        else
        {
            length = i2d_X509(cert,&temp);
            if (cert == 0)
            {
				ast_log(LOG_WARNING,"Failed to parse the Certificate from the File - %s, Length=0 \n",FileName);
				return -1;
            }
            else
			{
               *len = length;
            }
        }
    }

    if (bioIn != NULL)
    {
        BIO_free(bioIn);
    }

    if (cert != NULL)
    {
        X509_free(cert);
    }
    return retVal;
}

/*----------------------------------------------*
 *               Loads the Private Key          *
 *----------------------------------------------*/
static int loadPemPrivateKey(unsigned char *FileName, unsigned char *buffer, int *len)
{
    int length = 0;
    unsigned char *temp;
    BIO *bioIn = NULL;
    RSA *pKey = NULL;
    int retVal = OSPC_ERR_NO_ERROR;

    temp = buffer;

    bioIn = BIO_new_file((const char*)FileName,"r");
    if (bioIn == NULL)
    {
		ast_log(LOG_WARNING,"Failed to find the File - %s \n",FileName);
		return -1;
    }
    else
    {
        pKey = PEM_read_bio_RSAPrivateKey(bioIn,NULL,NULL,NULL);
        if (pKey == NULL)
        {
			ast_log(LOG_WARNING,"Failed to parse the Private Key from the File - %s \n",FileName);
			return -1;
        }
        else
        {
            length = i2d_RSAPrivateKey(pKey,&temp);
            if (length == 0)
            {
				ast_log(LOG_WARNING,"Failed to parse the Private Key from the File - %s, Length=0 \n",FileName);
				return -1;
            }
            else
            {
                *len = length;
            }
        }
    }
    if (bioIn != NULL)
    {
        BIO_free(bioIn);
    }

    if (pKey != NULL)
    {
       RSA_free(pKey);
    }
    return retVal;
}

int ast_osp_validate(char *provider, char *token, int *handle, unsigned int *timelimit, const char *callerid, struct in_addr addr, const char *extension)
{
	char tmp[256]="", *l, *n;
	char iabuf[INET_ADDRSTRLEN];
	char source[OSP_MAX] = ""; /* Same length as osp->source */
	char *token2;
	int tokenlen;
	struct osp_provider *osp;
	int res = 0;
	unsigned int authorised, dummy;

	if (!provider || !strlen(provider))
		provider = "default";

	token2 = ast_strdupa(token);
	if (!token2)
		return -1;
	tokenlen = ast_base64decode(token2, token, strlen(token));
	*handle = -1;
	if (!callerid)
		callerid = "";
	ast_copy_string(tmp, callerid, sizeof(tmp));
	ast_callerid_parse(tmp, &n, &l);
	if (!l)
		l = "";
	else {
		ast_shrink_phone_number(l);
		if (!ast_isphonenumber(l))
			l = "";
	}
	callerid = l;
	ast_mutex_lock(&osplock);
	ast_inet_ntoa(iabuf, sizeof(iabuf), addr);
	osp = providers;
	while(osp) {
		if (!strcasecmp(osp->name, provider)) {
			if (OSPPTransactionNew(osp->handle, handle)) {
				ast_log(LOG_WARNING, "Unable to create OSP Transaction handle!\n");
			} else {
				ast_copy_string(source, osp->source, sizeof(source));
				res = 1;
			}
			break;
		}
		osp = osp->next;
	}
	ast_mutex_unlock(&osplock);
	if (res) {
		res = 0;
		dummy = 0;
		if (!OSPPTransactionValidateAuthorisation(*handle, iabuf, source, NULL, NULL, 
			callerid, OSPC_E164, extension, OSPC_E164, 0, "", tokenlen, token2, &authorised, timelimit, &dummy, NULL, tokenformat)) {
			if (authorised) {
				ast_log(LOG_DEBUG, "Validated token for '%s' from '%s@%s'\n", extension, callerid, iabuf);
				res = 1;
			}
		}
	}
	return res;	
}

static int check_dest(struct ast_osp_result *result, char *token, int tokensize)
{
	OSPE_DEST_OSP_ENABLED enabled;
	OSPE_DEST_PROT prot;
	int res = 1;

	/* Check destination OSP version */
	if (!OSPPTransactionIsDestOSPEnabled(result->handle, &enabled) && (enabled == OSPE_OSP_FALSE)) {
		result->token[0] = 0;
	} else {
		ast_base64encode(result->token, token, tokensize, sizeof(result->token) - 1);
	}

	/* Check destination protocol */
	if (OSPPTransactionGetDestProtocol(result->handle, &prot)) {
		prot = OSPE_DEST_PROT_UNDEFINED;
	}
	switch(prot) {
	case OSPE_DEST_PROT_UNDEFINED:	/* Protocol is not configured, use SIP as default */
	case OSPE_DEST_PROT_SIP:
		ast_copy_string(result->tech, "SIP", sizeof(result->tech));
		break;
	case OSPE_DEST_PROT_H323_SETUP:
		ast_copy_string(result->tech, "H323", sizeof(result->tech));
		break;
	case OSPE_DEST_PROT_IAX:
		ast_copy_string(result->tech, "IAX", sizeof(result->tech));
		break;
	case OSPE_DEST_PROT_H323_LRQ:
	case OSPE_DEST_PROT_UNKNOWN:
	default:
		ast_log(LOG_DEBUG, "Unknown destination protocol '%d', skipping...\n", prot);
		res = 0;
	}

	return res;
}

int ast_osp_lookup(struct ast_channel *chan, char *provider, char *extension, char *callerid, struct ast_osp_result *result)
{
	int cres;
	int res = 0;
	int counts;
	int tokenlen;
	unsigned int dummy=0;
	unsigned int timelimit;
	unsigned int callidlen;
	char callidstr[OSPC_CALLID_MAXSIZE] = "";
	struct osp_provider *osp;
	char source[OSP_MAX] = ""; /* Same length as osp->source */
	char callednum[2048]="";
	char callingnum[2048]="";
	char destination[2048]="";
	char token[2000];
	char tmp[256]="", *l, *n;
	const char *devinfo = NULL;

	result->handle = -1;
	result->numresults = 0;
	result->tech[0] = '\0';
	result->dest[0] = '\0';
	result->token[0] = '\0';

	if (!provider || !strlen(provider))
		provider = "default";

	if (!callerid)
		callerid = "";
	ast_copy_string(tmp, callerid, sizeof(tmp));
	ast_callerid_parse(tmp, &n, &l);
	if (!l)
		l = "";
	else {
		ast_shrink_phone_number(l);
		if (!ast_isphonenumber(l))
			l = "";
	}
	callerid = l;

	if (chan) {
		cres = ast_autoservice_start(chan);
		if (cres < 0)
			return cres;
	}
	ast_mutex_lock(&osplock);
	osp = providers;
	while(osp) {
		if (!strcasecmp(osp->name, provider)) {
			if (OSPPTransactionNew(osp->handle, &result->handle)) {
				ast_log(LOG_WARNING, "Unable to create OSP Transaction handle!\n");
			} else {
				ast_copy_string(source, osp->source, sizeof(source));
				res = 1;
			}
			break;
		}
		osp = osp->next;
	}
	ast_mutex_unlock(&osplock);
	if (res) {
		res = 0;
		/* No more than 10 back */
		counts = 10;
		dummy = 0;
		devinfo = pbx_builtin_getvar_helper (chan, "OSPPEER");
		if (!devinfo) {
			devinfo = "";
		}
		if (!OSPPTransactionRequestAuthorisation(result->handle, source, devinfo, 
			  callerid,OSPC_E164, extension, OSPC_E164, NULL, 0, NULL, NULL, &counts, &dummy, NULL)) {
			if (counts) {
				tokenlen = sizeof(token);
				result->numresults = counts - 1;
				callidlen = sizeof(callidstr);
				if (!OSPPTransactionGetFirstDestination(result->handle, 0, NULL, NULL, &timelimit, &callidlen, callidstr, 
					sizeof(callednum), callednum, sizeof(callingnum), callingnum, sizeof(destination), destination, 0, NULL, &tokenlen, token)) {
					ast_log(LOG_DEBUG, "Got destination '%s' and called: '%s' calling: '%s' for '%s' (provider '%s')\n",
						destination, callednum, callingnum, extension, provider);
					/* Only support OSP server with only one duration limit */
					if (ast_channel_cmpwhentohangup (chan, timelimit) < 0) {
						ast_channel_setwhentohangup (chan, timelimit);	
					}
					do {
						if ((strlen(destination) > 2) && (check_dest(result, token, tokenlen))) {
							/* Strip leading and trailing brackets */
							destination[strlen(destination) - 1] = '\0';
							snprintf(result->dest, sizeof(result->dest), "%s@%s", callednum, destination + 1);
							res = 1;
						} else {
							if(result->numresults) {
								result->numresults--;
								callidlen = sizeof(callidstr);
								if (!OSPPTransactionGetNextDestination(result->handle, OSPC_FAIL_INCOMPATIBLE_DEST, 0, NULL, NULL, &timelimit, &callidlen, 
									callidstr, sizeof(callednum), callednum, sizeof(callingnum), callingnum, sizeof(destination), destination, 0, NULL, 
									&tokenlen, token))
								{
									continue;
								}
							}
						}
						break;
					} while (1);
				}
			}
		}
		if (!res) {
			OSPPTransactionDelete(result->handle);
			result->handle = -1;
		}
		
	}
	if (!osp) 
		ast_log(LOG_NOTICE, "OSP Provider '%s' does not exist!\n", provider);
	if (chan) {
		cres = ast_autoservice_stop(chan);
		if (cres < 0)
			return cres;
	}
	return res;
}

int ast_osp_next(struct ast_osp_result *result, int cause)
{
	int res = 0;
	int tokenlen;
	unsigned int timelimit;
	unsigned int callidlen;
	char callidstr[OSPC_CALLID_MAXSIZE] = "";
	char callednum[2048]="";
	char callingnum[2048]="";
	char destination[2048]="";
	char token[2000];

	result->tech[0] = '\0';
	result->dest[0] = '\0';
	result->token[0] = '\0';

	if (result->handle > -1) {
		if (result->numresults) {
			tokenlen = sizeof(token);
			while(!res && result->numresults) {
				result->numresults--;
				callidlen = sizeof(callidstr);
				if (!OSPPTransactionGetNextDestination(result->handle, OSPC_FAIL_INCOMPATIBLE_DEST, 0, NULL, NULL, &timelimit, &callidlen, callidstr, 
					sizeof(callednum), callednum, sizeof(callingnum), callingnum, sizeof(destination), destination, 0, NULL, &tokenlen, token)) 
				{
					if ((strlen(destination) > 2) && check_dest(result, token, tokenlen)) {
						/* Strip leading and trailing brackets */
						destination[strlen(destination) - 1] = '\0';
						snprintf(result->dest, sizeof(result->dest), "%s@%s", callednum, destination + 1);
						res = 1;
					}
				}
			}
		}
		if (!res) {
			OSPPTransactionDelete(result->handle);
			result->handle = -1;
		}
	}
	return res;
}

static enum OSPEFAILREASON cause2reason(int cause)
{
	switch(cause) {
	case AST_CAUSE_BUSY:
		return OSPC_FAIL_USER_BUSY;
	case AST_CAUSE_CONGESTION:
		return OSPC_FAIL_SWITCHING_EQUIPMENT_CONGESTION;
	case AST_CAUSE_UNALLOCATED:
		return OSPC_FAIL_UNALLOC_NUMBER;
	case AST_CAUSE_NOTDEFINED:
		return OSPC_FAIL_NORMAL_UNSPECIFIED;
	case AST_CAUSE_NOANSWER:
		return OSPC_FAIL_NO_ANSWER_FROM_USER;
	case AST_CAUSE_NORMAL:
	default:
		return OSPC_FAIL_NORMAL_CALL_CLEARING;
	}
}

int ast_osp_terminate(int handle, int cause, time_t start, time_t duration)
{
	unsigned int dummy = 0;
	int res = -1;
	enum OSPEFAILREASON reason;

	time_t endTime = 0;
	time_t alertTime = 0;
	time_t connectTime = 0;
	unsigned isPddInfoPresent = 0;
	unsigned pdd = 0;
	unsigned releaseSource = 0;
	unsigned char *confId = "";
	
	reason = cause2reason(cause);
	if (OSPPTransactionRecordFailure(handle, reason))
		ast_log(LOG_WARNING, "Failed to record call termination for handle %d\n", handle);
	else if (OSPPTransactionReportUsage(handle, duration, start,
			       endTime,alertTime,connectTime,isPddInfoPresent,pdd,releaseSource,confId,
		       	       0, 0, 0, 0, &dummy, NULL))
		ast_log(LOG_WARNING, "Failed to report duration for handle %d\n", handle);
	else {
		ast_log(LOG_DEBUG, "Completed recording handle %d\n", handle);
		OSPPTransactionDelete(handle);
		res = 0;
	}
	return res;
}

static int config_load(void)
{
	struct ast_config *cfg;
	char *cat;
	struct osp_provider *osp, *prev = NULL, *next;
	ast_mutex_lock(&osplock);
	osp = providers;
	while(osp) {
		osp->dead = 1;
		osp = osp->next;
	}
	ast_mutex_unlock(&osplock);
	cfg = ast_config_load("osp.conf");
	if (cfg) {
		if (!initialized) {
			cat = ast_variable_retrieve(cfg, "general", "accelerate");
			if (cat && ast_true(cat))
				if (OSPPInit(1)) {
					ast_log(LOG_WARNING, "Failed to enable hardware accelleration, falling back to software mode\n");
					OSPPInit(0);
				} else
					hardware = 1;
			else
				OSPPInit(0);
			initialized = 1;
		}
		cat = ast_variable_retrieve(cfg, "general", "tokenformat");
		if (cat) {
			if ((sscanf(cat, "%d", &tokenformat) != 1) || (tokenformat < TOKEN_ALGO_SIGNED) || (tokenformat > TOKEN_ALGO_BOTH)) {
				tokenformat = TOKEN_ALGO_SIGNED;
				ast_log(LOG_WARNING, "tokenformat should be an integer from 0 to 2, not '%s'\n", cat);
			}
		}
		cat = ast_category_browse(cfg, NULL);
		while(cat) {
			if (strcasecmp(cat, "general"))
				osp_build(cfg, cat);
			cat = ast_category_browse(cfg, cat);
		}
		ast_config_destroy(cfg);
	} else
		ast_log(LOG_NOTICE, "No OSP configuration found.  OSP support disabled\n");
	ast_mutex_lock(&osplock);
	osp = providers;
	while(osp) {
		next = osp->next;
		if (osp->dead) {
			if (prev)
				prev->next = next;
			else
				providers = next;
			/* XXX Cleanup OSP structure first XXX */
			free(osp);
		} else 
			prev = osp;
		osp = next;
	}
	ast_mutex_unlock(&osplock);
	return 0;
}

static char show_osp_usage[] = 
"Usage: show osp\n"
"       Displays information on Open Settlement Protocol\n";

static struct ast_cli_entry cli_show_osp = 
{ { "show", "osp", NULL }, show_osp, "Displays OSP information", show_osp_usage };

int reload(void)
{
	config_load();
	ast_log(LOG_NOTICE, "XXX Should reload OSP config XXX\n");
	return 0;
}

int load_module(void)
{
	config_load();
	ast_cli_register(&cli_show_osp);
	return 0;
}

int unload_module(void)
{
	/* Can't unload this once we're loaded */
	return -1;
}

char *description(void)
{
	return "Open Settlement Protocol Support";
}

int usecount(void)
{
	/* We should never be unloaded */
	return 1;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
