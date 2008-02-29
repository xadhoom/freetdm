/*
 * Copyright (c) 2007, Anthony Minessale II
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "openzap.h"
#include "ss7_boost_client.h"
#include "zap_ss7_boost.h"

typedef uint16_t ss7_boost_request_id_t;

typedef enum {
	BST_FREE,
	BST_WAITING,
	BST_READY,
	BST_FAIL
} ss7_boost_request_status_t;

typedef struct {
	ss7_boost_request_status_t status;
	ss7bc_event_t event;
	zap_span_t *span;
	zap_channel_t *zchan;
} ss7_boost_request_t;

static ss7_boost_request_id_t current_request = 0;

static ss7_boost_request_t OUTBOUND_REQUESTS[ZAP_MAX_CHANNELS_SPAN] = {{ 0 }};

static zap_mutex_t *request_mutex = NULL;
static zap_mutex_t *signal_mutex = NULL;

static ss7_boost_request_id_t next_request_id(void)
{
	 ss7_boost_request_id_t r;

	zap_mutex_lock(request_mutex);
	if (current_request == ZAP_MAX_CHANNELS_SPAN) {
		current_request = 0;
	}
	r = current_request++;
	zap_mutex_unlock(request_mutex);
	
	return r + 1;
}

static zap_channel_t *find_zchan(zap_span_t *span, ss7bc_event_t *event)
{
	int i;
	zap_channel_t *zchan = NULL;

	for(i = 0; i < span->chan_count; i++) {
		if (span->channels[i].physical_span_id == event->span+1 && span->channels[i].physical_chan_id == event->chan+1) {
			zchan = &span->channels[i];
			break;
		}
	}

	return zchan;
}

static ZIO_CHANNEL_REQUEST_FUNCTION(ss7_boost_channel_request)
{
	zap_ss7_boost_data_t *ss7_boost_data = span->signal_data;
	zap_status_t status = ZAP_SUCCESS;
	ss7_boost_request_id_t r = next_request_id();
	ss7bc_event_t event = {0};
	int sanity = 60000;

	ss7bc_call_init(&event, caller_data->cid_num, caller_data->ani, r);
	OUTBOUND_REQUESTS[r].status = BST_WAITING;
	OUTBOUND_REQUESTS[r].span = span;

	if (ss7bc_connection_write(&ss7_boost_data->mcon, &event) <= 0) {
		zap_log(ZAP_LOG_CRIT, "Failed to tx on ISUP socket [%s]\n", strerror(errno));
		status = ZAP_FAIL;
		goto done;
	}

	while(OUTBOUND_REQUESTS[r].status == BST_WAITING) {
		zap_sleep(1);
		if (!--sanity) {
			status = ZAP_FAIL;
			goto done;
		}
	}

	if (OUTBOUND_REQUESTS[r].status == BST_READY) {
		*zchan = OUTBOUND_REQUESTS[r].zchan;
	}

 done:
	
	OUTBOUND_REQUESTS[r].status = BST_FREE;

	return status;
}

static ZIO_CHANNEL_OUTGOING_CALL_FUNCTION(ss7_boost_outgoing_call)
{
	zap_status_t status = ZAP_SUCCESS;
	return status;
}

static void handle_call_start_ack(ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	zap_channel_t *zchan;

	OUTBOUND_REQUESTS[event->call_setup_id].event = *event;

	if ((zchan = find_zchan(OUTBOUND_REQUESTS[event->call_setup_id].span, event))) {
		OUTBOUND_REQUESTS[event->call_setup_id].status = BST_READY;
		if (zap_channel_open_chan(zchan) != ZAP_SUCCESS) {
			zap_log(ZAP_LOG_ERROR, "OPEN ERROR [%s]\n", zchan->last_error);
		} else {
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DIALING);
			OUTBOUND_REQUESTS[event->call_setup_id].zchan = zchan;
			return;
		}
	} 

	ss7bc_exec_command(mcon,
					   event->span,
					   event->chan,
					   event->call_setup_id,
					   SIGBOOST_EVENT_CALL_STOPPED,
					   0);
	OUTBOUND_REQUESTS[event->call_setup_id].status = BST_FAIL;
	
}

static void handle_call_start_nack(ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	OUTBOUND_REQUESTS[event->call_setup_id].event = *event;
	OUTBOUND_REQUESTS[event->call_setup_id].status = BST_FAIL;

	ss7bc_exec_command(mcon,
					   event->span,
					   event->chan,
					   event->call_setup_id,
					   SIGBOOST_EVENT_CALL_START_NACK_ACK,
					   0);
}

static void handle_call_stop(zap_span_t *span, ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	zap_channel_t *zchan;
	
	if ((zchan = find_zchan(span, event))) {
		zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_TERMINATING);
	} else {
		ss7bc_exec_command(mcon,
						   event->span,
						   event->chan,
						   0,
						   SIGBOOST_EVENT_CALL_STOPPED,
						   0);
	}
}

static void handle_call_answer(zap_span_t *span, ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	zap_channel_t *zchan;
	
	if ((zchan = find_zchan(span, event))) {
		zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_UP);
	} else {
		ss7bc_exec_command(mcon,
						   event->span,
						   event->chan,
						   0,
						   SIGBOOST_EVENT_CALL_STOPPED,
						   0);
	}
}

static void handle_call_start(zap_span_t *span, ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	zap_channel_t *zchan;

	if ((zchan = find_zchan(span, event))) {
		ss7bc_exec_command(mcon,
						   event->span,
						   event->chan,
						   0,
						   SIGBOOST_EVENT_CALL_START_ACK,
						   0);

		zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_RING);
	} else {
		ss7bc_exec_command(mcon,
						   event->span,
						   event->chan,
						   0,
						   SIGBOOST_EVENT_CALL_START_NACK,
						   0);
	}
}


static void handle_heartbeat(ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	int err = ss7bc_connection_write(mcon, event);

	if (err <= 0) {
		zap_log(ZAP_LOG_CRIT, "Failed to tx on ISUP socket [%s]: %s\n", strerror(errno));
	}

    return;
}

static void handle_restart_ack(ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
    mcon->rxseq_reset = 0;
}

static int parse_ss7_event(zap_span_t *span, ss7bc_connection_t *mcon, ss7bc_event_t *event)
{
	zap_mutex_lock(signal_mutex);

	zap_log(ZAP_LOG_DEBUG,
			"RX EVENT: %s:(%X) [w%dg%d] Rc=%i CSid=%i Seq=%i Cd=[%s] Ci=[%s]\n",
			   ss7bc_event_id_name(event->event_id),
			   event->event_id,
			   event->span+1,
			   event->chan+1,
			   event->release_cause,
			   event->call_setup_id,
			   event->fseqno,
			   (event->called_number_digits_count ? (char *) event->called_number_digits : "N/A"),
			   (event->calling_number_digits_count ? (char *) event->calling_number_digits : "N/A")
			   );


    switch(event->event_id) {

    case SIGBOOST_EVENT_CALL_START:
		handle_call_start(span, mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_STOPPED:
		handle_call_stop(span, mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_START_ACK:
		handle_call_start_ack(mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_START_NACK:
		handle_call_start_nack(mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_ANSWERED:
		handle_call_answer(span, mcon, event);
		break;
    case SIGBOOST_EVENT_HEARTBEAT:
		handle_heartbeat(mcon, event);
		break;
    case SIGBOOST_EVENT_CALL_START_NACK_ACK:
		//handle_call_start_nack_ack(event);
		break;
    case SIGBOOST_EVENT_CALL_STOPPED_ACK:
		//handle_call_stop_ack(event);
		break;
    case SIGBOOST_EVENT_INSERT_CHECK_LOOP:
		//handle_call_loop_start(event);
		break;
    case SIGBOOST_EVENT_REMOVE_CHECK_LOOP:
		//handle_call_stop(event);
		break;
    case SIGBOOST_EVENT_SYSTEM_RESTART_ACK:
		handle_restart_ack(mcon,event);
		break;
    case SIGBOOST_EVENT_AUTO_CALL_GAP_ABATE:
		//handle_gap_abate(event);
		break;
    default:
		zap_log(ZAP_LOG_WARNING, "No handler implemented for [%s]\n", ss7bc_event_id_name(event->event_id));
		break;
    }

	zap_mutex_unlock(signal_mutex);

	return 0;
}

static __inline__ void state_advance(zap_channel_t *zchan)
{

	zap_ss7_boost_data_t *ss7_boost_data = zchan->span->signal_data;
	ss7bc_connection_t *mcon = &ss7_boost_data->mcon;
	zap_sigmsg_t sig;
	zap_status_t status;

	zap_log(ZAP_LOG_ERROR, "%d:%d STATE [%s]\n", 
			zchan->span_id, zchan->chan_id, zap_channel_state2str(zchan->state));

	memset(&sig, 0, sizeof(sig));
	sig.chan_id = zchan->chan_id;
	sig.span_id = zchan->span_id;
	sig.channel = zchan;

	switch (zchan->state) {
	case ZAP_CHANNEL_STATE_DOWN:
		{
			zap_channel_done(zchan);
		}
		break;
	case ZAP_CHANNEL_STATE_PROGRESS:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_PROGRESS;
				if ((status = ss7_boost_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				//send progress
			}
		}
		break;
	case ZAP_CHANNEL_STATE_RING:
		{
			if (!zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_START;
				if ((status = ss7_boost_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			}

		}
		break;
	case ZAP_CHANNEL_STATE_RESTART:
		{
			if (zchan->last_state > ZAP_CHANNEL_STATE_HANGUP) {
				zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
			}
		}
		break;
	case ZAP_CHANNEL_STATE_PROGRESS_MEDIA:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_PROGRESS_MEDIA;
				if ((status = ss7_boost_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				// send alerting
			}
		}
		break;
	case ZAP_CHANNEL_STATE_UP:
		{
			if (zap_test_flag(zchan, ZAP_CHANNEL_OUTBOUND)) {
				sig.event_id = ZAP_SIGEVENT_UP;
				if ((status = ss7_boost_data->signal_cb(&sig) != ZAP_SUCCESS)) {
					zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_HANGUP);
				}
			} else {
				// send connect
			}
		}
		break;
	case ZAP_CHANNEL_STATE_DIALING:
		{
		}
		break;
	case ZAP_CHANNEL_STATE_HANGUP:
		{
			ss7bc_exec_command(mcon,
							   zchan->physical_span_id-1,
							   zchan->physical_chan_id-1,
							   0,
							   SIGBOOST_EVENT_CALL_STOPPED,
							   0);
			zap_set_state_locked(zchan, ZAP_CHANNEL_STATE_DOWN);
			
		}
		break;
	case ZAP_CHANNEL_STATE_TERMINATING:
		{
			sig.event_id = ZAP_SIGEVENT_STOP;
			status = ss7_boost_data->signal_cb(&sig);
		}
	default:
		break;
	}
}


static __inline__ void check_state(zap_span_t *span)
{
	

	if (zap_test_flag(span, ZAP_SPAN_STATE_CHANGE)) {
		uint32_t j;
		for(j = 1; j <= span->chan_count; j++) {
			if (zap_test_flag((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE)) {
				zap_mutex_lock(signal_mutex);
				state_advance(&span->channels[j]);
				zap_mutex_unlock(signal_mutex);
				zap_clear_flag_locked((&span->channels[j]), ZAP_CHANNEL_STATE_CHANGE);
			}
		}
		zap_clear_flag_locked(span, ZAP_SPAN_STATE_CHANGE);
	}
}


static void *zap_ss7_boost_run(zap_thread_t *me, void *obj)
{
    zap_span_t *span = (zap_span_t *) obj;
    zap_ss7_boost_data_t *ss7_boost_data = span->signal_data;
	ss7bc_connection_t *mcon, *pcon;


	ss7_boost_data->pcon = ss7_boost_data->mcon;

	if (ss7bc_connection_open(&ss7_boost_data->mcon,
							  ss7_boost_data->mcon.cfg.local_ip,
							  ss7_boost_data->mcon.cfg.local_port,
							  ss7_boost_data->mcon.cfg.remote_ip,
							  ss7_boost_data->mcon.cfg.remote_port) < 0) {
		zap_log(ZAP_LOG_DEBUG, "Error: Opening MCON Socket [%d] %s\n", ss7_boost_data->mcon.socket, strerror(errno));
		goto end;
    }

	if (ss7bc_connection_open(&ss7_boost_data->pcon,
							  ss7_boost_data->pcon.cfg.local_ip,
							  ++ss7_boost_data->pcon.cfg.local_port,
							  ss7_boost_data->pcon.cfg.remote_ip,
							  ss7_boost_data->pcon.cfg.remote_port) < 0) {
		zap_log(ZAP_LOG_DEBUG, "Error: Opening PCON Socket [%d] %s\n", ss7_boost_data->pcon.socket, strerror(errno));
		goto end;
    }
	
	mcon = &ss7_boost_data->mcon;
	pcon = &ss7_boost_data->pcon;

	mcon->rxseq_reset = 1;

	ss7bc_exec_command(mcon,
					   0,
					   0,
					   -1,
					   SIGBOOST_EVENT_SYSTEM_RESTART,
					   0);
	
	while (zap_running() && zap_test_flag(ss7_boost_data, ZAP_SS7_BOOST_RUNNING)) {
		fd_set rfds, efds;
		struct timeval tv = { 0, 100000 };
		int max, activity, i = 0;
		ss7bc_event_t *event = NULL;

		FD_ZERO(&rfds);
		FD_ZERO(&efds);
		FD_SET(mcon->socket, &rfds);
		FD_SET(mcon->socket, &efds);
		FD_SET(pcon->socket, &rfds);
		FD_SET(pcon->socket, &efds);

		max = ((pcon->socket > mcon->socket) ? pcon->socket : mcon->socket) + 1;
		
		if ((activity = select(max, &rfds, NULL, &efds, &tv)) < 0) {
			goto error;
		}
		
		if (!activity) {
			continue;
		}

		if (FD_ISSET(pcon->socket, &efds) || FD_ISSET(mcon->socket, &efds)) {
			goto error;
		}

		if (FD_ISSET(pcon->socket, &rfds)) {
			if ((event = ss7bc_connection_readp(pcon, i))) {
				parse_ss7_event(span, pcon, event);
			}
		}

		if (FD_ISSET(mcon->socket, &rfds)) {
			if ((event = ss7bc_connection_read(mcon, i))) {
				parse_ss7_event(span, mcon, event);
			}
		}
		
		check_state(span);
		
	}

	goto end;

 error:
	zap_log(ZAP_LOG_CRIT, "Socket Error!\n");

 end:

	ss7bc_connection_close(&ss7_boost_data->mcon);
	ss7bc_connection_close(&ss7_boost_data->pcon);

	zap_clear_flag(ss7_boost_data, ZAP_SS7_BOOST_RUNNING);

	zap_log(ZAP_LOG_DEBUG, "SS7_BOOST thread ended.\n");
	return NULL;
}

zap_status_t zap_ss7_boost_init(void)
{
	zap_mutex_create(&request_mutex);
	zap_mutex_create(&signal_mutex);

	return ZAP_SUCCESS;
}

zap_status_t zap_ss7_boost_start(zap_span_t *span)
{
	zap_ss7_boost_data_t *ss7_boost_data = span->signal_data;
	zap_set_flag(ss7_boost_data, ZAP_SS7_BOOST_RUNNING);
	return zap_thread_create_detached(zap_ss7_boost_run, span);
}

zap_status_t zap_ss7_boost_configure_span(zap_span_t *span,
										  const char *local_ip, int local_port, 
										  const char *remote_ip, int remote_port,
										  zio_signal_cb_t sig_cb)
{
	zap_ss7_boost_data_t *ss7_boost_data = NULL;
	
	if (!local_ip && local_port && remote_ip && remote_port && sig_cb) {
		return ZAP_FAIL;
	}

	ss7_boost_data = malloc(sizeof(*ss7_boost_data));
	assert(ss7_boost_data);
	memset(ss7_boost_data, 0, sizeof(*ss7_boost_data));
	
	zap_set_string(ss7_boost_data->mcon.cfg.local_ip, local_ip);
	ss7_boost_data->mcon.cfg.local_port = local_port;
	zap_set_string(ss7_boost_data->mcon.cfg.remote_ip, remote_ip);
	ss7_boost_data->mcon.cfg.remote_port = remote_port;
	ss7_boost_data->signal_cb = sig_cb;

	span->signal_data = ss7_boost_data;
    span->signal_type = ZAP_SIGTYPE_SS7BOOST;
    span->outgoing_call = ss7_boost_outgoing_call;
	span->channel_request = ss7_boost_channel_request;
	
	return ZAP_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */