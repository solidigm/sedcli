/*
 * Copyright (C) 2018-2019, 2022-2023 Solidigm. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <errno.h>

#include "argp.h"
#include "libsed.h"
#include "kmip_lib.h"


#define PADDING "   "
#define MAX_OPT_HELP_LEN 40
#define TIMESTAMP_LEN (4096)

#define MAX_LOG_LEVEL LOG_WARNING
#define SEDCLI_LOGFILE "/var/log/sedcli.log"

#ifndef F_ULOCK
    #define F_ULOCK 0
#endif
#ifndef F_LOCK
    #define F_LOCK 1
#endif
int vsedcli_log(int log_level, const char *template, va_list args)
{
    FILE *log;
    time_t t;
    struct tm *tm;
    char *timestamp;
    int ret;

    if (log_level > MAX_LOG_LEVEL)
        return 0;

    log = fopen(SEDCLI_LOGFILE, "a");
    if (!log)
        return FAILURE;

    ret = lockf(fileno(log), F_LOCK, 0);
    if (ret < 0)
        goto out;

    t = time(NULL);
    tm = localtime(&t);
    if (!tm) {
        ret = FAILURE;
        goto out;
    }

    timestamp = asctime(tm);
    if (!timestamp) {
        ret = FAILURE;
        goto out;
    }

    timestamp[strnlen(timestamp, TIMESTAMP_LEN)-1] = 0;

    fseek(log, 0, SEEK_END);
    fprintf(log, "%s sedcli: ", timestamp);
    vfprintf(log, template, args);
    fflush(log);

    ret = lockf(fileno(log), F_ULOCK, 0);

out:
    fclose(log);
    return ret;
}

__attribute__((format(printf, 2, 3)))
int sedcli_log(int log_level, const char *template, ...)
{
    va_list args;
    va_start(args, template);
    vsedcli_log(log_level, template, args);
    va_end(args);
    return 0;
}

__attribute__((format(printf, 2, 3)))
int std_printf(int log_level, const char *template, ...)
{
    va_list args;


    va_start(args, template);
    if (LOG_WARNING >= log_level) {
        va_list args_copy;
        va_copy(args_copy, args);
        vfprintf(stderr, template, args);
        vsedcli_log(log_level, template, args_copy);
        va_end(args_copy);
    } else {
        vfprintf(stdout, template, args);
    }
    va_end(args);
    return 0;
}

sedcli_printf_t sedcli_printf = std_printf;

static int is_su_required(const cli_command* commands, int cmd)
{
    return commands[cmd].flags & CLI_SU_REQUIRED;
}

static int is_command_hidden(const cli_command* commands, int cmd)
{
    return commands[cmd].flags & CLI_COMMAND_HIDDEN;
}

static void print_short_usage(const app *app_values)
{
    sedcli_printf(LOG_INFO, "Usage: %s %s\n\n", app_values->name, app_values->info);
    sedcli_printf(LOG_INFO, "The '<device>' must be a block device (e.g. /dev/nvme0n1).\n");
}

static void print_info(const app *app_values)
{
    sedcli_printf(LOG_INFO, "Try `%s --help' for more information.\n", app_values->name);
}

char *get_short_name_string(const char short_name, char *buf)
{
    if (short_name) {
        snprintf(buf, 3, "-%c", short_name);
    } else {
        buf[0] = 0;
    }
    return buf;
}

char *command_name_with_slash(char *buf, size_t buf_size, char short_name, char *long_name) {
    if (short_name) {
        snprintf(buf, buf_size, "-%c/--%s", short_name, long_name);
    } else {
        snprintf(buf, buf_size, "--%s", long_name);
    }
    return buf;
}

char *command_name_in_brackets(char *buf, size_t buf_size, char short_name, char *long_name) {
    if (short_name) {
        snprintf(buf, buf_size, "--%s (-%c)", long_name, short_name);
     } else {
        snprintf(buf, buf_size, "--%s", long_name);
     }
    return buf;
}

void print_options_usage(cli_option* options, const char *separator,
        int (*view)(cli_option* options, int flag), int flag)
{
    int print_separator = 0;
    int i;

    if (NULL == options) {
        return;
    }

    for (i = 0; options[i].long_name != NULL; ++i) {
        if (0 == view(&options[i], flag)) {
            continue;
        }

        if (print_separator) {
            /* Separator */
            sedcli_printf(LOG_INFO, "%s", separator);
        }
        print_separator = 1;

        /* Long option name */
        sedcli_printf(LOG_INFO, "--%s", options[i].long_name);

        /* Parameter */
        if (options[i].arg != NULL) {
            sedcli_printf(LOG_INFO, " <%s>",
            options[i].arg);
        }
    }
}

void print_command_header(const cli_command *cmd)
{
    sedcli_printf(LOG_INFO, "%s%s\n\n", PADDING, cmd->long_desc != NULL ? cmd->long_desc : cmd->desc);
}

void print_list_options(cli_option* options, int flag, int (*view)(cli_option* options, int flag))
{
    for (; options->long_name != NULL; options++) {
        char *desc = options->desc;
        char short_name[3];

        if (0 == view(options, flag))
            continue;

        get_short_name_string(options->short_name, short_name);
        if (options->arg != NULL) {
            char buf[MAX_OPT_HELP_LEN];
            if (options->flags & CLI_OPTION_OPTIONAL)
                snprintf(buf, MAX_OPT_HELP_LEN, "--%s [<%s>]", options->long_name, options->arg);
            else
                snprintf(buf, MAX_OPT_HELP_LEN, "--%s <%s>", options->long_name, options->arg);

            sedcli_printf(LOG_INFO, "%s%-4s%-32s%s\n", PADDING, short_name, buf, desc);
        } else
            sedcli_printf(LOG_INFO, "%s%-4s--%-30s%s\n", PADDING, short_name, options->long_name, desc);
    }
}

static void print_options_help(cli_option *options)
{
    int i;

    for (i = 0; options[i].long_name != NULL; ++i) {
        char *desc = options[i].desc;
        char short_name[3];
        if (options[i].flags & CLI_OPTION_HIDDEN)
            continue;

        get_short_name_string(options[i].short_name, short_name);
        if (options[i].arg != NULL) {
            char buf[MAX_OPT_HELP_LEN];
            if (options[i].flags & CLI_OPTION_OPTIONAL) {
                snprintf(buf, MAX_OPT_HELP_LEN, "--%s [<%s>]",
                     options[i].long_name,
                     options[i].arg);
            } else {
                snprintf(buf, MAX_OPT_HELP_LEN, "--%s <%s>",
                     options[i].long_name,
                     options[i].arg);
            }

            sedcli_printf(LOG_INFO, "%s%-4s%-38s%s\n", PADDING, short_name, buf, desc);
        } else
            sedcli_printf(LOG_INFO, "%s%-4s--%-36s%s\n", PADDING, short_name, options[i].long_name, desc);
    }
}

static void print_namespace_help(app *app_values, cli_command *cmd)
{
    char command_name[MAX_STR_LEN];
    char option_name[MAX_STR_LEN];
    cli_namespace *ns = cmd->namespace;
    int i;

    sedcli_printf(LOG_INFO, "Usage: %s --%s --%s <NAME>\n\n", app_values->name, cmd->name, ns->long_name);

    print_command_header(cmd);

    command_name_in_brackets(command_name, MAX_STR_LEN, cmd->short_name, cmd->name);
    command_name_in_brackets(option_name, MAX_STR_LEN, ns->short_name, ns->long_name);

    sedcli_printf(LOG_INFO, "Valid values of NAME are:\n");
    for (i = 0; ns->entries[i].name; ++i)
        sedcli_printf(LOG_INFO, "%s%s - %s\n", PADDING, ns->entries[i].name, ns->entries[i].desc);

    sedcli_printf(LOG_INFO, "\n");

    for (i = 0; ns->entries[i].name; ++i) {
        sedcli_printf(LOG_INFO, "Options that are valid with %s %s %s are:\n", command_name, option_name,
        ns->entries[i].name);
        print_options_help(ns->entries[i].options);
        if (ns->entries[i + 1].name)
            sedcli_printf(LOG_INFO, "\n");
    }
}

static void print_command_help(app *app_values, cli_command *cmd)
{
    int all_mandatory = 1;
    int all_hidden = 1;
    int i;

    if (cmd->help) {
        (cmd->help)(app_values, cmd);
        return;
    }

    if (cmd->namespace) {
        print_namespace_help(app_values, cmd);
        return;
    }

    sedcli_printf(LOG_INFO, "Usage: %s --%s", app_values->name, cmd->name);

    if (cmd->options != NULL) {
        for (i = 0; cmd->options[i].long_name != NULL; ++i) {
            if (cmd->options[i].flags & CLI_OPTION_HIDDEN) {
                continue;
            }

            all_hidden = 0;

            if (cmd->options[i].flags & CLI_OPTION_REQUIRED) {
                sedcli_printf(LOG_INFO, " --%s", cmd->options[i].long_name);
                if (cmd->options[i].arg != NULL) {
                    if (cmd->options[i].flags & CLI_OPTION_OPTIONAL) {
                        sedcli_printf(LOG_INFO, " [<%s>]", cmd->options[i].arg);
                    } else {
                        sedcli_printf(LOG_INFO, " <%s>", cmd->options[i].arg);
                    }
                }
            } else {
                all_mandatory = 0;
            }
        }

        if (!all_mandatory) {
            sedcli_printf(LOG_INFO, " [option...]");
        }
    }
    sedcli_printf(LOG_INFO, "\n\n");

    print_command_header(cmd);

    if (cmd->options && !all_hidden) {
        char option_name[MAX_STR_LEN];
        command_name_in_brackets(option_name, MAX_STR_LEN, cmd->short_name, cmd->name);
        sedcli_printf(LOG_INFO, "Options that are valid with %s are:\n", option_name);
        print_options_help(cmd->options);
    }
}

void print_help(const app *app_values, const cli_command *commands)
{
    sedcli_printf(LOG_INFO, "%s\n\n", app_values->title);
    print_short_usage(app_values);

    sedcli_printf(LOG_INFO, "\nAvailable commands:\n");
    for (int i = 0;; ++i) {
        char short_name[3];

        if (commands[i].name == NULL)
            break;

        if (is_command_hidden(commands, i))
            continue;

        char* buf = get_short_name_string(commands[i].short_name, short_name);
        if (buf[0])
            sedcli_printf(LOG_INFO, "%s%-4s--%-25s%s\n", PADDING, short_name, commands[i].name, commands[i].desc);
        else
            sedcli_printf(LOG_INFO, "%s--%-25s%s\n", PADDING, commands[i].name, commands[i].desc);
    }

    sedcli_printf(LOG_INFO, "\nSee 'sedcli <command> --help' for more information on a specific command.\n"
            "e.g.\n%s%s --%s --help\n", PADDING, app_values->name, commands[0].name);

    if (app_values->man != NULL)
        sedcli_printf(LOG_INFO, "For more information, please refer to manpage (man %s).\n", app_values->man);
    else
        sedcli_printf(LOG_INFO, "For more information, please refer to manpage.\n");
}

static int args_is_unrecognized(const char *cmd)
{
    if (strempty(cmd)) {
        return 1;
    }

    if ('-' == cmd[0]) {
        char c = cmd[1];

        /* Check if short option (command) is proper */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if ('\0' == cmd[2]) {
                return 0;
            } else {
                return 1;
            }
        }

        if ('-' == cmd[1]) {
            char c = cmd[2];
            /* Long option (command), check if it is valid */

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                return 0;
            }
        }
    }

    return 1;
}

static int args_is(const char *in, const char *arg, const char c)
{
    if (strempty(in))
        return 0;

    if ('-' == in[0]) {
        if (0 != c && c == in[1]) {
            if ('\0' == in[2]) {
                return 1;
            }
        }

        if ('-' == in[1]) {
            /* Long option */
            if (0 == strncmp(&(in[2]), arg, MAX_STR_LEN)) {
                return 1;
            }
        }
    }

    return 0;
}

static int is_help(const char* cmd)
{
    return args_is(cmd, "help", 'H');
}

static int is_version(const char* cmd)
{
    return args_is(cmd, "version", 'V');
}

static int get_help_position(int argc, char **argv)
{
    for (int i = 2; i < argc; i++)
        if (is_help(argv[i])) {
            return i;
        }

    return -1;
}

static int get_option(const cli_option *options, const char* opt)
{
    for (int i = 0; options[i].long_name; ++i) {
        if (args_is(opt, options[i].long_name, options[i].short_name)) {
            return i;
        }
    }

    return -1;
}

/**
 * log command as it was entered in CLI
 */
void log_command(int argc, char **argv, int result, long long int timespan)
{
    const int def_cmd_buf_len = 100;
    int cmd_buf_len = def_cmd_buf_len;
    int cmd_len = 0;

    char *command = malloc(cmd_buf_len);
    if (!command) {
        sedcli_printf(LOG_ERR, "sedcli: Memory allocation failed for logging.");
        return;
    }

    for (int i = 0 ; i != argc ; ++i) {
        int tok_len = strnlen(argv[i], MAX_STR_LEN);
        /* if reconstructed command width is longer than current
         * cmd_buf_len (length of command buffer), than resize it
         * to make it twice as big.
         */
        if (tok_len + 1 + cmd_len > cmd_buf_len) {
            cmd_buf_len = (tok_len + 1 + cmd_buf_len) * 2;
            char *tmp = realloc(command, cmd_buf_len);
            /* if reallocation failed, cancel logging */
            if (!tmp) {
                sedcli_printf(LOG_ERR, "sedcli: Memory allocation failed for logging.");
                free(command);
                return;
            }
            command = tmp;
        }
        /* append additional token to a string */
        /*memcpy_s(command + cmd_len, cmd_buf_len - cmd_len,
             argv[i], tok_len);*/
        memcpy(command + cmd_len, argv[i], tok_len);
        cmd_len += tok_len;
        /* either a space or a null terminator */
        command[cmd_len] = (i == argc - 1) ? 0 : ' ';
        cmd_len++;
    }

    sedcli_log(LOG_DEBUG, "sedcli invoked with: \"%s\". "
            "Exit status is %d (%s). Command took %lld.%02lld s.",
            command, result, result? "failure" : "success",
            timespan / 1000, (timespan % 1000) / 10);
    free(command);
}

static const char *sed_statuses[] = {
    /* 0x00 */ [SED_SUCCESS] = "SUCCESS",
    /* 0x01 */ [SED_NOT_AUTHORIZED] = "NOT_AUTHORIZED",
    /* 0x02 */ [SED_UNKNOWN_ERROR] = "OBSOLETE", // as in spec
    /* 0x03 */ [SED_SP_BUSY] = "SP_BUSY",
    /* 0x04 */ [SED_SP_FAILED] = "SP_FAILED",
    /* 0x05 */ [SED_SP_DISABLED] = "SP_DISABLED",
    /* 0x06 */ [SED_SP_FROZEN] = "SP_FROZEN",
    /* 0x07 */ [SED_NO_SESSIONS_AVAILABLE] = "NO_SESSIONS_AVAILABLE",
    /* 0x08 */ [SED_UNIQUENESS_CONFLICT] = "UNIQUENESS_CONFLICT",
    /* 0x09 */ [SED_INSUFFICIENT_SPACE] = "INSUFFICIENT_SPACE",
    /* 0x0A */ [SED_INSUFFICIENT_ROWS] = "INSUFFICIENT_ROWS",
    /* 0x0B */ [SED_INVALID_FUNCTION] = "OBSOLETE", // not in spec
    /* 0x0C */ [SED_INVALID_PARAMETER] = "INVALID PARAMETER",
    /* 0x0D */ [SED_INVALID_REFERENCE] = "OBSOLETE", // as in spec
    /* 0x0E */ [SED_UNKNOWN_ERROR_1] = "OBSOLETE",  // as in spec
    /* 0x0F */ [SED_TPER_MALFUNCTION] = "TPER_MALFUNCTION",
    /* 0x10 */ [SED_TRANSACTION_FAILURE] = "TRANSACTION_FAILURE",
    /* 0x11 */ [SED_RESPONSE_OVERFLOW] = "RESPONSE_OVERFLOW",
    /* 0x12 */ [SED_AUTHORITY_LOCKED_OUT] = "AUTHORITY_LOCKED_OUT",
    /* 0x3F */ [SED_FAIL] = "FAIL",
};

const char *sed_error_text(int sed_status)
{
    if (sed_status < SED_SUCCESS || sed_status > SED_FAIL)
        return NULL;

    return sed_statuses[sed_status];
}

typedef union {
    struct {
        uint8_t SC       : 8;
        uint8_t SCT      : 3;
        uint8_t CRD      : 2;
        uint8_t M        : 1;
        uint8_t DNR      : 1;
        uint8_t Reserved : 1;
    } error_bits;
    uint16_t status;
} nvme_error_status;

extern uint32_t nvme_error;
static void print_sed_status(int status)
{
    const char *sed_status = NULL;

    if (status < 0) {
        if (sed_cli == SED_CLI_STANDARD) {
            if (status == -EINVAL)
                sedcli_printf(LOG_ERR, "sedcli: Invalid parameter.\n");
            else if (status == -ENODEV)
                sedcli_printf(LOG_ERR, "sedcli: Couldn't determine device state.\n");
            else if (status == -ENOMEM)
                sedcli_printf(LOG_ERR, "sedcli: No memory.\n");
            else
                sedcli_printf(LOG_ERR, "sedcli: Unknown error.\n");
        } else {
            if (status == KMIP_FAILURE)
                sedcli_printf(LOG_ERR, "sedcli-kmip: Failure.\n");
            else
                sedcli_printf(LOG_ERR, "sedcli-kmip: Unknown error.\n");
        }
    } else if (sed_cli == SED_CLI_KMIP && status >= KMIP_SUCCESS) {
        if (status == KMIP_SUCCESS_CONNECTED)
            sedcli_printf(LOG_ERR, "sedcli-kmip: Successful connection to the KMIP server.\n");
    } else {
        if (nvme_error == 4)
        {
            sedcli_printf(LOG_ERR, "sedcli: IOCTL error: 0x04 Interrupted system call.\n");
            return;
        } else if (nvme_error == 5) {
            sedcli_printf(LOG_ERR, "sedcli: IOCTL error: 0x05 I/O error.\n");
            return;
        }

        sed_status = sed_error_text(status);
        if (sed_status == NULL && status > 0 && status <= 0xFFFF) {
            if (nvme_error) {
                nvme_error_status nes = { 0 };
                nes.status = (uint32_t)status;
                sedcli_printf(LOG_ERR, "sedcli: NVMe error: %d\nSC: %d | SCT: %d | CRD: %d | M: %d | DNR: %d\n",
                    status, nes.error_bits.SC, nes.error_bits.SCT, nes.error_bits.CRD, nes.error_bits.M, nes.error_bits.DNR);
            }
            else
                sedcli_printf(LOG_ERR, "status: Unknown status: %d\n", status);
        }
        else
            sedcli_printf((status == 0) ? LOG_INFO : LOG_ERR, "status: 0x%02x %s\n", status, sed_status);
    }
}

/**
 * run command. Additionally log its execution and report any errors if
 * they've happened
 */
#ifndef CLOCK_REALTIME
    #define CLOCK_REALTIME 0
#endif
int run_command(cli_command *commands, int cmd, int argc, char **argv)
{
    int result;
    const char *syslog_path = "/var/log/messages";
    /* time buffer and stat buffer after running command */
    struct timespec t0;
    FILE *messages_f;
    /* time buffer and stat buffer after running command */
    struct timespec t1;
    long long int timespan;

    /* collect time */
    clock_gettime(CLOCK_REALTIME, &t0);
    /* collect stat buffer for syslog */
    messages_f = fopen(syslog_path, "r");
    if (messages_f) {
        fseek(messages_f, 0, SEEK_END);
        /* if opening file failed, don't stop command execution.
         * - just omit checking for /var/log/messages at the end
         */
    } else {
        /* ubuntu case*/
        syslog_path = "/var/log/syslog";
        messages_f = fopen(syslog_path, "r");
        if (messages_f)
            fseek(messages_f, 0, SEEK_END);
    }

    /* execute command */
    result = commands[cmd].handle();
    if (is_help(argv[1]) == false && is_version(argv[1]) == false)
        print_sed_status(result);

    clock_gettime(CLOCK_REALTIME, &t1);
    timespan = (1000 * (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1000000);

    if (commands[cmd].short_name != 'V')
        log_command(argc, argv, result, timespan);

    if (messages_f)
        fclose(messages_f);

    return result;
}

static int count_arg_params(char **argv, int argc)
{
    int i = 0;

    for (; i < argc; i++)
        if ('-' == argv[i][0] && 0 != argv[i][1]) {
            return i;
        }

    return i;
}

void configure_cli_commands(cli_command *commands)
{
    cli_command *cmd = commands;
    while(cmd && cmd->name) {
        if (cmd->configure) {
            if (cmd->configure(cmd) < 0) {
                cmd->flags |= CLI_COMMAND_HIDDEN;
            }
        }
        cmd++;
    }
}

int args_parse(app *app_values, cli_command *commands, int argc, char **argv)
{
    int i, j, k, status = SUCCESS;
    int args_count, args_offset;
    char **args_list = NULL;
    const char* cmd_name = argv[1];
    cli_ns_entry *entry = NULL;
    cli_option *options;
    int cmd = -1, first_opt;

    if (argc < 2) {
        sedcli_printf(LOG_ERR, "sedcli: No command given.\n");
        print_info(app_values);
        return FAILURE;
    }

    if (args_is_unrecognized(cmd_name)) {
        sedcli_printf(LOG_ERR, "sedcli: Unrecognized command %s.\n", cmd_name);
        print_info(app_values);
        return FAILURE;
    }

    for (i = 0;; ++i) {
        if (commands[i].name == NULL) {
            if (is_help(cmd_name)) {
                print_help(app_values, commands);
                return SUCCESS;
            }
            break;
        } else if (args_is(cmd_name, commands[i].name, commands[i].short_name)) {
            cmd = i;
            break;
        }
    }

    if (cmd == -1) {
        sedcli_printf(LOG_ERR, "sedcli: Unrecognized command %s.\n", cmd_name);
        print_info(app_values);
        return FAILURE;
    }

    configure_cli_commands(commands);

    if (argc >= 3 && get_help_position(argc, argv) != -1) {
        if (!is_command_hidden(commands, i)) {
            print_command_help(app_values, &commands[i]);
        }
        return SUCCESS;
    }

    if (is_su_required(commands, cmd)) {
        if (getuid() != 0) {
            sedcli_printf(LOG_ERR, "sedcli: Must be run as root.\n");
            return FAILURE;
        }
    }

    if (commands[cmd].options) {
        options = commands[cmd].options;
        first_opt = 2;
    } else if (commands[cmd].namespace) {
        if (argc < 3) {
            sedcli_printf(LOG_ERR, "sedcli: Missing namespace option.\n");
            print_info(app_values);
            return FAILURE;
        }

        if (argc < 4) {
            sedcli_printf(LOG_ERR, "sedcli: Missing namespace name.\n");
            print_info(app_values);
            return FAILURE;
        }

        if (!args_is(argv[2], commands[cmd].namespace->long_name, commands[cmd].namespace->short_name)) {
            sedcli_printf(LOG_ERR, "sedcli: Unrecognized option.\n");
            print_info(app_values);
            return FAILURE;
        }

        entry = commands[cmd].namespace->entries;
        while (true) {
            if (!strncmp(argv[3], entry->name, 255))
                break;

            if (!(++entry)->name) {
                sedcli_printf(LOG_ERR, "sedcli: Unrecognized namespace entry.\n");
                print_info(app_values);
                return FAILURE;
            }
        }

        options = entry->options;
        first_opt = 4;
    } else {
        return run_command(commands, cmd, argc, argv);
    }

    /* for each possible option:
     *  - if it is required, check if it is supplied exactly once
     *  - if it is not required, check if it is supplied at most once
     */
    for (i = 0; options[i].long_name; ++i) {
        char option_name[MAX_STR_LEN];

        /* count occurrences of an option (k as counter) */
        k = 0;
        for (j = first_opt; j < argc; ++j) {
            if (args_is(argv[j], options[i].long_name, options[i].short_name)) {
                k++;
            }
        }

        command_name_with_slash(option_name, MAX_STR_LEN, options[i].short_name, options[i].long_name);

        if (options[i].flags & CLI_OPTION_REQUIRED) {
            if (!k) {
                sedcli_printf(LOG_ERR, "sedcli: Missing required option %s.\n",  option_name);
                print_info(app_values);
                return FAILURE;
            }
        }

        if (options[i].args_count != 0 && k > options[i].args_count) {
            sedcli_printf(LOG_ERR, "sedcli: Option supplied too many times %s.\n", option_name);
            print_info(app_values);
            return FAILURE;
        }
    }

    /* Store parameters for arguments. Terminate each list with NULL element.
     * Accomodate for max no of parameters */
    args_list = malloc(sizeof(*args_list) * (argc + 1));
    if (args_list == NULL) {
        return FAILURE;
    }

    /* iterate over all arguments that were actually passed to the CLI */
    args_count = args_offset = 0;
    for (i = first_opt; i < argc; ++i) {
        if (args_is_unrecognized(argv[i])) {
            sedcli_printf(LOG_ERR, "sedcli: Invalid format %s.\n", argv[i]);
            print_info(app_values);

            status = FAILURE;
            goto free_args;
        }

        int opt = get_option(options, argv[i]);
        if (opt == -1) {
            sedcli_printf(LOG_ERR, "sedcli: Unrecognized option %s.\n", argv[i]);
            print_info(app_values);

            status = FAILURE;
            goto free_args;
        }

        if (options[opt].arg != NULL) {
            /* Count params for current argument. */
            args_count = count_arg_params(&argv[i + 1], argc - i - 1);
            bool invalid = false;

            if (options[opt].args_count > 0)
            {
                if (options[opt].flags & CLI_OPTION_REQUIRED ||
                    options[opt].flags & CLI_OPTION_OPTIONAL) {
                    if (args_count == 0) {
                        invalid = true;
                    }
                }

                if (args_count > options[opt].args_count) {
                    invalid = true;
                }
            }

            if (invalid) {
                sedcli_printf(LOG_ERR, "sedcli: Invalid number of arguments for %s.\n", argv[i]);
                print_info(app_values);

                status = FAILURE;
                goto free_args;
            }

            /* Add params for current argument.
             * Terminate list with NULL element.*/
            for (k = args_offset, j = 0; j < args_count; j++) {
                args_list[k++] = argv[j + i + 1];
            }
            args_list[args_offset + args_count] = NULL;

            i += args_count;
        }

        if (commands[cmd].options_parse) {
            status = commands[cmd].options_parse(
                    options[opt].long_name,
                    &args_list[args_offset]);
        } else if (commands[cmd].namespace_opts_parse && entry != NULL) {
            status = commands[cmd].namespace_opts_parse(
                    entry->name,
                    options[opt].long_name,
                    &args_list[args_offset]);
        } else {
            sedcli_printf(LOG_ERR, "sedcli: Internal error.\n");
            status = FAILURE;
            goto free_args;
        }
        args_offset += args_count;

        if (0 != status) {
            sedcli_printf(LOG_ERR, "sedcli: Error during options handling.\n");
            print_info(app_values);

            status = FAILURE;
            goto free_args;
        }
    }

    status = run_command(commands, cmd, argc, argv);

free_args:
    if (NULL != args_list) {
        free(args_list);
        args_list = NULL;
    }

    return status;
}
