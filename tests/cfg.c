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
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include <crystal.h>
#include <libconfig.h>
#include <ela_carrier.h>

#include "mkdirs.h"
#include "cfg.h"

TestConfig tc;

const char *get_cfg_file(const char *config_file, const char *default_config_files[])
{
    const char **file = config_file ? &config_file : default_config_files;

    for (; *file; ) {
        int fd = open(*file, O_RDONLY);
        if (fd < 0) {
            if (file == &config_file)
                file = default_config_files;
            else
                file++;

            continue;
        }

        close(fd);

        return *file;
    }

    return NULL;
}

static void bootstraps_dtor(void *p)
{
    size_t i;
    size_t *size = (size_t *)p;
    BootstrapNode *bootstraps = (struct BootstrapNode *)(size + 1);

    for (i = 0; i < *size; i++) {
        BootstrapNode *node = bootstraps + i;

        if (node->ipv4)
            free((void *)node->ipv4);

        if (node->ipv6)
            free((void *)node->ipv6);

        if (node->port)
            free((void *)node->port);

        if (node->public_key)
            free((void *)node->public_key);
    }
}

#define PATH_SEP "/"
#define HOME_ENV "HOME"
static void qualified_path(const char *path, const char *ref, char *qualified)
{
    assert(strlen(path) >= 1);

    if (*path == PATH_SEP[0] || path[1] == ':') {
        strcpy(qualified, path);
    } else if (*path == '~') {
        sprintf(qualified, "%s%s", getenv(HOME_ENV), path+1);
    } else {
        getcwd(qualified, PATH_MAX);
        strcat(qualified, PATH_SEP);
        strcat(qualified, path);
    }
}

#define DEFAULT_LOG_LEVEL ElaLogLevel_Info
#define DEFAULT_DATA_DIR  "~/.ela-feeds-tests"
TestConfig *load_cfg(const char *cfg_file)
{
    config_setting_t *nodes_setting;
    config_setting_t *node_setting;
    char path[PATH_MAX];
    const char *stropt;
    char number[64];
    size_t *mem;
    int entries;
    int intopt;
    int rc;
    int i;

    if (!cfg_file || !*cfg_file)
        return NULL;

    memset(&tc, 0, sizeof(tc));

    config_init(&tc.cfg);

    rc = config_read_file(&tc.cfg, cfg_file);
    if (!rc) {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&tc.cfg),
                config_error_line(&tc.cfg), config_error_text(&tc.cfg));
        free_cfg();
        return NULL;
    }

    nodes_setting = config_lookup(&tc.cfg, "carrier.bootstraps");
    if (!nodes_setting) {
        fprintf(stderr, "Missing bootstraps section.\n");
        free_cfg();
        return NULL;
    }

    entries = config_setting_length(nodes_setting);
    if (entries <= 0) {
        fprintf(stderr, "Empty bootstraps option.\n");
        free_cfg();
        return NULL;
    }

    mem = (size_t *)rc_zalloc(sizeof(size_t) +
                              sizeof(BootstrapNode) * entries, bootstraps_dtor);
    if (!mem) {
        fprintf(stderr, "Load configuration failed, out of memory.\n");
        free_cfg();
        return NULL;
    }

    *mem = entries;
    tc.carrier.bootstraps_sz = entries;
    tc.carrier.bootstraps = (BootstrapNode *)(++mem);

    for (i = 0; i < entries; i++) {
        BootstrapNode *node = tc.carrier.bootstraps + i;

        node_setting = config_setting_get_elem(nodes_setting, i);

        rc = config_setting_lookup_string(node_setting, "ipv4", &stropt);
        if (rc && *stropt)
            node->ipv4 = (const char *)strdup(stropt);
        else
            node->ipv4 = NULL;

        rc = config_setting_lookup_string(node_setting, "ipv6", &stropt);
        if (rc && *stropt)
            node->ipv6 = (const char *)strdup(stropt);
        else
            node->ipv6 = NULL;

        rc = config_setting_lookup_int(node_setting, "port", &intopt);
        if (rc && intopt) {
            sprintf(number, "%d", intopt);
            node->port = (const char *)strdup(number);
        } else
            node->port = NULL;

        rc = config_setting_lookup_string(node_setting, "public-key", &stropt);
        if (rc && *stropt)
            node->public_key = (const char *)strdup(stropt);
        else
            node->public_key = NULL;
    }

    tc.carrier.udp_enabled = true;
    rc = config_lookup_bool(&tc.cfg, "carrier.udp-enabled", &intopt);
    if (rc)
        tc.carrier.udp_enabled = !!intopt;

    rc = config_lookup_string(&tc.cfg, "data-dir", &stropt);
    if (!rc || !*stropt)
        stropt = DEFAULT_DATA_DIR;
    qualified_path(stropt, cfg_file, path);
    tc.data_dir = strdup(path);
    if (!tc.data_dir || mkdirs(tc.data_dir, S_IRWXU) < 0) {
        fprintf(stderr, "Making data dir[%s] failed.\n", tc.data_dir);
        free_cfg();
        return NULL;
    }

    tc.tests.log_lv = DEFAULT_LOG_LEVEL;
    rc = config_lookup_int(&tc.cfg, "tests.log-level", &intopt);
    if (rc)
        tc.tests.log_lv = intopt;

    rc = config_lookup_string(&tc.cfg, "robot.host", &stropt);
    if (!rc || !(tc.robot.host = *stropt ? strdup(stropt) : "localhost")) {
        fprintf(stderr, "Missing robot.host entry.\n");
        free_cfg();
        return NULL;
    }

    intopt = 7238;
    rc = config_lookup_int(&tc.cfg, "robot.port", &intopt);
    if (!rc || (intopt <= 0 || intopt > 65535)) {
        fprintf(stderr, "Missing robot.port entry.\n");
        free_cfg();
        return NULL;
    }
    sprintf(number, "%d", intopt);
    tc.robot.port = strdup(number);

    tc.tests.log_lv = DEFAULT_LOG_LEVEL;
    rc = config_lookup_int(&tc.cfg, "tests.log-level", &intopt);
    if (rc)
        tc.robot.log_lv = intopt;

    return &tc;
}

void free_cfg()
{
    config_destroy(&tc.cfg);

    if (tc.data_dir)
        free(tc.data_dir);

    if (tc.carrier.bootstraps) {
        size_t *p = (size_t *)tc.carrier.bootstraps;
        deref(--p);
    }

    if (tc.robot.host)
        free(tc.robot.host);

    if (tc.robot.port)
        free(tc.robot.port);
}
