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
#include "feeds_client.h"

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

static
void decl_owner()
{
    DeclOwnerResp *resp;
    ErrResp *err;
    int rc;

    rc = feeds_client_decl_owner(owner1.fc, feedsd_addr, &resp, &err);

    CU_ASSERT_TRUE(rc == 0);
    CU_ASSERT_PTR_NOT_NULL(resp);
    CU_ASSERT_STRING_EQUAL(resp->result.phase, "owner_declared");
    CU_ASSERT_PTR_NULL(resp->result.did);
    CU_ASSERT_PTR_NULL(resp->result.tsx_payload);

    deref(resp);
}

DECL_TESTCASE(decl_owner)

static CU_TestInfo cases[] = {
    DEFINE_TESTCASE(decl_owner),
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
    feeds_client_kill(tfc->fc);
    pthread_join(tfc->tid, NULL);
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
    opts.data_dir              = path;
    opts.log_file              = log;
    opts.log_lv                = tc.tests.log_lv;
    opts.carrier.bootstraps_sz = tc.carrier.bootstraps_sz;
    opts.carrier.bootstraps    = tc.carrier.bootstraps;
    opts.carrier.udp_enabled   = tc.carrier.udp_enabled;
    opts.did.mnemo             = mnemo;
    opts.did.idx               = idx;
    opts.did.passphrase        = "secret";
    opts.did.passwd            = "default";

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

    return 0;
}

int feeds_suite_cleanup()
{
    tfc_deinit(&owner1);
    tfc_deinit(&owner2);
    tfc_deinit(&follower);
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
