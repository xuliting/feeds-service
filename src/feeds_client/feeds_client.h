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

#ifndef __FEEDS_CLIENT_H__
#define __FEEDS_CLIENT_H__

#include <ela_carrier.h>

#include "cfg.h"
#include "../rpc.h"
#include "../obj.h"
#include "../c-vector/cvector.h"

typedef struct {
    const char *data_dir;
    const char *log_file;
    int log_lv;

    struct {
        size_t bootstraps_sz;
        BootstrapNode *bootstraps;
        bool udp_enabled;
    } carrier;

    struct {
        const char *mnemo;
        uint64_t idx;
        const char *passphrase;
        const char *passwd;
    } did;
} FeedsClientOpts;

typedef struct FeedsClient FeedsClient;

FeedsClient *feeds_client_create(FeedsClientOpts *opts);
int feeds_client_run(FeedsClient *fc, int interval);
int feeds_client_decl_owner(FeedsClient *fc, const char *addr, DeclOwnerResp **resp, ErrResp **err);
int feeds_client_decl_owner_async(FeedsClient *fc, const char *addr,
                                  void (*cb)(DeclOwnerResp *, ErrResp *, void *),
                                  void *user_data);
void feeds_client_kill(FeedsClient *fc);
/*int feeds_client_imp_did(FeedsClient *fc, const char *svc_node_id, ImpDIDResp **resp, ErrResp **err);
int feeds_client_iss_vc(FeedsClient *fc, const char *svc_node_id, const char *sub, IssVCResp **resp, ErrResp **err);
int feeds_client_signin1(FeedsClient *fc, const char *svc_node_id, SigninReqChalResp **resp, ErrResp **err);
int feeds_client_signin2(FeedsClient *fc, const char *svc_node_id,
                         const char *realm, const char *nonce, SigninConfChalResp **resp, ErrResp **err);
int feeds_client_create_channel(FeedsClient *fc, const char *svc_node_id, const char *name,
                                const char *intro, size_t avatar_sz, CreateChanResp **resp, ErrResp **err);
int feeds_client_publish_post(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                              size_t content_sz, PubPostResp **resp, ErrResp **err);
int feeds_client_post_comment(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                              uint64_t post_id, uint64_t comment_id, size_t content_sz,
                              PostCmtResp **resp, ErrResp **err);
int feeds_client_post_like(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                           uint64_t post_id, uint64_t comment_id, PostLikeResp **resp, ErrResp **err);
int feeds_client_post_unlike(FeedsClient *fc, const char *svc_node_id, uint64_t channel_id,
                             uint64_t post_id, uint64_t comment_id, PostUnlikeResp **resp, ErrResp **err);
int feeds_client_get_my_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                 uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                 GetMyChansResp **resp, ErrResp **err);
int feeds_client_get_my_channels_metadata(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                          uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                          GetMyChansMetaResp **resp, ErrResp **err);
int feeds_client_get_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                              uint64_t upper, uint64_t lower, uint64_t maxcnt,
                              GetChansResp **resp, ErrResp **err);
int feeds_client_get_channel_detail(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                    GetChanDtlResp **resp, ErrResp **err);
int feeds_client_get_subscribed_channels(FeedsClient *fc, const char *svc_node_id, QryFld qf,
                                         uint64_t upper, uint64_t lower, uint64_t maxcnt,
                                         GetSubChansResp **resp, ErrResp **err);
int feeds_client_get_posts(FeedsClient *fc, const char *svc_node_id, uint64_t cid, QryFld qf,
                           uint64_t upper, uint64_t lower, uint64_t maxcnt, GetPostsResp **resp, ErrResp **err);
int feeds_client_get_liked_posts(FeedsClient *fc, const char *svc_node_id, QryFld qf, uint64_t upper,
                                 uint64_t lower, uint64_t maxcnt, GetLikedPostsResp **resp, ErrResp **err);
int feeds_client_get_comments(FeedsClient *fc, const char *svc_node_id, uint64_t cid, uint64_t pid,
                              QryFld qf, uint64_t upper, uint64_t lower, uint64_t maxcnt,
                              GetCmtsResp **resp, ErrResp **err);
int feeds_client_get_statistics(FeedsClient *fc, const char *svc_node_id, GetStatsResp **resp, ErrResp **err);
int feeds_client_subscribe_channel(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                   SubChanResp **resp, ErrResp **err);
int feeds_client_unsubscribe_channel(FeedsClient *fc, const char *svc_node_id, uint64_t id,
                                     UnsubChanResp **resp, ErrResp **err);
int feeds_client_enable_notification(FeedsClient *fc, const char *svc_node_id,
                                     EnblNotifResp **resp, ErrResp **err);*/

#endif // __FEEDS_CLIENT_H__
