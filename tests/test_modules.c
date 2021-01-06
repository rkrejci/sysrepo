/**
 * @file test_modules.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief test for adding/removing modules
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE

#include <unistd.h>
#include <setjmp.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "tests/config.h"
#include "sysrepo.h"

struct state {
    sr_conn_ctx_t *conn;
};

static int
setup_f(void **state)
{
    struct state *st;

    st = calloc(1, sizeof *st);
    *state = st;

    if (sr_connect(0, &st->conn) != SR_ERR_OK) {
        return 1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static void
cmp_int_data(sr_conn_ctx_t *conn, const char *module_name, const char *expected)
{
    char *str, *ptr, buf[1024];
    struct lyd_node *data, *sr_mod;
    struct ly_set *set;
    LY_ERR ret;

    /* parse internal data */
    sprintf(buf, "%s/data/sysrepo.startup", sr_get_repo_path());
    assert_int_equal(LY_SUCCESS, lyd_parse_data_path(sr_get_context(conn), buf, LYD_LYB, LYD_PARSE_ONLY, 0, &data));

    /* filter the module */
    sprintf(buf, "/sysrepo:sysrepo-modules/*[name='%s']", module_name);
    assert_int_equal(LY_SUCCESS, lyd_find_xpath(data, buf, &set));
    assert_int_equal(set->count, 1);
    sr_mod = set->objs[0];
    ly_set_free(set, NULL);

    /* remove YANG module is present */
    assert_int_equal(LY_SUCCESS, lyd_find_xpath(sr_mod, "module-yang", &set));
    if (set->count) {
        lyd_free_tree(set->objs[0]);
    }
    ly_set_free(set, NULL);

    /* check current internal data */
    ret = lyd_print_mem(&str, sr_mod, LYD_XML, LYD_PRINT_SHRINK);
    lyd_free_all(data);
    assert_int_equal(ret, LY_SUCCESS);

    /* set replay support timestamp to zeroes */
    for (ptr = strstr(str, "<replay-support>"); ptr; ptr = strstr(ptr, "<replay-support>")) {
        for (ptr += 16; isdigit(ptr[0]); ++ptr) {
            ptr[0] = '0';
        }
    }

    assert_string_equal(str, expected);
    free(str);
}

static void
test_install_module(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    const char *en_feats[] = {"feat", NULL};
    uint32_t conn_count;

    /* install test-module */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/test-module.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* module should be installed with its dependency */
    ret = sr_remove_module(st->conn, "test-module");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "referenced-data");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "test-module");
    assert_int_equal(ret, SR_ERR_EXISTS);

    /* install main-mod */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/main-mod.yang", TESTS_DIR "/files", en_feats);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current internal data */
    cmp_int_data(st->conn, "main-mod",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>main-mod</name>"
        "<enabled-feature>feat</enabled-feature>"
    "</module>"
    );

    ret = sr_remove_module(st->conn, "main-mod");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_data_deps(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    ret = sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/iana-if-type.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/refs.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_module_replay_support(st->conn, "ietf-interfaces", 1);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_module_replay_support(st->conn, "refs", 1);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_remove_module(st->conn, "refs");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "ietf-interfaces");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "iana-if-type");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);

    /* check current internal data */
    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
        "<removed/>"
        "<inverse-data-deps>refs</inverse-data-deps>"
    "</module>"
    );
    cmp_int_data(st->conn, "ietf-interfaces",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-interfaces</name>"
        "<revision>2014-05-08</revision>"
        "<replay-support>0000000000</replay-support>"
        "<removed/>"
    "</module>"
    );
    cmp_int_data(st->conn, "iana-if-type",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>iana-if-type</name>"
        "<revision>2014-05-08</revision>"
        "<removed/>"
    "</module>"
    );
    cmp_int_data(st->conn, "refs",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>refs</name>"
        "<replay-support>0000000000</replay-support>"
        "<removed/>"
        "<data-deps>"
            "<module>test</module>"
            "<inst-id>"
                "<xpath xmlns:r=\"urn:refs\">/r:cont/r:def-inst-id</xpath>"
                "<default-module>test</default-module>"
            "</inst-id>"
            "<inst-id>"
                "<xpath xmlns:r=\"urn:refs\">/r:inst-id</xpath>"
            "</inst-id>"
        "</data-deps>"
    "</module>"
    );
}

static void
test_op_deps(void **state)
{
    struct state *st = (struct state *)*state;
    uint32_t conn_count;
    int ret;

    ret = sr_install_module(st->conn, TESTS_DIR "/files/ops-ref.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ops.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_module_replay_support(st->conn, "ops-ref", 1);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_module_replay_support(st->conn, "ops", 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current internal data */
    cmp_int_data(st->conn, "ops-ref",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ops-ref</name>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );

    cmp_int_data(st->conn, "ops",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ops</name>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:cont/o:list1/o:cont2/o:act1</xpath>"
            "<out>"
                "<module>ops</module>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:cont/o:list1/o:cont2/o:act1/o:l8</xpath>"
                    "<default-module>ops</default-module>"
                "</inst-id>"
            "</out>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:cont/o:cont3/o:notif2</xpath>"
            "<in>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:cont/o:cont3/o:notif2/o:l13</xpath>"
                "</inst-id>"
            "</in>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc1</xpath>"
            "<in>"
                "<module>ops-ref</module>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:rpc1/o:l2</xpath>"
                    "<default-module>ops-ref</default-module>"
                "</inst-id>"
            "</in>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc2</xpath>"
            "<out>"
                "<module>ops-ref</module>"
            "</out>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc3</xpath>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:notif4</xpath>"
        "</op-deps>"
    "</module>"
    );

    /* enable feature that should enable 2 more operations */
    ret = sr_enable_module_feature(st->conn, "ops-ref", "feat1");
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "ops",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ops</name>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:cont/o:list1/o:cont2/o:act1</xpath>"
            "<out>"
                "<module>ops</module>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:cont/o:list1/o:cont2/o:act1/o:l8</xpath>"
                    "<default-module>ops</default-module>"
                "</inst-id>"
            "</out>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:cont/o:list1/o:act2</xpath>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:cont/o:cont3/o:notif2</xpath>"
            "<in>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:cont/o:cont3/o:notif2/o:l13</xpath>"
                "</inst-id>"
            "</in>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc1</xpath>"
            "<in>"
                "<module>ops-ref</module>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:rpc1/o:l2</xpath>"
                    "<default-module>ops-ref</default-module>"
                "</inst-id>"
            "</in>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc2</xpath>"
            "<out>"
                "<module>ops-ref</module>"
            "</out>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:rpc3</xpath>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:notif3</xpath>"
            "<in>"
                "<module>ops-ref</module>"
                "<inst-id>"
                    "<xpath xmlns:o=\"urn:ops\">/o:notif3/o:list2/o:l15</xpath>"
                    "<default-module>ops</default-module>"
                "</inst-id>"
            "</in>"
        "</op-deps>"
        "<op-deps>"
            "<xpath xmlns:o=\"urn:ops\">/o:notif4</xpath>"
        "</op-deps>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "ops");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "ops-ref");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_inv_deps(void **state)
{
    struct state *st = (struct state *)*state;
    uint32_t conn_count;
    int ret;

    ret = sr_install_module(st->conn, TESTS_DIR "/files/ietf-routing.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_remove_module(st->conn, "ietf-routing");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "ietf-interfaces");
    assert_int_equal(ret, SR_ERR_OK);

    /* check current internal data */
    cmp_int_data(st->conn, "ietf-routing",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-routing</name>"
        "<revision>2015-04-17</revision>"
        "<removed/>"
        "<data-deps>"
            "<module>ietf-interfaces</module>"
        "</data-deps>"
        "<op-deps>"
            "<xpath xmlns:rt=\"urn:ietf:params:xml:ns:yang:ietf-routing\">/rt:fib-route</xpath>"
            "<in>"
                "<module>ietf-routing</module>"
            "</in>"
            "<out>"
                "<module>ietf-routing</module>"
            "</out>"
        "</op-deps>"
        "<inverse-data-deps>ietf-interfaces</inverse-data-deps>"
    "</module>"
    );

    cmp_int_data(st->conn, "ietf-interfaces",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-interfaces</name>"
        "<revision>2014-05-08</revision>"
        "<removed/>"
        "<data-deps>"
            "<module>ietf-routing</module>"
        "</data-deps>"
        "<inverse-data-deps>ietf-routing</inverse-data-deps>"
    "</module>"
    );
}

static void
test_remove_module(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /* install modules with one depending on the other */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ietf-ip.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* remove module with augments */
    ret = sr_remove_module(st->conn, "ietf-ip");
    assert_int_equal(ret, SR_ERR_OK);

    /* close connection so that changes are applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection, apply changes */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* cleanup */
    ret = sr_remove_module(st->conn, "ietf-interfaces");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_remove_dep_module(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /* install modules with one depending on the other */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ops-ref.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ops.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_module_replay_support(st->conn, "ops-ref", 1);
    assert_int_equal(ret, SR_ERR_OK);

    /* remove module required by the other module */
    ret = sr_remove_module(st->conn, "ops-ref");
    assert_int_equal(ret, SR_ERR_OK);

    /* close connection so that changes are applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection, changes fail to be applied and should remain scheduled */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "ops-ref",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ops-ref</name>"
        "<replay-support>0000000000</replay-support>"
        "<removed/>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "ops");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_remove_imp_module(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /* install modules with one importing the other */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/simple.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/simple-imp.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* remove module imported by the other module */
    ret = sr_remove_module(st->conn, "simple");
    assert_int_equal(ret, SR_ERR_OK);

    /* close connection so that changes are applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection, changes should be applied */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "simple-imp",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>simple-imp</name>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "simple-imp");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_update_module(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /* install rev */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/rev.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "rev",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>rev</name>"
    "</module>"
    );

    /* schedule an update */
    ret = sr_update_module(st->conn, TESTS_DIR "/files/rev@1970-01-01.yang", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_update_module(st->conn, TESTS_DIR "/files/rev@1970-01-01.yang", NULL);
    assert_int_equal(ret, SR_ERR_EXISTS);

    /* cancel the update */
    ret = sr_cancel_update_module(st->conn, "rev");
    assert_int_equal(ret, SR_ERR_OK);

    /* reschedule */
    ret = sr_update_module(st->conn, TESTS_DIR "/files/rev@1970-01-01.yang", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* close connection so that changes are applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* check that the module was updated */
    cmp_int_data(st->conn, "rev",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>rev</name>"
        "<revision>1970-01-01</revision>"
        "<op-deps>"
            "<xpath xmlns:r=\"urn:rev\">/r:notif</xpath>"
        "</op-deps>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "rev");
    assert_int_equal(ret, SR_ERR_OK);

    /* delete the module */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_change_feature(void **state)
{
    struct state *st = (struct state *)*state;
    sr_session_ctx_t *sess;
    const char *en_feats[] = {"feat1", NULL};
    sr_val_t *val;
    int ret;
    uint32_t conn_count;

    /* install features with feat1 (will also install test) */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/features.yang", TESTS_DIR "/files", en_feats);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "features",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>features</name>"
        "<enabled-feature>feat1</enabled-feature>"
        "<data-deps><module>test</module></data-deps>"
    "</module>"
    );
    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
        "<inverse-data-deps>features</inverse-data-deps>"
    "</module>"
    );

    /* enable feat2 and feat3 */
    ret = sr_enable_module_feature(st->conn, "features", "feat2");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_enable_module_feature(st->conn, "features", "feat3");
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "features",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>features</name>"
        "<enabled-feature>feat1</enabled-feature>"
        "<enabled-feature>feat2</enabled-feature>"
        "<enabled-feature>feat3</enabled-feature>"
        "<data-deps><module>test</module></data-deps>"
    "</module>"
    );

    ret = sr_session_start(st->conn, SR_DS_STARTUP, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* set all data */
    ret = sr_set_item_str(sess, "/test:test-leaf", "2", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/features:l1", "val1", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/features:l2", "2", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/features:l3", "val3", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* disable all features */
    ret = sr_disable_module_feature(st->conn, "features", "feat1");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_disable_module_feature(st->conn, "features", "feat2");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_disable_module_feature(st->conn, "features", "feat3");
    assert_int_equal(ret, SR_ERR_OK);

    /* apply changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(st->conn, SR_DS_STARTUP, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* check that the features were disabled and dependency removed */
    cmp_int_data(st->conn, "features",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>features</name>"
    "</module>"
    );

    /* check that the inverse dependency was removed */
    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
    "</module>"
    );

    /* check that the conditional data were removed */
    ret = sr_get_item(sess, "/features:l2", 0, &val);
    assert_int_equal(ret, SR_ERR_NOT_FOUND);

    /* cleanup */
    ret = sr_session_switch_ds(sess, SR_DS_RUNNING);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/test:test-leaf", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/features:l1", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/features:l3", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_copy_config(sess, NULL, SR_DS_STARTUP, 0, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_session_stop(sess);

    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "features");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_replay_support(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    ret = sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/iana-if-type.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/simple.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* replay support for 2 modules */
    ret = sr_set_module_replay_support(st->conn, "ietf-interfaces", 1);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_module_replay_support(st->conn, "simple", 1);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
    "</module>"
    );
    cmp_int_data(st->conn, "ietf-interfaces",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-interfaces</name>"
        "<revision>2014-05-08</revision>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );
    cmp_int_data(st->conn, "iana-if-type",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>iana-if-type</name>"
        "<revision>2014-05-08</revision>"
    "</module>"
    );
    cmp_int_data(st->conn, "simple",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>simple</name>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );

    /* replay support for all modules */
    ret = sr_set_module_replay_support(st->conn, NULL, 1);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );
    cmp_int_data(st->conn, "ietf-interfaces",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-interfaces</name>"
        "<revision>2014-05-08</revision>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );
    cmp_int_data(st->conn, "iana-if-type",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>iana-if-type</name>"
        "<revision>2014-05-08</revision>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );
    cmp_int_data(st->conn, "simple",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>simple</name>"
        "<replay-support>0000000000</replay-support>"
    "</module>"
    );

    /* replay support for no modules */
    ret = sr_set_module_replay_support(st->conn, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "test",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
    "</module>"
    );
    cmp_int_data(st->conn, "ietf-interfaces",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>ietf-interfaces</name>"
        "<revision>2014-05-08</revision>"
    "</module>"
    );
    cmp_int_data(st->conn, "iana-if-type",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>iana-if-type</name>"
        "<revision>2014-05-08</revision>"
    "</module>"
    );
    cmp_int_data(st->conn, "simple",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>simple</name>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "ietf-interfaces");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "iana-if-type");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "simple");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_foreign_aug(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /*
     * install modules together
     */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/aug.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "aug",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>aug</name>"
        "<inverse-data-deps>aug-trg</inverse-data-deps>"
    "</module>"
    );

    cmp_int_data(st->conn, "aug-trg",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>aug-trg</name>"
        "<data-deps>"
            "<module>aug</module>"
        "</data-deps>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "aug");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "aug-trg");
    assert_int_equal(ret, SR_ERR_OK);

    /* close connection so that changes are applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);

    /* recreate connection */
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * install modules one-by-one
     */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/aug-trg.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/aug.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "aug",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>aug</name>"
        "<inverse-data-deps>aug-trg</inverse-data-deps>"
    "</module>"
    );

    cmp_int_data(st->conn, "aug-trg",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>aug-trg</name>"
        "<data-deps>"
            "<module>aug</module>"
        "</data-deps>"
    "</module>"
    );

    /* cleanup */
    ret = sr_remove_module(st->conn, "aug");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "aug-trg");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_empty_invalid(void **state)
{
    struct state *st = (struct state *)*state;
    sr_session_ctx_t *sess;
    struct lyd_node *tree;
    const char data[] = "<cont xmlns=\"mand\"><l1/></cont>";
    int ret;
    uint32_t conn_count;

    /* install the module */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/mandatory.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* no startup data set so it should fail and remain scheduled */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/mandatory.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_EXISTS);

    /* set startup data */
    ret = sr_install_module_data(st->conn, "mandatory", data, NULL, LYD_XML);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* should have succeeded now */
    cmp_int_data(st->conn, "mandatory",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>mandatory</name>"
    "</module>"
    );

    /* check startup data */
    ret = sr_session_start(st->conn, SR_DS_STARTUP, &sess);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_get_data(sess, "/mandatory:*", 0, 0, 0, &tree);
    assert_int_equal(ret, SR_ERR_OK);
    assert_string_equal(tree->schema->name, "cont");
    assert_string_equal(lyd_child(tree)->schema->name, "l1");
    assert_null(tree->next);

    /* check running data */
    ret = sr_session_switch_ds(sess, SR_DS_RUNNING);
    lyd_free_all(tree);
    ret = sr_get_data(sess, "/mandatory:*", 0, 0, 0, &tree);
    assert_int_equal(ret, SR_ERR_OK);
    assert_string_equal(tree->schema->name, "cont");
    assert_string_equal(lyd_child(tree)->schema->name, "l1");
    assert_null(tree->next);

    /* cleanup, remove its data so that it can be uninstalled */
    lyd_free_all(tree);
    sr_session_stop(sess);

    /* actually remove the module */
    ret = sr_remove_module(st->conn, "mandatory");
    assert_int_equal(ret, SR_ERR_OK);
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_remove_module(st->conn, "mandatory");
    assert_int_equal(ret, SR_ERR_NOT_FOUND);
}

static void
test_startup_data_foreign_identityref(void **state)
{
    struct state *st = (struct state *)*state;
    sr_session_ctx_t *sess;
    struct lyd_node *tree;
    const char data[] =
        "<haha xmlns=\"http://www.example.net/t1\">"
            "<layer-protocol-name xmlns:x=\"http://www.example.net/t2\">x:desc</layer-protocol-name>"
        "</haha>";
    int ret;
    uint32_t conn_count;

    /* install module with types */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/t-types.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* install module with top-level default data */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/defaults.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* install t1 */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/t1.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* scheduled changes not applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "t1",
    "<installed-module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>t1</name>"
    "</installed-module>"
    );

    /* install t2 */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/t2.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* scheduled changes not applied */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    cmp_int_data(st->conn, "t2",
    "<installed-module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>t2</name>"
    "</installed-module>"
    );

    /* finally set startup data */
    ret = sr_install_module_data(st->conn, "t1", data, NULL, LYD_XML);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* should have succeeded now */
    cmp_int_data(st->conn, "t1",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>t1</name>"
    "</module>"
    );
    cmp_int_data(st->conn, "t2",
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>t2</name>"
    "</module>"
    );

    /* check startup data */
    ret = sr_session_start(st->conn, SR_DS_STARTUP, &sess);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_get_data(sess, "/t1:*", 0, 0, 0, &tree);
    assert_int_equal(ret, SR_ERR_OK);
    assert_string_equal(tree->schema->name, "haha");
    assert_string_equal(lyd_child(tree)->schema->name, "layer-protocol-name");
    assert_string_equal(LYD_CANON_VALUE(lyd_child(tree)), "t2:desc");
    assert_null(tree->next);

    /* check running data */
    ret = sr_session_switch_ds(sess, SR_DS_RUNNING);
    assert_int_equal(ret, SR_ERR_OK);
    lyd_free_all(tree);
    ret = sr_get_data(sess, "/t1:*", 0, 0, 0, &tree);
    assert_int_equal(ret, SR_ERR_OK);
    assert_string_equal(tree->schema->name, "haha");
    assert_string_equal(lyd_child(tree)->schema->name, "layer-protocol-name");
    assert_string_equal(LYD_CANON_VALUE(lyd_child(tree)), "t2:desc");
    assert_null(tree->next);

    /* cleanup, remove its data so that it can be uninstalled */
    lyd_free_all(tree);
    sr_session_stop(sess);

    /* actually remove the modules */
    ret = sr_remove_module(st->conn, "t1");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "t2");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "t-types");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "defaults");
    assert_int_equal(ret, SR_ERR_OK);

    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_remove_module(st->conn, "t1");
    assert_int_equal(ret, SR_ERR_NOT_FOUND);
    ret = sr_remove_module(st->conn, "t2");
    assert_int_equal(ret, SR_ERR_NOT_FOUND);
    ret = sr_remove_module(st->conn, "t-types");
    assert_int_equal(ret, SR_ERR_NOT_FOUND);
    ret = sr_remove_module(st->conn, "defaults");
    assert_int_equal(ret, SR_ERR_NOT_FOUND);
}

static void
test_set_module_access(void **state)
{
    struct state *st = (struct state *)*state;
    struct passwd *pwd;
    struct group *grp;
    const char *user;
    const char *group;
    uint32_t conn_count;
    int ret;

    /* install module test */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* get user */
    pwd = getpwuid(getuid());
    user = pwd->pw_name;

    /* get group */
    grp = getgrgid(getgid());
    group = grp->gr_name;

    /* params error, connection NULL or owner NULL/group NULL/(int)perm=-1 */
    ret = sr_set_module_access(NULL, "test", user, group, 00666);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    ret = sr_set_module_access(st->conn, NULL, user, group, 00666);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    ret = sr_set_module_access(st->conn, "test", NULL, NULL, (mode_t)-1);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    /* param perm error,invalid permissions */
    ret = sr_set_module_access(st->conn, "test", user, group, 01777);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    /* param perm error,setting execute permissions has no effect */
    ret = sr_set_module_access(st->conn, "test", user, group, 00771);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    /* non-existing module */
    ret = sr_set_module_access(st->conn, "no-module", user, group, 00666);
    assert_int_equal(ret, SR_ERR_NOT_FOUND);

    /* invalid user (can return SR_ERR_NOT_FOUND or SR_ERR_SYS) */
    ret = sr_set_module_access(st->conn, "test", "no-user", group, 00666);
    assert_int_not_equal(ret, SR_ERR_OK);

    /* invalid group (can return SR_ERR_NOT_FOUND or SR_ERR_SYS) */
    ret = sr_set_module_access(st->conn, "test", user, "no-group", 00666);
    assert_int_not_equal(ret, SR_ERR_OK);

    /* user NULL and group NULL */
    ret = sr_set_module_access(st->conn, "test", NULL, NULL, 00666);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_module_access(st->conn, "test", user, group, 00666);
    assert_int_equal(ret, SR_ERR_OK);

    /* cleanup */
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_get_module_access(void **state)
{
    struct state *st = (struct state *)*state;
    struct passwd *pwd;
    struct group *grp;
    const char *user;
    char *owner, *group;
    uint32_t conn_count;
    int ret;
    mode_t perm;

    /* install module test */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* get user */
    pwd = getpwuid(getuid());
    user = pwd->pw_name;
    /* get group */
    grp = getgrgid(getgid());
    group = grp->gr_name;

    /* change module test permissions */
    ret = sr_set_module_access(st->conn, "test", user, group, 00600);
    assert_int_equal(ret, SR_ERR_OK);

    /* params error, connection NULL or module name NULL or ower/group/perm NULL */
    ret = sr_get_module_access(NULL, "test", &owner, &group, &perm);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    ret = sr_get_module_access(st->conn, NULL, &owner, &group, &perm);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    ret = sr_get_module_access(st->conn, "test", NULL, NULL, NULL);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    /* non-existing module */
    ret = sr_get_module_access(st->conn, "no-module", &owner, &group, &perm);
    assert_int_equal(ret, SR_ERR_NOT_FOUND);

    ret = sr_get_module_access(st->conn, "test", &owner, &group, &perm);
    assert_int_equal(ret, SR_ERR_OK);
    assert_string_equal(owner, pwd->pw_name);
    assert_string_equal(group, grp->gr_name);
    assert_int_equal(perm, 00600);

    free(owner);
    free(group);

    /* cleanup */
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_get_module_info(void **state)
{
    struct state *st = (struct state *)*state;
    struct lyd_node *data, *sr_mod;
    uint32_t conn_count;
    char *str, *str2;
    int ret;

    ret = sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_get_module_info(st->conn, &data);
    assert_int_equal(ret, SR_ERR_OK);

    /* filter module test */
    struct ly_set *set;
    assert_int_equal(LY_SUCCESS, lyd_find_xpath(data, "/sysrepo:sysrepo-modules/*[name='test']", &set));
    assert_int_equal(set->count, 1);
    sr_mod = set->objs[0];
    ly_set_free(set, NULL);

    ret = lyd_print_mem(&str, sr_mod, LYD_XML, LYD_PRINT_SHRINK);
    lyd_free_all(data);
    assert_int_equal(ret, SR_ERR_OK);

    str2 =
    "<module xmlns=\"http://www.sysrepo.org/yang/sysrepo\">"
        "<name>test</name>"
    "</module>";
    assert_string_equal(str, str2);
    free(str);

    /* cleanup */
    ret = sr_remove_module(st->conn, "test");
    assert_int_equal(ret, SR_ERR_OK);
}

static void
test_feature_dependencies_across_modules(void **state)
{
    struct state *st = (struct state *)*state;
    int ret;
    uint32_t conn_count;

    /* install modules */
    ret = sr_install_module(st->conn, TESTS_DIR "/files/feature-deps.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_install_module(st->conn, TESTS_DIR "/files/feature-deps2.yang", TESTS_DIR "/files", NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(SR_CONN_ERR_ON_SCHED_FAIL, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* enable independent feature */
    ret = sr_enable_module_feature(st->conn, "feature-deps2", "featx");
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(SR_CONN_ERR_ON_SCHED_FAIL, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* enable dependent features */
    ret = sr_enable_module_feature(st->conn, "feature-deps", "feat1");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_enable_module_feature(st->conn, "feature-deps", "feat2");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_enable_module_feature(st->conn, "feature-deps", "feat3");
    assert_int_equal(ret, SR_ERR_OK);

    /* apply scheduled changes */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(SR_CONN_ERR_ON_SCHED_FAIL, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* check if modules can be loaded again */
    sr_disconnect(st->conn);
    st->conn = NULL;
    ret = sr_connection_count(&conn_count);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(conn_count, 0);
    ret = sr_connect(0, &st->conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_remove_module(st->conn, "feature-deps");
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_remove_module(st->conn, "feature-deps2");
    assert_int_equal(ret, SR_ERR_OK);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_install_module, setup_f, teardown_f),
        //cmocka_unit_test_setup_teardown(test_data_deps, setup_f, teardown_f),
        //cmocka_unit_test_setup_teardown(test_op_deps, setup_f, teardown_f),
        //cmocka_unit_test_setup_teardown(test_inv_deps, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_remove_module, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_remove_dep_module, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_remove_imp_module, setup_f, teardown_f),
        //cmocka_unit_test_setup_teardown(test_update_module, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_feature, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_replay_support, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_foreign_aug, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_empty_invalid, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_startup_data_foreign_identityref, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_set_module_access, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_get_module_access, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_get_module_info, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_feature_dependencies_across_modules, setup_f, teardown_f),
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    sr_log_stderr(SR_LL_INF);
    return cmocka_run_group_tests(tests, NULL, NULL);
}
