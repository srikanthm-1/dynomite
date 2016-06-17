#include <dyn_core.h>
#include <dyn_thread_ctx.h>
#include <dyn_server.h>
#include <dyn_dnode_peer.h>

static tid_t tid_counter = 0;
__thread pthread_ctx g_ptctx = NULL;
pthread_ctx
thread_ctx_create(void)
{
    pthread_ctx ptctx = dn_zalloc(sizeof(*ptctx));
    return ptctx;
}

static rstatus_t
handle_ipc_events(void *arg, uint32_t events)
{
	rstatus_t status;
	pthread_ipc ptipc = arg;
    ASSERT_LOG(ptipc == g_ptctx->ptipc,
               "wrong ipc notification, my ptipc:%p, received for %p",
               g_ptctx->ptipc, ptipc);

	//struct context *ctx = conn_to_ctx(conn);
    log_debug(LOG_VVVERB, "event %04"PRIX32" on %s %d", events,
              pollable_get_type_string(&ptipc->p), ptipc->p.sd);

	/* error takes precedence over read | write */
	if (events & EVENT_ERR) {
        log_error("handling error on ipc %d", ptipc->p.sd);
		ASSERT(NULL);

		return DN_ERROR;
	}

	/* read takes precedence over write */
	if (events & EVENT_READ) {
        log_debug(LOG_VVERB, "handling read on ipc %d", ptipc->p.sd);

        struct msg * msg = NULL;
        uint32_t count = 0;
        while ((msg = thread_ipc_receive(ptipc)) != NULL) {
            count++;
            if (msg->request) {
                // received a message, if its a request forward it to peer
                struct peer *dst_peer = msg->dst_peer;
                ASSERT(dst_peer != NULL);
                ASSERT(g_ptctx == dst_peer->ptctx);
                log_debug(LOG_VERB, "forward req %d:%d to peer %p", msg->id, msg->parent_id, dst_peer);
                dnode_peer_req_forward(g_ptctx->ctx, dst_peer, msg);
            } else {
                // if its a response, call the client associated with it.
                // THere is swallowing business here because if we are receiving a
                // response here it means that the client(CLIENT or DNODE_CLIENT) is
                // expecting it. Either the response is for a remote dc forwarded
                // request or its part of a quorum response
                struct conn *c_conn = msg->client_conn;
                ASSERT(c_conn->ptctx == g_ptctx);
                log_info("sending rsp %p %lu:%lu upstream for req %lu", msg, msg->id, msg->parent_id, msg->req_id);
                log_info("setting peer on msg %p %lu:%lu to NULL", msg, msg->id, msg->parent_id);
                msg->peer = NULL;
                status = conn_handle_response(msg->client_conn, msg);
                IGNORE_RET_VAL(status);
            }
        }
        log_debug(LOG_DEBUG, "ptctx %p handled %u messages", g_ptctx, count);
    }

	if (events & EVENT_WRITE) {
        ASSERT_LOG(NULL, "received write notifiction on ipc %d, But I never registered for one", ptipc->p.sd);
	}

	return DN_OK;
}
    
static rstatus_t
handle_connection_events(void *arg, uint32_t events)
{
	rstatus_t status;
	struct conn *conn = arg;

	struct context *ctx = conn_to_ctx(conn);

    log_debug(LOG_VVVERB, "event %04"PRIX32" on %s %d", events,
              conn_get_type_string(conn), conn->p.sd);

	conn->events = events;

	/* error takes precedence over read | write */
	if (events & EVENT_ERR) {
        log_debug(LOG_VVVERB, "handing error on %s %d",
                  conn_get_type_string(conn), conn->p.sd);
		if (conn->err && conn->dyn_mode) {
			loga("conn err on dnode EVENT_ERR: %d", conn->err);
		}
		conn_error(ctx, conn);

		return DN_ERROR;
	}

	/* read takes precedence over write */
	if (events & EVENT_READ) {
        log_debug(LOG_VVVERB, "handing read on %s %d",
                  conn_get_type_string(conn), conn->p.sd);
		status = conn_recv(ctx, conn);

		if (status != DN_OK || conn->done || conn->err) {
			if (conn->dyn_mode) {
				if (conn->err) {
					loga("conn err on dnode EVENT_READ: %d", conn->err);
					conn_close(ctx, conn);
					return DN_ERROR;
				}
				return DN_OK;
			}

			conn_close(ctx, conn);
			return DN_ERROR;
		}
	}

	if (events & EVENT_WRITE) {
        log_debug(LOG_VVVERB, "handing write on %s %d",
                  conn_get_type_string(conn), conn->p.sd);
		status = conn_send(ctx, conn);
		if (status != DN_OK || conn->done || conn->err) {
			if (conn->dyn_mode) {
				if (conn->err) {
					loga("conn err on dnode EVENT_WRITE: %d", conn->err);
					conn_close(ctx, conn);
					return DN_ERROR;
				}
				return DN_OK;
			}

			conn_close(ctx, conn);
			return DN_ERROR;
		}
	}

	return DN_OK;
}

static rstatus_t
thread_ctx_core(void *arg, uint32_t events)
{
	struct pollable *pollable = arg;

    // Depending on the type of pollable, call the appropriate functions
    switch(pollable->type) {
        case CONN_PROXY :
        case CONN_CLIENT:
        case CONN_SERVER:
        case CONN_DNODE_PEER_PROXY:
        case CONN_DNODE_PEER_CLIENT:
        case CONN_DNODE_PEER_SERVER:
                               return handle_connection_events(arg, events);
        case CONN_THREAD_IPC_MQ:
                               return handle_ipc_events(arg, events);
        default:
            ASSERT_LOG(NULL, "invalid type of pollable object %d", pollable->type);
    }
    return DN_OK;
}

rstatus_t
thread_ctx_datastore_preconnect(void *elem, void *arg)
{
    pthread_ctx ptctx = elem;

    if (ptctx->datastore_conn == NULL)
        ptctx->datastore_conn = get_datastore_conn(ptctx);
    if (ptctx->datastore_conn == NULL) {
        log_error("Could not preconnect to datastore");
        return DN_ERROR;
    }
	return DN_OK;
}

rstatus_t
thread_ctx_init(pthread_ctx ptctx, struct context *ctx)
{
    ptctx->ctx = ctx;
    ptctx->tid = tid_counter++;
    ptctx->pthread_id = 0; // real thrread id thats running
	ptctx->evb = event_base_create(EVENT_SIZE, &thread_ctx_core);
    ptctx->datastore_conn = NULL;
    ptctx->ptipc = thread_ipc_mq_create();
    thread_ipc_init(ptctx->ptipc, ptctx);
    msg_tmo_init(&ptctx->tmo, ptctx);
	if (ptctx->evb == NULL) {
		loga("Failed to create socket event handling!!!");
		return DN_ERROR;
	}
    return DN_OK;
}

rstatus_t
thread_ctx_deinit(void *elem, void *arg)
{
    pthread_ctx ptctx = elem;
	event_base_destroy(ptctx->evb);
    msg_tmo_deinit(&ptctx->tmo, ptctx);
    thread_ipc_destroy(ptctx->ptipc);
    ptctx->ptipc = NULL;
    conn_close(ptctx->ctx, ptctx->datastore_conn);
    ptctx->datastore_conn = NULL;
    return DN_OK;
}

rstatus_t
thread_ctx_add_conn(pthread_ctx ptctx, struct pollable *conn)
{
    log_debug(LOG_VVVERB, "ptctx %p: adding conn %p, %s", ptctx, conn, pollable_get_type_string(conn));
    return event_add_conn(ptctx->evb, conn);
}

rstatus_t
thread_ctx_del_conn(pthread_ctx ptctx, struct pollable *conn)
{
    log_debug(LOG_VVVERB, "ptctx %p: deleting conn %p, %s", ptctx, conn, pollable_get_type_string(conn));
    return event_del_conn(ptctx->evb, conn);
}

rstatus_t
thread_ctx_add_out(pthread_ctx ptctx, struct pollable *conn)
{
    log_debug(LOG_VVVERB, "ptctx %p: adding out conn %p, %s", ptctx, conn, pollable_get_type_string(conn));
    return event_add_out(ptctx->evb, conn);
}

rstatus_t
thread_ctx_del_out(pthread_ctx ptctx, struct pollable *conn)
{
    log_debug(LOG_VVVERB, "ptctx %p: deleting out conn %p, %s", ptctx, conn, pollable_get_type_string(conn));
    return event_del_out(ptctx->evb, conn);
}

rstatus_t
thread_ctx_add_in(pthread_ctx ptctx, struct pollable *conn)
{
    log_debug(LOG_VVVERB, "ptctx %p: adding in conn %p, %s", ptctx, conn, pollable_get_type_string(conn));
    return event_add_in(ptctx->evb, conn);
}

rstatus_t
thread_ctx_forward_req(pthread_ctx ptctx, struct msg *req)
{
    ASSERT(req->dst_peer->ptctx == ptctx);
    log_debug(LOG_VERB, "%p(%d) forwarding req %d:%d", ptctx, ptctx->tid, req->id, req->parent_id);
    return thread_ipc_send(ptctx->ptipc, req);
}

rstatus_t
thread_ctx_forward_rsp(pthread_ctx ptctx, struct msg *rsp)
{
    ASSERT(!rsp->request);
    ASSERT(rsp->owner != NULL);
    return thread_ipc_send(ptctx->ptipc, rsp);
}

static void
thread_ctx_timeout(pthread_ctx ptctx)
{
    struct context *ctx = ptctx->ctx;
	for (;;) {
		struct msg *msg;
		struct conn *conn;
		msec_t now, then;

		msg = msg_tmo_min(&ptctx->tmo);
		if (msg == NULL) {
			ptctx->timeout = ctx->max_timeout;
			return;
		}

		/* skip over req that are in-error or done */

		if (msg->error || msg->done) {
			msg_tmo_delete(&ptctx->tmo, msg);
			continue;
		}

		/*
		 * timeout expired req and all the outstanding req on the timing
		 * out server
		 */

		conn = msg->tmo_rbe.data;
		then = msg->tmo_rbe.key;

		now = dn_msec_now();
		if (now < then) {
			msec_t delta = then - now;
			ptctx->timeout = MIN(delta, ctx->max_timeout);
			return;
		}

        log_warn("req %"PRIu64" on %s %d timedout, timeout was %d", msg->id,
                 conn_get_type_string(conn), conn->p.sd, msg->tmo_rbe.timeout);

		msg_tmo_delete(&ptctx->tmo, msg);

		if (conn->dyn_mode) {
			if (conn->p.type == CONN_DNODE_PEER_SERVER) { //outgoing peer requests
                if (conn->same_dc)
			        stats_pool_incr(ctx, peer_timedout_requests);
                else
			        stats_pool_incr(ctx, remote_peer_timedout_requests);
			}
		} else {
			if (conn->p.type == CONN_SERVER) { //storage server requests
			   stats_server_incr(ctx, server_dropped_requests);
			}
		}

		conn->err = ETIMEDOUT;

		conn_close(ctx, conn);
	}
}

void *
notify_main_thread()
{
    int rc = pthread_barrier_wait(&datastore_preconnect_barr);
    if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
    {
        log_error("Could not wait on barrier");
    }
}

void *
thread_ctx_run(void *arg)
{
    pthread_ctx ptctx = arg;
    g_ptctx = ptctx;

    if (ptctx->ctx->pool.preconnect) {
        if (thread_ctx_datastore_preconnect(ptctx, NULL) != DN_OK) {
            log_error("Failed to preconnect to datastore");
        }
    }

    // notify main thread
    notify_main_thread();

	int nsd;
    for (;;) {
        nsd = event_wait(ptctx->evb, (int)ptctx->timeout);
        ASSERT(nsd >= 0);
	    thread_ctx_timeout(ptctx);
    }
    return 0;
}
