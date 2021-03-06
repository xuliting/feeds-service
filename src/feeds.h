/*
 * Copyright (c) 2020 trinity-tech
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

#ifndef __FEEDS_H__
#define __FEEDS_H__

#include <stddef.h>

#include <ela_carrier.h>

#include "cfg.h"
#include "rpc.h"

int feeds_init(FeedsConfig *cfg);
void feeds_deinit();
void feeds_deactivate_suber(const char *node_id);
void hdl_create_chan_req(ElaCarrier *c, const char *from, Req *base);
void hdl_upd_chan_req(ElaCarrier *c, const char *from, Req *base);
void hdl_pub_post_req(ElaCarrier *c, const char *from, Req *base);
void hdl_post_cmt_req(ElaCarrier *c, const char *from, Req *base);
void hdl_post_like_req(ElaCarrier *c, const char *from, Req *base);
void hdl_post_unlike_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_my_chans_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_my_chans_meta_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_chans_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_chan_dtl_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_sub_chans_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_posts_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_liked_posts_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_cmts_req(ElaCarrier *c, const char *from, Req *base);
void hdl_get_stats_req(ElaCarrier *c, const char *from, Req *base);
void hdl_sub_chan_req(ElaCarrier *c, const char *from, Req *base);
void hdl_unsub_chan_req(ElaCarrier *c, const char *from, Req *base);
void hdl_enbl_notif_req(ElaCarrier *c, const char *from, Req *base);
void hdl_unknown_req(ElaCarrier *c, const char *from, Req *base);

#endif //__FEEDS_H__
