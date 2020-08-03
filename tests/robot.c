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

#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include <crystal.h>

#include "cfg.h"
#include "../src/mkdirs.h"

extern const char *cfg_file;

static char publisher_node_id[ELA_MAX_ID_LEN + 1];
static char data_dir[PATH_MAX];
static char feedsd_cfg_file[PATH_MAX];
static SOCKET msg_sock = INVALID_SOCKET;
static pid_t feedsd_pid;

int wait_cases_connecting(const char *host, const char *port)
{
    int rc;
    SOCKET svr_sock;

    svr_sock = socket_create(SOCK_STREAM, host, port);
    if (svr_sock == INVALID_SOCKET)
        return -1;

    rc = listen(svr_sock, 1);
    if (rc < 0) {
        socket_close(svr_sock);
        return -1;
    }

    msg_sock = accept(svr_sock, NULL, NULL);
    socket_close(svr_sock);

    if (msg_sock == INVALID_SOCKET)
        return -1;

    struct timeval timeout = {900,0};
    setsockopt(msg_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(msg_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    return 0;
}

static
int get_publisher_node_id_from_test_case()
{
    int rc;

    rc = recv(msg_sock, publisher_node_id, sizeof(publisher_node_id) - 1, 0);
    if (rc < 0 || !ela_id_is_valid(publisher_node_id))
        return -1;

    return 0;
}

static
int gen_feedsd_cfg(TestConfig *tc)
{
    config_setting_t *root;
    char path[PATH_MAX];

    root = config_root_setting(&tc->cfg);

    config_setting_remove(root, "data-dir");
    config_setting_remove(root, "tests");
    config_setting_remove(root, "root");

    {
        config_setting_t *did;
        config_setting_t *passwd;
        config_setting_t *svr;
        config_setting_t *ip;
        config_setting_t *port;

        did = config_setting_add(root, "did", CONFIG_TYPE_GROUP);
        if (!did)
            return -1;

        passwd = config_setting_add(did, "store-password", CONFIG_TYPE_STRING);
        if (!passwd)
            return -1;
        config_setting_set_string(passwd, "default");

        svr = config_setting_add(did, "binding-server", CONFIG_TYPE_GROUP);
        if (!svr)
            return -1;

        ip = config_setting_add(svr, "ip", CONFIG_TYPE_STRING);
        if (!ip)
            return -1;
        config_setting_set_string(ip, "localhost");

        port = config_setting_add(svr, "port", CONFIG_TYPE_INT);
        if (!port)
            return -1;
        config_setting_set_int(ip, 8080);
    }

    {
        config_setting_t *log_lv;

        log_lv = config_setting_add(root, "log-level", CONFIG_TYPE_INT);
        if (!log_lv)
            return -1;
        config_setting_set_int(log_lv, tc->robot.log_lv);
    }

    {
        config_setting_t *log_file;

        sprintf(path, "%s/feedsd.log", data_dir);

        log_file = config_setting_add(root, "log-file", CONFIG_TYPE_STRING);
        if (!log_file)
            return -1;
        config_setting_set_string(log_file, path);
    }

    {
        config_setting_t *feedsd_data_dir;

        feedsd_data_dir = config_setting_add(root, "data-dir", CONFIG_TYPE_STRING);
        if (!feedsd_data_dir)
            return -1;
        config_setting_set_string(feedsd_data_dir, data_dir);
    }

    sprintf(path, "%s/feedsd.conf", data_dir);
    return config_write_file(&tc->cfg, path) == CONFIG_TRUE ? 0 : -1;
}

static
int pass_feedsd_addr_to_cases()
{
    char feedsd_addr[ELA_MAX_ADDRESS_LEN + 1];
    char fpath[PATH_MAX];
    int fd;
    int rc;

    memset(feedsd_addr, 0, sizeof(feedsd_addr));
    sprintf(fpath, "%s/address.txt", data_dir);

    while (access(fpath, F_OK))
        sleep(1);

    fd = open(fpath, O_RDONLY);
    if (fd < 0)
        return -1;

    rc = read(fd, feedsd_addr, sizeof(feedsd_addr) - 1);
    close(fd);
    if (rc < 0 || !ela_address_is_valid(feedsd_addr))
        return -1;

    rc = send(msg_sock, feedsd_addr, strlen(feedsd_addr), 0);
    if (rc < 0)
        return -1;

    return 0;
}

static
int wait_test_case_disconnected()
{
    char buf[1024];
    int rc;

    while ((rc = recv(msg_sock, buf, sizeof(buf), 0)) > 0) ;

    return rc;
}

static
void cleanup()
{
    char path[PATH_MAX];

    if (msg_sock != INVALID_SOCKET)
        socket_close(msg_sock);

    if (feedsd_pid > 0) {
        kill(feedsd_pid, SIGINT);
        waitpid(feedsd_pid, NULL, 0);
        sprintf(path, "%s/db/feeds.sqlite3", data_dir);
        remove(path);
        sprintf(path, "%s/didstore", data_dir);
        remove(path);
    }

    sprintf(path, "%s/feedsd.conf", data_dir);
    remove(path);

    free_cfg();
}

static
void sig_hdlr(int signum)
{
    cleanup();
    exit(-1);
}

int robot_main(int argc, char *argv[])
{
    char feeds_path[PATH_MAX];
    int rc;

    signal(SIGINT, sig_hdlr);
    signal(SIGTERM, sig_hdlr);

    if (!load_cfg(cfg_file)) {
        fprintf(stderr, "Loading config failed!\n");
        return -1;
    }

    sprintf(data_dir, "%s/robot", tc.data_dir);
    rc = mkdirs(data_dir, S_IRWXU);
    if (rc < 0) {
        fprintf(stderr, "mkdirs [%s] failed!\n", tc.data_dir);
        goto cleanup;
    }

    sprintf(feedsd_cfg_file, "%s/feedsd.conf", data_dir);
    rc = gen_feedsd_cfg(&tc);
    if (rc < 0) {
        fprintf(stderr, "Generating feedsd config failed!\n");
        goto cleanup;
    }

    printf("Starting service feeds ...");
    fflush(stdout);
    feedsd_pid = fork();
    if (feedsd_pid < 0) {
        fprintf(stderr, "failed!\n");
        goto cleanup;
    } else if (!feedsd_pid) {
        char path[PATH_MAX];

        strcpy(path, argv[0]);
        sprintf(feeds_path, "%s/ela-feedsd", dirname(path));
        execl(feeds_path, feeds_path, "-c", feedsd_cfg_file,
              "-r", "http://api.elastos.io:21606", NULL);
        return -1;
    }
    printf("ok\n");

    printf("Waiting test cases connection ...");
    fflush(stdout);
    rc = wait_cases_connecting(tc.robot.host, tc.robot.port);
    if (rc < 0) {
        fprintf(stderr, "failed!\n");
        goto cleanup;
    }
    printf("ok\n");

    printf("Passing feedsd carrier address to test cases ...");
    fflush(stdout);
    rc = pass_feedsd_addr_to_cases();
    if (rc) {
        fprintf(stderr, "failed!\n");
        goto cleanup;
    }
    printf("ok\n");

    rc = wait_test_case_disconnected();

cleanup:
    cleanup();

    return rc;
}
