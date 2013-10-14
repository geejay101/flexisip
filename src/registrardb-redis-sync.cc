/*
        Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2012  Belledonne Communications SARL.
    Author: Guillaume Beraudo

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "recordserializer.hh"
#include "registrardb-redis.hh"
#include "common.hh"

#include <ctime>
#include <cstdio>
#include <vector>
#include <algorithm>

#include "configmanager.hh"

#include <hiredis/hiredis.h>

#include <sofia-sip/sip_protos.h>

using namespace::std;


RegistrarDbRedisSync::RegistrarDbRedisSync ( const string &preferredRoute, RecordSerializer *serializer, RedisParameters params) :
RegistrarDb ( preferredRoute ), mContext ( NULL ), mSerializer(serializer), sDomain(params.domain),
sAuthPassword(params.auth), sPort(params.port),sTimeout(params.timeout) {
}



RegistrarDbRedisSync::~RegistrarDbRedisSync() {
	if ( mContext )
		redisFree ( mContext );
}

bool RegistrarDbRedisSync::isConnected() {
	return mContext && REDIS_CONNECTED == ( mContext->flags & REDIS_CONNECTED );
}

bool RegistrarDbRedisSync::connect() {
	if ( isConnected() ) {
		LOGW ( "Redis already connected" );
		return true;
	}

	int seconds = sTimeout / 1000;
	struct timeval timeout = {seconds, sTimeout - seconds};
	mContext = redisConnectWithTimeout ( sDomain.c_str(), sPort, timeout );

	if ( mContext->err ) {
		LOGE ( "Redis Connection error: %s", mContext->errstr );
		redisFree ( mContext );
		mContext = NULL;
		return false;
	}

	if ( !sAuthPassword.empty() ) {
		redisReply *reply = ( redisReply* ) redisCommand ( mContext, "AUTH %s", sAuthPassword.c_str() );

		if ( reply->type == REDIS_REPLY_ERROR ) {
			LOGE ( "Couldn't authenticate with redis server: %s", reply->str);
			redisFree ( mContext );
			mContext = NULL;
			return false;
		}
	}

	return true;
}

static bool isPrintable(const char* str) {
	while(str) {
		if (!isprint(*str)) return false;
		++str;
	}
	return true;
}

void RegistrarDbRedisSync::doBind ( const RegistrarDb::BindParameters& p, const shared_ptr< RegistrarDbListener >& listener ) {
	char key[AOR_KEY_SIZE] = {0};
	defineKeyFromUrl ( key, AOR_KEY_SIZE - 1, p.sip.from );

	if ( errorOnTooMuchContactInBind ( p.sip.contact, key, listener ) ) {
		listener->onError();
		return;
	}

	if ( !isConnected() && !connect() ) {
		listener->onError();
		return;
	}

	redisReply *reply = ( redisReply* ) redisCommand ( mContext, "GET aor:%s", key );

	if ( reply->type == REDIS_REPLY_ERROR ) {
		LOGE ( "Redis error getting aor:%s - %s", key, reply->str );
		listener->onError();
		return;
	}

	LOGD ( "GOT aor:%s --> %s", key, isPrintable(reply->str) ? reply->str : "binary" );
	Record r ( key );
	mSerializer->parse ( reply->str, reply->len, &r );
	freeReplyObject ( reply );

	if ( r.isInvalidRegister ( p.sip.call_id, p.sip.cs_seq ) ) {
		listener->onInvalid();
		return;
	}

	time_t now = getCurrentTime();
	r.clean ( p.sip.contact, p.sip.call_id, p.sip.cs_seq, now );
	r.update ( p.sip.contact, p.sip.path, p.global_expire, p.sip.call_id, p.sip.cs_seq, now, p.alias );
	mLocalRegExpire->update ( r );

	string updatedAorString;
	mSerializer->serialize ( &r, updatedAorString );

	reply = ( redisReply* ) redisCommand ( mContext, "SET aor:%s %s", key, updatedAorString.c_str() );

	if ( reply->type == REDIS_REPLY_ERROR ) {
		LOGE ( "Redis error setting aor:%s with %s - %s", key, updatedAorString.c_str(), reply->str );
		listener->onError();
		freeReplyObject ( reply );
		return;
	}

	LOGD ( "Sent updated aor:%s --> %s", key, updatedAorString.c_str() );
	freeReplyObject ( reply );

	redisCommand ( mContext,"EXPIREAT aor:%s %lu",key, r.latestExpire() );
	listener->onRecordFound ( &r );
}


void RegistrarDbRedisSync::doClear ( const sip_t *sip, const shared_ptr<RegistrarDbListener> &listener ) {
	char key[AOR_KEY_SIZE] = {0};
	defineKeyFromUrl ( key, AOR_KEY_SIZE - 1, sip->sip_from->a_url );

	if ( !isConnected() && !connect() ) {
		listener->onError();
		return;
	}

	redisReply *reply = ( redisReply* ) redisCommand ( mContext, "GET aor:%s", key );

	if ( reply->type == REDIS_REPLY_ERROR ) {
		LOGE ( "Redis error getting aor:%s - %s", key, reply->str );
		listener->onError();
		return;
	}

	LOGD ( "GOT aor:%s --> %s", key, reply->str );

	if ( reply->str > 0 ) {
		Record r ( key );
		mSerializer->parse ( reply->str, reply->len, &r );

		if ( r.isInvalidRegister ( sip->sip_call_id->i_id, sip->sip_cseq->cs_seq ) ) {
			listener->onInvalid();
			freeReplyObject ( reply );
			return;
		}
	}

	freeReplyObject ( reply );


	reply = ( redisReply* ) redisCommand ( mContext, "DEL aor:%s", key );

	if ( reply->type == REDIS_REPLY_ERROR ) {
		LOGE ( "Redis error removing aor:%s - %s", key, reply->str );
		listener->onError();
		freeReplyObject ( reply );
		return;
	}

	LOGD ( "Removed aor:%s", key );
	freeReplyObject ( reply );

	mLocalRegExpire->remove ( key );
	listener->onRecordFound ( NULL );
}

void RegistrarDbRedisSync::doFetch ( const url_t *url, const shared_ptr<RegistrarDbListener> &listener ) {
	char key[AOR_KEY_SIZE] = {0};
	defineKeyFromUrl ( key, AOR_KEY_SIZE - 1, url );

	if ( !isConnected() && !connect() ) {
		listener->onError();
		return;
	}

	redisReply *reply = ( redisReply* ) redisCommand ( mContext, "GET aor:%s", key );

	if ( reply->type == REDIS_REPLY_ERROR ) {
		LOGE ( "Redis error getting aor:%s - %s", key, reply->str );
		listener->onError();
		return;
	}

	LOGD ( "GOT aor:%s --> %s", key, isPrintable(reply->str) ? reply->str : "binary" );
	if (reply->len>0){
		Record r ( key );
		mSerializer->parse ( reply->str, reply->len, &r );

		time_t now = getCurrentTime();
		r.clean ( now );

		listener->onRecordFound ( &r );
	}else{
		listener->onRecordFound (NULL);
	}
	freeReplyObject ( reply );
}
