/*
 * ast_h323.cpp
 *
 * OpenH323 Channel Driver for ASTERISK PBX.
 *			By  Jeremy McNamara
 *			For The NuFone Network
 * 
 * This code has been derived from code created by
 *              Michael Manousos and Mark Spencer
 *
 * This file is part of the chan_h323 driver for Asterisk
 *
 * chan_h323 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. 
 *
 * chan_h323 is distributed WITHOUT ANY WARRANTY; without even 
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
 * PURPOSE. See the GNU General Public License for more details. 
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 * Version Info: $Id$
 */
#include <asterisk/logger.h>
#include "ast_h323.h"


/* PWlib Required Components  */
#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUILD_TYPE    ReleaseCode
#define BUILD_NUMBER  0

/** Counter for the number of connections */
int channelsOpen;

/* DTMF Mode */
int mode = H323_DTMF_RFC2833;

/** Options for connections creation */
BOOL	noFastStart;
BOOL	noH245Tunnelling;
BOOL	noSilenceSuppression;

/**
 * We assume that only one endPoint should exist.
 * The application cannot run the h323_end_point_create() more than once
 * FIXME: Singleton this, for safety
 */
MyH323EndPoint *endPoint = NULL;

/** PWLib entry point */
MyProcess *localProcess = NULL;


MyProcess::MyProcess(): PProcess("The NuFone Network's", "H.323 Channel Driver for Asterisk",
             MAJOR_VERSION, MINOR_VERSION, BUILD_TYPE, BUILD_NUMBER)
{
	Resume();
}

void MyProcess::Main()
{
	ast_verbose("  == Creating H.323 Endpoint\n");
	endPoint = new MyH323EndPoint();
	PTrace::Initialise(0, NULL, PTrace::Timestamp | PTrace::Thread | PTrace::FileAndLine);
}

#define H323_NAME OPAL_G7231_6k3"{sw}"
#define H323_G729  OPAL_G729 "{sw}"
#define H323_G729A OPAL_G729A"{sw}"

H323_REGISTER_CAPABILITY(H323_G7231Capability, H323_NAME);
H323_REGISTER_CAPABILITY(AST_G729Capability,  H323_G729);
H323_REGISTER_CAPABILITY(AST_G729ACapability, H323_G729A);

H323_G7231Capability::H323_G7231Capability(BOOL annexA_)
  : H323AudioCapability(7, 4)
{
  annexA = annexA_;
}

PObject::Comparison H323_G7231Capability::Compare(const PObject & obj) const
{
  Comparison result = H323AudioCapability::Compare(obj);
  if (result != EqualTo)
    return result;

  PINDEX otherAnnexA = ((const H323_G7231Capability &)obj).annexA;
  if (annexA < otherAnnexA)
    return LessThan;
  if (annexA > otherAnnexA)
    return GreaterThan;
  return EqualTo;
}

PObject * H323_G7231Capability::Clone() const
{
  return new H323_G7231Capability(*this);
}


PString H323_G7231Capability::GetFormatName() const
{
  return H323_NAME;
}


unsigned H323_G7231Capability::GetSubType() const
{
  return H245_AudioCapability::e_g7231;
}


BOOL H323_G7231Capability::OnSendingPDU(H245_AudioCapability & cap,
                                          unsigned packetSize) const
{
  cap.SetTag(H245_AudioCapability::e_g7231);

  H245_AudioCapability_g7231 & g7231 = cap;
  g7231.m_maxAl_sduAudioFrames = packetSize;
  g7231.m_silenceSuppression = annexA;

  return TRUE;
}


BOOL H323_G7231Capability::OnReceivedPDU(const H245_AudioCapability & cap,
                                           unsigned & packetSize)
{
  if (cap.GetTag() != H245_AudioCapability::e_g7231)
    return FALSE;

  const H245_AudioCapability_g7231 & g7231 = cap;
  packetSize = g7231.m_maxAl_sduAudioFrames;
  annexA = g7231.m_silenceSuppression;

  return TRUE;
}


H323Codec * H323_G7231Capability::CreateCodec(H323Codec::Direction direction) const
{
  return NULL;
}


/////////////////////////////////////////////////////////////////////////////

AST_G729Capability::AST_G729Capability()
  : H323AudioCapability(24, 6)
{
}


PObject * AST_G729Capability::Clone() const
{
  return new AST_G729Capability(*this);
}


unsigned AST_G729Capability::GetSubType() const
{
  return H245_AudioCapability::e_g729;
}


PString AST_G729Capability::GetFormatName() const
{
  return H323_G729;
}


H323Codec * AST_G729Capability::CreateCodec(H323Codec::Direction direction) const
{
  return NULL;
}
/////////////////////////////////////////////////////////////////////////////

AST_G729ACapability::AST_G729ACapability()
  : H323AudioCapability(24, 6)
{
}


PObject * AST_G729ACapability::Clone() const
{
  return new AST_G729ACapability(*this);
}


unsigned AST_G729ACapability::GetSubType() const
{
  return H245_AudioCapability::e_g729AnnexA;
}


PString AST_G729ACapability::GetFormatName() const
{
  return H323_G729A;
}


H323Codec * AST_G729ACapability::CreateCodec(H323Codec::Direction direction) const
{
  return NULL;
}

/** MyH323EndPoint 
  * The fullAddress parameter is used directly in the MakeCall method so
  * the General form for the fullAddress argument is :
  * [alias@][transport$]host[:port]
  * default values:	alias = the same value as host.
  *					transport = ip.
  *					port = 1720.
  */
int MyH323EndPoint::MakeCall(const PString & dest, PString & token, 
							  			unsigned int *callReference, unsigned int port, char *callerid)
{
	PString fullAddress;
	MyH323Connection * connection;

	/* Determine whether we are using a gatekeeper or not. */
	if (GetGatekeeper() != NULL) {
		fullAddress = dest;
		if (h323debug)
			cout << " -- Making call to " << fullAddress << " using gatekeeper." << endl;
	} else {
			fullAddress = dest; /* host */
			if (h323debug)
				cout << " -- Making call to " << fullAddress << "." << endl;
	}

	if (!(connection = (MyH323Connection *)H323EndPoint::MakeCallLocked(fullAddress, token))) {
		if (h323debug)
			cout << "Error making call to \"" << fullAddress << '"' << endl;
		return 1;
	}
	
	*callReference = connection->GetCallReference();
	
	if (callerid)
		connection->SetLocalPartyName(PString(callerid));

	connection->Unlock(); 	

	if (h323debug) {
		cout << "	-- " << GetLocalUserName() << " is calling host " << fullAddress << endl;
		cout << "	-- " << "Call token is " << (const char *)token << endl;
		cout << "	-- Call reference is " << *callReference << endl;
	}
	return 0;
}

void MyH323EndPoint::SetEndpointTypeInfo( H225_EndpointType & info ) const
{
  H323EndPoint::SetEndpointTypeInfo(info);
  info.m_gateway.IncludeOptionalField(H225_GatewayInfo::e_protocol);
  info.m_gateway.m_protocol.SetSize(1);
  H225_SupportedProtocols &protocol=info.m_gateway.m_protocol[0];
  protocol.SetTag(H225_SupportedProtocols::e_voice);
  PINDEX as=SupportedPrefixes.GetSize();
  ((H225_VoiceCaps &)protocol).m_supportedPrefixes.SetSize(as);
  for (PINDEX p=0; p<as; p++) {
    H323SetAliasAddress(SupportedPrefixes[p], ((H225_VoiceCaps &)protocol).m_supportedPrefixes[p].m_prefix);
  }
}

void MyH323EndPoint::SetGateway(void)
{
	terminalType = e_GatewayOnly;
}

H323Capabilities MyH323EndPoint::GetCapabilities(void)
{
	return capabilities;
}

BOOL MyH323EndPoint::ClearCall(const PString & token)
{
	if (h323debug) {
		cout << "	-- ClearCall: Request to clear call with token " << token << endl;
	}
	return H323EndPoint::ClearCall(token);
}

void MyH323EndPoint::SendUserTone(const PString &token, char tone)
{
	H323Connection *connection = NULL;
		
	connection = FindConnectionWithLock(token);
	if (connection != NULL) {
		connection->SendUserInputTone(tone, 500);
		connection->Unlock();
	}
}

void MyH323EndPoint::OnClosedLogicalChannel(H323Connection & connection, const H323Channel & channel)
{
	channelsOpen--;
	if (h323debug)
		cout << "		channelsOpen = " << channelsOpen << endl;
	H323EndPoint::OnClosedLogicalChannel(connection, channel);
}

BOOL MyH323EndPoint::OnConnectionForwarded(H323Connection & connection,
 		const PString & forwardParty,
 		const H323SignalPDU & pdu)
 {
 	if (h323debug) {
	 	cout << "       -- Call Forwarded to " << forwardParty << endl;
 	}
	return FALSE;
 }
 
BOOL MyH323EndPoint::ForwardConnection(H323Connection & connection,
 		const PString & forwardParty,
 		const H323SignalPDU & pdu)
{
 	if (h323debug) {
 		cout << "       -- Forwarding call to " << forwardParty << endl;
 	}
	return H323EndPoint::ForwardConnection(connection, forwardParty, pdu);
}

void MyH323EndPoint::OnConnectionEstablished(H323Connection & connection, const PString & estCallToken)
{
	if (h323debug) {
		cout << "	-- Connection Established with \"" << connection.GetRemotePartyName() << "\"" << endl;
	}
	on_connection_established(connection.GetCallReference());
}

/** OnConnectionCleared callback function is called upon the dropping of an established
  * H323 connection. 
  */
void MyH323EndPoint::OnConnectionCleared(H323Connection & connection, const PString & clearedCallToken)
{
	PString remoteName = connection.GetRemotePartyName();
	
	call_details_t cd;

	cd.call_reference = connection.GetCallReference();
	cd.call_token = (const char *)connection.GetCallToken();
	cd.call_source_aliases = (const char *)connection.GetRemotePartyName();
	
	/* Convert complex strings */
	char *s;
	if ((s = strchr(cd.call_source_aliases, ' ')) != NULL)
		*s = '\0';

	/* Invoke the PBX application registered callback */
	on_connection_cleared(cd);

	switch (connection.GetCallEndReason()) {
		case H323Connection::EndedByCallForwarded :
			if (h323debug)
				cout << " -- " << remoteName << " has forwarded the call" << endl;
			break;
		case H323Connection::EndedByRemoteUser :
			if (h323debug)
				cout << " -- " << remoteName << " has cleared the call" << endl;
			break;
		case H323Connection::EndedByCallerAbort :
			if (h323debug)
				cout << " -- " << remoteName << " has stopped calling" << endl;
			break;
		case H323Connection::EndedByRefusal :
			if (h323debug)
				cout << " -- " << remoteName << " did not accept your call" << endl;
			break;
		case H323Connection::EndedByRemoteBusy :
			if (h323debug)
			cout << " -- " << remoteName << " was busy" << endl;
			break;
		case H323Connection::EndedByRemoteCongestion :
			if (h323debug)
				cout << " -- Congested link to " << remoteName << endl;
			break;
		case H323Connection::EndedByNoAnswer :
			if (h323debug)
				cout << " -- " << remoteName << " did not answer your call" << endl;
			break;
		case H323Connection::EndedByTransportFail :
			if (h323debug)
				cout << " -- Call with " << remoteName << " ended abnormally" << endl;
			break;
		case H323Connection::EndedByCapabilityExchange :
			if (h323debug)
				cout << " -- Could not find common codec with " << remoteName << endl;
			break;
		case H323Connection::EndedByNoAccept :
			if (h323debug)
				cout << " -- Did not accept incoming call from " << remoteName << endl;
			break;
		case H323Connection::EndedByAnswerDenied :
			if (h323debug)
				cout << " -- Refused incoming call from " << remoteName << endl;
			break;
		case H323Connection::EndedByNoUser :
			if (h323debug)
				cout << " -- Remote endpoint could not find user: " << remoteName << endl;
			break;
		case H323Connection::EndedByNoBandwidth :
			if (h323debug)
				cout << " -- Call to " << remoteName << " aborted, insufficient bandwidth." << endl;
			break;
		case H323Connection::EndedByUnreachable :
			if (h323debug)
				cout << " -- " << remoteName << " could not be reached." << endl;
			break;
		case H323Connection::EndedByHostOffline :
			if (h323debug)
				cout << " -- " << remoteName << " is not online." << endl;
			break;
		case H323Connection::EndedByNoEndPoint :
			if (h323debug)
				cout << " -- No phone running for " << remoteName << endl;
			break;
		case H323Connection::EndedByConnectFail :
			if (h323debug)
				cout << " -- Transport error calling " << remoteName << endl;
			break;
		default :
			if (h323debug)
				cout << " -- Call with " << remoteName << " completed (" << connection.GetCallEndReason() << ")" << endl;

	}

	if(connection.IsEstablished()) 
		if (h323debug)
			cout << "	 -- Call duration " << setprecision(0) << setw(5) << (PTime() - connection.GetConnectionStartTime()) << endl;
}


H323Connection * MyH323EndPoint::CreateConnection(unsigned callReference, void *outbound)
{
	unsigned options = 0;

	if (noFastStart)
		options |= H323Connection::FastStartOptionDisable;

	if (noH245Tunnelling)
		options |= H323Connection::H245TunnelingOptionDisable;

	return new MyH323Connection(*this, callReference, options);
}

/* MyH323Connection */    
MyH323Connection::MyH323Connection(MyH323EndPoint & ep, unsigned callReference,
							unsigned options)
	: H323Connection(ep, callReference, options)
{
    	remoteIpAddress = 0; 	// IP Address of remote endpoint
	remotePort      = 0;	// remote endpoint Data port (control is dataPort+1)

	if (h323debug) {
		cout << "	== New H.323 Connection created." << endl;
	}
	return;
}

MyH323Connection::~MyH323Connection()
{
	if (h323debug) {
		cout << "	== H.323 Connection deleted." << endl;
	}
	return;
}

H323Connection::AnswerCallResponse MyH323Connection::OnAnswerCall(const PString & caller,
																   const H323SignalPDU & /*setupPDU*/,
																   H323SignalPDU & /*connectPDU*/)
{
	/* The call will be answered later with "AnsweringCall()" function.
	 */ 
	return H323Connection::AnswerCallAlertWithMedia;
}

BOOL  MyH323Connection::OnAlerting(const H323SignalPDU & /*alertingPDU*/, const PString & username)
{
	
	if (h323debug) {
		cout << "	-- Ringing phone for \"" << username << "\"" << endl;
	}
	return TRUE;
}

BOOL MyH323Connection::OnReceivedSignalSetup(const H323SignalPDU & setupPDU)
{
	
	if (h323debug) {
		ast_verbose("	-- Received SETUP message\n");
	}
	
	call_details_t cd;
	PString sourceE164;
	PString destE164;
	PString sourceAliases;	
	PString destAliases;
	PString sourceIp;	
	PIPSocket::Address Ip;
	WORD sourcePort;

	sourceAliases = setupPDU.GetSourceAliases();
	destAliases = setupPDU.GetDestinationAlias();
			
	sourceE164 = "";
	setupPDU.GetSourceE164(sourceE164);
	destE164 = "";
	setupPDU.GetDestinationE164(destE164);

	/* Convert complex strings */
	//  FIXME: deal more than one source alias 
	char *s; 
    	if ((s = strchr(sourceAliases, ' ')) != NULL)
                *s = '\0';
    	if ((s = strchr(sourceAliases, '\t')) != NULL)
                *s = '\0';
	char *s1;
 	if ((s1 = strchr(destAliases, ' ')) != NULL)
         	*s1 = '\0';
	if ((s1 = strchr(destAliases, '\t')) != NULL)
         	*s1 = '\0';

	GetSignallingChannel()->GetRemoteAddress().GetIpAndPort(Ip, sourcePort);

	sourceIp = Ip.AsString();

	cd.call_reference		= GetCallReference();
	cd.call_token			= (const char *)GetCallToken();
	cd.call_source_aliases  = (const char *)sourceAliases;
	cd.call_dest_alias		= (const char *)destAliases;
	cd.call_source_e164		= (const char *)sourceE164;
	cd.call_dest_e164		= (const char *)destE164;
	cd.sourceIp				= (const char *)sourceIp;
	
	/* Notify Asterisk of the request */
	int res = on_incoming_call(cd); 

	if (!res) {
		if (h323debug) {
			cout << "	-- Call Failed" << endl;
		}
		return FALSE;
	}
	
	return H323Connection::OnReceivedSignalSetup(setupPDU);
}

BOOL MyH323Connection::OnSendSignalSetup(H323SignalPDU & setupPDU)
{
	call_details_t cd;
	
	if (h323debug) { 
		cout << "	-- Sending SETUP message" << endl;
	}
	sourceAliases = setupPDU.GetSourceAliases();
	destAliases = setupPDU.GetDestinationAlias();

	sourceE164 = "";
	setupPDU.GetSourceE164(sourceE164);
	destE164 = "";
	setupPDU.GetDestinationE164(destE164);

	/* Convert complex strings */
	//  FIXME: deal more than one source alias 
	char *s; 
    	if ((s = strchr(sourceAliases, ' ')) != NULL)
                *s = '\0';
    	if ((s = strchr(sourceAliases, '\t')) != NULL)
                *s = '\0';
	char *s1;
    	if ((s1 = strchr(destAliases, ' ')) != NULL)
        	 *s1 = '\0';
	if ((s1 = strchr(destAliases, '\t')) != NULL)
         	*s1 = '\0';

	cd.call_reference		= GetCallReference();
	cd.call_token			= (const char *)GetCallToken();
	cd.call_source_aliases  = (const char *)sourceAliases;
	cd.call_dest_alias		= (const char *)destAliases;
	cd.call_source_e164		= (const char *)sourceE164;
	cd.call_dest_e164		= (const char *)destE164;

	int res = on_outgoing_call(cd);	
		
	if (!res) {
		if (h323debug) {
			cout << "	-- Call Failed" << endl;
		}
		return FALSE;
	}

	return H323Connection::OnSendSignalSetup(setupPDU);
}

BOOL MyH323Connection::OnSendReleaseComplete(H323SignalPDU & releaseCompletePDU)
{
	if (h323debug) {
		cout << "	-- Sending RELEASE COMPLETE" << endl;
	}
	return H323Connection::OnSendReleaseComplete(releaseCompletePDU);
}

BOOL MyH323Connection::OnReceivedFacility(const H323SignalPDU & pdu)
{
	if (h323debug) {
		cout << "	-- Received Facility message... " << endl;
	}	
	return H323Connection::OnReceivedFacility(pdu);
}

void MyH323Connection::OnReceivedReleaseComplete(const H323SignalPDU & pdu)
{
	if (h323debug) {
		cout <<  "	-- Received RELEASE COMPLETE message..." << endl;
	}
	return H323Connection::OnReceivedReleaseComplete(pdu);
}

BOOL MyH323Connection::OnClosingLogicalChannel(H323Channel & channel)
{
	if (h323debug) {
		cout << "	-- Closing logical channel..." << endl;
	}
	return H323Connection::OnClosingLogicalChannel(channel);
}


void MyH323Connection::SendUserInputTone(char tone, unsigned duration)
{
	if (h323debug) {
		cout << "	-- Sending user input tone (" << tone << ") to remote" << endl;
	}
	on_send_digit(GetCallReference(), tone);	
	H323Connection::SendUserInputTone(tone, duration);
}

void MyH323Connection::OnUserInputTone(char tone, unsigned duration, unsigned logicalChannel, unsigned rtpTimestamp)
{
	if (mode == H323_DTMF_INBAND) {
		if (h323debug) {
			cout << "	-- Received user input tone (" << tone << ") from remote" << endl;
		}
		on_send_digit(GetCallReference(), tone);
	}
	H323Connection::OnUserInputTone(tone, duration, logicalChannel, rtpTimestamp);
}

void MyH323Connection::OnUserInputString(const PString &value)
{
	if (mode == H323_DTMF_RFC2833) {
		if (h323debug) {
			cout <<  "	-- Received user input string (" << value << ") from remote." << endl;
		}
		on_send_digit(GetCallReference(), value[0]);
	}	
}

H323Channel * MyH323Connection::CreateRealTimeLogicalChannel(const H323Capability & capability,	
								   H323Channel::Directions dir,
								   unsigned sessionID,
		 					           const H245_H2250LogicalChannelParameters * /*param*/)
{
	struct rtp_info *info;
	WORD port;

	/* Determine the Local (A side) IP Address and port */
	info = on_create_connection(GetCallReference()); 

	if (!info) {
		return NULL;
	}

        GetControlChannel().GetLocalAddress().GetIpAndPort(externalIpAddress, port);
	externalPort = info->port;
	
	if (h323debug) {
		cout << "	=*= In CreateRealTimeLogicalChannel for call " << GetCallReference() << endl;
		cout << "		-- externalIpAddress: " << externalIpAddress << endl;
		cout << "		-- externalPort: " << externalPort << endl;
		cout << "		-- SessionID: " << sessionID << endl;
		cout << "		-- Direction: " << dir << endl;
	}
	return new MyH323_ExternalRTPChannel(*this, capability, dir, sessionID, externalIpAddress, externalPort);
}

/** This callback function is invoked once upon creation of each
  * channel for an H323 session 
  */
BOOL MyH323Connection::OnStartLogicalChannel(H323Channel & channel)
{    
	if (h323debug) {
		cout << "	 -- Started logical channel: ";	
		cout << ((channel.GetDirection()==H323Channel::IsTransmitter)?"sending ":((channel.GetDirection()==H323Channel::IsReceiver)?"receiving ":" ")); 
		cout << (const char *)(channel.GetCapability()).GetFormatName() << endl;
	}
	// adjust the count of channels we have open
	channelsOpen++;
	if (h323debug) {
		cout <<  "		-- channelsOpen = " << channelsOpen << endl;
	}
	H323_ExternalRTPChannel & external = (H323_ExternalRTPChannel &)channel;
 	external.GetRemoteAddress(remoteIpAddress, remotePort); 

	if (h323debug) {
		cout << "		-- remoteIpAddress: " << remoteIpAddress << endl;
		cout << "		-- remotePort: " << remotePort << endl;
		cout << "		-- ExternalIpAddress: " << externalIpAddress << endl;
		cout << "		-- ExternalPort: " << externalPort << endl;
	}
	/* Notify Asterisk of remote RTP information */
	on_start_logical_channel(GetCallReference(), (const char *)remoteIpAddress.AsString(), remotePort);

	return TRUE;	
}

/* MyH323_ExternalRTPChannel */
MyH323_ExternalRTPChannel::MyH323_ExternalRTPChannel(MyH323Connection & connection,
													const H323Capability & capability,
													Directions direction,
													unsigned sessionID,
													const PIPSocket::Address & ip,
													WORD dataPort)
	: H323_ExternalRTPChannel(connection, capability, direction, sessionID, ip, dataPort)
{
	if (h323debug) {
		cout << "	== New H.323 ExternalRTPChannel created." << endl;
	}
	return;
}

MyH323_ExternalRTPChannel::~MyH323_ExternalRTPChannel()
{
	if (h323debug) {
		cout << "	== H.323 ExternalRTPChannel deleted." << endl;
	}
	return;
}

BOOL MyH323_ExternalRTPChannel::OnReceivedPDU(
				const H245_H2250LogicalChannelParameters & param,
				unsigned & errorCode)
{
	if (h323debug) {
		cout << "	MyH323_ExternalRTPChannel::OnReceivedPDU " << endl;
	}
	return H323_ExternalRTPChannel::OnReceivedPDU( param, errorCode );
}

BOOL MyH323_ExternalRTPChannel::OnReceivedAckPDU(
				const H245_H2250LogicalChannelAckParameters & param)
{

	PIPSocket::Address remoteIpAddress;		// IP Address of remote endpoint
	WORD			   remotePort;			// remote endpoint Data port (control is dataPort+1)

	if (h323debug) {
		cout << "	MyH323_ExternalRTPChannel::OnReceivedAckPDU " << endl;
	}

	if (H323_ExternalRTPChannel::OnReceivedAckPDU( param )) {
		GetRemoteAddress(remoteIpAddress, remotePort);
		if (h323debug) {
			cout << "		-- remoteIpAddress: " << remoteIpAddress << endl;
			cout << "		-- remotePort: " << remotePort << endl;
		}
		/* Notify Asterisk of remote RTP information */
		on_start_logical_channel(connection.GetCallReference(), (const char *)remoteIpAddress.AsString(), remotePort);
		return TRUE;
	}
	return FALSE;
}


/** IMPLEMENTATION OF C FUNCTIONS */

/**
 * The extern "C" directive takes care for 
 * the ANSI-C representation of linkable symbols 
 */
extern "C" {

int h323_end_point_exist(void)
{
	if (!endPoint) {
		return 0;
	}
	return 1;
}
    
void h323_end_point_create(void)
{
	channelsOpen = 0;
	localProcess = new MyProcess();	
	localProcess->Main();
}

void h323_gk_urq(void)
{
	if (!h323_end_point_exist()) {
		cout << " ERROR: [h323_gk_urq] No Endpoint, this is bad" << endl;
		return;
	}	
	endPoint->RemoveGatekeeper();
}

void h323_end_process(void)
{
	endPoint->ClearAllCalls();
	endPoint->RemoveListener(NULL);
	delete endPoint;
	delete localProcess;
}

void h323_debug(int flag, unsigned level)
{
	if (flag) {
		PTrace:: SetLevel(level); 
	} else { 
		PTrace:: SetLevel(0); 
	}
}
	
/** Installs the callback functions on behalf of the PBX application  */
void h323_callback_register(setup_incoming_cb  	ifunc,
			    setup_outbound_cb  	sfunc,
 			    on_connection_cb   	confunc,
			    start_logchan_cb   	lfunc,
 			    clear_con_cb	clfunc,
 			    con_established_cb 	efunc,
 			    send_digit_cb	dfunc)
{
	on_incoming_call = ifunc;
	on_outgoing_call = sfunc;
	on_create_connection = confunc;
	on_start_logical_channel = lfunc;
	on_connection_cleared = clfunc;
	on_connection_established = efunc;
	on_send_digit = dfunc;
}

/**
 * Add capability to the capability table of the end point. 
 */
int h323_set_capability(int cap, int dtmfMode)
{
	int g711Frames = 30;
	int gsmFrames  = 4;

	if (!h323_end_point_exist()) {
		cout << " ERROR: [h323_set_capablity] No Endpoint, this is bad" << endl;
		return 1;
	}

	/* clean up old capabilities list before changing */
	endPoint->RemoveCapabilities(PStringArray("All"));

	mode = dtmfMode;
	if (dtmfMode == H323_DTMF_INBAND) {
	    endPoint->SetSendUserInputMode(H323Connection::SendUserInputAsTone);
	} else {
		endPoint->SetSendUserInputMode(H323Connection::SendUserInputAsInlineRFC2833);
	}
	if (cap & AST_FORMAT_SPEEX) {
		/* Not real sure if Asterisk acutally supports all
		   of the various different bit rates so add them 
		   all and figure it out later*/

		endPoint->SetCapability(0, 0, new SpeexNarrow2AudioCapability());
		endPoint->SetCapability(0, 0, new SpeexNarrow3AudioCapability());
		endPoint->SetCapability(0, 0, new SpeexNarrow4AudioCapability());
		endPoint->SetCapability(0, 0, new SpeexNarrow5AudioCapability());
		endPoint->SetCapability(0, 0, new SpeexNarrow6AudioCapability());
	}

	if (cap & AST_FORMAT_G729A) {
		AST_G729ACapability *g729aCap;
		AST_G729Capability *g729Cap;
		endPoint->SetCapability(0, 0, g729aCap = new AST_G729ACapability);
		endPoint->SetCapability(0, 0, g729Cap = new AST_G729Capability);
	}
	
	if (cap & AST_FORMAT_G723_1) {
		H323_G7231Capability *g7231Cap;
		endPoint->SetCapability(0, 0, g7231Cap = new H323_G7231Capability);
	} 

	if (cap & AST_FORMAT_GSM) {
		H323_GSM0610Capability *gsmCap;
	    	endPoint->SetCapability(0, 0, gsmCap = new H323_GSM0610Capability);
	    	gsmCap->SetTxFramesInPacket(gsmFrames);
	} 

	if (cap & AST_FORMAT_ULAW) {
		H323_G711Capability *g711uCap;
	    	endPoint->SetCapability(0, 0, g711uCap = new H323_G711Capability(H323_G711Capability::muLaw));
		g711uCap->SetTxFramesInPacket(g711Frames);
	} 

	if (cap & AST_FORMAT_ALAW) {
		H323_G711Capability *g711aCap;
		endPoint->SetCapability(0, 0, g711aCap = new H323_G711Capability(H323_G711Capability::ALaw));
		g711aCap->SetTxFramesInPacket(g711Frames);
	} 

	if (h323debug) {
		cout <<  "Allowed Codecs:\n\t" << setprecision(2) << endPoint->GetCapabilities() << endl;
	}
	return 0;
}

/** Start the H.323 listener */
int h323_start_listener(int listenPort, struct sockaddr_in bindaddr)
{
	
	if (!h323_end_point_exist()) {
		cout << "ERROR: [h323_start_listener] No Endpoint, this is bad!" << endl;
		return 1;
	}
	
	PIPSocket::Address interfaceAddress(bindaddr.sin_addr);

	if (!listenPort) {
		listenPort = 1720;
	}

	/** H.323 listener */  
	H323ListenerTCP *tcpListener;

	tcpListener = new H323ListenerTCP(*endPoint, interfaceAddress, (WORD)listenPort);

	if (!endPoint->StartListener(tcpListener)) {
		cout << "ERROR: Could not open H.323 listener port on " << ((H323ListenerTCP *) tcpListener)->GetListenerPort() << endl;
		delete tcpListener;
		return 1;
		
	}
	cout << "  == H.323 listener started" << endl;

	return 0;
};
 
   
int h323_set_alias(struct oh323_alias *alias)
{
	char *p;
	char *num;
	PString h323id(alias->name);
	PString e164(alias->e164);
	
	if (!h323_end_point_exist()) {
		cout << "ERROR: [h323_set_alias] No Endpoint, this is bad!" << endl;
		return 1;
	}

	cout << "  == Adding alias \"" << h323id << "\" to endpoint" << endl;
	endPoint->AddAliasName(h323id);	
	endPoint->RemoveAliasName(localProcess->GetUserName());

	if (!e164.IsEmpty()) {
		cout << "  == Adding E.164 \"" << e164 << "\" to endpoint" << endl;
		endPoint->AddAliasName(e164);
	}
	if (strlen(alias->prefix)) {
		p = alias->prefix;
		num = strsep(&p, ",");
		while(num) {
	        cout << "  == Adding Prefix \"" << num << "\" to endpoint" << endl;
			endPoint->SupportedPrefixes += PString(num);
			endPoint->SetGateway();
	        num = strsep(&p, ",");		
		}
	}

	return 0;
}


void h323_set_id(char *id)
{
	PString h323id(id);

	if (h323debug) {
		cout << "  == Using '" << h323id << "' as our H.323ID for this call" << endl;
	}

	/* EVIL HACK */
	endPoint->SetLocalUserName(h323id);
}

void h323_show_tokens(void)
{
	cout << "Current call tokens: " << setprecision(2) << endPoint->GetAllConnections() << endl;
}


/** Establish Gatekeeper communiations, if so configured, 
  *	register aliases for the H.323 endpoint to respond to.
  */
int h323_set_gk(int gatekeeper_discover, char *gatekeeper, char *secret)
{
	PString gkName = PString(gatekeeper);
	PString pass   = PString(secret);
	H323TransportUDP *rasChannel;

	if (!h323_end_point_exist()) {
		cout << "ERROR: [h323_set_gk] No Endpoint, this is bad!" << endl;
		return 1;
	}

	if (!gatekeeper) {
		cout << "Error: Gatekeeper cannot be NULL" << endl;
		return 1;
	}

	if (strlen(secret)) {
		endPoint->SetGatekeeperPassword(pass);
	}

	if (gatekeeper_discover) {
		/* discover the gk using multicast */
		if (endPoint->DiscoverGatekeeper(new H323TransportUDP(*endPoint))) {
			cout << "  == Using " << (endPoint->GetGatekeeper())->GetName() << " as our Gatekeeper." << endl;
		} else {
			cout << "  *** Could not find a gatekeeper." << endl;
			return 1;
		}	
	} else {
		rasChannel = new H323TransportUDP(*endPoint);

		if (!rasChannel) {
			cout << "  *** No RAS Channel, this is bad" << endl;
			return 1;
		}
		if (endPoint->SetGatekeeper(gkName, rasChannel)) {
			cout << "  == Using " << (endPoint->GetGatekeeper())->GetName() << " as our Gatekeeper." << endl;
		} else {
			cout << "  *** Error registering with gatekeeper \"" << gkName << "\". " << endl;
			
			/* XXX Maybe we should fire a new thread to attempt to re-register later and not kill asterisk here? */
			return 1;
		}
	}
	
	return 0;
}

/** Send a DTMF tone over the H323Connection with the
  * specified token.
  */
void h323_send_tone(const char *call_token, char tone)
{
	if (!h323_end_point_exist()) {
		cout << "ERROR: [h323_send_tone] No Endpoint, this is bad!" << endl;
		return;
	}

	PString token = PString(call_token);
	endPoint->SendUserTone(token, tone);
}

/** Make a call to the remote endpoint.
  */
int h323_make_call(char *host, call_details_t *cd, call_options_t call_options)
{
	int res;
	PString	token;

	if (!h323_end_point_exist()) {
		return 1;
	}
	
	PString dest(host);

	res = endPoint->MakeCall(dest, token, &cd->call_reference, call_options.port, call_options.callerid);
	memcpy((char *)(cd->call_token), (const unsigned char *)token, token.GetLength());
	
	return res;
};

int h323_clear_call(const char *call_token)
{
	if (!h323_end_point_exist()) {
		return 1;
	}

        endPoint->ClearCall(PString(call_token));
	return 0;
};

/** This function tells the h.323 stack to either 
    answer or deny an incoming call  */
int h323_answering_call(const char *token, int busy) 
{
	const PString currentToken(token);
	H323Connection * connection;
	
	connection = endPoint->FindConnectionWithLock(currentToken);
	
	if (connection == NULL) {
		cout << "No connection found for " << token << endl;
		return -1;
	}

	if (!busy){
		connection->AnsweringCall(H323Connection::AnswerCallNow);
		connection->Unlock();

	} else {
		connection->AnsweringCall(H323Connection::AnswerCallDenied);
		connection->Unlock();
	};

	return 0;
}


int h323_show_codec(int fd, int argc, char *argv[])
{
	cout <<  "Allowed Codecs:\n\t" << setprecision(2) << endPoint->GetCapabilities() << endl;
	return 0;
}

int h323_soft_hangup(const char *data)
{
	PString token(data);
	BOOL result;
	
	result = endPoint->ClearCall(token);	
	return result;
}

/* alas, this doesn't work :(   */
void h323_native_bridge(const char *token, char *them, char *capability)
{
	H323Channel *channel;
	MyH323Connection *connection = (MyH323Connection *)endPoint->FindConnectionWithLock(token);
	
	if (!connection){
		cout << "ERROR: No connection found, this is bad\n";
		return;
	}

	cout << "Native Bridge:  them [" << them << "]" << endl; 

	channel = connection->FindChannel(connection->sessionId, TRUE);
	connection->bridging = TRUE;
	connection->CloseLogicalChannelNumber(channel->GetNumber());
	
	connection->Unlock();
	return;

}

} /* extern "C" */

