/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#include <hse_util/platform.h>
#include <hse_util/hse_err.h>
#include <hse_util/event_counter.h>
#include <hse_util/timing.h>
#include <hse_util/alloc.h>
#include <hse_util/slab.h>
#include <hse_util/condvar.h>
#include <hse_util/rcu.h>

#define MTF_MOCK_IMPL_c0

#include <hse/hse.h>

#include <hse_ikvdb/ikvdb.h>
#include <hse_ikvdb/kvdb_health.h>
#include <hse_ikvdb/c0.h>
#include <hse_ikvdb/c0sk.h>
#include <hse_ikvdb/c0skm.h>
#include <hse_ikvdb/cn.h>
#include <hse_ikvdb/c0_kvset.h>
#include <hse_ikvdb/c0_kvmultiset.h>
#include <hse_ikvdb/kvdb_ctxn.h>

struct rcu_head;
struct cursor_summary;

#define c0_h2r(handle) container_of(handle, struct c0_impl, c0_handle)

struct c0 {
};

/**
 * struct c0_impl - private representation of c0
 * @c0_handle:          opaque handle for users of a struct c0
 * @c0_index:           index assigned to this C0
 * @c0_cn:              struct cn to ingest into
 * @c0_rp:              configuration data
 * @c0_c0sk:            handle to container poly C0, if within a poly C0
 * @c0_pfx_len:         prefix length for this c0
 * @c0_sfx_len:         suffix length for this c0
 *
 * [HSE_REVISIT]
 */
struct c0_impl {
    struct c0           c0_handle;
    struct c0sk *       c0_c0sk;
    u32                 c0_index;
    s32                 c0_pfx_len;
    u32                 c0_sfx_len;
    u64                 c0_hash;
    struct cn *         c0_cn;
    struct kvs_rparams *c0_rp; /* not owned by c0 */
};

merr_t
c0_init(void)
{
    rcu_init();
    c0kvs_init();
    c0kvms_init();
    kvdb_ctxn_locks_init();

    return 0;
}

void
c0_fini(void)
{
    /* [HSE_REVISIT] */

    c0kvs_fini();
    c0kvms_fini();
    kvdb_ctxn_locks_fini();
}

s32
c0_get_pfx_len(struct c0 *handle)
{
    struct c0_impl *self = c0_h2r(handle);

    return self->c0_pfx_len;
}

u32
c0_get_sfx_len(struct c0 *handle)
{
    struct c0_impl *self = c0_h2r(handle);

    return self->c0_sfx_len;
}

merr_t
c0_put(struct c0 *handle, const struct kvs_ktuple *kt, const struct kvs_vtuple *vt, u64 seqno)
{
    struct c0_impl *self = c0_h2r(handle);

    assert(self->c0_index < HSE_KVS_COUNT_MAX);
    return c0sk_put(self->c0_c0sk, self->c0_index, kt, vt, seqno);
}

merr_t
c0_del(struct c0 *handle, struct kvs_ktuple *kt, u64 seqno)
{
    struct c0_impl *self = c0_h2r(handle);

    assert(self->c0_index < HSE_KVS_COUNT_MAX);
    return c0sk_del(self->c0_c0sk, self->c0_index, kt, seqno);
}

merr_t
c0_prefix_del(struct c0 *handle, struct kvs_ktuple *kt, u64 seqno)
{
    struct c0_impl *self = c0_h2r(handle);

    assert(self->c0_index < HSE_KVS_COUNT_MAX);
    return c0sk_prefix_del(self->c0_c0sk, self->c0_index, kt, seqno);
}

/*
 * Tombstone indicated by:
 *     return value == 0 && res == FOUND_TOMB
 */
merr_t
c0_get(
    struct c0 *              handle,
    const struct kvs_ktuple *kt,
    u64                      view_seqno,
    uintptr_t                seqnoref,
    enum key_lookup_res *    res,
    struct kvs_buf *         vbuf)
{
    struct c0_impl *self;

    self = c0_h2r(handle);

    assert(self->c0_index < HSE_KVS_COUNT_MAX);
    return c0sk_get(
        self->c0_c0sk, self->c0_index, self->c0_pfx_len, kt, view_seqno, seqnoref, res, vbuf);
}

merr_t
c0_pfx_probe(
    struct c0 *              handle,
    const struct kvs_ktuple *kt,
    u64                      view_seqno,
    uintptr_t                seqnoref,
    enum key_lookup_res *    res,
    struct query_ctx *       qctx,
    struct kvs_buf *         kbuf,
    struct kvs_buf *         vbuf)
{
    struct c0_impl *self;

    self = c0_h2r(handle);

    assert(self->c0_index < HSE_KVS_COUNT_MAX);
    return c0sk_pfx_probe(
        self->c0_c0sk,
        self->c0_index,
        self->c0_pfx_len,
        self->c0_sfx_len,
        kt,
        view_seqno,
        seqnoref,
        res,
        qctx,
        kbuf,
        vbuf);
}

merr_t
c0_open(
    struct ikvdb *      kvdb,
    struct kvs_rparams *rp,
    struct cn *         cn,
    struct mpool *      mp_dataset,
    struct c0 **        c0)
{
    struct c0_impl *    new_c0 = 0;
    merr_t              err;
    u16                 skidx;
    struct kvs_cparams *cp = cn_get_cparams(cn);

    new_c0 = calloc(1, sizeof(*new_c0));
    if (!new_c0) {
        err = merr(ENOMEM);
        hse_elog(HSE_ERR "Allocation failed for struct c0: @@e", err);
        goto err_exit;
    }

    assert(cn);
    new_c0->c0_pfx_len = cp->cp_pfx_len;
    new_c0->c0_sfx_len = cp->cp_sfx_len;
    new_c0->c0_cn = cn;
    new_c0->c0_rp = rp;

    ikvdb_get_c0sk(kvdb, &new_c0->c0_c0sk);
    if (!new_c0->c0_c0sk) {
        free(new_c0);
        return merr(ev(EINVAL));
    }

    err = c0sk_c0_register(new_c0->c0_c0sk, new_c0->c0_cn, &skidx);
    if (ev(err))
        goto err_exit;

    new_c0->c0_hash = cn_hash_get(cn);
    new_c0->c0_index = skidx;
    *c0 = &new_c0->c0_handle;

    return 0;

err_exit:
    free(new_c0);
    hse_elog(HSE_INFO "c0_open failed: @@e", err);

    return err;
}

merr_t
c0_close(struct c0 *handle)
{
    merr_t          err = 0, tmp_err;
    struct c0_impl *self;

    if (!handle)
        return merr(ev(EINVAL));

    self = c0_h2r(handle);

    tmp_err = c0_sync(handle);

    if (ev(tmp_err))
        err = tmp_err;

    tmp_err = c0sk_c0_deregister(self->c0_c0sk, self->c0_index);
    if (!err && ev(tmp_err))
        err = tmp_err;

    free(self);

    return err;
}

merr_t
c0_cursor_create(
    struct c0 *            handle,
    u64                    seqno,
    bool                   reverse,
    const void *           prefix,
    size_t                 pfx_len,
    struct cursor_summary *summary,
    struct c0_cursor **    c0cur)
{
    struct c0_impl *self = c0_h2r(handle);
    merr_t          err;

    err = c0sk_cursor_create(
        self->c0_c0sk,
        seqno,
        self->c0_index,
        reverse,
        self->c0_pfx_len,
        prefix,
        pfx_len,
        summary,
        c0cur);
    return ev(err);
}

merr_t
c0_cursor_bind_txn(struct c0_cursor *c0cur, struct kvdb_ctxn *ctxn)
{
    return c0sk_cursor_bind_txn(c0cur, ctxn);
}

bool
c0_cursor_ctxn_preserve_tombspan(
    struct c0_cursor *c0cur,
    const void *      kmin,
    u32               kmin_len,
    const void *      kmax,
    u32               kmax_len)
{
    return c0sk_cursor_ctxn_preserve_tombspan(c0cur, kmin, kmin_len, kmax, kmax_len);
}

merr_t
c0_cursor_seek(
    struct c0_cursor * c0cur,
    const void *       seek,
    size_t             seeklen,
    struct kc_filter * filter,
    struct kvs_ktuple *kt)
{
    merr_t err;

    err = c0sk_cursor_seek(c0cur, seek, seeklen, filter, kt);
    return ev(err);
}

merr_t
c0_cursor_read(struct c0_cursor *c0cur, struct kvs_kvtuple *kvt, bool *eof)
{
    merr_t err;

    err = c0sk_cursor_read(c0cur, kvt, eof);
    return ev(err);
}

merr_t
c0_cursor_save(struct c0_cursor *c0cur)
{
    c0sk_cursor_save(c0cur);
    return 0;
}

merr_t
c0_cursor_restore(struct c0_cursor *c0cur)
{
    merr_t err;

    err = c0sk_cursor_restore(c0cur);
    return ev(err);
}

merr_t
c0_cursor_update(
    struct c0_cursor *       c0cur,
    u64                      seqno,
    const struct kvs_ktuple *kt_min,
    const struct kvs_ktuple *kt_max,
    u32 *                    flags_out)
{
    merr_t err;

    err = c0sk_cursor_update(c0cur, seqno, kt_min, kt_max, flags_out);
    return ev(err);
}

merr_t
c0_cursor_destroy(struct c0_cursor *c0cur)
{
    merr_t err;

    err = c0sk_cursor_destroy(c0cur);
    return ev(err);
}

/* Sync only forces all current data to media -- it does not
 * prevent new data from being created while the sync blocks.
 */
merr_t
c0_sync(struct c0 *handle)
{
    struct c0_impl *self = c0_h2r(handle);

    merr_t err;

    if (self->c0_rp->rdonly)
        return 0;

    /* Issue c0sk_sync first so that the contents of the closing KVS is
     * already in cN when c0skm_sync is invoked, hence less work for it.
     * */
    err = c0sk_sync(self->c0_c0sk);
    if (err)
        return ev(err);

    return c0skm_sync(self->c0_c0sk);
}

u16
c0_index(struct c0 *handle)
{
    struct c0_impl *self = c0_h2r(handle);

    return self->c0_index;
}

u64
c0_hash_get(struct c0 *handle)
{
    struct c0_impl *self = c0_h2r(handle);

    return self->c0_hash;
}

#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
#include "c0_ut_impl.i"
#endif /* HSE_UNIT_TEST_MODE */
