/*
 * Copyright (c) 2020 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string.h>
#include <pthread.h>
#include <limits.h>

#include <crystal.h>
#include <ela_did.h>
#include <ela_jwt.h>

#include "feeds_client.h"
#include "mkdirs.h"

static const char *resolver = "http://api.elastos.io:21606";
const char *mnemonic = "advance duty suspect finish space matter squeeze elephant twenty over stick shield";

struct FeedsClient {
    DIDStore *store;
    char *passwd;
    char did[ELA_MAX_DID_LEN];
    pthread_mutex_t lock;
    ElaCarrier *carrier;
    char *access_token;
    struct {
        list_t *not_friend;
        list_t *friend_offline;
        list_t *ongoing;
    } tsx;
    char buf[0];
};

typedef struct Transaction Transaction;
struct Transaction {
    const char *addr;
    uint64_t tsx_id;
    const Req *req;
    Marshalled *(*marshal_req)(const Req *);
    int (*unmarshal_resp)(Resp **, ErrResp **);
    bool (*hdl_multi_resp)(Transaction *, Resp **, ErrResp **);
    void (*user_cb)(Resp *, ErrResp *, void *);
    void *user_data;

    Resp *multi_resp_cache;

    list_entry_t le;
    char node_id[ELA_MAX_ID_LEN + 1];
    Marshalled *marshalled;
    char buf[0];
};

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool fin;
    void *resp;
    void *err;
} SyncAPI;

#define list_foreach(list, entry)                    \
    for (list_iterate((list), &it);                  \
         list_iterator_next(&it, (void **)&(entry)); \
         deref((entry)))

static
void on_conn_status_change(ElaCarrier *w, ElaConnectionStatus status, void *context)
{
    FeedsClient *fc = (FeedsClient *)context;

    if (status == ElaConnectionStatus_Connected) {
        Transaction *tsx;
        int rc;

        pthread_mutex_lock(&fc->lock);
        for (; !list_is_empty(fc->tsx.not_friend); deref(tsx)) {
            tsx = list_pop_head(fc->tsx.not_friend);

            rc = ela_add_friend(fc->carrier, tsx->addr, "hello");
            if (rc < 0) {
                tsx->user_cb(NULL, NULL, tsx->user_data);
                continue;
            }

            list_push_tail(fc->tsx.friend_offline, &tsx->le);
        }
        pthread_mutex_unlock(&fc->lock);
    }
}

static
void on_friend_conn_status_change(ElaCarrier *w, const char *friendid,
                                  ElaConnectionStatus status, void *context)
{
    FeedsClient *fc = (FeedsClient *)context;
    list_iterator_t it;
    Transaction *tsx;
    int rc;

    pthread_mutex_lock(&fc->lock);
    if (status == ElaConnectionStatus_Connected) {
        list_foreach(fc->tsx.friend_offline, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;
            list_iterator_remove(&it);
            list_push_tail(fc->tsx.ongoing, &tsx->le);
        }

        list_foreach(fc->tsx.ongoing, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;

            rc = ela_send_friend_message(fc->carrier, tsx->node_id, tsx->marshalled->data,
                                         tsx->marshalled->sz, NULL);
            if (rc < 0) {
                list_iterator_remove(&it);
                /* Caution! Callback with lock held */
                tsx->user_cb(NULL, NULL, tsx->user_data);
            }
        }
    } else {
        list_foreach(fc->tsx.ongoing, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;

            deref(tsx->multi_resp_cache);
            tsx->multi_resp_cache = NULL;
        }
    }
    pthread_mutex_unlock(&fc->lock);
}

static
void on_msg(ElaCarrier *carrier, const char *from,
            const void *msg, size_t len, int64_t timestamp,
            bool offline, void *context)
{
    FeedsClient *fc = (FeedsClient *)context;
    Notif *notif;
    uint64_t id;
    int rc;

    rc = rpc_unmarshal_notif_or_resp_id(msg, len, &notif, &id);
    if (rc < 0)
        return;

    if (!notif) {
        list_iterator_t it;
        Transaction *tsx;

        pthread_mutex_lock(&fc->lock);
        list_foreach(fc->tsx.ongoing, tsx) {
            Resp *resp;
            ErrResp *err;

            if (tsx->tsx_id != id)
                continue;

            rc = tsx->unmarshal_resp(&resp, &err);
            if (rc < 0) {
                list_iterator_remove(&it);
                /* Caution! Callback with lock held */
                tsx->user_cb(NULL, NULL, tsx->user_data);
                deref(tsx);
                break;
            }

            if (tsx->hdl_multi_resp && !tsx->hdl_multi_resp(tsx, &resp, &err)) {
                deref(tsx);
                break;
            }

            /* Caution! Callback with lock held */
            tsx->user_cb(resp, err, tsx->user_data);
            deref(tsx);
            break;
        }
        pthread_mutex_unlock(&fc->lock);
    }
}

static
void *carrier_routine(void *arg)
{
    ela_run((ElaCarrier *)arg, 10);
    return NULL;
}

static
bool create_id_tsx(DIDAdapter *adapter, const char *payload, const char *memo)
{
    (void)adapter;
    (void)memo;
    (void)strdup(payload);

    return true;
}

static
void feeds_client_dtor(void *obj)
{
    FeedsClient *fc = obj;

    if (fc->store)
        DIDStore_Close(fc->store);

    pthread_mutex_destroy(&fc->lock);

    if (fc->carrier)
        ela_kill(fc->carrier);

    if (fc->access_token)
        free(fc->access_token);

    deref(fc->tsx.not_friend);
    deref(fc->tsx.friend_offline);
    deref(fc->tsx.ongoing);
}

FeedsClient *feeds_client_create(FeedsClientOpts *opts)
{
    DIDAdapter adapter = {
        .createIdTransaction = create_id_tsx
    };
    DIDDocument *doc = NULL;
    FeedsClient *fc = NULL;
    ElaCallbacks ela_cbs;
    ElaOptions ela_opts;
    char path[PATH_MAX];
    DID *did = NULL;
    int rc;

    if (!opts->data_dir || !*opts->data_dir || !opts->log_file || !*opts->log_file ||
        !opts->carrier.bootstraps_sz || !opts->carrier.bootstraps || !opts->did.passwd ||
        !*opts->did.passwd)
        goto failure;

    fc = rc_zalloc(sizeof(FeedsClient) + strlen(opts->did.passwd) + 1, feeds_client_dtor);
    if (!fc)
        goto failure;

    sprintf(path, "%s/didcache", opts->data_dir);
    rc = mkdirs(path, S_IRWXU);
    if (rc < 0)
        goto failure;
    DIDBackend_InitializeDefault(resolver, path);

    sprintf(path, "%s/didstore", opts->data_dir);
    fc->store = DIDStore_Open(path, &adapter);
    if (!fc->store)
        goto failure;

    if (!DIDStore_ContainsPrivateIdentity(fc->store)) {
        if (!opts->did.mnemo || !*opts->did.mnemo)
            goto failure;

        if (DIDStore_InitPrivateIdentity(fc->store, opts->did.passwd, opts->did.mnemo,
                                         opts->did.passphrase, "english", true))
            goto failure;
    } else if (opts->did.mnemo && *opts->did.mnemo) {
        char mnemo[ELA_MAX_MNEMONIC_LEN + 1];

        if (DIDStore_ExportMnemonic(fc->store, opts->did.passwd, mnemo, sizeof(mnemo)) < 0)
            goto failure;

        if (strcmp(mnemo, opts->did.mnemo))
            goto failure;
    }

    if (!(did = DIDStore_GetDIDByIndex(fc->store, opts->did.idx)))
        goto failure;

    DID_ToString(did, fc->did, sizeof(fc->did));
    fc->passwd = strcpy(fc->buf, opts->did.passwd);

    doc = DID_Resolve(did, false);
    if (!doc)
        goto failure;

    rc = DIDStore_StoreDID(fc->store, doc);
    if (rc)
        goto failure;

    pthread_mutex_init(&fc->lock, NULL);

    memset(&ela_opts, 0, sizeof(ela_opts));
    sprintf(path, "%s/carrier", opts->data_dir);
    ela_opts.persistent_location = path;
    ela_opts.udp_enabled         = opts->carrier.udp_enabled;
    ela_opts.log_level           = opts->log_lv;
    ela_opts.log_file            = (void *)opts->log_file;
    ela_opts.bootstraps_size     = opts->carrier.bootstraps_sz;
    ela_opts.bootstraps          = opts->carrier.bootstraps;

    memset(&ela_cbs, 0, sizeof(ela_cbs));
    ela_cbs.connection_status = on_conn_status_change;
    ela_cbs.friend_connection = on_friend_conn_status_change;
    ela_cbs.friend_message    = on_msg;

    fc->carrier = ela_new(&ela_opts, &ela_cbs, fc);
    if (!fc->carrier)
        goto failure;

    fc->tsx.not_friend = list_create(0, NULL);
    if (!fc->tsx.not_friend)
        goto failure;

    fc->tsx.friend_offline = list_create(0, NULL);
    if (!fc->tsx.friend_offline)
        goto failure;

    fc->tsx.ongoing = list_create(0, NULL);
    if (!fc->tsx.ongoing)
        goto failure;

    goto finally;

failure:
    deref(fc);
    fc = NULL;

finally:
    if (did)
        DID_Destroy(did);
    if (doc)
        DIDDocument_Destroy(doc);
    return fc;
}

void feeds_client_kill(FeedsClient *fc)
{
    deref(fc);
}

int feeds_client_run(FeedsClient *fc, int interval)
{
    if (!fc || !fc->carrier)
        return -1;

    return ela_run(fc->carrier, interval);
}

static
void tsx_dtor(void *obj)
{
    Transaction *tsx = obj;

    deref(tsx->multi_resp_cache);
    deref(tsx->marshalled);
}

static
Transaction *tsx_create(const Transaction *in)
{
    Transaction *tsx;

    tsx = rc_zalloc(sizeof(*tsx) + strlen(in->addr) + 1, tsx_dtor);
    if (!tsx)
        return NULL;

    tsx->addr           = strcpy(tsx->buf, in->addr);
    tsx->tsx_id         = in->tsx_id;
    tsx->unmarshal_resp = in->unmarshal_resp;
    tsx->hdl_multi_resp = in->hdl_multi_resp;
    tsx->user_cb        = in->user_cb;
    tsx->user_data      = in->user_data;

    tsx->le.data        = tsx;
    ela_get_id_by_address(in->addr, tsx->node_id, sizeof(tsx->node_id));
    tsx->marshalled     = in->marshal_req(in->req);

    if (!tsx->marshalled) {
        deref(tsx);
        return NULL;
    }

    return tsx;
}

static
int tsx_start(FeedsClient *fc, const Transaction *in)
{
    Transaction *tsx = NULL;
    int rc = 0;

    tsx = tsx_create(in);
    if (!tsx)
        return -1;

    pthread_mutex_lock(&fc->lock);
    if (!ela_is_friend(fc->carrier, tsx->node_id) && !ela_is_ready(fc->carrier)) {
        list_push_tail(fc->tsx.not_friend, &tsx->le);
        goto finally;
    } else if (!ela_is_friend(fc->carrier, tsx->node_id)) {
        rc = ela_add_friend(fc->carrier, in->addr, "hello");
        if (rc < 0)
            goto finally;

        list_push_tail(fc->tsx.friend_offline, &tsx->le);
        goto finally;
    } else {
        ElaFriendInfo finfo;

        rc = ela_get_friend_info(fc->carrier, tsx->node_id, &finfo);
        if (rc < 0)
            goto finally;

        if (finfo.status != ElaConnectionStatus_Connected)
            list_push_tail(fc->tsx.friend_offline, &tsx->le);
        else {
            rc = ela_send_friend_message(fc->carrier, tsx->node_id, tsx->marshalled->data,
                                         tsx->marshalled->sz, NULL);
            if (rc < 0)
                goto finally;

            list_push_tail(fc->tsx.ongoing, &tsx->le);
        }
    }

finally:
    pthread_mutex_unlock(&fc->lock);
    deref(tsx);

    return rc;
}

static
void sync_api_cb(void *resp, void *err, void *ctx)
{
    SyncAPI *api = ctx;

    pthread_mutex_lock(&api->lock);
    api->fin  = true;
    api->resp = resp;
    api->err  = err;
    pthread_mutex_unlock(&api->lock);
    pthread_cond_signal(&api->cond);
}

static
void sync_api_init(SyncAPI *api)
{
    memset(api, 0, sizeof(*api));
    pthread_mutex_init(&api->lock, NULL);
    pthread_cond_init(&api->cond, NULL);
}

static
void sync_api_deinit(SyncAPI *api)
{
    pthread_mutex_destroy(&api->lock);
    pthread_cond_destroy(&api->cond);
}

static
void wait_tsx_fin(SyncAPI *api)
{
    pthread_mutex_lock(&api->lock);
    while (!api->fin)
        pthread_cond_wait(&api->cond, &api->lock);
    pthread_mutex_unlock(&api->lock);
}

int feeds_client_decl_owner(FeedsClient *fc, const char *addr, DeclOwnerResp **resp, ErrResp **err)
{
    SyncAPI me;
    int rc;

    sync_api_init(&me);

    rc = feeds_client_decl_owner_async(fc, addr, (void *)sync_api_cb, &me);
    if (rc < 0) {
        sync_api_deinit(&me);
        return -1;
    }

    wait_tsx_fin(&me);

    *resp = me.resp;
    *err  = me.err;
    rc    = me.resp || me.err ? 0 : -1;

    sync_api_deinit(&me);

    return rc;
}

int feeds_client_decl_owner_async(FeedsClient *fc, const char *addr,
                                  void (*cb)(DeclOwnerResp *, ErrResp *, void *),
                                  void *user_data)
{
    DeclOwnerReq req = {
        .method = "declare_owner",
        .tsx_id = random(),
        .params = {
            .nonce = "abc",
            .owner_did = fc->did
        }
    };
    Transaction tsx = {
        .addr           = addr,
        .tsx_id         = req.tsx_id,
        .req            = (Req *)&req,
        .marshal_req    = (void *)rpc_marshal_decl_owner_req,
        .unmarshal_resp = (void *)rpc_unmarshal_decl_owner_resp,
        .hdl_multi_resp = NULL,
        .user_cb        = (void *)cb,
        .user_data      = user_data
    };

    if (!fc || !addr || !*addr || !ela_address_is_valid(addr) || !cb)
        return -1;

    return tsx_start(fc, &tsx);
}
