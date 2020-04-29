/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#include <mpool/mpool.h>

#include "mock_mpool.h"

#include "../../kvdb/kvdb_log.h"

#include <hse_util/string.h>

#define C1_IO_MAX_LOG 8192

static FILE *c1_mlog_fp[C1_IO_MAX_LOG];
static FILE *c1_mdc_fp[C1_IO_MAX_LOG];
static int   c1_next_logoid;
static int   c1_next_mdcoid;
static char  c1_test_dir[MAXPATHLEN + 1];

static char *
c1_mock_mpool_get_test_dir(void)
{
    return c1_test_dir;
}

static void
c1_mock_mpool_remove_test_dir(void)
{
    char cmd[MAXPATHLEN + 101];

    snprintf(cmd, MAXPATHLEN, "rm -rf %s", c1_mock_mpool_get_test_dir());

    system(cmd);
}

static int
c1_mock_mpool_create_test_dir(void)
{
    size_t      n;
    const char *tmp;
    char template[MAXPATHLEN + 1];

    /* Set to /XXXX in case creating the
     * temp dir fails.  It is a backup in case caller ignores
     * a failed return codes.  It is intended to not work,
     * but also be somewhat safe if code executes
     * "rm -fr /XXXX".
     */
    n = strlcpy(c1_test_dir, "/XXXX", sizeof(c1_test_dir));
    if (n >= sizeof(c1_test_dir)) {
        assert(0);
        return -1;
    }

    tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";

    n = snprintf(template, sizeof(template), "%s/hse-smoke-XXXXXX", tmp);
    if (n >= sizeof(template)) {
        assert(0);
        return -1;
    }

    tmp = mkdtemp(template);
    if (!tmp) {
        assert(0);
        return -1;
    }

    n = strlcpy(c1_test_dir, tmp, sizeof(c1_test_dir));
    if (n >= sizeof(c1_test_dir)) {
        assert(0);
        return -1;
    }

    return 0;
}

static void
c1_mock_mpool_init(void)
{
    int i;

    c1_next_logoid = 1;
    c1_next_mdcoid = 1;

    for (i = 0; i < C1_IO_MAX_LOG; i++) {
        c1_mlog_fp[i] = NULL;
        c1_mdc_fp[i] = NULL;
    }

    c1_mock_mpool_create_test_dir();
}

static void
c1_mock_mpool_fini(void)
{
    int i;

    for (i = 0; i < C1_IO_MAX_LOG; i++) {
        if (c1_mlog_fp[i]) {
            fclose(c1_mlog_fp[i]);
            c1_mlog_fp[i] = NULL;
        }

        if (c1_mdc_fp[i]) {
            fclose(c1_mdc_fp[i]);
            c1_mdc_fp[i] = NULL;
        }
    }

    c1_mock_mpool_remove_test_dir();
}

static FILE *
mock_open_mdc(u64 oid1, u64 oid2)
{
    char  filename[MAXPATHLEN + 1];
    FILE *fp;
    int   idx;

    idx = (int)oid1;
    if (idx > c1_next_mdcoid)
        return NULL;

    if (c1_mdc_fp[idx]) {
        rewind(c1_mdc_fp[idx]);
        return c1_mdc_fp[idx];
    }

    snprintf(filename, MAXPATHLEN, "%s/mdc-%d", c1_mock_mpool_get_test_dir(), idx);
    hse_log(HSE_DEBUG "c1 replay MDC %d-%d filename %s", idx, idx, filename);

    fp = fopen(filename, "w+");
    if (!fp)
        return NULL;

    c1_mdc_fp[idx] = fp;

    return fp;
}

static FILE *
mock_open_mlog(u64 oid)
{
    char  filename[MAXPATHLEN + 1];
    FILE *fp;
    int   idx;

    idx = (int)oid;

    if (idx < 0 || idx > c1_next_logoid)
        return NULL;

    if (c1_mlog_fp[idx])
        return c1_mlog_fp[idx];

    snprintf(filename, MAXPATHLEN, "%s/mlog-%d", c1_mock_mpool_get_test_dir(), idx);

    fp = fopen(filename, "w+");
    if (!fp)
        return NULL;

    hse_log(HSE_DEBUG "c1 replay MLOG %d-%d filename %s fp %p", idx, idx, filename, fp);

    c1_mlog_fp[idx] = fp;

    return fp;
}

static uint64_t
_mpool_mlog_len(struct mpool *ds, struct mpool_mlog *mlh, size_t *len)
{
    FILE *fp = (FILE *)mlh;

    *len = ftell(fp);

    return 0;
}

static uint64_t
_mpool_mlog_alloc(
    struct mpool *        ds,
    struct mlog_capacity *capreq,
    enum mp_media_classp  mclassp,
    struct mlog_props *   props,
    struct mpool_mlog **  mlh)
{
    FILE *fp;
    char  filename[MAXPATHLEN + 1];

    snprintf(filename, MAXPATHLEN, "%s/mlog-%d", c1_mock_mpool_get_test_dir(), c1_next_logoid);

    if (c1_next_logoid >= C1_IO_MAX_LOG)
        return merr(ev(EINVAL));

    fp = fopen(filename, "w+");
    if (!fp)
        return merr(ev(EINVAL));

    if (ftruncate(fileno(fp), 0)) {
        fclose(fp);
        return merr(ev(EINVAL));
    }

    hse_log(HSE_DEBUG "MLOG open oid %d filename %s fp %p", c1_next_logoid, filename, fp);

    c1_mlog_fp[c1_next_logoid] = fp;
    props->lpr_objid = (u64)c1_next_logoid;
    *mlh = (struct mpool_mlog *)fp;

    ++c1_next_logoid;

    return 0;
}

static uint64_t
_mpool_mdc_alloc(
    struct mpool *             ds,
    uint64_t *                 logid1,
    uint64_t *                 logid2,
    enum mp_media_classp       mclassp,
    const struct mdc_capacity *capreq,
    struct mdc_props *         props)
{
    char  filename[MAXPATHLEN + 1];
    FILE *fp;

    snprintf(filename, MAXPATHLEN, "%s/mdc-%d", c1_mock_mpool_get_test_dir(), c1_next_mdcoid);

    if (c1_next_mdcoid >= C1_IO_MAX_LOG)
        return merr(ev(EINVAL));

    fp = fopen(filename, "w+");
    if (!fp)
        return merr(ev(EINVAL));

    if (ftruncate(fileno(fp), 0)) {
        fclose(fp);
        return merr(ev(EINVAL));
    }

    fclose(fp);

    *logid1 = c1_next_mdcoid;
    *logid2 = c1_next_mdcoid;

    ++c1_next_mdcoid;

    return 0;
}

static uint64_t
_mpool_mdc_open(
    struct mpool *     mp,
    uint64_t           logid1,
    uint64_t           logid2,
    uint8_t            flags,
    struct mpool_mdc **mdc_out)
{
    FILE *fp;

    fp = mock_open_mdc(logid1, logid2);
    if (!fp)
        return merr(ev(EINVAL));

    *mdc_out = (struct mpool_mdc *)fp;

    return 0;
}

static uint64_t
_mpool_mdc_close(struct mpool_mdc *mdc)
{
    FILE *fp = (FILE *)mdc;
    int   i;

    assert(fp);
    if (!fp)
        return merr(ev(EIO));

    for (i = 0; i < C1_IO_MAX_LOG; i++) {
        if (c1_mdc_fp[i] == fp) {
            /*
            fclose(c1_mdc_fp[i]);
            c1_mdc_fp[i] = NULL;
            */
            return 0;
        }
    }

    return merr(ev(EINVAL));
}

static uint64_t
_mpool_mdc_cstart(struct mpool_mdc *mdc)
{
    FILE *fp = (FILE *)mdc;
    int   fd;

    assert(fp);

    rewind(fp);

    fd = fileno(fp);
    if (ftruncate(fd, 0))
        return merr(ev(EINVAL));

    return 0;
}

static uint64_t
_mpool_mlog_find_get(
    struct mpool *      ds,
    u64                 oid,
    struct mlog_props * props,
    struct mpool_mlog **mlh_out)
{
    FILE *fp;

    fp = mock_open_mlog(oid);
    if (!fp)
        return merr(ev(EINVAL));

    *mlh_out = (struct mpool_mlog *)fp;

    return 0;
}

static uint64_t
_mpool_mlog_resolve(
    struct mpool *      ds,
    uint64_t            objid,
    struct mlog_props * props,
    struct mpool_mlog **mlh_out)
{
    return _mpool_mlog_find_get(ds, objid, props, mlh_out);
}

static uint64_t
_mpool_mlog_open(struct mpool *ds, struct mpool_mlog *mlh, uint8_t flags, uint64_t *gen)
{
    FILE *fp = (FILE *)mlh;

    assert(fp);
    if (!fp)
        return merr(ev(EIO));

    return 0;
}

static uint64_t
_mpool_mlog_put(struct mpool *ds, struct mpool_mlog *mlh)
{
    return 0;
}

static uint64_t
_mpool_mlog_delete(struct mpool *ds, struct mpool_mlog *mlh)
{
    return 0;
}

static uint64_t
_mpool_mlog_close(struct mpool *ds, struct mpool_mlog *mlh)
{
    FILE *fp = (FILE *)mlh;
    int   i;

    assert(fp);
    if (!fp)
        return merr(ev(EIO));

    for (i = 0; i < C1_IO_MAX_LOG; i++) {
        if (c1_mlog_fp[i] == fp) {
            /*
            fclose(c1_mlog_fp[i]);
            c1_mlog_fp[i] = NULL;
            */
            return 0;
        }
    }

    return merr(ev(EINVAL));
}

static uint64_t
_mpool_mdc_append(struct mpool_mdc *mdc, void *data, ssize_t len, bool sync)
{
    FILE *fp = (FILE *)mdc;

    assert(fp);

    if (fwrite(data, 1, len, fp) == len) {
        fflush(fp);
        return 0;
    }

    return merr(ev(EIO));
}

static uint64_t
_mpool_mlog_append_data(struct mpool *ds, struct mpool_mlog *mlh, void *data, size_t len, int sync)
{
    FILE *fp = (FILE *)mlh;

    /*
    hse_log(HSE_DEBUG "mlog_append fp %p pos %ld size %ld",
        fp, ftell(fp), len);
    */

    if (fwrite(data, 1, len, fp) == len) {
        fflush(fp);
        return 0;
    }

    return merr(ev(EIO));
}

static uint64_t
_mpool_mlog_append_datav(
    struct mpool *     ds,
    struct mpool_mlog *mlh,
    struct iovec *     iov,
    size_t             len,
    int                sync)
{
    FILE * fp = (FILE *)mlh;
    size_t bytes = len;
    int    iovcnt = 0;

    while (bytes > 0) {
        bytes -= iov[iovcnt].iov_len;
        iovcnt++;
    }

    bytes = writev(fileno(fp), iov, iovcnt);
    if (bytes == len) {
        fflush(fp);
        return 0;
    }

    return merr(ev(EIO));
}

static uint64_t
_mpool_mlog_read_data_next(
    struct mpool *     ds,
    struct mpool_mlog *mlh,
    void *             data,
    size_t             len,
    size_t *           rdlen)
{
    FILE *fp = (FILE *)mlh;

    assert(fp);

    *rdlen = fread(data, 1, len, fp);
    if (*rdlen)
        return 0;

    return merr(ev(ERANGE));
}

uint64_t
_mpool_mlog_seek_read_data_next(
    struct mpool *     ds,
    struct mpool_mlog *mlh,
    size_t             seek,
    void *             data,
    size_t             len,
    size_t *           rdlen)
{
    FILE *fp = (FILE *)mlh;
    int   sklen;

    assert(fp);

    /*
    hse_log(HSE_DEBUG "mlog seek read fp %p pos %ld seek %ld len %ld",
        fp, ftell(fp), seek, len);
    */

    if (seek != 0) {
        sklen = fseek(fp, seek, SEEK_CUR);
        if (sklen < 0)
            fseek(fp, 0, SEEK_END);
    }

    return _mpool_mlog_read_data_next(ds, mlh, data, len, rdlen);
}

static uint64_t
_mpool_mdc_read(struct mpool_mdc *mdc, void *data, size_t len, size_t *rdlen)
{
    FILE *fp = (FILE *)mdc;

    errno = 0;

    assert(fp);

    *rdlen = fread(data, 1, len, fp);

    hse_log(
        HSE_DEBUG "mpool_mdc_read fp %p offset %ld bytes %ldi read %ld",
        fp,
        ftell(fp),
        len,
        *rdlen);

    if (errno == ENOENT)
        return 0;

    return merr(ev(errno));
}

static uint64_t
_mpool_mdc_rewind(struct mpool_mdc *mdc)
{
    FILE *fp = (FILE *)mdc;

    assert(fp);

    rewind(fp);

    return 0;
}

static uint64_t
_mpool_mlog_read_data_init(struct mpool *ds, struct mpool_mlog *mlh)
{
    FILE *fp = (FILE *)mlh;

    assert(fp);

    rewind(fp);

    return 0;
}

static uint64_t
_mpool_mlog_flush(struct mpool *ds, struct mpool_mlog *mlh)
{
    return 0;
}

uint64_t
_mpool_mlog_erase(struct mpool *ds, struct mpool_mlog *mlh, uint64_t mingen)
{
    FILE *fp = (FILE *)mlh;

    rewind(fp);
    if (ftruncate(fileno(fp), 0))
        return merr(EIO);

    fflush(fp);

    return 0;
}

static void
c1_mpool_unset_mock(void)
{
    MOCK_UNSET(mpool, _mpool_mlog_len);
    MOCK_UNSET(mpool, _mpool_mdc_append);
    MOCK_UNSET(mpool, _mpool_mdc_rewind);
    MOCK_UNSET(mpool, _mpool_mdc_read);
    MOCK_UNSET(mpool, _mpool_mdc_open);
    MOCK_UNSET(mpool, _mpool_mdc_close);
    MOCK_UNSET(mpool, _mpool_mlog_append_data);
    MOCK_UNSET(mpool, _mpool_mlog_append_datav);
    MOCK_UNSET(mpool, _mpool_mlog_flush);
    MOCK_UNSET(mpool, _mpool_mlog_read_data_next);
    MOCK_UNSET(mpool, _mpool_mlog_seek_read_data_next);
    MOCK_UNSET(mpool, _mpool_mlog_read_data_init);
    MOCK_UNSET(mpool, _mpool_mdc_alloc);
    MOCK_UNSET(mpool, _mpool_mlog_alloc);
    MOCK_UNSET(mpool, _mpool_mlog_find_get);
    MOCK_UNSET(mpool, _mpool_mlog_resolve);
    MOCK_UNSET(mpool, _mpool_mlog_close);
    MOCK_UNSET(mpool, _mpool_mlog_delete);
    MOCK_UNSET(mpool, _mpool_mlog_put);
    MOCK_UNSET(mpool, _mpool_mlog_open);
    MOCK_UNSET(mpool, _mpool_mdc_cstart);
    MOCK_UNSET(mpool, _mpool_mlog_erase);

    mapi_inject_unset(mapi_idx_mpool_mdc_close);
    mapi_inject_unset(mapi_idx_c0_put);
    mapi_inject_unset(mapi_idx_mpool_mdc_commit);
    mapi_inject_unset(mapi_idx_mpool_mlog_commit);
    mapi_inject_unset(mapi_idx_mpool_mdc_get_root);
    mapi_inject_unset(mapi_idx_mpool_mdc_sync);
    mapi_inject_unset(mapi_idx_mpool_mdc_destroy);
    mapi_inject_unset(mapi_idx_mpool_mdc_cend);
}

static void
c1_mpool_set_mock(void)
{
    c1_mpool_unset_mock();

    MOCK_SET(mpool, _mpool_mlog_len);
    MOCK_SET(mpool, _mpool_mdc_append);
    MOCK_SET(mpool, _mpool_mdc_rewind);
    MOCK_SET(mpool, _mpool_mdc_read);
    MOCK_SET(mpool, _mpool_mdc_open);
    MOCK_SET(mpool, _mpool_mdc_close);
    MOCK_SET(mpool, _mpool_mlog_append_data);
    MOCK_SET(mpool, _mpool_mlog_append_datav);
    MOCK_SET(mpool, _mpool_mlog_flush);
    MOCK_SET(mpool, _mpool_mlog_read_data_next);
    MOCK_SET(mpool, _mpool_mlog_seek_read_data_next);
    MOCK_SET(mpool, _mpool_mlog_read_data_init);
    MOCK_SET(mpool, _mpool_mdc_alloc);
    MOCK_SET(mpool, _mpool_mlog_alloc);
    MOCK_SET(mpool, _mpool_mlog_find_get);
    MOCK_SET(mpool, _mpool_mlog_resolve);
    MOCK_SET(mpool, _mpool_mlog_close);
    MOCK_SET(mpool, _mpool_mlog_delete);
    MOCK_SET(mpool, _mpool_mlog_put);
    MOCK_SET(mpool, _mpool_mlog_open);
    MOCK_SET(mpool, _mpool_mdc_cstart);
    MOCK_SET(mpool, _mpool_mlog_erase);

    mapi_inject(mapi_idx_mpool_mdc_commit, 0);
    mapi_inject(mapi_idx_mpool_mlog_commit, 0);
    mapi_inject(mapi_idx_mpool_mdc_get_root, 0);
    mapi_inject(mapi_idx_mpool_mdc_cend, 0);
    mapi_inject(mapi_idx_mpool_mdc_sync, 0);
    mapi_inject(mapi_idx_mpool_mdc_destroy, 0);
}

void
c1_mock_mpool(void)
{
    c1_mock_mpool_fini();
    c1_mock_mpool_init();
    c1_mpool_unset_mock();
    c1_mpool_set_mock();
}

void
c1_unmock_mpool(void)
{
    c1_mock_mpool_fini();
    c1_mpool_unset_mock();
}
