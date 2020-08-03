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
    pthread_mutex_t lock;
    pthread_cond_t cond;
    ElaCarrier *carrier;
    bool waiting_response;
    pthread_t carrier_routine_tid;
    DIDStore *store;
    char did[ELA_MAX_DID_LEN];
    char *passwd;
    char *access_token;
    void *resp;
    ErrResp *err;
    void *unmarshal;
    struct {
        list_t *not_friend;
        list_t *peer_offline;
        list_t *ongoing;
    } tsx;
    char buf[0];
};

typedef struct Transaction Transaction;
struct Transaction {
    list_entry_t le;
    const char *addr;
    char node_id[ELA_MAX_ID_LEN + 1];
    uint64_t tsx_id;
    const Req *req;
    Resp *resp;
    Marshalled *(*marshal)(const Req *);
    Marshalled *marshalled;
    int (*unmarshal)(Resp **, ErrResp **);
    bool (*resp_hdlr)(Transaction *, Resp *, ErrResp *);
    void (*user_cb)(Resp **, ErrResp **, void *);
    void *user_data;
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
void on_conn(ElaCarrier *w, ElaConnectionStatus status, void *context)
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

            list_push_tail(fc->tsx.peer_offline, &tsx->le);
        }
        pthread_mutex_unlock(&fc->lock);
    }
}

static
void on_friend_conn(ElaCarrier *w, const char *friendid,
                    ElaConnectionStatus status, void *context)
{
    FeedsClient *fc = (FeedsClient *)context;
    list_iterator_t it;
    Transaction *tsx;
    int rc;

    pthread_mutex_lock(&fc->lock);

    if (status == ElaConnectionStatus_Connected) {

        list_foreach(fc->tsx.peer_offline, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;

            list_iterator_remove(&it);

            rc = ela_send_friend_message(fc->carrier, tsx->node_id,
                                         tsx->marshalled->data, tsx->marshalled->sz, NULL);
            if (rc < 0) {
                tsx->user_cb(NULL, NULL, tsx->user_data);
                continue;
            }

            list_push_tail(fc->tsx.ongoing, &tsx->le);
        }

        list_foreach(fc->tsx.ongoing, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;

            rc = ela_send_friend_message(fc->carrier, tsx->node_id, tsx->marshalled->data,
                                         tsx->marshalled->sz, NULL);
            if (rc < 0) {
                list_iterator_remove(&it);
                tsx->user_cb(NULL, NULL, tsx->user_data);
            }
        }
    } else {
        list_foreach(fc->tsx.ongoing, tsx) {
            if (strcmp(tsx->node_id, friendid))
                continue;

            deref(tsx->resp);
            tsx->resp = NULL;
        }
    }

    pthread_mutex_unlock(&fc->lock);
}

static
void console(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
}

static
void on_msg(ElaCarrier *carrier, const char *from,
            const void *msg, size_t len, int64_t timestamp,
            bool offline, void *context)
{
    FeedsClient *fc = (FeedsClient *)context;
    int (*unmarshal)(void **, ErrResp **) = fc->unmarshal;
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

            rc = tsx->unmarshal(&resp, &err);
            if (rc < 0) {
                list_iterator_remove(&it);
                tsx->user_cb(NULL, NULL, tsx->user_data);
                continue;
            }

            if (tsx->resp_hdlr && !tsx->resp_hdlr(tsx, resp, err))
                continue;


        }

        pthread_mutex_unlock(&fc->lock);
    }

    if (notif) {
        if (!strcmp(notif->method, "new_post")) {
            NewPostNotif *n = (NewPostNotif *)notif;
            console("New post:");
            console("  channel_id: %llu, id: %llu, content_length: %" PRIu64 ", created_at: %llu",
                    n->params.pinfo->chan_id, n->params.pinfo->post_id,
                    n->params.pinfo->len, n->params.pinfo->created_at);
        } else if (!strcmp(notif->method, "new_comment")) {
            NewCmtNotif *n = (NewCmtNotif *)notif;
            console("New comment:");
            console("  channel_id: %llu, post_id: %llu, id: %llu, comment_id: %llu, "
                    "user_name: %s, content_length: %" PRIu64 ", created_at: %llu",
                    n->params.cinfo->chan_id,
                    n->params.cinfo->post_id,
                    n->params.cinfo->cmt_id,
                    n->params.cinfo->reply_to_cmt,
                    n->params.cinfo->user.name,
                    n->params.cinfo->len,
                    n->params.cinfo->created_at);
        } else if (!strcmp(notif->method, "new_like")) {
            NewLikeNotif *n = (NewLikeNotif *)notif;
            console("New like:");
            console("  channel_id: %" PRIu64 ", post_id: %" PRIu64 ", comment_id: %" PRIu64
                    ", user_name: %s, user_did: %s, count: %" PRIu64,
                    n->params.li->chan_id, n->params.li->post_id, n->params.li->cmt_id,
                    n->params.li->user.name, n->params.li->user.did, n->params.li->total_cnt);
        } else {
            NewSubNotif *n = (NewSubNotif *)notif;
            console("New subscription:");
            console("  channel_id: %" PRIu64 ", user_name: %s, user_did: %s",
                    n->params.chan_id, n->params.uinfo->name, n->params.uinfo->did);
        }
        deref(notif);
        return;
    }

    pthread_mutex_lock(&fc->lock);
    if (fc->waiting_response) {
        while (fc->resp || fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        if (!unmarshal(&fc->resp, &fc->err))
            pthread_cond_signal(&fc->cond);
    }
    pthread_mutex_unlock(&fc->lock);
}

static
void *carrier_routine(void *arg)
{
    ela_run((ElaCarrier *)arg, 10);
    return NULL;
}

static
void feeds_client_dtor(void *obj)
{
    FeedsClient *fc = (FeedsClient *)obj;

    pthread_mutex_destroy(&fc->lock);
    pthread_cond_destroy(&fc->cond);

    if (fc->carrier)
        ela_kill(fc->carrier);

    if (fc->store)
        DIDStore_Close(fc->store);

    if (fc->passwd)
        free(fc->passwd);

    if (fc->access_token)
        free(fc->access_token);

    deref(fc->resp);
    deref(fc->err);
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
DIDDocument* merge_to_localcopy(DIDDocument *chaincopy, DIDDocument *localcopy)
{
    if (!chaincopy && !localcopy)
        return NULL;

    if (!chaincopy)
        return chaincopy;

    return localcopy;
}

FeedsClient *feeds_client_create(FeedsClientOpts *opts)
{
    DIDAdapter adapter = {
        .createIdTransaction = create_id_tsx
    };
    ElaCallbacks ela_cbs;
    char path[PATH_MAX];
    DIDDocument *doc;
    FeedsClient *fc;
    DID *did;
    int rc;

    if (!opts->data_dir || !*opts->data_dir || !opts->log_file || !*opts->log_file ||
        !opts->carrier.bootstraps_sz || !opts->carrier.bootstraps || !opts->did.passwd ||
        !*opts->did.passwd)
        return NULL;

    fc = rc_zalloc(sizeof(FeedsClient) + strlen(opts->did.passwd) + 1, feeds_client_dtor);
    if (!fc)
        return NULL;

    sprintf(path, "%s/didcache", opts->data_dir);
    rc = mkdirs(path, S_IRWXU);
    if (rc < 0)
        return NULL;
    DIDBackend_InitializeDefault(resolver, path);

    sprintf(path, "%s/didstore", opts->data_dir);
    fc->store = DIDStore_Open(path, &adapter);
    if (!fc->store) {
        deref(fc);
        return NULL;
    }

    if (!DIDStore_ContainsPrivateIdentity(fc->store)) {
        if (!opts->did.mnemo || !*opts->did.mnemo) {
            deref(fc);
            return NULL;
        }

        if (DIDStore_InitPrivateIdentity(fc->store, opts->did.passwd, opts->did.mnemo,
                                         opts->did.passphrase, "english", true)) {
            deref(fc);
            return NULL;
        }
    } else if (opts->did.mnemo && *opts->did.mnemo) {
        char mnemo[ELA_MAX_MNEMONIC_LEN + 1];

        if (DIDStore_ExportMnemonic(fc->store, opts->did.passwd, mnemo, sizeof(mnemo))) {
            deref(fc);
            return NULL;
        }

        if (strcmp(mnemo, opts->did.mnemo)) {
            deref(fc);
            return NULL;
        }
    }

    if (!(did = DIDStore_GetDIDByIndex(fc->store, opts->did.idx))) {
        deref(fc);
        return NULL;
    }

    DID_ToString(did, fc->did, sizeof(fc->did));
    fc->passwd = strcpy(fc->buf, opts->did.passwd);

    doc = DID_Resolve(did, false);
    DID_Destroy(did);
    if (!doc) {
        deref(fc);
        return NULL;
    }

    rc = DIDStore_StoreDID(fc->store, doc);
    DIDDocument_Destroy(doc);
    if (rc) {
        deref(fc);
        return NULL;
    }

    memset(&ela_cbs, 0, sizeof(ela_cbs));
    ela_cbs.connection_status = on_conn;
    ela_cbs.friend_connection = on_friend_conn;
    ela_cbs.friend_message = on_msg;


    pthread_mutex_init(&fc->lock, NULL);
    pthread_cond_init(&fc->cond, NULL);

    fc->carrier = ela_new(&opts->carrier_opts, &ela_cbs, fc);
    if (!fc->carrier) {
        deref(fc);
        return NULL;
    }

    rc = pthread_create(&fc->carrier_routine_tid, NULL, carrier_routine, fc->carrier);
    if (rc < 0) {
        deref(fc);
        return NULL;
    }

    return fc;
}

void feeds_client_wait_online(FeedsClient *fc)
{
    pthread_mutex_lock(&fc->lock);
    while (!ela_is_ready(fc->carrier))
        pthread_cond_wait(&fc->cond, &fc->lock);
    pthread_mutex_unlock(&fc->lock);
}

void feeds_client_delete(FeedsClient *fc)
{
    pthread_t tid = fc->carrier_routine_tid;

    deref(fc);
    pthread_join(tid, NULL);
}

ElaCarrier *feeds_client_get_carrier(FeedsClient *fc)
{
    return fc->carrier;
}

int feeds_client_friend_add(FeedsClient *fc, const char *address, const char *hello)
{
    char node_id[ELA_MAX_ID_LEN + 1];
    int rc = 0;

    ela_get_id_by_address(address, node_id, sizeof(node_id));
    if (!ela_is_friend(fc->carrier, node_id)) {
        rc = ela_add_friend(fc->carrier, address, hello);
        if (rc < 0)
            return -1;
    }

    feeds_client_wait_until_friend_connected(fc, node_id);

    return rc;
}

int feeds_client_wait_until_friend_connected(FeedsClient *fc, const char *friend_node_id)
{
    ElaFriendInfo info;
    int rc;

    pthread_mutex_lock(&fc->lock);
    while (1) {
        rc = ela_get_friend_info(fc->carrier, friend_node_id, &info);
        if (rc < 0)
            break;

        if (info.status != ElaConnectionStatus_Connected)
            pthread_cond_wait(&fc->cond, &fc->lock);
        else
            break;
    }
    pthread_mutex_unlock(&fc->lock);

    return rc;
}

int feeds_client_friend_remove(FeedsClient *fc, const char *user_id)
{
    return ela_remove_friend(fc->carrier, user_id);
}

static
void sync_api_cb(void *resp, void *err, void *ctx)
{
    SyncAPI *api = ctx;

    pthread_mutex_lock(&api->lock);
    api->fin = true;
    api->resp = resp;
    api->err = err;
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
    *err = me.err;
    rc = me.resp || me.err ? 0 : -1;

    sync_api_deinit(&me);

    return rc;
}

static
void tsx_dtor(void *obj)
{

}

static
Transaction *tsx_create(const Transaction *in)
{
    Transaction *tsx;

    tsx = rc_zalloc(sizeof(*tsx) + strlen(in->addr) + 1, tsx_dtor);
    if (!tsx)
        return NULL;

    tsx->le.data    = tsx;
    tsx->addr       = strcpy(tsx->buf, in->addr);
    tsx->tsx_id     = in->tsx_id;
    tsx->marshalled = in->marshal(in->req);
    tsx->unmarshal  = tsx->unmarshal;
    tsx->resp_hdlr  = tsx->resp_hdlr;
    tsx->user_cb    = tsx->user_cb;
    tsx->user_data  = tsx->user_data;

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

        list_push_tail(fc->tsx.peer_offline, &tsx->le);
        goto finally;
    } else {
        ElaFriendInfo finfo;

        rc = ela_get_friend_info(fc->carrier, tsx->node_id, &finfo);
        if (rc < 0)
            goto finally;

        if (finfo.status != ElaConnectionStatus_Connected) {
            list_push_tail(fc->tsx.peer_offline, &tsx->le);
        } else {
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

int feeds_client_decl_owner_async(FeedsClient *fc, const char *addr,
                                  void (*cb)(DeclOwnerResp *, ErrResp *, void *),
                                  void *ctx)
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
        .addr      = addr,
        .tsx_id    = req.tsx_id,
        .req       = (Req *)&req,
        .marshal   = rpc_marshal_decl_owner_req,
        .unmarshal = rpc_unmarshal_decl_owner_resp,
        .resp_hdlr = NULL,
        .user_cb   = cb,
        .user_data = ctx
    };

    return tsx_start(fc, &tsx);
}

int feeds_client_imp_did(FeedsClient *fc, const char *svc_node_id, ImpDIDResp **resp, ErrResp **err)
{
    ImpDIDReq req = {
        .method = "import_did",
        .tsx_id = 1,
        .params = {
            .mnemo = (char *)mnemonic,
            .passphrase = "secret",
            .idx = 0
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_imp_did_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_imp_did_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

static
char *iss_vc(FeedsClient *fc, const char *sub)
{
    DID *did = DID_FromString(fc->did);
    Issuer *iss = Issuer_Create(did, NULL, fc->store);
    DID *owner = DID_FromString(sub);
    DIDURL *url = DIDURL_NewByDid(owner, "credential");
    const char *types[] = {"BasicProfileCredential"};
    const char *propdata = "{\"name\":\"Jay Holtslander\"}";
    Credential *vc = Issuer_CreateCredentialByString(iss, owner, url, types, 1, propdata,
                                         time(NULL) + 3600 * 24 * 365 * 20, fc->passwd);
    char *vc_str = (char *)Credential_ToJson(vc, true);
    DID_Destroy(did);
    Issuer_Destroy(iss);
    DID_Destroy(owner);
    DIDURL_Destroy(url);
    Credential_Destroy(vc);

    return vc_str;
}

int feeds_client_iss_vc(FeedsClient *fc, const char *svc_node_id, const char *sub,
                        IssVCResp **resp, ErrResp **err)
{
    IssVCReq req = {
        .method = "issue_credential",
        .tsx_id = 1,
        .params = {
            .vc = iss_vc(fc, sub)
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_iss_vc_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_iss_vc_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_signin1(FeedsClient *fc, const char *svc_node_id, SigninReqChalResp **resp, ErrResp **err)
{
    SigninReqChalReq req = {
        .method = "signin_request_challenge",
        .tsx_id = 1,
        .params = {
            .iss = fc->did,
            .vc_req = true
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_signin_req_chal_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_signin_req_chal_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

static
char *gen_jws(FeedsClient *fc, const char *realm, const char *nonce)
{
    DID *did = DID_FromString(fc->did);
    Presentation *vp = Presentation_Create(did, NULL, fc->store, fc->passwd, nonce, realm, 0);
    DIDDocument *doc = DIDStore_LoadDID(fc->store, did);
    JWTBuilder *b = DIDDocument_GetJwtBuilder(doc);
    JWTBuilder_SetClaimWithJson(b, "presentation", Presentation_ToJson(vp, true));
    JWTBuilder_Sign(b, NULL, fc->passwd);
    char *str = (char *)JWTBuilder_Compact(b);
    JWTBuilder_Destroy(b);
    Presentation_Destroy(vp);

    return str;
}

static
char *gen_vc(FeedsClient *fc)
{
    DID *did = DID_FromString(fc->did);
    Issuer *iss = Issuer_Create(did, NULL, fc->store);
    DIDURL *url = DIDURL_NewByDid(did, "credential");
    const char *types[] = {"BasicProfileCredential"};
    const char *propdata = "{\"name\":\"Jay Holtslander\",\"email\":\"djd@aaa.com\"}";
    Credential *vc = Issuer_CreateCredentialByString(iss, did, url, types, 1, propdata,
                                                     time(NULL) + 3600 * 24 * 365 * 20, fc->passwd);
    char *vc_str = (char *)Credential_ToJson(vc, true);
    DID_Destroy(did);
    Issuer_Destroy(iss);
    DIDURL_Destroy(url);
    Credential_Destroy(vc);

    return vc_str;
}

int feeds_client_signin2(FeedsClient *fc, const char *svc_node_id,
                         const char *realm, const char *nonce, SigninConfChalResp **resp, ErrResp **err)
{
    SigninConfChalReq req = {
        .method = "signin_confirm_challenge",
        .tsx_id = 1,
        .params = {
            .jws = gen_jws(fc, realm, nonce),
            .vc = gen_vc(fc)
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_signin_conf_chal_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_signin_conf_chal_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    if (!rc)
        fc->access_token = strdup((*resp)->result.tk);
    deref(marshal);
    return rc;
}

int feeds_client_create_channel(FeedsClient *fc, const char *svc_node_id, const char *name,
                                const char *intro, size_t avatar_sz, CreateChanResp **resp, ErrResp **err)
{
    CreateChanReq req = {
        .method = "create_channel",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .name = (char *)name,
            .intro = (char *)intro,
            .avatar = malloc(avatar_sz * 1024 * 1024),
            .sz = avatar_sz * 1024 * 1024
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_create_chan_req(&req);
    free(req.params.avatar);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_create_chan_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_publish_post(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                              size_t content_sz, PubPostResp **resp, ErrResp **err)
{
    PubPostReq req = {
        .method = "publish_post",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = channel_id,
            .content = malloc(content_sz * 1024 * 1024),
            .sz = content_sz * 1024 * 1024
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_pub_post_req(&req);
    free(req.params.content);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_pub_post_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_post_comment(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                              uint64_t post_id, uint64_t comment_id, size_t content_sz,
                              PostCmtResp **resp, ErrResp **err)
{
    PostCmtReq req = {
        .method = "post_comment",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = channel_id,
            .post_id = post_id,
            .cmt_id = comment_id,
            .content = malloc(content_sz * 1024 * 1024),
            .sz = content_sz * 1024 * 1024
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_post_cmt_req(&req);
    free(req.params.content);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_post_cmt_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_post_like(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                           uint64_t post_id, uint64_t comment_id, PostLikeResp **resp, ErrResp **err)
{
    PostLikeReq req = {
        .method = "post_like",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = channel_id,
            .post_id = post_id,
            .cmt_id = comment_id
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_post_like_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_post_like_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_post_unlike(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                             uint64_t post_id, uint64_t comment_id, PostUnlikeResp **resp, ErrResp **err)
{
    PostUnlikeReq req = {
        .method = "post_unlike",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = channel_id,
            .post_id = post_id,
            .cmt_id = comment_id
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_post_unlike_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_post_unlike_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_get_my_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                 uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                 GetMyChansResp **resp, ErrResp **err)
{
    GetMyChansReq req = {
        .method = "get_my_channels",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetMyChansResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_my_chans_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_my_chans_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetMyChansResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                ChanInfo **ci;
                cvector_foreach(r->result.cinfos, ci)
                    cvector_push_back(resp_tmp->result.cinfos, ref(*ci));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_my_channels_metadata(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                          uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                          GetMyChansMetaResp **resp, ErrResp **err)
{
    GetMyChansMetaReq req = {
        .method = "get_my_channels_metadata",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_my_chans_meta_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_get_my_chans_meta_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_get_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                              uint64_t upper, uint64_t lower, uint64_t maxcnt,
                              GetChansResp **resp, ErrResp **err)
{
    GetChansReq req = {
        .method = "get_channels",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetChansResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_chans_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_chans_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetChansResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                ChanInfo **ci;
                cvector_foreach(r->result.cinfos, ci)
                    cvector_push_back(resp_tmp->result.cinfos, ref(*ci));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_channel_detail(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                    GetChanDtlResp **resp, ErrResp **err)
{
    GetChanDtlReq req = {
        .method = "get_channel_detail",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .id = id
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_chan_dtl_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_get_chan_dtl_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_get_subscribed_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                         uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                         GetSubChansResp **resp, ErrResp **err)
{
    GetSubChansReq req = {
        .method = "get_subscribed_channels",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetSubChansResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_sub_chans_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_sub_chans_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetSubChansResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                ChanInfo **ci;
                cvector_foreach(r->result.cinfos, ci)
                    cvector_push_back(resp_tmp->result.cinfos, ref(*ci));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_posts(FeedsClient *fc, const char *svc_node_id, uint64_t cid, QryFld qf,
                           uint64_t upper, uint64_t lower, uint64_t maxcnt, GetPostsResp **resp, ErrResp **err)
{
    GetPostsReq req = {
        .method = "get_posts",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = cid,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetPostsResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_posts_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_posts_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetPostsResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                PostInfo **pi;
                cvector_foreach(r->result.pinfos, pi)
                    cvector_push_back(resp_tmp->result.pinfos, ref(*pi));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_liked_posts(FeedsClient *fc, const char *svc_node_id, QryFld qf, uint64_t upper,
                                 uint64_t lower, uint64_t maxcnt, GetLikedPostsResp **resp, ErrResp **err)
{
    GetLikedPostsReq req = {
        .method = "get_liked_posts",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetLikedPostsResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_liked_posts_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_liked_posts_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetLikedPostsResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                PostInfo **pi;
                cvector_foreach(r->result.pinfos, pi)
                    cvector_push_back(resp_tmp->result.pinfos, ref(*pi));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_comments(FeedsClient *fc, const char *svc_node_id, uint64_t cid, uint64_t pid,
                              QryFld qf, uint64_t upper, uint64_t lower, uint64_t maxcnt,
                              GetCmtsResp **resp, ErrResp **err)
{
    GetCmtsReq req = {
        .method = "get_comments",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .chan_id = cid,
            .post_id = pid,
            .qc = {
                .by = qf,
                .upper = upper,
                .lower = lower,
                .maxcnt = maxcnt
            }
        }
    };
    GetCmtsResp *resp_tmp = NULL;
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_cmts_req(&req);
    if (!marshal)
        return -1;

    pthread_mutex_lock(&fc->lock);
    fc->waiting_response = true;
    fc->unmarshal = rpc_unmarshal_get_cmts_resp;

    rc = ela_send_friend_message(fc->carrier, svc_node_id, marshal->data, marshal->sz, NULL);
    deref(marshal);
    if (rc < 0) {
        fc->unmarshal = NULL;
        fc->waiting_response = false;
        pthread_mutex_unlock(&fc->lock);
        return -1;
    }

    while (1) {
        while (!fc->resp && !fc->err)
            pthread_cond_wait(&fc->cond, &fc->lock);
        pthread_cond_signal(&fc->cond);

        if (fc->resp) {
            GetCmtsResp *r = fc->resp;
            fc->resp = NULL;

            if (!resp_tmp)
                resp_tmp = ref(r);
            else {
                CmtInfo **ci;
                cvector_foreach(r->result.cinfos, ci)
                    cvector_push_back(resp_tmp->result.cinfos, ref(*ci));
            }

            if (!r->result.is_last) {
                deref(r);
                continue;
            }

            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            deref(r);
            *resp = resp_tmp;

            return 0;
        } else {
            *err = fc->err;

            fc->err = NULL;
            fc->unmarshal = NULL;
            fc->waiting_response = false;

            pthread_mutex_unlock(&fc->lock);

            return 0;
        }
    }
}

int feeds_client_get_statistics(FeedsClient *fc, const char *svc_node_id, GetStatsResp **resp, ErrResp **err)
{
    GetStatsReq req = {
        .method = "get_statistics",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_get_stats_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_get_stats_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_subscribe_channel(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                   SubChanResp **resp, ErrResp **err)
{
    SubChanReq req = {
        .method = "subscribe_channel",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .id = id
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_sub_chan_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_sub_chan_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_unsubscribe_channel(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                     UnsubChanResp **resp, ErrResp **err)
{
    UnsubChanReq req = {
        .method = "unsubscribe_channel",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
            .id = id
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_unsub_chan_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_unsub_chan_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}

int feeds_client_enable_notification(FeedsClient *fc, const char *svc_node_id,
                                     EnblNotifResp **resp, ErrResp **err)
{
    EnblNotifReq req = {
        .method = "enable_notification",
        .tsx_id = 1,
        .params = {
            .tk = fc->access_token,
        }
    };
    Marshalled *marshal;
    int rc;

    marshal = rpc_marshal_enbl_notif_req(&req);
    if (!marshal)
        return -1;

    fc->unmarshal = rpc_unmarshal_enbl_notif_resp;
    rc = transaction_start(fc, svc_node_id, marshal->data, marshal->sz, (void **)resp, err);
    deref(marshal);
    return rc;
}
