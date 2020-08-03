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

#include <limits.h>
#include <unistd.h>

#include <CUnit/Basic.h>
#include <ela_carrier.h>
#include <crystal.h>
#include <cfg.h>

#include "case.h"
#include "../../../src/feeds_client/feeds_client.h"

typedef struct {
    FeedsClient *fc;
    pthread_t tid;
} TestsFeedsClient;

static char data_dir[PATH_MAX];
static TestsFeedsClient owner1;
static TestsFeedsClient owner2;
static TestsFeedsClient follower;
static const char *mnemo = "advance duty suspect finish space matter squeeze elephant twenty over stick shield";
static SOCKET msg_sock = INVALID_SOCKET;
static char feedsd_addr[ELA_MAX_ADDRESS_LEN + 1];
static char feedsd_node_id[ELA_MAX_ID_LEN + 1];
static char *topic_name = "topic";
static char topic_desc[ELA_MAX_APP_MESSAGE_LEN * 2 + 1];
static char *non_existent_topic_name = "non-existant-topic";

static
void create_topic_without_permission(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_create_channel(follower, feedsd_node_id,
                                     topic_name, topic_desc, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of creating topic without permission");

    if (resp)
        cJSON_Delete(resp);
}

static
void create_topic(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_create_channel(owner1, feedsd_node_id,
                                     topic_name, topic_desc, &resp);
    if (rc < 0)
        CU_FAIL("wrong result of creating topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void create_existing_topic(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_create_channel(owner1, feedsd_node_id,
                                     topic_name, topic_desc, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of creating existing topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void post_event_without_permission(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_publish_post(follower, feedsd_node_id,
                                   topic_name, "event1", &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of posting event without permission");

    if (resp)
        cJSON_Delete(resp);
}

static
void post_event_on_non_existent_topic(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_publish_post(owner1, feedsd_node_id,
                                   non_existent_topic_name, "event1", &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of posting non-existent event");

    if (resp)
        cJSON_Delete(resp);
}

static
void post_event_when_no_subscribers(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_publish_post(owner1, feedsd_node_id,
                                   topic_name, "event1", &resp);
    if (rc < 0) {
        CU_FAIL("wrong result of posting event when there is no subscribers");
        goto finally;
    }
    cJSON_Delete(resp);

    sleep(2);
    resp = feeds_client_get_new_posts(follower);
    if (resp)
        CU_FAIL("got new event(s) when there should not be");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void post_event_when_subscriber_exists(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_publish_post(owner1, feedsd_node_id,
                                   topic_name, "event2", &resp);
    if (rc < 0) {
        CU_FAIL("wrong result of posting event when only subscriber exists");
        goto finally;
    }
    cJSON_Delete(resp);

    sleep(2);
    resp = feeds_client_get_new_posts(follower);
    if (resp)
        CU_FAIL("got new event(s) when there should not be");

finally:
    if (resp)
        cJSON_Delete(resp);

}

static
void post_event_when_active_subscriber_exists(void)
{
    cJSON *event;
    cJSON *resp;
    int rc;

    rc = feeds_client_publish_post(owner1, feedsd_node_id,
                                   topic_name, "event3", &resp);
    if (rc < 0) {
        CU_FAIL("wrong result of posting event when there is an active subscriber");
        goto finally;
    }
    cJSON_Delete(resp);

    sleep(2);
    resp = feeds_client_get_new_posts(follower);
    if (!resp || cJSON_GetArraySize(resp) != 1) {
        CU_FAIL("received incorrect number of new events");
        goto finally;
    }

    event = cJSON_GetArrayItem(resp, 0);
    if (strcmp(cJSON_GetObjectItemCaseSensitive(event, "topic")->valuestring, topic_name) ||
        strcmp(cJSON_GetObjectItemCaseSensitive(event, "event")->valuestring, "event3") ||
        (size_t)cJSON_GetObjectItemCaseSensitive(event, "seqno")->valuedouble != 3)
        CU_FAIL("wrong event info");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void list_owned_topics_when_no_topics(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_get_my_channels(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp))) {
        CU_FAIL("wrong result of listing owned topics when there is no topics");
        goto finally;
    }
    cJSON_Delete(resp);


    rc = feeds_client_get_my_channels(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp)))
        CU_FAIL("wrong result of listing owned topics when there is no topics");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void list_owned_topics_when_topic_exists(void)
{
    const cJSON *res;
    cJSON *topic;
    cJSON *name;
    cJSON *desc;
    cJSON *resp;
    int rc;

    rc = feeds_client_get_my_channels(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(res = jsonrpc_get_result(resp)) != 1) {
        CU_FAIL("wrong result of listing owned topics");
        goto finally;
    }

    topic = cJSON_GetArrayItem(res, 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc)) {
        CU_FAIL("wrong list owned topics response");
        goto finally;
    }
    cJSON_Delete(resp);

    rc = feeds_client_get_my_channels(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp)))
        CU_FAIL("wrong result of listing owned topics");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void list_owned_topics_batch_when_topic_exists(void)
{
    JsonRPCBatchInfo *bi;
    JsonRPCType type;
    const cJSON *res;
    cJSON *topic;
    cJSON *name;
    cJSON *desc;
    cJSON *resp;
    cJSON *req1;
    cJSON *req2;
    cJSON *batch;
    char *batch_str;
    int rc;

    req1 = jsonrpc_encode_request("get_my_channels", NULL, NULL);
    if (!req1) {
        CU_FAIL("can not create request");
        return;
    }

    req2 = jsonrpc_encode_request("get_my_channels", NULL, NULL);
    if (!req2) {
        CU_FAIL("can not create request");
        cJSON_Delete(req1);
        return;
    }

    batch = jsonrpc_encode_empty_batch();
    if (!batch) {
        CU_FAIL("can not create batch");
        cJSON_Delete(req1);
        cJSON_Delete(req2);
        return;
    }

    jsonrpc_add_object_to_batch(batch, req1);
    jsonrpc_add_object_to_batch(batch, req2);
    batch_str = cJSON_PrintUnformatted(batch);
    cJSON_Delete(batch);
    if (!batch_str) {
        CU_FAIL("can not create batch string");
        return;
    }

    rc = transaction_start(owner1, feedsd_node_id, batch_str, strlen(batch_str) + 1,
                           &resp, &type, &bi);
    free(batch_str);
    if (rc < 0) {
        CU_FAIL("send request failure");
        return;
    }

    if (type != JSONRPC_TYPE_BATCH || bi->nobjs != 2 ||
        bi->obj_types[0] != JSONRPC_TYPE_SUCCESS_RESPONSE ||
        bi->obj_types[1] != JSONRPC_TYPE_SUCCESS_RESPONSE) {
        CU_FAIL("invalid response");
        goto finally;
    }

    res = jsonrpc_batch_get_object(resp, 0);
    topic = cJSON_GetArrayItem(cJSON_GetObjectItem(res, "result"), 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc)) {
        CU_FAIL("wrong list owned topics response");
        goto finally;
    }

    res = jsonrpc_batch_get_object(resp, 1);
    topic = cJSON_GetArrayItem(cJSON_GetObjectItem(res, "result"), 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc))
        CU_FAIL("wrong list owned topics response");

finally:
    if (resp)
        cJSON_Delete(resp);
    if (bi)
        free(bi);
}

static
void subscribe_non_existent_topic(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_subscribe_channel(follower, feedsd_node_id, non_existent_topic_name, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of subscribing non-existent topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void subscribe(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_subscribe_channel(follower, feedsd_node_id, topic_name, &resp);
    if (rc < 0)
        CU_FAIL("wrong result of subscribing topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void subscribe_when_already_subscribed(void)
{
    subscribe();
}

static
void unsubscribe_non_existent_topic(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_unsubscribe_channel(follower, feedsd_node_id, non_existent_topic_name, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of unsubscribing non-existent topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void unsubscribe(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_unsubscribe_channel(follower, feedsd_node_id, topic_name, &resp);
    if (rc < 0)
        CU_FAIL("wrong result of unsubscribing topic");

    if (resp)
        cJSON_Delete(resp);
}

static
void unsubscribe_without_subscription(void)
{
    unsubscribe();
}

static
void explore_topics_when_no_topics(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_explore_topics(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp))) {
        CU_FAIL("wrong result of exploring topics when there is no topic");
        goto finally;
    }
    cJSON_Delete(resp);


    rc = feeds_client_explore_topics(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp)))
        CU_FAIL("wrong result of exploring topics when there is no topic");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void explore_topics_when_topic_exists(void)
{
    const cJSON *res;
    cJSON *topic;
    cJSON *name;
    cJSON *desc;
    cJSON *resp;
    int rc;

    rc = feeds_client_explore_topics(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(res = jsonrpc_get_result(resp)) != 1) {
        CU_FAIL("wrong result of exploring topics");
        goto finally;
    }

    topic = cJSON_GetArrayItem(res, 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc)) {
        CU_FAIL("wrong topic info");
        goto finally;
    }
    cJSON_Delete(resp);

    rc = feeds_client_explore_topics(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(res = jsonrpc_get_result(resp)) != 1) {
        CU_FAIL("wrong result of exploring topics");
        goto finally;
    }

    topic = cJSON_GetArrayItem(res, 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc))
        CU_FAIL("wrong topic info");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void list_subscribed_topics_before_subscription(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_list_subscribed(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp))) {
        CU_FAIL("wrong result of listing subscribed topics");
        goto finally;
    }
    cJSON_Delete(resp);


    rc = feeds_client_list_subscribed(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp)))
        CU_FAIL("wrong result of listing subscribed topics");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void list_subscribed_topics_after_subscription(void)
{
    const cJSON *res;
    cJSON *topic;
    cJSON *name;
    cJSON *desc;
    cJSON *resp;
    int rc;

    rc = feeds_client_list_subscribed(owner1, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(jsonrpc_get_result(resp))) {
        CU_FAIL("wrong result of listing subscribed topics");
        goto finally;
    }
    cJSON_Delete(resp);

    rc = feeds_client_list_subscribed(follower, feedsd_node_id, &resp);
    if (rc < 0 || cJSON_GetArraySize(res = jsonrpc_get_result(resp)) != 1) {
        CU_FAIL("wrong result of listing subscribed topics");
        goto finally;
    }

    topic = cJSON_GetArrayItem(res, 0);
    name = cJSON_GetObjectItemCaseSensitive(topic, "name");
    desc = cJSON_GetObjectItemCaseSensitive(topic, "desc");
    if (strcmp(name->valuestring, topic_name) || strcmp(desc->valuestring, topic_desc))
        CU_FAIL("wrong list owned topics response");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void fetch_unreceived_without_subscription(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_fetch_unreceived(follower, feedsd_node_id, topic_name, 1, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of fetching unreceived without subscription");

    if (resp)
        cJSON_Delete(resp);
}

static
void fetch_unreceived(void)
{
    const cJSON *res;
    cJSON *event;
    cJSON *resp;
    int rc;

    rc = feeds_client_fetch_unreceived(follower, feedsd_node_id, topic_name, 1, &resp);
    if (rc < 0 || cJSON_GetArraySize(res = jsonrpc_get_result(resp)) != 2)
        CU_FAIL("wrong result of fetching unreceived");

    event = cJSON_GetArrayItem(res, 0);
    if (strcmp(cJSON_GetObjectItemCaseSensitive(event, "event")->valuestring, "event1") ||
        (size_t)cJSON_GetObjectItemCaseSensitive(event, "seqno")->valuedouble != 1) {
        CU_FAIL("wrong event info");
        goto finally;
    }

    event = cJSON_GetArrayItem(res, 1);
    if (strcmp(cJSON_GetObjectItemCaseSensitive(event, "event")->valuestring, "event2") ||
        (size_t)cJSON_GetObjectItemCaseSensitive(event, "seqno")->valuedouble != 2)
        CU_FAIL("wrong event info");

finally:
    if (resp)
        cJSON_Delete(resp);
}

static
void fetch_unreceived_when_already_active(void)
{
    cJSON *resp;
    int rc;

    rc = feeds_client_fetch_unreceived(follower, feedsd_node_id, topic_name, 1, &resp);
    if (!rc || !resp)
        CU_FAIL("wrong result of fetching unreceived");

    if (resp)
        cJSON_Delete(resp);
}

static
void decl_owner()
{
    DeclOwnerResp *resp;
    ErrResp *err;

    feeds_client_decl_owner(owner1.fc, feedsd_addr, &resp, &err);
}

DECL_TESTCASE(decl_owner)
DECL_TESTCASE(create_topic_without_permission)
DECL_TESTCASE(create_topic)
DECL_TESTCASE(create_existing_topic)
DECL_TESTCASE(post_event_without_permission)
DECL_TESTCASE(post_event_on_non_existent_topic)
DECL_TESTCASE(post_event_when_no_subscribers)
DECL_TESTCASE(post_event_when_subscriber_exists)
DECL_TESTCASE(post_event_when_active_subscriber_exists)
DECL_TESTCASE(list_owned_topics_when_no_topics)
DECL_TESTCASE(list_owned_topics_when_topic_exists)
DECL_TESTCASE(list_owned_topics_batch_when_topic_exists)
DECL_TESTCASE(subscribe_non_existent_topic)
DECL_TESTCASE(subscribe)
DECL_TESTCASE(subscribe_when_already_subscribed)
DECL_TESTCASE(unsubscribe_non_existent_topic)
DECL_TESTCASE(unsubscribe)
DECL_TESTCASE(unsubscribe_without_subscription)
DECL_TESTCASE(explore_topics_when_no_topics)
DECL_TESTCASE(explore_topics_when_topic_exists)
DECL_TESTCASE(list_subscribed_topics_before_subscription)
DECL_TESTCASE(list_subscribed_topics_after_subscription)
DECL_TESTCASE(fetch_unreceived_without_subscription)
DECL_TESTCASE(fetch_unreceived)
DECL_TESTCASE(fetch_unreceived_when_already_active)

static CU_TestInfo cases[] = {
    // no topic, no subscription
    DEFINE_TESTCASE(create_topic_without_permission),
    DEFINE_TESTCASE(post_event_on_non_existent_topic),
    DEFINE_TESTCASE(list_owned_topics_when_no_topics),
    DEFINE_TESTCASE(subscribe_non_existent_topic),
    DEFINE_TESTCASE(unsubscribe_non_existent_topic),
    DEFINE_TESTCASE(explore_topics_when_no_topics),
    DEFINE_TESTCASE(create_topic),

    // has topic, no subscription
    DEFINE_TESTCASE(create_existing_topic),
    DEFINE_TESTCASE(post_event_without_permission),
    DEFINE_TESTCASE(post_event_when_no_subscribers),
    DEFINE_TESTCASE(list_owned_topics_when_topic_exists),
    DEFINE_TESTCASE(list_owned_topics_batch_when_topic_exists),
    DEFINE_TESTCASE(unsubscribe_without_subscription),
    DEFINE_TESTCASE(explore_topics_when_topic_exists),
    DEFINE_TESTCASE(list_subscribed_topics_before_subscription),
    DEFINE_TESTCASE(fetch_unreceived_without_subscription),
    DEFINE_TESTCASE(subscribe),

    // has topic, has subscription
    DEFINE_TESTCASE(subscribe_when_already_subscribed),
    DEFINE_TESTCASE(post_event_when_subscriber_exists),
    DEFINE_TESTCASE(list_subscribed_topics_after_subscription),
    DEFINE_TESTCASE(fetch_unreceived),

    // has topic, has active subscription
    DEFINE_TESTCASE(fetch_unreceived_when_already_active),
    DEFINE_TESTCASE(post_event_when_active_subscriber_exists),
    DEFINE_TESTCASE(unsubscribe),

    DEFINE_TESTCASE_NULL
};

CU_TestInfo* feeds_get_cases()
{
    return cases;
}

void *tfc_routine(void *arg)
{
    feeds_client_run((FeedsClient *)arg, 10);
    return NULL;
}

static
void tfc_deinit(TestsFeedsClient *tfc)
{

}

static
int tfc_init(TestsFeedsClient *tfc, const char *role, uint64_t idx)
{
    FeedsClientOpts opts;
    char path[PATH_MAX];
    char log[PATH_MAX];
    int rc;

    sprintf(path, "%s/%s", data_dir, role);
    sprintf(log, "%s/%s.log", data_dir, role);

    memset(&opts, 0, sizeof(opts));
    opts.data_dir = path;
    opts.log_file = log;
    opts.log_lv = tc.tests.log_lv;
    opts.carrier.bootstraps_sz = tc.carrier.bootstraps_sz;
    opts.carrier.bootstraps = tc.carrier.bootstraps;
    opts.carrier.udp_enabled = tc.carrier.udp_enabled;
    opts.did.mnemo = mnemo;
    opts.did.idx = idx;
    opts.did.passphrase = "secret";
    opts.did.passwd = "default";

    tfc->fc = feeds_client_create(&opts);
    if (!tfc->fc)
        return -1;

    rc = pthread_create(&tfc->tid, NULL, tfc_routine, tfc->fc);
    if (rc < 0) {
        tfc_deinit(tfc);
        return -1;
    }

    return 0;
}

static
int connect_robot(const char *host, const char *port)
{
    int ntries = 0;
    assert(host && *host);
    assert(port && *port);

    while (ntries < 3) {
        msg_sock = socket_connect(host, port);
        if (msg_sock != INVALID_SOCKET) {
            struct timeval timeout = {900,0};//900s
            setsockopt(msg_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(msg_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            break;
        }

        if (socket_errno() == ENODATA) {
            ntries++;
            sleep(1);
        }
    }

    if (msg_sock == INVALID_SOCKET)
        return -1;

    return 0;
}

static
void disconnect_robot()
{
    if (msg_sock != INVALID_SOCKET) {
        socket_close(msg_sock);
        msg_sock = INVALID_SOCKET;
    }
}

static
int get_feedsd_addr()
{
    int rc;

    rc = connect_robot(tc.robot.host, tc.robot.port);
    if (rc < 0)
        return -1;

    rc = recv(msg_sock, feedsd_addr, sizeof(feedsd_addr) - 1, 0);
    if (rc < 0 || !ela_address_is_valid(feedsd_addr)) {
        disconnect_robot();
        return -1;
    }

    ela_get_id_by_address(feedsd_addr, feedsd_node_id, sizeof(feedsd_node_id));

    return 0;
}

int feeds_suite_cleanup()
{
    if (owner1)
        feeds_client_delete(owner1);

    if (owner2)
        feeds_client_delete(owner2);

    if (follower)
        feeds_client_delete(follower);

    disconnect_robot();

    return 0;
}

int feeds_suite_init()
{
    int rc;

    sprintf(data_dir, "%s/cases", tc.data_dir);

    rc = tfc_init(&owner1, "owner1", 0);
    if (rc < 0)
        goto cleanup;

    rc = tfc_init(&owner2, "owner2", 1);
    if (rc < 0)
        goto cleanup;

    rc = tfc_init(&follower, "follower", 2);
    if (rc < 0)
        goto cleanup;

    fprintf(stderr, "Getting carrier address of feedsd ...");
    fflush(stderr);
    rc = get_feedsd_addr();
    if (rc < 0)
        goto cleanup;
    fprintf(stderr, "ok\n");

    return 0;

cleanup:
    feeds_suite_cleanup();
    CU_FAIL("Test suite initialize error");
    return -1;
}
