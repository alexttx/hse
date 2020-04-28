/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2016-2018 Micron Technology, Inc.  All rights reserved.
 */

#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <stdarg.h>
#include <assert.h>

#include <sys/types.h>

#include <hse/hse.h>
#include <hse/hse_experimental.h>

#include <hse_util/parse_num.h>
#include <hse_util/yaml.h>
#include <hse_util/string.h>

#include <mpool/mpool.h>

#include "cli_util.h"

#define OPTION_HELP                 \
    {                               \
        "[-h|--help]", "Print help" \
    }
#define OPTION_CFILE                                \
    {                                               \
        "[-c|--config FILE]", "Use hse config file" \
    }

#define CONFIG_LOG_LVL                                              \
    {                                                               \
        "kvdb.log_lvl=<int>", "Set log level, range 0..7 (7=debug)" \
    }
#define CONFIG_KVS_PFX_LEN                                                      \
    {                                                                           \
        "kvs.pfx_len=<int>", "Set KVS prefix length, range [0..32], default: 0" \
    }

#define min(a, b)                  \
    ({                             \
        typeof(a) arg1 = (a);      \
        typeof(b) arg2 = (b);      \
        arg1 < arg2 ? arg1 : arg2; \
    })

#define max(a, b)                  \
    ({                             \
        typeof(a) arg1 = (a);      \
        typeof(b) arg2 = (b);      \
        arg1 > arg2 ? arg1 : arg2; \
    })

#define STR(X) X

#define YAML_BUF_SIZE (16 * 1024)

typedef uint64_t mp_err_t;
typedef uint64_t hse_err_t;

#define INTERNAL_ERROR()                                                \
    do {                                                                \
        fprintf(stderr, "%s:%d: internal error\n", __FILE__, __LINE__); \
        assert(0);                                                      \
        exit(-1);                                                       \
    } while (0)

enum help_style {
    help_style_full = 0,
    help_style_leaf_summary,
    help_style_usage,
};

/* Max len of any individual command.  Not enforced.  It is only used
 * for sizing.  See CLI_CMD_PATH_LEN_MAX.
 */
#define CLI_CMD_NAME_LEN_MAX 32

/* CLI_MENU_DEPTH_MAX need to be exact, but it must be larger
 * than the actual max depth.  Current depth is 3, so use 4.
 */
#define CLI_MENU_DEPTH_MAX 4

/* CLI_CMD_PATH_LEN_MAX must be large enough to hold concatenated
 * command names along a path in the command tree from root to leaf.
 * If it is too small, it will be discovered during initialization and
 * an error will be raised by the INTERNAL_ERROR macro.
 */
#define CLI_CMD_PATH_LEN_MAX ((CLI_MENU_DEPTH_MAX * (CLI_CMD_NAME_LEN_MAX + 1)) + 1)

#define CLI_CMD_OPSTR_LEN_MAX 256

struct cli;
struct cli_cmd;

struct name_desc {
    char *name;
    char *desc;
};

struct cmd_spec {
    const char *           usagev[4];
    const struct name_desc optionv[8];
    const struct name_desc configv[8];
    const struct option    longoptv[8];
};

typedef int(cli_cmd_func_t)(struct cli_cmd *self, struct cli *cli);

struct cli_cmd {
    const char *    cmd_name;
    const char *    cmd_describe;
    cli_cmd_func_t *cmd_main;
    struct cli_cmd *cmd_subcommandv;

    /* Initialized at runtime:
     */
    const struct cmd_spec *cmd_spec;
    char                   cmd_path[CLI_CMD_PATH_LEN_MAX];
};

struct cli {
    bool            hse_init;
    bool            help_show_all;
    enum help_style help_style;
    struct cli_cmd *cmd;
    int             argc;
    char **         argv;
    int             optind;
    char            opstr[CLI_CMD_OPSTR_LEN_MAX];
};

/*****************************************************************
 * HSE KVDB commands:
 *    hse kvdb create
 *    hse kvdb list
 *    hse kvdb compact
 */
static cli_cmd_func_t cli_hse_kvdb_create;
static cli_cmd_func_t cli_hse_kvdb_list;
static cli_cmd_func_t cli_hse_kvdb_compact;
struct cli_cmd        cli_hse_kvdb_commands[] = {
    { "create", "Create a KVDB", cli_hse_kvdb_create, 0 },
    { "list", "List KVDBs", cli_hse_kvdb_list, 0 },
    { "compact", "Compact a KVDB", cli_hse_kvdb_compact, 0 },
    { 0 },
};

/*****************************************************************
 * HSE KVS commands:
 *    hse kvs create
 *    hse kvs destroy
 */
static cli_cmd_func_t cli_hse_kvs_create;
static cli_cmd_func_t cli_hse_kvs_destroy;
struct cli_cmd        cli_hse_kvs_commands[] = {
    { "create", "Create a KVS", cli_hse_kvs_create, 0 },
    { "destroy", "Destroy a KVS", cli_hse_kvs_destroy, 0 },
    { 0 },
};

/*****************************************************************
 * HSE commands:
 *    hse version
 *    hse kvdb
 *    hse kvs
 */
static cli_cmd_func_t cli_hse_version;
static cli_cmd_func_t cli_hse_kvdb;
static cli_cmd_func_t cli_hse_kvs;
struct cli_cmd        cli_hse_commands[] = {
    { "version", "Show HSE version", cli_hse_version, 0 },
    { "kvdb", "Manage KVDB", cli_hse_kvdb, cli_hse_kvdb_commands },
    { "kvs", "Manage KVS", cli_hse_kvs, cli_hse_kvs_commands },
    { 0 },
};

/****************************************************************
 * Root of command tree
 */
static cli_cmd_func_t cli_hse;
struct cli_cmd        cli_root = { "hse", "HSE command line interface", cli_hse, cli_hse_commands };

/**
 * cmd_tree_set_paths() - walk comand tree to set paths
 *
 * Set @cmd_path of each @cli_cmd in the command tree. 
 */
int
cmd_tree_set_path_recurse(struct cli_cmd *self, int argc_max, int argc, const char **argv)
{
    struct cli_cmd *sub;
    size_t          n;

    if (argc == argc_max) {
        INTERNAL_ERROR();
        return -1;
    }

    argv[argc++] = self->cmd_name;

    self->cmd_path[0] = '\0';
    for (int i = 0; i < argc; i++) {
        strlcat(self->cmd_path, argv[i], sizeof(self->cmd_path));
        strlcat(self->cmd_path, " ", sizeof(self->cmd_path));
    }

    n = strlen(self->cmd_path);
    if (n + 1 == sizeof(self->cmd_path)) {
        INTERNAL_ERROR();
        return -1;
    }

    if (n == 0) {
        INTERNAL_ERROR();
        return -1;
    }

    if (self->cmd_path[n - 1] != ' ') {
        INTERNAL_ERROR();
        return -1;
    }

    self->cmd_path[n - 1] = '\0';

    for (sub = self->cmd_subcommandv; sub && sub->cmd_name; sub++) {
        if (cmd_tree_set_path_recurse(sub, argc_max, argc, argv)) {
            INTERNAL_ERROR();
            return -1;
        }
    }

    return 0;
}

int
cmd_tree_set_paths(struct cli_cmd *root)
{
    const char *argv[CLI_MENU_DEPTH_MAX];

    return cmd_tree_set_path_recurse(root, CLI_MENU_DEPTH_MAX, 0, argv);
}

/**
 * cli_cmd_lookup() - find a command handler by name
 *
 * The table, @cmdv, must be terminated by an entry
 * with an @cmd_name==NULL.
 */
struct cli_cmd *
cli_cmd_lookup(struct cli_cmd *cmdv, const char *name)
{
    struct cli_cmd *cmd;

    for (cmd = cmdv; cmd->cmd_name; cmd++)
        if (!strcmp(name, cmd->cmd_name))
            return cmd;

    return 0;
}

/**
 * cmd_print_help() - print help info for a command.
 *
 * If @sub_commands != 0, the subcomands will be listed.
 */
static void
cmd_print_help(struct cli_cmd *cmd, enum help_style style, FILE *fp)
{
    const struct name_desc *nd;
    const char *            vert_space = "";

    bool have_subs;
    int  i, width;
    int  ilvl = 0; /* current indent level */
    int  tabw = 2; /* spaces per indent level (ie, tab width) */

    have_subs = cmd->cmd_subcommandv && cmd->cmd_subcommandv->cmd_name;

    switch (style) {

        case help_style_leaf_summary:
            if (!have_subs) {
                fprintf(fp, "%*s%s:\n", ilvl * tabw, "", cmd->cmd_describe);
                fprintf(fp, "%*s%s", (ilvl + 1) * tabw, "", cmd->cmd_path);
                for (i = 0; cmd->cmd_spec->usagev[i]; i++)
                    printf(
                        "%*s%s\n", i == 0 ? 1 : ((ilvl + 2) * tabw), "", cmd->cmd_spec->usagev[i]);
                if (!i)
                    fprintf(fp, "\n");
            }
            break;

        case help_style_full:
            fprintf(fp, "\n%*s%s:\n\n", ilvl * tabw, "", cmd->cmd_describe);
            vert_space = "\n";
            ilvl += 1;
        /* FALLTHROUGH */
        default: {
            fprintf(fp, "%*s%s", ilvl * tabw, "", cmd->cmd_path);
            for (i = 0; cmd->cmd_spec->usagev[i]; i++)
                printf("%*s%s\n", i == 0 ? 1 : ((ilvl + 1) * tabw), "", cmd->cmd_spec->usagev[i]);
            if (i == 0)
                fprintf(fp, "\n");
            if (i > 0)
                fprintf(fp, "%s", vert_space);

            nd = cmd->cmd_spec->optionv;
            width = 0;
            for (i = 0; nd[i].name; i++)
                width = max(width, strlen(nd[i].name));
            width += 4;
            width = max(width, 24);
            for (i = 0; nd[i].name; i++) {
                if (i == 0)
                    fprintf(fp, "%*sOptions:\n", ilvl * tabw, "");
                fprintf(fp, "%*s%-*s%s\n", (ilvl + 1) * tabw, "", width, nd[i].name, nd[i].desc);
            }
            if (i > 0)
                fprintf(fp, "%s", vert_space);

            nd = cmd->cmd_spec->configv;
            width = 0;
            for (i = 0; nd[i].name; i++)
                width = max(width, strlen(nd[i].name));
            width += 4;
            width = max(width, 24);
            for (i = 0; nd[i].name; i++) {
                if (i == 0)
                    fprintf(fp, "%*sConfig parameters:\n", ilvl * tabw, "");
                fprintf(fp, "%*s%-*s%s\n", (ilvl + 1) * tabw, "", width, nd[i].name, nd[i].desc);
            }
            if (i > 0)
                fprintf(fp, "%s", vert_space);

            if (have_subs) {

                struct cli_cmd *sub;

                width = 0;
                for (sub = cmd->cmd_subcommandv; sub->cmd_name; sub++)
                    width = max(width, strlen(sub->cmd_name));
                width += 4;
                width = max(width, 24);

                fprintf(fp, "%*scommands:\n", ilvl * tabw, "");
                for (sub = cmd->cmd_subcommandv; sub->cmd_name; sub++) {
                    fprintf(
                        fp,
                        "%*s%-*s%s\n",
                        (ilvl + 1) * tabw,
                        "",
                        width,
                        sub->cmd_name,
                        sub->cmd_describe);
                }
                fprintf(fp, "%s", vert_space);
            }
        } break;
    }
}

/**
 * cli_init() -- intialize a cli context
 */
void
cli_init(struct cli *self, int argc, char **argv)
{
    memset(self, 0, sizeof(*self));
    self->argc = argc;
    self->argv = argv;
}

/**
 * cli_push() -- prepare to parse a sub-command
 */
void
cli_push(struct cli *cli, struct cli_cmd *cmd)
{
    size_t i, sz;
    char * s;

    cli->cmd = cmd;

    if (cli->optind > cli->argc)
        INTERNAL_ERROR();

    cli->argc -= cli->optind;
    cli->argv += cli->optind;

    cli->optind = 0;

    sz = 3;
    for (i = 0; cli->cmd->cmd_spec->longoptv[i].name; i++)
        sz += 3;

    if (sz > sizeof(cli->opstr))
        INTERNAL_ERROR();

    s = cli->opstr;
    *s++ = '+';
    *s++ = ':';

    for (i = 0; cli->cmd->cmd_spec->longoptv[i].name; i++) {

        const struct option *lo = cli->cmd->cmd_spec->longoptv + i;

        if (!lo->flag && lo->val) {
            *s++ = lo->val;
            if (lo->has_arg > 0)
                *s++ = ':';
            if (lo->has_arg > 1)
                *s++ = ':';
        }
    }

    *s = '\0';
}

int
cli_hook(struct cli *cli, struct cli_cmd *cmd, const struct cmd_spec *spec)
{
    cmd->cmd_spec = spec;

    if (cli->help_show_all) {
        struct cli_cmd *sub;
        cmd_print_help(cmd, cli->help_style, stdout);
        for (sub = cmd->cmd_subcommandv; sub && sub->cmd_name; sub++)
            sub->cmd_main(sub, cli);
        return 1;
    }

    cli_push(cli, cmd);
    return 0;
}

/**
 * cli_getopt() -- run one iteration of getopt_long on the argument list
 */
int
cli_getopt(struct cli *self)
{
    int c;
    int longind = -1;

    if (!self->argc)
        return -1;

    optind = self->optind;

    c = getopt_long(self->argc, self->argv, self->opstr, self->cmd->cmd_spec->longoptv, &longind);

    assert(optind <= self->argc);

    self->optind = optind;

    if (c == ':' || c == '?') {

        char name[32];

        if (longind >= 0)
            snprintf(name, sizeof(name), "--%s", self->cmd->cmd_spec->longoptv[longind].name);
        else if (optopt || (c && c != '?'))
            snprintf(name, sizeof(name), "-%c", optopt ?: c);
        else
            strlcpy(name, self->argv[optind - 1], sizeof(name));

        if (c == ':')
            fprintf(stderr, "%s: option '%s' requires an argument\n", self->cmd->cmd_path, name);
        else
            fprintf(stderr, "%s: invalid option: '%s'\n", self->cmd->cmd_path, name);
    }

    return c;
}

/**
 * cli_next_arg() -- get next arg, returns NULL if none left. Advances 'optind'.
 *
 * Note:
 * - Use this function to get fixed args (e.g., kvdb name)
 * - Do not use this function to get options such as '-v' (use cli_getopt() for that).
 */
const char *
cli_next_arg(struct cli *cli)
{
    assert(cli->optind <= cli->argc);

    if (cli->optind < cli->argc)
        return cli->argv[cli->optind++];

    return NULL;
}


/**
 * print_mpool_err() -- print details about an mpool error
 */
void
print_mpool_err(struct cli *cli, const char *api, mp_err_t err)
{
    char msg[256];

    mpool_strinfo(err, msg, sizeof(msg));
    fprintf(stderr, "%s: mpool error from %s: %s\n", cli->cmd->cmd_path, api, msg);

}

/**
 * print_hse_err() -- print details about an hse error
 */
void
print_hse_err(struct cli *cli, const char *api, hse_err_t err)
{
    char msg[256];

    hse_err_to_string(err, msg, sizeof(msg), 0);
    fprintf(stderr, "%s: error from %s: %s\n", cli->cmd->cmd_path, api, msg);
}

/**
 * print_hse_params_err() -- print details about an hse
 *                               parameter parsing error
 */
void
print_hse_params_err(struct cli *cli, const char *api, struct hse_params *hp)
{
    char msg[256];

    hse_params_err_exp(hp, msg, sizeof(msg));
    fprintf(stderr, "%s: hse parameter error from %s: %s\n", cli->cmd->cmd_path, api, msg);
}

/**
 * cli_hse_init() -- call hse_kvdb_init() if it hasn't already been called
 */
int
cli_hse_init(struct cli *cli)
{
    hse_err_t err;

    if (cli->hse_init)
        return 0;

    err = hse_kvdb_init();
    if (err) {
        print_hse_err(cli, "hse_kvdb_init", err);
        return -1;
    }

    cli->hse_init = true;
    return 0;
}

/**
 * cli_hse_init() -- call hse_kvdb_fini() if hse_kvdb_init() has been called
 */
void
cli_hse_fini(struct cli *cli)
{
    if (cli->hse_init)
        hse_kvdb_fini();
    cli->hse_init = false;
}

int
cli_hse_params_set_one(struct cli *cli, struct hse_params *hp, const char *pv_in)
{
    hse_err_t herr;
    char *    p, *v;

    p = strdup(pv_in);
    if (!p) {
        fprintf(stderr, "%s: out of memory\n", cli->cmd->cmd_path);
        return -1;
    }

    v = strchr(p, '=');
    if (!v) {
        fprintf(
            stderr,
            "%s: invalid hse config"
            " parameter syntax: '%s'"
            " (expected <param>=<value>)\n",
            cli->cmd->cmd_path,
            pv_in);
        free(p);
        return -1;
    }

    *v++ = '\0';

    herr = hse_params_set(hp, p, v);

    free(p);

    if (herr) {
        print_hse_params_err(cli, "hse_params_set", hp);
        return -1;
    }

    return 0;
}

struct hse_params *
cli_hse_params(struct cli *cli, const char *cfile, int argc, char **argv, ...)
{
    hse_err_t          herr = 0;
    struct hse_params *hp = 0;
    va_list            ap;
    const char *       pv;

    herr = hse_params_create(&hp);
    if (herr) {
        print_hse_err(cli, "hse_params_create", herr);
        goto done;
    }

    if (cfile) {
        herr = hse_params_from_file(hp, cfile);
        if (herr) {
            print_hse_params_err(cli, "hse_params_from_file", hp);
            goto done;
        }
    }

    for (int i = 0; !herr && i < argc; i++)
        herr = cli_hse_params_set_one(cli, hp, argv[i]);

    va_start(ap, argv);

    while (!herr && NULL != (pv = va_arg(ap, char *)))
        herr = cli_hse_params_set_one(cli, hp, pv);

    va_end(ap);

done:
    if (herr) {
        hse_params_destroy(hp);
        hp = 0;
    }

    return hp;
}

int
cli_hse_kvdb_create_impl(struct cli *cli, const char *cfile, const char *mp_name)
{
    struct hse_params *hp = 0;
    hse_err_t          herr = 0;
    int                rc = 0;

    if (cli_hse_init(cli))
        return -1;

    hp = cli_hse_params(
        cli, cfile, cli->argc - cli->optind, cli->argv + cli->optind, "kvdb.excl=1", 0);
    if (!hp)
        return -1;

    herr = hse_kvdb_make(mp_name, hp);
    if (herr) {
        print_hse_err(cli, "hse_kvdb_make", herr);
        switch (hse_err_to_errno(herr)) {
            case EEXIST:
                fprintf(
                    stderr,
                    STR("A KVDB already exists on mpool '%s'.\n"
                        "You can 1) destroy and recreate that mpool, or 2) create a\n"
                        "new mpool with different name and create a KVDB on it.\n"),
                    mp_name);
                break;
            case ENOENT:
                fprintf(
                    stderr,
                    STR("No such mpool: '%s'\n"
                        "You must create an mpool before creating a KVDB.\n"
                        "For example:\n"
                        "  pvcreate /dev/nvmeXXX\n"
                        "  vgcreate 'vg_%s' /dev/nvmeXXX\n"
                        "  lvcreate -l '100%%FREE -n 'lv_%s' 'vg_%s'\n"
                        "  mpool create '%s' '/dev/vg_%s/lv_%s'\n"
                        "  hse kvdb create '%s'\n"),
                    mp_name,
                    mp_name,
                    mp_name,
                    mp_name,
                    mp_name,
                    mp_name,
                    mp_name,
                    mp_name);
                break;
            default:
                break;
        }
        goto done;
    }

    printf("Successfully created KVDB %s\n", mp_name);

done:
    hse_params_destroy(hp);
    return (herr || rc) ? -1 : 0;
}

int
cli_hse_kvdb_list_impl(struct cli *cli, const char *mp_name, bool verbose)
{
    struct hse_params *hp = 0;
    char               buf[YAML_BUF_SIZE];
    int                rc;
    int                count;

    struct yaml_context yc = {
        .yaml_buf = buf,
        .yaml_buf_sz = sizeof(buf),
        .yaml_indent = 0,
        .yaml_offset = 0,
        .yaml_emit = yaml_print_and_rewind,
    };

    if (cli_hse_init(cli))
        return -1;

    hp = cli_hse_params(cli, NULL, 0, NULL, "kvdb.excl=1", "kvdb.rdonly=1", 0);
    if (!hp)
        return -1;

    rc = kvdb_list_print(mp_name, hp, &yc, verbose, &count);
    if (rc) {
        fprintf(stderr, "%s: unable to list KVDBs\n", cli->cmd->cmd_path);
        return -1;
    }

    if (count == 0) {
        if (mp_name) {
            fprintf(stderr, "No such KVDB: %s\n", mp_name);
            return -1;
        }
        fprintf(stderr, "No KVDBs found\n");
    }

    printf("%s", buf);

    return 0;
}

int
cli_hse_kvdb_compact_impl(
    struct cli *cli,
    const char *cfile,
    const char *kvdb,
    bool        compact,
    bool        status,
    bool        cancel,
    uint32_t    timeout_secs)
{
    int                count;
    const char *       req;
    struct hse_params *hp = 0;

    count = compact + status + cancel;
    if (count != 1) {
        fprintf(
            stderr,
            "%s: must specify exactly one of:"
            " -C, -s, -x\n",
            cli->cmd->cmd_path);
        return -1;
    }

    if (compact)
        req = "request";
    else if (cancel)
        req = "cancel";
    else
        req = "status";

    if (cli_hse_init(cli))
        return -1;

    hp = cli_hse_params(
        cli, cfile, cli->argc - cli->optind, cli->argv + cli->optind, "kvdb.excl=1", 0);
    if (!hp)
        return -1;

    return kvdb_compact_request(kvdb, hp, req, timeout_secs);
}

int
cli_hse_kvdb(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <command> ...", NULL,
            },
        .optionv =
            {
                OPTION_HELP, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { NULL },
            },
        .configv =
            {
                { NULL },
            },
    };

    const char *    sub_name;
    struct cli_cmd *sub_cmd;
    int             c;
    bool            help = false;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            default:
                return EX_USAGE;
        }
    }

    if (cli->optind == cli->argc || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    sub_name = cli->argv[cli->optind];
    sub_cmd = cli_cmd_lookup(self->cmd_subcommandv, sub_name);
    if (!sub_cmd) {
        fprintf(stderr, "%s: invalid command '%s', use -h fo help\n", self->cmd_path, sub_name);
        return EX_USAGE;
    }

    return sub_cmd->cmd_main(sub_cmd, cli);
}

int
cli_hse_kvdb_create(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <mpool> [<config_param>=<value>]...", NULL,
            },
        .optionv =
            {
                OPTION_HELP, OPTION_CFILE, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { "config", required_argument, 0, 'c' }, { NULL },
            },
        .configv =
            {
                CONFIG_LOG_LVL, { NULL },
            },
    };

    bool        help = false;
    const char *cfile = 0;
    const char *mp_name = 0;
    int         c;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            case 'c':
                cfile = optarg;
                break;
            default:
                return EX_USAGE;
        }
    }

    c = cli->argc - cli->optind;
    assert(c >= 0);

    if (c == 0 || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    mp_name = cli->argv[cli->optind++];

    return cli_hse_kvdb_create_impl(cli, cfile, mp_name);
}

int
cli_hse_kvdb_list(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] [<kvdb>]", NULL,
            },
        .optionv =
            {
                OPTION_HELP, { "[-v|--verbose]", "Print KVDB details" }, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { "verbose", no_argument, 0, 'v' }, { NULL },
            },
        .configv =
            {
                { NULL },
            },
    };

    const char *mp_name = 0;
    bool        help = false;
    bool        verbose = false;
    int         c;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            case 'v':
                verbose = true;
                break;
            default:
                return EX_USAGE;
        }
    }

    c = cli->argc - cli->optind;
    assert(c >= 0);

    if (help) {
        cmd_print_help(self, help_style_usage, stdout);
        return 0;
    }

    if (c > 1) {
        fprintf(stderr, "%s: extra agrument '%s'\n", self->cmd_path, cli->argv[cli->optind + 1]);
        return EX_USAGE;
    }

    mp_name = cli->argv[cli->optind];

    return cli_hse_kvdb_list_impl(cli, mp_name, verbose);
}

int
cli_hse_kvdb_compact(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] [<kvdb>] [<config_param>=<value>]...", NULL,
            },
        .optionv =
            {
                OPTION_HELP,
                OPTION_CFILE,
                { "[-C|--compact]", "Issue compaction request" },
                { "[-t|--timeout SECS]", "Set compaction timeout in seconds (default: 300)" },
                { "[-s|--status]", "Get status of compaction request" },
                { "[-x|--cancel]", "Cancel compaction request" },
                { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' },
                { "config", required_argument, 0, 'c' },
                { "compact", no_argument, 0, 'C' },
                { "timeout", required_argument, 0, 't' },
                { "status", no_argument, 0, 's' },
                { "cancel", no_argument, 0, 'x' },
                { NULL },
            },
        .configv =
            {
                CONFIG_LOG_LVL, { NULL },
            },
    };

    const char *kvdb = 0;
    const char *cfile = 0;
    uint32_t    timeout_secs = 300;
    bool        compact = false;
    bool        status = false;
    bool        cancel = false;
    bool        help = false;
    int         c;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {

        switch (c) {
            case 'h':
                help = true;
                break;
            case 'c':
                cfile = optarg;
                break;
            case 'C':
                compact = true;
                break;
            case 's':
                status = true;
                break;
            case 'x':
                cancel = true;
                break;

            case 't':
                if (parse_u32(optarg, &timeout_secs)) {
                    fprintf(
                        stderr,
                        STR("%s: unable to parse"
                            " '%s' as an unsigned 32-bit"
                            " scalar value\n"),
                        self->cmd_path,
                        optarg);
                    return EX_USAGE;
                }
                break;

            default:
                return EX_USAGE;
        }
    }

    c = cli->argc - cli->optind;
    assert(c >= 0);

    if (help) {
        cmd_print_help(self, help_style_usage, stdout);
        return EX_USAGE;
    }

    kvdb = cli_next_arg(cli);
    if (!kvdb) {
        fprintf(stderr, "%s: missing kvdb name, use -h for help\n", self->cmd_path);
        return EX_USAGE;
    }


    return cli_hse_kvdb_compact_impl(cli, cfile, kvdb, compact, status, cancel, timeout_secs);
}

int
cli_hse_kvs_create_impl(struct cli *cli, const char *cfile, const char *kvdb, const char *kvs)
{
    struct hse_params *hp = 0;
    struct hse_kvdb *  db = 0;
    hse_err_t          herr = 0;
    int                rc = 0;

    if (!kvdb || !kvs) {
        assert(0);
        INTERNAL_ERROR();
        return -1;
    }

    if (cli_hse_init(cli))
        return -1;

    hp = cli_hse_params(
        cli, cfile, cli->argc - cli->optind, cli->argv + cli->optind, "kvdb.excl=1", 0);
    if (!hp)
        return -1;

    herr = hse_kvdb_open(kvdb, hp, &db);
    if (herr) {
        print_hse_err(cli, "hse_kvdb_open", herr);
        goto done;
    }

    herr = hse_kvdb_kvs_make(db, kvs, hp);
    if (herr) {
        print_hse_err(cli, "hse_kvdb_kvs_make", herr);
        goto done;
    }

    printf("Successfully created KVS %s/%s\n", kvdb, kvs);

done:
    hse_kvdb_close(db);
    hse_params_destroy(hp);
    return (herr || rc) ? -1 : 0;
}

int
cli_hse_kvs_destroy_impl(struct cli *cli, const char *cfile, const char *kvdb, const char *kvs)
{
    struct hse_params *hp = 0;
    struct hse_kvdb *  db = 0;
    hse_err_t          herr = 0;
    int                rc = 0;

    if (cli_hse_init(cli))
        return -1;

    hp = cli_hse_params(
        cli, cfile, cli->argc - cli->optind, cli->argv + cli->optind, "kvdb.excl=1", 0);
    if (!hp)
        return -1;

    herr = hse_kvdb_open(kvdb, hp, &db);
    if (herr) {
        print_hse_err(cli, "hse_kvdb_open", herr);
        goto done;
    }

    herr = hse_kvdb_kvs_drop(db, kvs);
    if (herr) {
        print_hse_err(cli, "hse_kvdb_kvs_drop", herr);
        goto done;
    }

    printf("Successfully destroyed KVS %s/%s\n", kvdb, kvs);

done:
    hse_kvdb_close(db);
    hse_params_destroy(hp);
    return (herr || rc) ? -1 : 0;
}

int
cli_hse_kvs_create(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <kvdb>/<kvs> [<config_param>=<value>]...", NULL,
            },
        .optionv =
            {
                OPTION_HELP, OPTION_CFILE, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { "config", required_argument, 0, 'c' }, { NULL },
            },
        .configv =
            {
                CONFIG_LOG_LVL, CONFIG_KVS_PFX_LEN, { NULL },
            },
    };

    const char *cfile = 0;
    char *      kvdb = 0;
    char *      kvs = 0;
    bool        help = false;
    int         c, rc;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            case 'c':
                cfile = optarg;
                break;
            default:
                return EX_USAGE;
        }
    }

    c = cli->argc - cli->optind;
    assert(c >= 0);

    if (c == 0 || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    kvdb = strdup(cli->argv[cli->optind++]);
    if (!kvdb) {
        fprintf(stderr, "%s: out of memory\n", self->cmd_path);
        return -1;
    }

    kvs = strchr(kvdb, '/');
    if (!kvs) {
        fprintf(stderr, "%s: invalid usage for <kvdb>/<kvs>: '%s'\n", self->cmd_path, kvdb);
        free(kvdb);
        return -1;
    }

    *kvs++ = '\0';

    rc = cli_hse_kvs_create_impl(cli, cfile, kvdb, kvs);

    free(kvdb);
    return rc;
}

int
cli_hse_kvs_destroy(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <kvdb>/<kvs> [<config_param>=<value>]...", NULL,
            },
        .optionv =
            {
                OPTION_HELP, OPTION_CFILE, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { "config", required_argument, 0, 'c' }, { NULL },
            },
        .configv =
            {
                CONFIG_LOG_LVL, { NULL },
            },
    };

    const char *cfile = 0;
    char *      kvdb = 0;
    char *      kvs = 0;
    bool        help = false;
    int         c, rc;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            case 'c':
                cfile = optarg;
                break;
            default:
                return EX_USAGE;
        }
    }

    c = cli->argc - cli->optind;
    assert(c >= 0);

    if (c == 0 || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    kvdb = strdup(cli->argv[cli->optind++]);
    if (!kvdb) {
        fprintf(stderr, "%s: out of memory\n", self->cmd_path);
        return -1;
    }

    kvs = strchr(kvdb, '/');
    if (!kvs) {
        fprintf(stderr, "%s: invalid help_usagev for <kvdb>/<kvs>: '%s'\n", self->cmd_path, kvdb);
        free(kvdb);
        return -1;
    }

    *kvs++ = '\0';

    rc = cli_hse_kvs_destroy_impl(cli, cfile, kvdb, kvs);

    free(kvdb);
    return rc;
}

int
cli_hse_kvs(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <command> ...", NULL,
            },
        .optionv =
            {
                OPTION_HELP, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { NULL },
            },
        .configv =
            {
                { NULL },
            },
    };

    const char *    sub_name;
    struct cli_cmd *sub_cmd;
    int             c;
    bool            help = false;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            default:
                return EX_USAGE;
        }
    }

    if (cli->optind == cli->argc || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    sub_name = cli->argv[cli->optind];
    sub_cmd = cli_cmd_lookup(self->cmd_subcommandv, sub_name);
    if (!sub_cmd) {
        fprintf(stderr, "%s: invalid command '%s', use -h fo help\n", self->cmd_path, sub_name);
        return EX_USAGE;
    }

    return sub_cmd->cmd_main(sub_cmd, cli);
}

int
cli_hse_version(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options]", NULL,
            },
        .optionv =
            {
                OPTION_HELP, { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' }, { NULL },
            },
        .configv =
            {
                { NULL },
            },
    };

    bool help = false;
    int  c;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                help = true;
                break;
            default:
                return EX_USAGE;
        }
    }

    if (cli->optind != cli->argc || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    printf("version: %s\n", hse_kvdb_version_string());

    return 0;
}

int
cli_hse(struct cli_cmd *self, struct cli *cli)
{
    const struct cmd_spec spec = {
        .usagev =
            {
                "[options] <command> ...", NULL,
            },
        .optionv =
            {
                OPTION_HELP,
                { "[-H|--longhelp]", "Print long help" },
                { "[-S|--summary]", "Print summary help" },
                { NULL },
            },
        .longoptv =
            {
                { "help", no_argument, 0, 'h' },
                { "longhelp", no_argument, 0, 'H' },
                { "summary", no_argument, 0, 'S' },
                { NULL },
            },
        .configv =
            {
                { NULL },
            },
    };

    const char *    sub_name;
    struct cli_cmd *sub_cmd;
    int             c;
    bool            help = false;

    if (cli_hook(cli, self, &spec))
        return 0;

    while (-1 != (c = cli_getopt(cli))) {
        switch (c) {
            case 'h':
                /* local help -- for this command */
                help = true;
                break;
            case 'H':
                /* global help -- all commands */
                cli->help_show_all = true;
                cli->help_style = help_style_full;
                return 0;
            case 'S':
                /* global help -- all commands */
                cli->help_show_all = true;
                cli->help_style = help_style_leaf_summary;
                return 0;
            default:
                return EX_USAGE;
        }
    }

    if (cli->optind == cli->argc || help) {
        cmd_print_help(self, help_style_usage, help ? stdout : stderr);
        return help ? 0 : EX_USAGE;
    }

    sub_name = cli->argv[cli->optind];
    sub_cmd = cli_cmd_lookup(self->cmd_subcommandv, sub_name);
    if (!sub_cmd) {
        fprintf(stderr, "%s: invalid command '%s', use -h fo help\n", self->cmd_path, sub_name);
        return EX_USAGE;
    }

    return sub_cmd->cmd_main(sub_cmd, cli);
}

int
main(int argc, char **argv)
{
    struct cli cli;
    char *     prog;
    int        rc;

    prog = strrchr(argv[0], '/');
    if (prog)
        argv[0] = prog + 1;

    cmd_tree_set_paths(&cli_root);

    cli_init(&cli, argc, argv);

    rc = cli_root.cmd_main(&cli_root, &cli);

    if (!rc && cli.help_show_all) {
        /* ugliness :( */
        enum help_style style = cli.help_style;
        cli_init(&cli, argc, argv);
        cli.help_show_all = true;
        cli.help_style = style;
        cli_root.cmd_main(&cli_root, &cli);
    }

    cli_hse_fini(&cli);

    return rc;
}
