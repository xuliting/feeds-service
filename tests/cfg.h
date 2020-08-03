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

#ifndef __CFG_H__
#define __CFG_H__

#include <ela_carrier.h>
#include <libconfig.h>

typedef struct {
    config_t cfg;

    char *data_dir;

    struct {
        size_t bootstraps_sz;
        BootstrapNode *bootstraps;
        bool udp_enabled;
    } carrier;

    struct {
        int log_lv;
    } tests;

    struct {
        char *host;
        char *port;
        int log_lv;
    } robot;
} TestConfig;

extern TestConfig tc;
const char *get_cfg_file(const char *config_file, const char *default_config_files[]);
TestConfig *load_cfg(const char *cfg_file);
void free_cfg();

#endif // __CFG_H__
