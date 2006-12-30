/*
  Copyright (C) 2005, 2004 Erik Eliasson, Johan Bilien, Joachim Orrblad
  
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*
 * Authors: Erik Eliasson <eliasson@it.kth.se>
 *          Johan Bilien <jobi@via.ecp.fr>
 *	    Joachim Orrblad <joachim@orrblad.com>
*/

#include <config.h>

#include "MikeyMessagePKE.h"
#include <libmikey/MikeyPayloadHDR.h>
#include <libmikey/MikeyPayloadT.h>
#include <libmikey/MikeyPayloadRAND.h>
#include <libmikey/MikeyException.h>
#include <libmikey/MikeyPayloadKeyData.h>
#include <libmikey/MikeyPayloadERR.h>
#include <libmikey/MikeyPayloadID.h>
#include <libmikey/MikeyPayloadKEMAC.h>
#include <libmikey/MikeyPayloadV.h>
#include <libmikey/MikeyPayloadPKE.h>
#include <libmcrypto/aes.h>
#include <libmcrypto/hmac.h>

using namespace std;

MikeyMessagePKE::MikeyMessagePKE(){
}

MikeyMessagePKE::MikeyMessagePKE(KeyAgreementPKE* ka, int encrAlg, int macAlg, MRef<certificate*> certInitiator){

	unsigned int csbId = rand();
	ka->setCsbId(csbId);
	MikeyPayloadT* tPayload;
	MikeyPayloadRAND* randPayload;

	//adding header payload
	addPayload(new MikeyPayloadHDR(HDR_DATA_TYPE_PK_INIT, 1, 
											HDR_PRF_MIKEY_1, csbId, ka->nCs(),
											ka->getCsIdMapType(), ka->csIdMap()));

	//adding timestamp payload
	addPayload(tPayload = new MikeyPayloadT());

	//adding security policy
	addPolicyToPayload(ka); //Is in MikeyMessage.cxx

	//keep a copy of the time stamp
	uint64_t t = tPayload->ts();
	ka->setTSent(t);

	//adding random payload
	addPayload(randPayload = new MikeyPayloadRAND());
	
	//keep a copy of the random value
	ka->setRand(randPayload->randData(), randPayload->randLength());

	// Derive the transport keys from the env_key:
	byte_t* encrKey = NULL;
	byte_t* iv = NULL;
	unsigned int encrKeyLength = 0;
	
	deriveTranspKeys( ka, encrKey, iv, encrKeyLength,
			  encrAlg, macAlg, t,
			  NULL );

	//adding KEMAC payload
	MikeyPayloads* subPayloads = new MikeyPayloads();
	MikeyPayloadKeyData* keydata = 
		new MikeyPayloadKeyData(KEYDATA_TYPE_TGK, ka->tgk(),
							ka->tgkLength(), ka->keyValidity());
	// FIXME get uri from certificate.
	const char uri[] = "sip:test";
	MikeyPayloadID* initId =
		new MikeyPayloadID( MIKEYPAYLOAD_ID_TYPE_URI, strlen( uri ), (byte_t*)uri );

	subPayloads->addPayload( initId );
	subPayloads->addPayload( keydata );
	initId = NULL;
	keydata = NULL;

	unsigned int rawKeyDataLength = subPayloads->rawMessageLength();
	byte_t* rawKeyData = new byte_t[ rawKeyDataLength ];
	memcpy( rawKeyData, subPayloads->rawMessageData(), rawKeyDataLength );
	
	addKemacPayload(rawKeyData, rawKeyDataLength,
			encrKey, iv, ka->authKey, encrAlg, macAlg, true );

	delete subPayloads;
	subPayloads = NULL;

	//adding PKE payload
	MRef<certificate*> certResponder = ka->getPublicKey();

	byte_t* env_key = ka->getEnvelopeKey();
	int encEnvKeyLength = 8192; // TODO autodetect?
	unsigned char* encEnvKey = new unsigned char[ encEnvKeyLength ];

	if( !certResponder->public_encrypt( env_key, ka->getEnvelopeKeyLength(),
					    encEnvKey, &encEnvKeyLength ) ){
		throw MikeyException( "PKE encryption of envelope key failed" );
	}

	addPayload(new MikeyPayloadPKE(2, encEnvKey, encEnvKeyLength));
	
	addSignaturePayload( certInitiator );

	//remove garbage
	if( encrKey != NULL )
		delete [] encrKey;

	if( iv != NULL )
		delete [] iv;
	
	delete [] rawKeyData;
	delete [] encEnvKey;
}

void MikeyMessagePKE::setOffer(KeyAgreement* kaBase){
	KeyAgreementPKE* ka = dynamic_cast<KeyAgreementPKE*>(kaBase);

	if( !ka ){
		throw MikeyExceptionMessageContent( 
				"Not a PKE keyagreement" );
	}

	MikeyPayload* i = extractPayload( MIKEYPAYLOAD_HDR_PAYLOAD_TYPE );
	bool error = false;
	//uint32_t csbId;
	MRef<MikeyCsIdMap*> csIdMap;
	MikeyMessage* errorMessage = new MikeyMessage();
	//uint8_t nCs;

	if( i == NULL || 
		i->payloadType() != MIKEYPAYLOAD_HDR_PAYLOAD_TYPE ){
		throw MikeyExceptionMessageContent( 
				"PKE init message had no HDR payload" );
	}

#define hdr ((MikeyPayloadHDR *)(i))
	if( hdr->dataType() != HDR_DATA_TYPE_PK_INIT ){
		throw MikeyExceptionMessageContent( 
				"Expected PKE init message" );
	}

	ka->setnCs( hdr->nCs() );
	ka->setCsbId( hdr->csbId() );
	ka->setV(hdr->v());

	if( hdr->csIdMapType() == HDR_CS_ID_MAP_TYPE_SRTP_ID || hdr->csIdMapType() == HDR_CS_ID_MAP_TYPE_IPSEC4_ID ){
		ka->setCsIdMap( hdr->csIdMap() );
		ka->setCsIdMapType( hdr->csIdMapType() );
	}
	else{
		throw MikeyExceptionMessageContent( 
				"Unknown type of CS ID map" );
	}
	

#undef hdr
	errorMessage->addPayload(
			new MikeyPayloadHDR( HDR_DATA_TYPE_ERROR, 0,
			HDR_PRF_MIKEY_1, ka->csbId(),
			ka->nCs(), ka->getCsIdMapType(), 
			ka->csIdMap() ) );

	//FIXME look at the other fields!

	remove( i );
	i = extractPayload( MIKEYPAYLOAD_T_PAYLOAD_TYPE );

	if( i == NULL )
		throw MikeyExceptionMessageContent( 
				"PKE init message had no T payload" );

	if( ((MikeyPayloadT*)i)->checkOffset( MAX_TIME_OFFSET ) ){
		error = true;
		errorMessage->addPayload( 
			new MikeyPayloadERR( MIKEY_ERR_TYPE_INVALID_TS ) );
	}	

	ka->t_received = ((MikeyPayloadT*)i)->ts();
	
	remove( i );

	addPolicyTo_ka(ka); //Is in MikeyMessage.cxx

	i = extractPayload( MIKEYPAYLOAD_RAND_PAYLOAD_TYPE );

	if( i == NULL ){
		error = true;
		errorMessage->addPayload( 
			new MikeyPayloadERR( MIKEY_ERR_TYPE_UNSPEC ) );
	}	

	ka->setRand( ((MikeyPayloadRAND *)i)->randData(),
			((MikeyPayloadRAND *)i)->randLength() );

	remove( i );
	i = extractPayload( MIKEYPAYLOAD_ID_PAYLOAD_TYPE );

	//FIXME treat the case of an ID payload
	if( i != NULL ){
		remove( i );
	}

	i = extractPayload( MIKEYPAYLOAD_KEMAC_PAYLOAD_TYPE );

	if( i == NULL ){
		error = true;
		errorMessage->addPayload( 
			new MikeyPayloadERR( MIKEY_ERR_TYPE_UNSPEC ) );
	}	

#define kemac ((MikeyPayloadKEMAC *)i)
	int encrAlg = kemac->encrAlg();
	int macAlg  = kemac->macAlg();
	ka->macAlg = macAlg;

	// Derive the transport keys
	byte_t * encrKey=NULL;
	byte_t * iv=NULL;
	unsigned int encrKeyLength = 0;
	
	if( !deriveTranspKeys( ka, encrKey, iv, encrKeyLength,
			      encrAlg, macAlg, ka->t_received,
			      errorMessage ) ){
		if( encrKey != NULL )
			delete [] encrKey;
		if( iv != NULL )
			delete [] iv;

		unsigned int authKeyLength = 20;
		byte_t* authKey = new byte_t[ authKeyLength ];
		ka->genTranspAuthKey( authKey, authKeyLength );
		
		errorMessage->addVPayload( MIKEY_MAC_HMAC_SHA1_160, 
				ka->t_received, authKey, authKeyLength  );
		
		delete [] authKey;
		throw MikeyExceptionMessageContent( errorMessage );
	}
	
	// decrypt the TGK
	MikeyPayloads* subPayloads = 
		kemac->decodePayloads( MIKEYPAYLOAD_ID_PAYLOAD_TYPE,
				 encrKey, encrKeyLength, iv );
	
	MikeyPayloadKeyData *keyData =
		dynamic_cast<MikeyPayloadKeyData*>(subPayloads->extractPayload( MIKEYPAYLOAD_KEYDATA_PAYLOAD_TYPE ));

	int tgkLength = keyData->keyDataLength();
	byte_t * tgk = keyData->keyData();

	ka->setTgk( tgk, tgkLength );
	ka->setKeyValidity( keyData->kv() );
#undef kemac

	if( encrKey != NULL )
		delete [] encrKey;
	if( iv != NULL )
		delete [] iv;
}

MikeyMessage* MikeyMessagePKE::buildResponse(KeyAgreement* kaBase){
	KeyAgreementPKE* ka = dynamic_cast<KeyAgreementPKE*>(kaBase);

	if( !ka ){
		throw MikeyExceptionMessageContent( 
				"Not a PKE keyagreement" );
	}
	
	if( ka->getV() || ka->getCsIdMapType() == HDR_CS_ID_MAP_TYPE_IPSEC4_ID ){
		// Build the response message
		MikeyMessage * result = new MikeyMessage();
		result->addPayload( 
			new MikeyPayloadHDR( HDR_DATA_TYPE_PK_RESP, 0, 
			HDR_PRF_MIKEY_1, ka->csbId(),
			ka->nCs(), ka->getCsIdMapType(), 
			ka->csIdMap() ) );

		result->addPayload( new MikeyPayloadT() );

		// TODO why do we call addPolicyToPayload here?
		addPolicyToPayload( ka ); //Is in MikeyMessage.cxx

		result->addVPayload( ka->macAlg, ka->t_received, 
				ka->authKey, ka->authKeyLength);

		if( ka->authKey != NULL ){
			delete [] ka->authKey;
			ka->authKey = NULL;
		}

		return result;
	}
	
	if( ka->authKey != NULL ){
		delete [] ka->authKey;
		ka->authKey = NULL;
	}
	
	return NULL;
}

MikeyMessage * MikeyMessagePKE::parseResponse( KeyAgreement * kaBase ){
	KeyAgreementPKE* ka = dynamic_cast<KeyAgreementPKE*>(kaBase);

	if( !ka ){
		throw MikeyExceptionMessageContent( 
				"Not a PKE keyagreement" );
	}

	MikeyPayload * i = extractPayload( MIKEYPAYLOAD_HDR_PAYLOAD_TYPE );
	bool error = false;
	MikeyMessage * errorMessage = new MikeyMessage();
	MRef<MikeyCsIdMap *> csIdMap;
	uint8_t nCs;
	
	if( i == NULL ||
		i->payloadType() != MIKEYPAYLOAD_HDR_PAYLOAD_TYPE ){

		throw MikeyExceptionMessageContent( 
				"PKE response message had no HDR payload" );
	}

#define hdr ((MikeyPayloadHDR *)(i))
	if( hdr->dataType() != HDR_DATA_TYPE_PK_RESP )
		throw MikeyExceptionMessageContent( 
				"Expected PKE response message" );

	if( hdr->csIdMapType() == HDR_CS_ID_MAP_TYPE_SRTP_ID || hdr->csIdMapType() == HDR_CS_ID_MAP_TYPE_IPSEC4_ID){
		csIdMap = hdr->csIdMap();
	}
	else{
		throw MikeyExceptionMessageContent( 
				"Unknown type of CS ID map" );
	}

	nCs = hdr->nCs();
#undef hdr
	ka->setCsIdMap( csIdMap );

	errorMessage->addPayload(
			new MikeyPayloadHDR( HDR_DATA_TYPE_ERROR, 0,
			HDR_PRF_MIKEY_1, ka->csbId(),
			nCs, HDR_CS_ID_MAP_TYPE_SRTP_ID,
			csIdMap ) );


	remove( i );
	i = extractPayload( MIKEYPAYLOAD_T_PAYLOAD_TYPE );

	if( i == NULL ){
		error = true;
		errorMessage->addPayload( 
			new MikeyPayloadERR( MIKEY_ERR_TYPE_UNSPEC ) );
	}	

	if( ((MikeyPayloadT*)i)->checkOffset( MAX_TIME_OFFSET ) ){
		error = true;
		errorMessage->addPayload( 
			new MikeyPayloadERR( MIKEY_ERR_TYPE_INVALID_TS ) );
	}	

	uint64_t t_received = ((MikeyPayloadT*)i)->ts();

	if( error ){
		byte_t authKey[20];
		unsigned int authKeyLength = 20;

		ka->genTranspAuthKey( authKey, 20 );
		
		errorMessage->addVPayload( MIKEY_MAC_HMAC_SHA1_160, 
				t_received, authKey, authKeyLength  );

		throw MikeyExceptionMessageContent( errorMessage );
	}
	addPolicyTo_ka(ka); //Is in MikeyMessage.cxx
	return NULL;
}

bool MikeyMessagePKE::authenticate(KeyAgreement* kaBase){
	KeyAgreementPKE* ka = dynamic_cast<KeyAgreementPKE*>(kaBase);

	if( !ka ){
		throw MikeyExceptionMessageContent( 
				"Not a PKE keyagreement" );
	}
	
	MikeyPayload * payload = *(lastPayload());
	int i;
	int macAlg;
	byte_t * receivedMac;
	byte_t * macInput;
	unsigned int macInputLength;
	list<MikeyPayload *>::iterator payload_i;
 
	if( ka->rand() == NULL ){
		
		MikeyPayloadRAND * randPayload;
		
		randPayload = (MikeyPayloadRAND*) extractPayload(MIKEYPAYLOAD_RAND_PAYLOAD_TYPE );
		
		if( randPayload == NULL ){
			ka->setAuthError(
				"The MIKEY init has no"
				"RAND payload."
			);
			
			return true;
		}

		ka->setRand( randPayload->randData(), 
			     randPayload->randLength() );
	}

	if( type() == HDR_DATA_TYPE_PK_INIT )
	{
		MikeyPayloadKEMAC * kemac;
		if( payload->payloadType() != MIKEYPAYLOAD_SIGN_PAYLOAD_TYPE){
			throw MikeyException( 
			   "PKE init did not end with a SIGN payload" );
		}
		
		MikeyPayloadSIGN* sig = (MikeyPayloadSIGN*)extractPayload(MIKEYPAYLOAD_SIGN_PAYLOAD_TYPE);
		
		int res;
		res = ka->getPublicKey()->verif_sign( rawMessageData(),
						      rawMessageLength() - sig->sigLength(),
						      sig->sigData(),
						      sig->sigLength() );
		if( res <= 0 ){
			cout << "Verification of the PKE init message SIGN payload failed! Code: "  << res << endl;
			cout << "Keypair of the initiator probably mismatch!" << endl;
			return true;
		}

		kemac = (MikeyPayloadKEMAC *) extractPayload(MIKEYPAYLOAD_KEMAC_PAYLOAD_TYPE);
		macAlg = kemac->macAlg();
		receivedMac = kemac->macData();
		
		macInputLength = kemac->length();
		macInput = new byte_t[macInputLength];

		kemac->writeData( macInput, macInputLength );
		macInput[0] = MIKEYPAYLOAD_LAST_PAYLOAD;
		macInputLength -= 20; // Subtract mac data

		ka->setCsbId( csbId() );

		MikeyPayload *payloadPke =
			extractPayload( MIKEYPAYLOAD_PKE_PAYLOAD_TYPE );
		MikeyPayloadPKE *pke =
			dynamic_cast<MikeyPayloadPKE*>( payloadPke );

		if( !pke ){
			throw MikeyException( "PKE init did not contain PKE payload" );
		}

		MRef<certificate*> cert = ka->getPublicKey();
		int envKeyLength = pke->dataLength();
		byte_t *envKey = new byte_t[ envKeyLength ];
		
		if( !cert->private_decrypt( pke->data(), pke->dataLength(),
					    envKey, &envKeyLength ) ){
			throw MikeyException( "Decryption of envelope key failed" );
		}

		ka->setEnvelopeKey( envKey, envKeyLength );

		delete[] envKey;
		envKey = NULL;
	}
	else if( type() == HDR_DATA_TYPE_PK_RESP )
	{
		if( ka->csbId() != csbId() ){
			ka->setAuthError( "CSBID mismatch\n" );
			return true;
		}
		MikeyPayloadV * v;
		uint64_t t_sent = ka->tSent();
		if( payload->payloadType() != MIKEYPAYLOAD_V_PAYLOAD_TYPE ){
			throw MikeyException( 
			   "PKE response did not end with a V payload" );
		}

		v = (MikeyPayloadV *)payload;
		macAlg = v->macAlg();
		receivedMac = v->verData();
		// macInput = raw_messsage without mac / sent_t
		macInputLength = rawMessageLength() - 20 + 8;
		macInput = new byte_t[macInputLength];
		memcpy( macInput, rawMessageData(), rawMessageLength() - 20 );
		
		for( i = 0; i < 8; i++ ){
			macInput[ macInputLength - i - 1 ] = 
				(byte_t)((t_sent >> (i*8))&0xFF);
		}
	}
	else{
		throw MikeyException( "Invalide type for a PKE message" );
	}

	byte_t authKey[20];
	byte_t computedMac[20];
	unsigned int computedMacLength;
	
	switch( macAlg ){
		case MIKEY_MAC_HMAC_SHA1_160:
			ka->genTranspAuthKey( authKey, 20 );

			hmac_sha1( authKey, 20,
				   macInput,
				   macInputLength,
				   computedMac, &computedMacLength );

			for( i = 0; i < 20; i++ ){
				if( computedMac[i] != receivedMac[i] ){
					ka->setAuthError(
						"MAC mismatch."
					);
					return true;
				}
			}
			return false;
		case MIKEY_MAC_NULL:
			return false;
		default:
			throw MikeyException( "Unknown MAC algorithm" );
	}
}

bool MikeyMessagePKE::deriveTranspKeys( KeyAgreementPKE* ka,
					byte_t*& encrKey, byte_t *& iv,
					unsigned int& encrKeyLength,
					int encrAlg, int macAlg,
					uint64_t t,
					MikeyMessage* errorMessage ){
	// Derive the transport keys from the env_key:
	byte_t* authKey = NULL;
	bool error = false;
	unsigned int authKeyLength = 0;
	int i;

	encrKey = NULL;
	iv = NULL;
	encrKeyLength = 0;

	switch( encrAlg ){
		case MIKEY_ENCR_AES_CM_128: {
			byte_t saltKey[14];
			encrKeyLength = 16;
			encrKey = new byte_t[ encrKeyLength ];
			ka->genTranspEncrKey(encrKey, encrKeyLength);
			ka->genTranspSaltKey(saltKey, sizeof(saltKey));
			iv = new byte_t[ encrKeyLength ];
			iv[0] = saltKey[0];
			iv[1] = saltKey[1];
			for( i = 2; i < 6; i++ ){
				iv[i] = saltKey[i] ^ (ka->csbId() >> (5-i)*8) & 0xFF;
			}

			for( i = 6; i < 14; i++ ){
				iv[i] = (byte_t)(saltKey[i] ^ (t >> (13-i)) & 0xFF);
			}
			iv[14] = 0x00;
			iv[15] = 0x00;
			break;
		}
		case MIKEY_ENCR_NULL:
			break;
		case MIKEY_ENCR_AES_KW_128:
			//TODO
		default:
			error = true;
			if( errorMessage ){
				errorMessage->addPayload( 
					new MikeyPayloadERR( MIKEY_ERR_TYPE_INVALID_EA ) );
			}
			else{
				throw MikeyException( "Unknown encryption algorithm" );
			}
	}
	switch( macAlg ){
		case MIKEY_MAC_HMAC_SHA1_160:
			authKeyLength = 20;
			authKey = new byte_t[ authKeyLength ];
			ka->genTranspAuthKey(authKey, authKeyLength);
			break;
		case MIKEY_MAC_NULL:
			authKey = NULL;
			break;
		default:
			error = true;
			if( errorMessage ){
				errorMessage->addPayload( 
					new MikeyPayloadERR( MIKEY_ERR_TYPE_INVALID_HA ) );
			}
			else{
				throw MikeyException( "Unknown MAC algorithm" );
			}
	}

	ka->authKey = authKey;
	ka->authKeyLength = authKeyLength;
	return !error;
}

bool MikeyMessagePKE::isInitiatorMessage() const{
	return type() == MIKEY_TYPE_PK_INIT;
}

bool MikeyMessagePKE::isResponderMessage() const{
	return type() == MIKEY_TYPE_PK_RESP;
}

int32_t MikeyMessagePKE::keyAgreementType() const{
	return KEY_AGREEMENT_TYPE_PK;
}
