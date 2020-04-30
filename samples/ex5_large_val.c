/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2017 Micron Technology, Inc.  All rights reserved.
 */

/*
 * This example demonstrates how one could add key-value pairs where the value
 * length could be larger than the allowed maximum HSE_KVS_VLEN_MAX.
 *
 * To put keys, this example uses files passed to it on the commandline. Each
 * file's name forms the key and its contents the value. For instance, if one
 * were to put /tmp/foo and /tmp/bar in the kvs mp1/kvdb1/kvs1, the commandline
 * would read:
 *
 *            large_val mp1 kvdb1 kvs1 /tmp/foo /tmp/bar
 *
 * This would put keys { /tmp/foo0, /tmp/foo1, /tmp/foo2, ... , /tmp/fooN } for
 * '/tmp/foo'. Each of these key chunks will hold a chunk of the value.
 * Similarly, /tmp/bar will be split into multiple chunks.
 *
 * To extract the key-value pairs, use the option '-x' on the commandline. For
 * the example above, the commandline will look like this:
 *
 *            large_val mp1 kvdb1 kvs1 -x /tmp/foo /tmp/bar
 *
 * And the values for each key/file will be output into '/tmp/foo-out' and
 * '/tmp/bar-out' respectively
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>

#include <hse/hse.h>

char * progname;

static void
err_print(const char *fmt, ...)
{
    char    msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "Error: %s: %s\n", progname, msg);
}

hse_err_t
extract_kv_to_files(struct hse_kvs *kvs, int file_cnt, char **files)
{
    int                    fd, i;
    struct hse_kvs_cursor *cur;

    for (i = 0; i < file_cnt; i++) {
        char        pfx[HSE_KVS_KLEN_MAX];
        char        outfile[NAME_MAX + 8]; /* Extra bytes for '.out' suffix */
        const void *key, *val;
        size_t      klen, vlen;
        bool        eof;

        snprintf(outfile, sizeof(outfile), "%s.%s", files[i], "out");
        snprintf(pfx, sizeof(pfx), "%s|", files[i]);
        printf("filename: %s\n", outfile);

        fd = open(outfile, O_RDWR | O_CREAT);
        if (fd < 0) {
            err_print("Error opening file %s: %s\n", outfile, strerror(errno));
            exit(1);
        }

        hse_kvs_cursor_create(kvs, NULL, pfx, strlen(pfx), &cur);

        do {
            hse_kvs_cursor_read(cur, NULL, &key, &klen, &val, &vlen, &eof);
            if (eof)
                break;

            write(fd, (char *)val, vlen);
        } while (!eof);

        hse_kvs_cursor_destroy(cur);

        close(fd);
    }

    return 0;
}

hse_err_t
put_files_as_kv(struct hse_kvdb *kvdb, struct hse_kvs *kvs, int kv_cnt, char **keys)
{
    int       fd, i;
    hse_err_t rc;

    for (i = 0; i < kv_cnt; i++) {
        char    val[HSE_KVS_VLEN_MAX];
        char    key_chunk[HSE_KVS_KLEN_MAX];
        ssize_t len;
        int     chunk_nr;

        printf("Inserting chunks for %s\n", (char *)keys[i]);
        fd = open(keys[i], O_RDONLY);
        if (fd < 0) {
            err_print("Error opening file %s: %s\n", keys[i], strerror(errno));
            exit(1);
        }

        chunk_nr = 0;
        do {
            len = read(fd, val, sizeof(val));
            if (len <= 0)
                break;

            snprintf(key_chunk, sizeof(key_chunk), "%s|%08x", (char *)keys[i], chunk_nr);

            rc = hse_kvs_put(kvs, NULL, key_chunk, strlen(key_chunk), val, len);

            chunk_nr++;

        } while (!rc && len > 0);

        close(fd);
    }

    return 0;
}

int
usage()
{
    printf(
        "usage: %s [options] <kvdb> <kvs> <file1> [<fileN> ...]\n"
        "-x  Extract specified files' contents to 'file.out'\n",
        progname);
    return 1;
}

int
main(int argc, char **argv)
{
    char *           mp_name, *kvs_name;
    struct hse_kvdb *kvdb;
    struct hse_kvs * kvs;
    char             c;
    bool             extract = false;
    hse_err_t        rc;
    char             ebuf[128];

    progname = argv[0];

    while ((c = getopt(argc, argv, "xh")) != -1) {
        switch (c) {
            case 'x':
                extract = true;
                break;
            case 'h':
                usage();
                return 0;
            default:
                break;
        }
    }

    if (argc < 4)
        return usage();

    mp_name = argv[optind++];
    kvs_name = argv[optind++];

    rc = hse_kvdb_init();
    if (rc) {
        err_print("Failed to initialize kvdb: %s\n", hse_err_to_string(rc, ebuf, sizeof(ebuf), 0));
        exit(1);
    }

    rc = hse_kvdb_open(mp_name, NULL, &kvdb);
    if (rc) {
        err_print("Cannot open kvdb: %s\n", hse_err_to_string(rc, ebuf, sizeof(ebuf), 0));
        exit(1);
    }

    rc = hse_kvdb_kvs_open(kvdb, kvs_name, NULL, &kvs);
    if (rc)
        exit(1);

    if (extract)
        rc = extract_kv_to_files(kvs, argc - optind, &argv[optind]);
    else
        rc = put_files_as_kv(kvdb, kvs, argc - optind, &argv[optind]);

    hse_kvdb_close(kvdb);
    hse_kvdb_fini();

    return rc;
}
