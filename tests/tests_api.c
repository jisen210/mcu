/*

 The MIT License (MIT)

 Copyright (c) 2015 Douglas J. Bakkum

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.

*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecc.h"
#include "utest.h"
#include "utils.h"
#include "flags.h"
#include "random.h"
#include "commander.h"
#include "yajl/src/api/yajl_tree.h"


int U_TESTS_RUN = 0;
int U_TESTS_FAIL = 0;


#define HID_REPORT_SIZE   COMMANDER_REPORT_SIZE

static const char tests_pwd[] = "0000";
static char command_sent[COMMANDER_REPORT_SIZE] = {0};
static int TEST_LIVE_DEVICE = 0;


#ifndef CONTINUOUS_INTEGRATION
// http://www.signal11.us/oss/hidapi/
#include <hidapi.h>

static hid_device *HID_HANDLE;
static unsigned char HID_REPORT[HID_REPORT_SIZE] = {0};

static int api_hid_init(void)
{
    HID_HANDLE = hid_open(0x03eb, 0x2402, NULL);
    if (!HID_HANDLE) {
        return DBB_ERROR;
    }
    return DBB_OK;
}


static void api_hid_read(void)
{
    int res, cnt = 0;
    memset(HID_REPORT, 0, HID_REPORT_SIZE);
    while (cnt < HID_REPORT_SIZE) {
        res = hid_read(HID_HANDLE, HID_REPORT + cnt, HID_REPORT_SIZE);
        if (res < 0) {
            printf("ERROR: Unable to read report.\n");
            return;
        }
        cnt += res;
    }
    utils_decrypt_report((char *)HID_REPORT);
    //printf("received:  >>%s<<\n", utils_read_decrypted_report());
}


static void api_hid_send_len(const char *cmd, int cmdlen)
{
    memset(HID_REPORT, 0, HID_REPORT_SIZE);
    memcpy(HID_REPORT, cmd, cmdlen );
    hid_write(HID_HANDLE, (unsigned char *)HID_REPORT, HID_REPORT_SIZE);
}


static void api_hid_send(const char *cmd)
{
    api_hid_send_len(cmd, strlens(cmd));
}


static void api_hid_send_encrypt(const char *cmd)
{
    int enc_len;
    char *enc = aes_cbc_b64_encrypt((const unsigned char *)cmd, strlens(cmd), &enc_len,
                                    PASSWORD_STAND);
    api_hid_send_len(enc, enc_len);
    free(enc);
}
#endif


static void api_send_cmd(const char *command, PASSWORD_ID id)
{
    memset(command_sent, 0, sizeof(command_sent));
    if (command) {
        memcpy(command_sent, command, strlens(command));
    }
    if (!TEST_LIVE_DEVICE) {
        utils_send_cmd(command, id);
    }
#ifndef CONTINUOUS_INTEGRATION
    else if (id == PASSWORD_NONE) {
        api_hid_send(command);
        api_hid_read();
    } else {
        api_hid_send_encrypt(command);
        api_hid_read();
    }
#endif
}


static void api_format_send_cmd(const char *cmd, const char *val, PASSWORD_ID id)
{
    char command[COMMANDER_REPORT_SIZE] = {0};
    strcpy(command, "{\"");
    strcat(command, cmd);
    strcat(command, "\": ");
    if (val[0] == '{') {
        strcat(command, val);
    } else {
        strcat(command, "\"");
        strcat(command, val);
        strcat(command, "\"");
    }
    strcat(command, "}");
    api_send_cmd(command, id);
}


static void api_reset_device(void)
{
    if (!TEST_LIVE_DEVICE) {
        commander_force_reset();
    } else {
        api_format_send_cmd(cmd_str(CMD_reset), attr_str(ATTR___ERASE__), PASSWORD_NONE);
    }
}


static const char *api_read_value(int cmd)
{
    static char value[HID_REPORT_SIZE];
    memset(value, 0, sizeof(value));

    yajl_val json_node = yajl_tree_parse(utils_read_decrypted_report(), NULL, 0);
    if (json_node && YAJL_IS_OBJECT(json_node)) {
        const char *path[] = { cmd_str(cmd), NULL };
        yajl_val v = yajl_tree_get(json_node, path, yajl_t_string);
        snprintf(value, sizeof(value), "%s", v->u.string);
    }

    yajl_tree_free(json_node);
    return value;
}


static char *api_read_value_decrypt(int cmd, PASSWORD_ID id)
{
    const char *val = api_read_value(cmd);
    static char val_dec[HID_REPORT_SIZE];
    memset(val_dec, 0, sizeof(val_dec));

    int decrypt_len;
    char *dec = aes_cbc_b64_decrypt((const unsigned char *)val, strlens(val),
                                    &decrypt_len, id);

    snprintf(val_dec, HID_REPORT_SIZE, "%.*s", decrypt_len, dec);
    free(dec);
    return val_dec;
}


static void tests_seed_xpub_backup(void)
{
    char xpub0[112], xpub1[112], *echo, seed_c[512], seed_b[512], back[512];
    char filename[] = "tests_backup.txt";
    char keypath[] = "m/44\'/0\'/";
    char seed_xpriv[] =
        "{\"source\":\"xprv9s21ZrQH143K2MkmL8hdyZk5uwTPEqkwS72jXDt5DGRtUVrfYiAvAnGmxmP3J5Z3BG5uQcy5UYUMDsqisyXEDNCG2uzixsckhnfCrJxKVme\"}";
    char seed_xpriv_wrong_len[] =
        "{\"source\":\"xprv9s21ZrQH143K2MkmL8hdyZk5uwTPEqkwS72jXDt5DGRtUVrfYiAvAnGmxmP3J5Z3BG5uQcy5UYUMDsqisyXEDNCG2uzixsckhnfCrJxKVm\"}";
    char mnemo_ok[] =
        "{\"source\": \"silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble\"}";
    char mnemo_err[] =
        "{\"source\": \"ilent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble\"}";
    char mnemo_extra_word[] =
        "{\"source\": \"silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble error\"}";
    char mnemo_long_salt[SALT_LEN_MAX + 2];
    memset(mnemo_long_salt, 'a', sizeof(mnemo_long_salt));
    mnemo_long_salt[SALT_LEN_MAX + 1] = '\0';

    const char **salt, **cipher, **run, **mnemo;
    static const char *options[] = {
        // run  salt              encrypt       mnemonic
        "y",    "",               NULL,         NULL,
        "y",    "",               "no",         NULL,
        "y",    "",               "yes",        NULL,
        "y",    NULL,             NULL,         NULL,
        "y",    NULL,             "no",         NULL,
        "y",    NULL,             "yes",        NULL,
        "y",    "Digital Bitbox", NULL,         NULL,
        "y",    "Digital Bitbox", "no",         NULL,
        "y",    "Digital Bitbox", "yes",        NULL,
        "y",    NULL,             NULL,         "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    NULL,             "no",         "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    "",               "no",         "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    "Digital Bitbox", "no",         "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    NULL,             "yes",        "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    "",               "yes",        "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        "y",    "Digital Bitbox", "yes",        "silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble",
        NULL,   NULL,             NULL,         NULL,
    };
    run = options;
    salt = options + 1;
    cipher = options + 2;
    mnemo = options + 3;

    while (*run) {
        memset(seed_c, 0, sizeof(seed_c));
        memset(seed_b, 0, sizeof(seed_b));
        memset(back, 0, sizeof(back));

        strcpy(seed_c, "{\"source\":\"");
        if (*mnemo) {
            strcat(seed_c, *mnemo);
        } else {
            strcat(seed_c, attr_str(ATTR_create));
        }
        strcat(seed_c, "\"");

        strcpy(seed_b, "{\"source\":\"");
        strcat(seed_b, filename);
        strcat(seed_b, "\"");

        strcpy(back, "{\"filename\":\"");
        strcat(back, filename);
        strcat(back, "\"");

        if (*cipher) {
            strcat(seed_b, ",\"decrypt\":\"");
            strcat(seed_b, *cipher);
            strcat(seed_b, "\"");
            strcat(back, ",\"encrypt\":\"");
            strcat(back, *cipher);
            strcat(back, "\"");
        }
        if (*salt) {
            strcat(seed_c, ",\"salt\":\"");
            strcat(seed_c, *salt);
            strcat(seed_c, "\"");
            strcat(seed_b, ",\"salt\":\"");
            strcat(seed_b, *salt);
            strcat(seed_b, "\"");
        }

        strcat(seed_c, "}");
        strcat(seed_b, "}");
        strcat(back, "}");

        // erase
        api_reset_device();

        api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        memset(xpub0, 0, sizeof(xpub0));
        memset(xpub1, 0, sizeof(xpub1));
        api_format_send_cmd(cmd_str(CMD_backup), back, PASSWORD_STAND);
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_MASTER));

        // create
        api_format_send_cmd(cmd_str(CMD_xpub), keypath, PASSWORD_STAND);
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

        api_format_send_cmd(cmd_str(CMD_seed), seed_c, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        api_format_send_cmd(cmd_str(CMD_xpub), keypath, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        memcpy(xpub0, api_read_value(CMD_xpub), sizeof(xpub0));
        u_assert_str_not_eq(xpub0, xpub1);

        if (!TEST_LIVE_DEVICE) {
            echo = api_read_value_decrypt(CMD_echo, PASSWORD_VERIFY);
            u_assert_str_eq(xpub0, echo);
        }

        // backup
        api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_erase), PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
        if (TEST_LIVE_DEVICE) {
            u_assert_str_has_not(utils_read_decrypted_report(), flag_msg(DBB_ERR_SD_ERASE));
        }

        api_format_send_cmd(cmd_str(CMD_backup), back, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        // erase
        api_reset_device();

        api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        // load backup default
        api_format_send_cmd(cmd_str(CMD_seed), seed_b, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        api_format_send_cmd(cmd_str(CMD_xpub), keypath, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        memcpy(xpub1, api_read_value(CMD_xpub), sizeof(xpub1));
        if (!TEST_LIVE_DEVICE) {
            echo = api_read_value_decrypt(CMD_echo, PASSWORD_VERIFY);
            u_assert_str_eq(xpub0, echo);
        }

        // check xpubs
        u_assert_str_eq(xpub0, xpub1);

        // check backup list and erase
        api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_list), PASSWORD_STAND);
        u_assert_str_has(utils_read_decrypted_report(), filename);

        api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_erase), PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_list), PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), filename);

        run += 4;
        salt += 4;
        cipher += 4;
        mnemo += 4;
    }

    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_seed), mnemo_err, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SEED_INVALID));

    api_format_send_cmd(cmd_str(CMD_seed), mnemo_extra_word, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SEED_INVALID));

    memset(seed_c, 0, sizeof(seed_c));
    sprintf(seed_c, "%s%s%s", "{\"source\":\"create\", \"salt\":\"", mnemo_long_salt, "\"}");

    api_format_send_cmd(cmd_str(CMD_seed), seed_c, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SEED_SALT_LEN));

    api_format_send_cmd(cmd_str(CMD_seed), mnemo_ok, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // test keypath
    api_format_send_cmd(cmd_str(CMD_xpub), "m/111", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), "\"xpub\":");

    api_format_send_cmd(cmd_str(CMD_xpub), "111", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "/111", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "m", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "m111", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "m/a", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "m/!", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    api_format_send_cmd(cmd_str(CMD_xpub), "m/-111", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_CHILD));

    // test xpriv seed
    memset(xpub0, 0, sizeof(xpub0));
    memset(xpub1, 0, sizeof(xpub1));

    api_format_send_cmd(cmd_str(CMD_xpub), "m/0", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    memcpy(xpub0, api_read_value(CMD_xpub), sizeof(xpub0));

    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_seed), seed_xpriv, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_seed), seed_xpriv_wrong_len, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SEED_INVALID));

    api_format_send_cmd(cmd_str(CMD_xpub), "m/0", PASSWORD_STAND);
    memcpy(xpub1, api_read_value(CMD_xpub), sizeof(xpub0));
    u_assert_str_eq(xpub0, xpub1);
}


static void tests_random(void)
{
    char number0[32] = {0};
    char number1[32] = {0};

    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_random), attr_str(ATTR_pseudo), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    memcpy(number0, api_read_value(CMD_random), sizeof(number0));

    api_format_send_cmd(cmd_str(CMD_random), attr_str(ATTR_pseudo), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    memcpy(number1, api_read_value(CMD_random), sizeof(number1));
    u_assert_str_not_eq(number0, number1);

    api_format_send_cmd(cmd_str(CMD_random), attr_str(ATTR_true), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    memcpy(number0, api_read_value(CMD_random), sizeof(number0));

    api_format_send_cmd(cmd_str(CMD_random), attr_str(ATTR_true), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    memcpy(number1, api_read_value(CMD_random), sizeof(number1));
    u_assert_str_not_eq(number0, number1);

    api_format_send_cmd(cmd_str(CMD_random), "invalid_cmd", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));
}


static void tests_name(void)
{
    char name0[] = "name0";
    char name1[] = "name1";

    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_name), name0, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_eq(name0, api_read_value(CMD_name));

    api_format_send_cmd(cmd_str(CMD_name), name1, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_eq(name1, api_read_value(CMD_name));

    api_format_send_cmd(cmd_str(CMD_name), "", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_eq(name1, api_read_value(CMD_name));
}


static void tests_device(void)
{
    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_led), attr_str(ATTR_toggle), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_led), "invalid_cmd", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_led), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_seed), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_xpub), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_reset), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_backup), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_random), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_device), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_verifypass), "", PASSWORD_STAND);

    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_sign), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));


    api_format_send_cmd(cmd_str(CMD_seed), "{\"source\":\"create\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_verifypass), attr_str(ATTR_create), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"backup\":{\"filename\":\"b.txt\"}}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_lock), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_seed), "{\"source\":\"create\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));

    api_format_send_cmd(cmd_str(CMD_verifypass), attr_str(ATTR_create), PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));

    api_send_cmd("{\"backup\":{\"filename\":\"b.txt\"}}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));

    api_format_send_cmd("invalid_cmd", "invalid_cmd", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_device), "invalid_cmd", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_serial), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_version), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_version));

    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_info), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_serial));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_version));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_name));
    u_assert_str_has(utils_read_decrypted_report(), "\"seeded\":true");
    u_assert_str_has(utils_read_decrypted_report(), "\"lock\":true");

    api_format_send_cmd(cmd_str(CMD_reset), attr_str(ATTR___ERASE__), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_info), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_serial));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_version));
    u_assert_str_has(utils_read_decrypted_report(), attr_str(ATTR_name));
    u_assert_str_has(utils_read_decrypted_report(), "\"seeded\":false");
    u_assert_str_has(utils_read_decrypted_report(), "\"lock\":false");

    api_format_send_cmd(cmd_str(CMD_reset), attr_str(ATTR___ERASE__), PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
}


static void tests_input(void)
{
    api_reset_device();

    if (!TEST_LIVE_DEVICE) {
        api_send_cmd("", PASSWORD_NONE);
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_NO_INPUT));

        api_send_cmd(NULL, PASSWORD_NONE);
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_NO_INPUT));
    }

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"name\"}", PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_DECRYPT));

    api_send_cmd("{\"name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("\"name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_format_send_cmd(cmd_str(CMD_name), "avoidreset", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name: \"name\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{\"name\": \"name}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_format_send_cmd(cmd_str(CMD_name), "avoidreset", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"name\"", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{\"name\": \"name\", }", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{\"name\": \"name\", \"name\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{\"name\": \"name\", \"name\": }", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_JSON_PARSE));

    api_send_cmd("{\"name\": \"name\", \"name\": \"name\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_MULT_CMD));

    api_format_send_cmd(cmd_str(CMD_name), "avoidreset", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": null}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"na\\nme\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"na\\r\\\\ \\/ \\f\\b\\tme\\\"\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_send_cmd("{\"name\": \"na\\u0066me\\ufc00\\u0000\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    int i;
    for (i = 0; i < COMMANDER_MAX_ATTEMPTS - 1; i++) {
        api_send_cmd("{\"name\": \"name\"}", PASSWORD_NONE);
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_DECRYPT));
        u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_WARN_RESET));
    }
    api_send_cmd("{\"name\": \"name\"}", PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_RESET));
}


static void tests_password(void)
{
    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_name), "", PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_NO_PASSWORD));

    api_format_send_cmd(cmd_str(CMD_password), "123", PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_PASSWORD_LEN));

    api_format_send_cmd(cmd_str(CMD_password), "", PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_PASSWORD_LEN));

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_DECRYPT));

    api_format_send_cmd(cmd_str(CMD_password), "123", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_PASSWORD_LEN));

    char ecdh[] =
        "{\"ecdh\":\"028d3bce812ac027fdea0e4ad98b2549a90bb9aa996396eec6bb1a8ed56e6976b8\"}";
    api_format_send_cmd(cmd_str(CMD_verifypass), ecdh, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_ecdh));
}


static void tests_echo_2FA(void)
{
    char hash_sign[] =
        "{\"type\":\"meta\", \"meta\":\"hash\", \"data\":[{\"keypath\":\"m/\", \"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}] }";
    char hash_sign2[] =
        "{\"type\":\"meta\", \"meta\":\"hash\", \"data\":[{\"keypath\":\"m/\", \"hash\":\"ffff456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}] }";
    char hash_sign3[] =
        "{\"type\":\"meta\", \"meta\":\"hash\", \"data\":[{\"keypath\":\"m/\", \"hash\":\"456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}] }";

    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign2, PASSWORD_STAND);
    u_assert_int_eq((strstr(utils_read_decrypted_report(), cmd_str(CMD_echo)) ||
                     strstr(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_MASTER))), 1);

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_MASTER));

    api_format_send_cmd(cmd_str(CMD_seed), "{\"source\":\"create\"}", PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // test verifypass
    api_format_send_cmd(cmd_str(CMD_verifypass), attr_str(ATTR_create), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_erase), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_list), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), VERIFYPASS_FILENAME);

    api_format_send_cmd(cmd_str(CMD_verifypass), attr_str(ATTR_export), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_list), PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), VERIFYPASS_FILENAME);

    api_format_send_cmd(cmd_str(CMD_verifypass), "invalid_cmd", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    // test echo
    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_2FA));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    // test hash length
    api_format_send_cmd(cmd_str(CMD_sign), hash_sign3, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign3, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SIGN_HASH_LEN));

    // test locked
    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_lock), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_sign, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));

    api_format_send_cmd(cmd_str(CMD_seed), "{\"source\":\"create\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));

    api_format_send_cmd(cmd_str(CMD_verifypass), attr_str(ATTR_export), PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));

    api_format_send_cmd(cmd_str(CMD_backup), attr_str(ATTR_list), PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_LOCKED));
}


#ifdef ECC_USE_UECC_LIB
const char hash_1_input[] =
    "41fa23804d6fe53c296a5ac93a2e21719f9c6f20b2645d04d047150087cd812acedefc98a7d87f1379efb84dc684ab947dc4e583d2c3e1d50f372012b3d8c95e";
const char hash_2_input_1[] =
    "031145194147dada762c77ff85fd5cb493f56596de20f235c35507cd72716134e49cbe288c46f90da19bd1552c406e64425169520d433113a78b480ca3c5d340";
const char hash_2_input_2[] =
    "d4464e76d679b062ec867c7ebb961fc27cab810ccd6198bd993acef5a84273bcf16b256cfd77768df1bbce20333904c5e93873cee26ac446afdd62a5394b73ad";
#else
const char hash_1_input[] =
    "41fa23804d6fe53c296a5ac93a2e21719f9c6f20b2645d04d047150087cd812a31210367582780ec861047b2397b546a3ce9f762dc84be66b09b3e7a1c5d77e3";
const char hash_2_input_1[] =
    "031145194147dada762c77ff85fd5cb493f56596de20f235c35507cd727161341b6341d773b906f25e642eaad3bf919a785d7394a2056f28184716802c706e01";
const char hash_2_input_2[] =
    "d4464e76d679b062ec867c7ebb961fc27cab810ccd6198bd993acef5a84273bc0e94da93028889720e4431dfccc6fb38d1766917ccdddbf50ff4fbe796eacd94";
#endif


static void tests_sign_meta(void)
{
    char one_input[] =
        "{\"type\":\"meta\", \"meta\":\"_meta_data_\", \"data\":[{\"hash\":\"c6fa4c236f59020ec8ffde22f85a78e7f256e94cd975eb5199a4a5cc73e26e4a\", \"keypath\":\"m/44'/0'/0'/1/7\"}]}";
    char pubkey_1_input[] =
        "02721be181276eebdc4dd29dce180afa7c6a8199fb5f4c09f2e03b8e4193f22ce5";

    /*
      raw_tx = 0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff0298080000000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000
      sha256(sha256(hex2byte(raw_tx))) = c6fa4c236f59020ec8ffde22f85a78e7f256e94cd975eb5199a4a5cc73e26e4a
    */

    char two_inputs[] =
        "{\"type\":\"meta\", \"meta\":\"_meta_data_\", \"data\":[{\"hash\":\"c12d791451bb41fd4b5145bcef25f794ca33c0cf4fe9d24f956086c5aa858a9d\", \"keypath\":\"m/44'/0'/0'/1/8\"},{\"hash\":\"3dfc3b1ed349e9b361b31c706fbf055ebf46ae725740f6739e2dfa87d2a98790\", \"keypath\":\"m/44'/0'/0'/0/5\"}]}";
    char pubkey_2_input_1[] =
        "0367d99d26d908bc11adaf05e1c18072b67e825f27dfadd504b013bafaa0f364a6";
    char pubkey_2_input_2[] =
        "032ab901fe42a05e970e6d5c701b4d7a6db33b0fa7daaaa709ebe755daf9dfe0ec";
    /*
      raw_tx = 01000000029ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acffffffff9ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f0100000000ffffffff01c8000000000000001976a914584495bb22f4cb66cd47f2255cbc7178c6f3caeb88ac0000000001000000
      sha256(sha256(hex2byte(raw_tx))) = 3dfc3b1ed349e9b361b31c706fbf055ebf46ae725740f6739e2dfa87d2a98790

      raw_tx = 01000000029ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f0000000000ffffffff9ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f010000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788acffffffff01c8000000000000001976a914584495bb22f4cb66cd47f2255cbc7178c6f3caeb88ac0000000001000000
      sha256(sha256(hex2byte(raw_tx))) = c12d791451bb41fd4b5145bcef25f794ca33c0cf4fe9d24f956086c5aa858a9d
    */

    char overflow[] =
        "{\"type\":\"meta\", \"meta\":\"_meta_data_\", \"data\": [{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"},{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}, {\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/\"}]}";

    char checkpub[] =
        "{\"type\":\"meta\", \"meta\":\"<<meta data here>>\", \"data\": [{\"hash\":\"c6fa4c236f59020ec8ffde22f85a78e7f256e94cd975eb5199a4a5cc73e26e4a\", \"keypath\":\"m/44p\"},{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/44p\"}], \"checkpub\":[{\"address\":\"0000000000000000000000000000000000\", \"keypath\":\"m/44p/0p/0p/1/8\"},{\"address\":\"1Q5pR8cZPtqyQf3xCVo25n9UDaiTWJZ4DC\", \"keypath\":\"m/44p/0p/0p/1/8\"}]}";
    char check_1[] = "\"address\":\"0000000000000000000000000000000000\", \"present\":false";
    char check_2[] = "\"address\":\"1Q5pR8cZPtqyQf3xCVo25n9UDaiTWJZ4DC\", \"present\":true";
    char check_sig_1[] =
        "72b2aa04281c7c2d79cb6e08168865fcd492d4d2d36acb3e46a185368c9ee00c1cbae958a8092a3689088d05ec5c0f58db65612f7bf4b34b0a9b3a34af9ad8be";
    char check_sig_2[] =
        "1ce2c6e97a86399417a782b9fc7b926b7d58e309adf49e86380ebc02fd3823ad2fa595eb18178f062c2cba1d252992622c5bde536de4b09e52976a0eeedeac01";
    char check_pubkey[] =
        "02793907771bab33d3fa7e4d7ee7c355067a623265926c46fc433a83c11dceb29e";

    char checkpub_wrong_addr_len[] =
        "{\"type\":\"meta\", \"meta\":\"<<meta data here>>\", \"data\": [{\"hash\":\"c6fa4c236f59020ec8ffde22f85a78e7f256e94cd975eb5199a4a5cc73e26e4a\", \"keypath\":\"m/44p\"},{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/44p\"}], \"checkpub\":[{\"address\":\"00\", \"keypath\":\"m/44p/0p/0p/1/8\"},{\"address\":\"1Q5pR8cZPtqyQf3xCVo25n9UDaiTWJZ4DC\", \"keypath\":\"m/44p/0p/0p/1/8\"}]}";

    char checkpub_missing_parameter[] =
        "{\"type\":\"meta\", \"meta\":\"<<meta data here>>\", \"data\": [{\"hash\":\"c6fa4c236f59020ec8ffde22f85a78e7f256e94cd975eb5199a4a5cc73e26e4a\", \"keypath\":\"m/44p\"},{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/44p\"}], \"checkpub\":[{\"address\":\"1Q5pR8cZPtqyQf3xCVo25n9UDaiTWJZ4DC\"}]}";


    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // seed
    char seed[] =
        "{\"source\":\"bronze science bulk conduct fragile genius bone miracle twelve grab maid peace observe illegal exchange space another usage hunt donate feed swarm arrest naive\"}";
    api_format_send_cmd(cmd_str(CMD_seed), seed, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // missing parameters
    api_format_send_cmd(cmd_str(CMD_sign), "{\"type\":\"meta\"}", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_sign), checkpub_missing_parameter, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_sign),
                        "{\"type\":\"meta\", \"data\":[{\"keypath\":\"m/\"}]}",
                        PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_sign),
                        "{\"type\":\"meta\", \"data\":[{\"hash\":\"empty\"}]}",
                        PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    // data is not an array
    api_format_send_cmd(cmd_str(CMD_sign),
                        "{\"type\":\"meta\", \"data\":{\"hash\":\"empty\"}}",
                        PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));


    // change output after echo (MITM attack)
    char hash_1[] =
        "{\"type\":\"meta\", \"data\":[{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/44'/0'/0'/1/7\"}]}";
    char hash_2[] =
        "{\"type\":\"meta\", \"data\":[{\"hash\":\"456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123\", \"keypath\":\"m/44'/0'/0'/1/7\"}]}";

    api_format_send_cmd(cmd_str(CMD_sign), hash_1, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), hash_2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));


    // check buffer overflow prevention
    api_format_send_cmd(cmd_str(CMD_sign), overflow, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), overflow, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_REPORT_BUF));


    // sign using one input
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), "_meta_data_");
        u_assert_str_has(utils_read_decrypted_report(), "m/44'/0'/0'/1/7");
        u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    }

    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), hash_1_input);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_1_input);

    // sign using two inputs
    api_format_send_cmd(cmd_str(CMD_sign), two_inputs, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), "_meta_data_");
        u_assert_str_has(utils_read_decrypted_report(), "m/44'/0'/0'/1/8");
        u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    }

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
    u_assert_str_has(utils_read_decrypted_report(), hash_2_input_1);
    u_assert_str_has(utils_read_decrypted_report(), hash_2_input_2);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_1);
    u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_2);


    // test checkpub
    api_format_send_cmd(cmd_str(CMD_sign), checkpub, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), "\"meta\":");
        u_assert_str_has(utils_read_decrypted_report(), check_1);
        u_assert_str_has(utils_read_decrypted_report(), check_2);
    }

    api_format_send_cmd(cmd_str(CMD_sign), checkpub, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
    u_assert_str_has(utils_read_decrypted_report(), check_sig_1);
    u_assert_str_has(utils_read_decrypted_report(), check_sig_2);
    u_assert_str_has(utils_read_decrypted_report(), check_pubkey);

    api_format_send_cmd(cmd_str(CMD_sign), checkpub_wrong_addr_len, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SIGN_ADDR_LEN));


    // lock to get 2FA PINs
    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_lock), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));


    // sign using one input
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        memory_write_aeskey(api_read_value(CMD_pin), 4, PASSWORD_2FA);
        u_assert_str_has(utils_read_decrypted_report(), "_meta_data_");
        u_assert_str_has(utils_read_decrypted_report(), "m/44'/0'/0'/1/7");
        u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pin));
    }

    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), hash_1_input);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), pubkey_1_input);
    }


    // sign using two inputs
    api_format_send_cmd(cmd_str(CMD_sign), two_inputs, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        memory_write_aeskey(api_read_value(CMD_pin), 4, PASSWORD_2FA);
        u_assert_str_has(utils_read_decrypted_report(), "_meta_data_");
        u_assert_str_has(utils_read_decrypted_report(), "m/44'/0'/0'/1/8");
        u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    }

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), hash_2_input_1);
        u_assert_str_has(utils_read_decrypted_report(), hash_2_input_2);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_1);
        u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_2);
    }
}


static void tests_sign(void)
{
    char one_input[] =
        "{\"type\":\"transaction\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff0298080000000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char two_inputs_1[] =
        "{\"type\":\"transaction\", \"data\":\"01000000029ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acffffffff9ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f0100000000ffffffff01c8000000000000001976a914584495bb22f4cb66cd47f2255cbc7178c6f3caeb88ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/0/5\", \"change_keypath\":\"\"}";

    char two_inputs_2[] =
        "{\"type\":\"transaction\", \"data\":\"01000000029ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f0000000000ffffffff9ecf1f09baed314ee1cc37ee2236dca5f71f7dddc83a2a1b6358e739ac68c43f010000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788acffffffff01c8000000000000001976a914584495bb22f4cb66cd47f2255cbc7178c6f3caeb88ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/1/8\", \"change_keypath\":\"\"}";

    char missing_keypath[] =
        "{\"type\":\"transaction\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff0298080000000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char missing_data[] =
        "{\"type\":\"transaction\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char missing_outputs[] =
        "{\"type\":\"transaction\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char wrong_type[] =
        "{\"type\":\"invalid\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff0298080000000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char wrong_change_keypath[] =
        "{\"type\":\"transaction\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acffffffff0298080000000000001976a91452922e52d08a2c1f1e4120803e56363fd7a8195188acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/\"}";

    char change_output_mitm[] =
        "{\"type\":\"transaction\", \"data\":\"0100000001e4b8a097d6d5cd351f69d9099e277b8a1c39a219991a4e5f9f86805faf649899010000001976a91488e6399fab42b2ea637da283dd87e7af4862e10c88acffffffff0298080000000000001976a91488e6399fab42b2ea637da283dd87e70f4862e10c88acb83d0000000000001976a914fd342347278e14013d17d53ed3c4aa7bf27eceb788ac0000000001000000\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char pubkey_1_input[] =
        "02721be181276eebdc4dd29dce180afa7c6a8199fb5f4c09f2e03b8e4193f22ce5";
    char pubkey_2_input_1[] =
        "0367d99d26d908bc11adaf05e1c18072b67e825f27dfadd504b013bafaa0f364a6";
    char pubkey_2_input_2[] =
        "032ab901fe42a05e970e6d5c701b4d7a6db33b0fa7daaaa709ebe755daf9dfe0ec";

    char script_1_input[] = "76a91452922e52d08a2c1f1e4120803e56363fd7a8195188ac";
    char script_2_inputs[] = "76a914584495bb22f4cb66cd47f2255cbc7178c6f3caeb88ac";

    char type_hash[] =
        "{\"type\":\"hash\", \"data\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", \"keypath\":\"m/44'/0'/0'/1/7\", \"changekeypath\":\"m/44'/0'/0'/1/8\"}";

    char seed[] =
        "{\"source\":\"bronze science bulk conduct fragile genius bone miracle twelve grab maid peace observe illegal exchange space another usage hunt donate feed swarm arrest naive\"}";


    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // signing before seeded
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SIGN_DESERIAL));

    // seed
    api_format_send_cmd(cmd_str(CMD_seed), seed, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // missing parameters
    api_format_send_cmd(cmd_str(CMD_sign), missing_data, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), missing_outputs, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SIGN_DESERIAL));
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), missing_keypath, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), missing_keypath, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    // wrong type
    api_format_send_cmd(cmd_str(CMD_sign), wrong_type, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    // wrong change keypath
    api_format_send_cmd(cmd_str(CMD_sign), wrong_change_keypath, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_SIGN_DESERIAL));

    // change output after echo (MITM attack)
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), change_output_mitm, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    u_assert_str_has_not(utils_read_decrypted_report(), cmd_str(CMD_pubkey));

    // sign using one input
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_value));
        u_assert_str_has(utils_read_decrypted_report(), "2200");
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_script));
        u_assert_str_has(utils_read_decrypted_report(), script_1_input);
    }

    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
    u_assert_str_has(utils_read_decrypted_report(), hash_1_input);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_1_input);

    // sign using two inputs
    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_1, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_value));
        u_assert_str_has(utils_read_decrypted_report(), "200");
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_script));
        u_assert_str_has(utils_read_decrypted_report(), script_2_inputs);
    }

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_1, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
    u_assert_str_has(utils_read_decrypted_report(), hash_2_input_1);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_1);

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
    u_assert_str_has(utils_read_decrypted_report(), hash_2_input_2);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_2);

    // sign hash
    api_format_send_cmd(cmd_str(CMD_sign), type_hash, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));

    api_format_send_cmd(cmd_str(CMD_sign), type_hash, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
    u_assert_str_has(utils_read_decrypted_report(), pubkey_1_input);

    // lock to get 2FA PINs
    api_format_send_cmd(cmd_str(CMD_device), attr_str(ATTR_lock), PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    // sign using one input
    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        memory_write_aeskey(api_read_value(CMD_pin), 4, PASSWORD_2FA);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_value));
        u_assert_str_has(utils_read_decrypted_report(), "2200");
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_script));
        u_assert_str_has(utils_read_decrypted_report(), script_1_input);
    }

    api_format_send_cmd(cmd_str(CMD_sign), one_input, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), hash_1_input);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), pubkey_1_input);
    }

    // sign using two inputs
    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_1, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_echo));
    if (!TEST_LIVE_DEVICE) {
        memory_write_aeskey(api_read_value(CMD_pin), 4, PASSWORD_2FA);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_value));
        u_assert_str_has(utils_read_decrypted_report(), "200");
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_script));
        u_assert_str_has(utils_read_decrypted_report(), script_2_inputs);
    }

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_1, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), hash_2_input_1);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_1);
    }

    api_format_send_cmd(cmd_str(CMD_sign), two_inputs_2, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_2FA));
    if (!TEST_LIVE_DEVICE) {
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_sign));
        u_assert_str_has(utils_read_decrypted_report(), hash_2_input_2);
        u_assert_str_has(utils_read_decrypted_report(), cmd_str(CMD_pubkey));
        u_assert_str_has(utils_read_decrypted_report(), pubkey_2_input_2);
    }
}


// test vectors generated from Python 2.7 code using aes, base64, and hashlib imports
static void tests_aes_cbc(void)
{
    const char **plainp, **cipherp;

    char encrypt[] = "{\"type\":\"encrypt\", \"data\":\"";
    char decrypt[] = "{\"type\":\"decrypt\", \"data\":\"";
    char password[] = "{\"type\":\"password\", \"data\":\"passwordpassword\"}";
    char xpub[] = "{\"type\":\"xpub\", \"data\":\"m/0'\"}";
    char enc[COMMANDER_REPORT_SIZE * 2], dec[COMMANDER_REPORT_SIZE * 2];
    memset(enc, 0, sizeof(enc));
    memset(dec, 0, sizeof(dec));

    static const char *aes_vector[] = {
        // plain                cipher (for 'passwordpassword')
        "digital bitbox", "mheIJghfKiPxQpvqbbRCZnTkbMd+BdRf+1jDAjk9h2Y=",
        "Satoshi Nakamoto", "28XHUwA+/5zHeSIxt1Ioaifl/BqWsTow1hrzJJ7p91EgYbw6MwzFMlLOWq22fUsw",
        "All those moments will be lost in time, like tears in rain. Time to die...", "qjfyIWCoY8caehZFoZStmtDz6FaKYCaCrJXyiF6I2LwnLPVV9oGv9NtJ7aVXAICeP0Q2Agh0oPlbBLKfjkdtZGuwV/tya7KcIl1ieC/276JwRl2+XdkK3uBb2Yrljl4T",
        "There is a computer disease that anybody who works with computers knows about. It's a very serious disease and it interferes completely with the work. The trouble with computers is that you 'play' with them!", "biR4Ce1vnvrYAOQRwO+bW4aXiySH4plHVc9LlN8hJAb/q6Tw0x6aI+A7EeOF5a11EPTjJ454nREZ9S4nIBwlGDto2GrEq+TwQOpKb/YU1VxeGGlFLg8comVnVSPmNQ1WNX/E5bnNX8osgF69QFxOgaPzfLdKGr4isUBVO3BlOPV4oUmIUc7+DC5PwabWV4XrxLQzzw79KRxL3iPk4Tbk3CDxDBgE5Z7HlvZfTM5J9d7majdQTMtHYP7d1MJZblyTkB1R7DemQhf7xHllkSXwHattstz/d1NmgGQXHlISoPs=",
        0, 0,
    };

    char seed[] =
        "{\"source\": \"silent answer fury celery kitten amused pudding struggle infant cake jealous ready curve more fame gown leave then client biology unusual lazy potato bubble\"}";


    api_reset_device();

    api_format_send_cmd(cmd_str(CMD_password), tests_pwd, PASSWORD_NONE);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    memcpy(dec, decrypt, strlens(decrypt));
    strcat(dec, "password not set error\"}");
    api_format_send_cmd(cmd_str(CMD_aes256cbc), dec, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_NO_PASSWORD));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), xpub, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_KEY_MASTER));

    api_format_send_cmd(cmd_str(CMD_seed), seed, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), xpub, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), password, PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), "type", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), "", PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));


    memcpy(enc, encrypt, strlens(encrypt));
    memset(enc + strlens(encrypt), 'a', AES_DATA_LEN_MAX + 1);
    strcat(enc, "\"}");
    api_format_send_cmd(cmd_str(CMD_aes256cbc), enc, PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_DATA_LEN));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), "{\"type\":\"\", \"data\":\"\"}",
                        PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_INVALID_CMD));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), "{\"type\":\"encrypt\", \"data\":\"\"}",
                        PASSWORD_STAND);
    u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

    api_format_send_cmd(cmd_str(CMD_aes256cbc), "{\"type\":\"decrypt\", \"data\":\"\"}",
                        PASSWORD_STAND);
    u_assert_str_has(utils_read_decrypted_report(), flag_msg(DBB_ERR_IO_DECRYPT));

    plainp = aes_vector;
    cipherp = aes_vector + 1;
    while (*plainp && *cipherp) {

        // check decryption
        memset(dec, 0, sizeof(dec));
        memcpy(dec, decrypt, strlens(decrypt));
        memcpy(dec + strlens(decrypt), *cipherp, strlens(*cipherp));
        strcat(dec, "\"}");

        api_format_send_cmd(cmd_str(CMD_aes256cbc), dec, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
        u_assert_mem_eq(*plainp, api_read_value(CMD_aes256cbc), strlens(*plainp));

        // check encryption by encrypting then decrypting
        memset(enc, 0, sizeof(enc));
        memcpy(enc, encrypt, strlens(encrypt));
        memcpy(enc + strlens(encrypt), *plainp, strlens(*plainp));
        strcat(enc, "\"}");

        api_format_send_cmd(cmd_str(CMD_aes256cbc), enc, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));

        const char *e = api_read_value(CMD_aes256cbc);

        memset(dec, 0, sizeof(dec));
        memcpy(dec, decrypt, strlens(decrypt));
        memcpy(dec + strlens(decrypt), e, strlens(e));
        strcat(dec, "\"}");

        api_format_send_cmd(cmd_str(CMD_aes256cbc), dec, PASSWORD_STAND);
        u_assert_str_has_not(utils_read_decrypted_report(), attr_str(ATTR_error));
        u_assert_mem_eq(*plainp, api_read_value(CMD_aes256cbc), strlens(*plainp));

        plainp += 2;
        cipherp += 2;
    }
}


static void run_utests(void)
{
    u_run_test(tests_echo_2FA);
    u_run_test(tests_aes_cbc);
    u_run_test(tests_sign_meta);
    u_run_test(tests_sign);
    u_run_test(tests_name);
    u_run_test(tests_password);
    u_run_test(tests_random);
    u_run_test(tests_device);
    u_run_test(tests_input);
    u_run_test(tests_seed_xpub_backup);

    if (!U_TESTS_FAIL) {
        printf("\nALL %i TESTS PASSED\n\n", U_TESTS_RUN);
    } else {
        printf("\n%i of %i TESTS PASSED\n\n", U_TESTS_RUN - U_TESTS_FAIL, U_TESTS_RUN);
    }
}


int main(void)
{
    // Test the C code API
    TEST_LIVE_DEVICE = 0;
    random_init();
    ecc_context_init();
    memory_setup();
    printf("\n\nInternal API Result:\n");
    run_utests();

#ifndef CONTINUOUS_INTEGRATION
    // Live test of the HID API
    // Requires the hidapi library to be installed:
    //     http://www.signal11.us/oss/hidapi/
    TEST_LIVE_DEVICE = 1;
    memory_write_aeskey(tests_pwd, 4, PASSWORD_STAND);
    if (api_hid_init() == DBB_ERROR) {
        printf("\n\nNot testing HID API. A device is not connected.\n\n");
    } else {
        printf("\n\nHID API Result:\n");
        run_utests();
    }
#endif

    ecc_context_destroy();
    return U_TESTS_FAIL;
}
