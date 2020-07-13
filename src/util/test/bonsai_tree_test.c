/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#define BONSAI_TREE_CLIENT_VERIFY
#define BONSAI_TREE_CURSOR_HEAP

#include <hse_util/string.h>
#include <hse_util/alloc.h>
#include <hse_util/hse_err.h>
#include <hse_util/cursor_heap.h>
#include <hse_util/atomic.h>
#include <hse_ut/framework.h>
#include <hse_util/logging.h>

#include "../src/bonsai_tree_urcu.h"

#include <hse_util/bonsai_tree.h>
#include <hse_util/platform.h>
#include <hse_util/keycmp.h>
#include <hse_util/seqno.h>

#include "../src/bonsai_tree_pvt.h"

#include <hse_test_support/mwc_rand.h>

#define ALLOC_LEN_MAX 1344

static struct cheap *cheap;

/* [HSE_REVISIT] Need to replace these constants, macros. */
#define HSE_CORE_TOMB_REG ((void *)~0x1UL)
#define HSE_CORE_TOMB_PFX ((void *)~0UL)
#define GB (1024 * 1024 * 1024)
#define MB (1024 * 1024)

struct bonsai_root *   broot;
static int             induce_alloc_failure;
unsigned long          key_begin = 1;
unsigned long          key_end = 999999999;
static int             stop_producer_threads;
static int             stop_consumer_threads;
static unsigned long   key_current = 1;
static int             num_consumers = 4;
static int             num_producers = 4;
static int             runtime_insecs = 7;
static int             random_number;
static size_t          key_size = 10;
static size_t          val_size = 100;
static pthread_mutex_t mtx;

static __thread struct mwc_rand mwc;

static void
xrand_init(uint64_t seed64)
{
    u32 seed32 = seed64;

    seed32 = seed32 ^ (seed64 >> 32);

    mwc_rand_init(&mwc, seed32);
}

static uint64_t
xrand(void)
{
    return mwc_rand64(&mwc);
}

static void
bonsai_client_insert_callback(
    void *                cli_rock,
    enum bonsai_ior_code *code,
    struct bonsai_kv *    kv,
    struct bonsai_val *   new_val,
    struct bonsai_val **  old_val)
{
    struct bonsai_val * old;
    struct bonsai_val **prevp;

    uintptr_t seqnoref;

    if (IS_IOR_INS(*code) || !new_val)
        /* Do only the stats */
        return;

    assert(IS_IOR_REPORADD(*code));

    /* Search for an existing value with the given seqnoref */
    prevp = &kv->bkv_values;
    SET_IOR_ADD(*code);
    seqnoref = new_val->bv_seqnoref;

    assert(kv->bkv_values);

    old = kv->bkv_values;
    while (old) {
        if (seqnoref == old->bv_seqnoref) {
            SET_IOR_REP(*code);
            break;
        }

        if (seqnoref_gt(seqnoref, old->bv_seqnoref))
            break;

        prevp = &old->bv_next;
        old = old->bv_next;
    }

    if (IS_IOR_REP(*code)) {
        /* in this case we'll just replace the old list element */
        new_val->bv_next = old->bv_next;
    } else if (HSE_SQNREF_ORDNL_P(seqnoref)) {
        /* slot the new element just in front of the next older one */
        new_val->bv_next = old;
    } else {
        /* rewind & slot the new element at the front of the list */
        prevp = &kv->bkv_values;
        new_val->bv_next = *prevp;
    }

    /* Publish the new value node.  New readers will see the new node,
     * while existing readers may continue to use the old node until
     * the end of the current grace period.
     */
    rcu_assign_pointer(*prevp, new_val);

    /* Do the stats here */

    if (IS_IOR_REP(*code))
        *old_val = old;
}

static int
cmpKey(const void *p1, const void *p2)
{
    const struct bonsai_skey *skp1 = p1;
    const struct bonsai_skey *skp2 = p2;

    return key_full_cmp(&skp1->bsk_key_imm, skp1->bsk_key, &skp2->bsk_key_imm, skp2->bsk_key);
}

static u64
decrement_key(u64 key, int numeric)
{
    if (numeric)
        return key - 1;
    else
        return ((key & 0x00ffffffffffffff) | 0x2000000000000000);
}

static u64
increment_key(u64 key, int numeric)
{
    if (numeric)
        return key + 1;
    else
        return ((key & 0x00ffffffffffffff) | 0x7b00000000000000);
}

void
init_tree(struct bonsai_root **tree, enum bonsai_alloc_mode allocm)
{
    merr_t err;

    if (allocm == HSE_ALLOC_CURSOR) {
        cheap = cheap_create(8, 128 * MB);
        if (!cheap)
            return;
    }

    err = bn_create(cheap, 64 * MB, bonsai_client_insert_callback, NULL, tree);
    if (err)
        hse_log(HSE_ERR "Bonsai tree create failed");
}

/*
 * c0's callback to pick the right value from the value list, based on
 * sequence numbers.
 */
static struct bonsai_val *
findValue(struct bonsai_kv *kv, u64 view_seqno, uintptr_t seqnoref)
{
    struct bonsai_val *val_ge, *val;
    u64                diff_ge, diff;

    diff_ge = ULONG_MAX;
    val_ge = NULL;

    for (val = kv->bkv_values; val; val = val->bv_next) {
        diff = seqnoref_ext_diff(view_seqno, val->bv_seqnoref);
        if (diff < diff_ge) {
            diff_ge = diff;
            val_ge = val;
        }

        if (!seqnoref) {
            if (diff_ge == 0)
                break;
            continue;
        }

        if (seqnoref == val->bv_seqnoref)
            return val;

        diff = seqnoref_diff(seqnoref, val->bv_seqnoref);
        if (diff < diff_ge) {
            diff_ge = diff;
            val_ge = val;
        }
    }

    return val_ge;
}

static struct bonsai_val *
findPfxValue(struct bonsai_kv *kv, uintptr_t seqnoref)
{
    struct bonsai_val *val;

    val = kv->bkv_values;
    while (val) {
        if (val->bv_valuep == HSE_CORE_TOMB_PFX) {
            if ((val->bv_seqnoref == seqnoref) || seqnoref_ge(seqnoref, val->bv_seqnoref))
                break;
        }
        val = val->bv_next;
    }

    return val;
}

int
test_collection_setup(struct mtf_test_info *info)
{
#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
    fail_nth_alloc_test_pre(info);
#endif

    xrand_init(time(NULL));

#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_REGISTER();
#endif
#endif

    return 0;
}

int
test_collection_teardown(struct mtf_test_info *info)
{

#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_UNREGISTER();
#endif
#endif

    return 0;
}

int
no_fail_pre(struct mtf_test_info *info)
{
#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
    g_fail_nth_alloc_cnt = 0;
    g_fail_nth_alloc_limit = -1;
#endif

    xrand_init(time(NULL));

    if (rcu_read_ongoing())
        rcu_read_unlock();

    return 0;
}

int
no_fail_post(struct mtf_test_info *info)
{
#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
    g_fail_nth_alloc_cnt = 0;
    g_fail_nth_alloc_limit = -1;
#endif

    return 0;
}

static void
bonsai_client_wait_for_test_completion(void)
{
    struct timeval start;
    struct timeval end;
    unsigned long  microseconds;

    gettimeofday(&start, NULL);

    while (1) {
        gettimeofday(&end, NULL);
        microseconds = ((end.tv_sec - start.tv_sec) * 1000 * 1000) + end.tv_usec - start.tv_usec;

        if (microseconds >= (runtime_insecs * 1000 * 1000))
            break;

        usleep(1000);
    }
}

static void *
bonsai_client_producer(void *arg)
{
    unsigned long  i;
    merr_t         err = 0;
    unsigned long *key = NULL;
    unsigned long *val = NULL;

    struct bonsai_skey skey = { 0 };
    struct bonsai_sval sval = { 0 };

/*
     * Register is not required for BP. For QSBR, it is required only for
     * clients.
     */
#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_REGISTER();
#endif
#endif
    xrand_init(time(NULL));

    if (key_size < sizeof(*key))
        key = calloc(1, sizeof(*key));
    else
        key = calloc(1, key_size);

    if (!key)
        goto exit;

    assert(val_size >= key_size);

    val = calloc(1, val_size);
    if (!val)
        goto exit;

    for (i = key_begin; i <= key_end; i++) {
        if (random_number == 0)
            *key = i;
        else
            *key = xrand();

        *val = *key;

        while (!stop_producer_threads) {
            bn_skey_init(key, key_size, 0, &skey);
            bn_sval_init(val, val_size, *val, &sval);

            pthread_mutex_lock(&mtx);
            err = bn_insert_or_replace(broot, &skey, &sval, false);
            if (merr_errno(err) == 0) {
                key_current = i;
                __sync_synchronize();
            }
            pthread_mutex_unlock(&mtx);

            if (merr_errno(err) == EEXIST)
                err = 0;
            else if (err)
                break;
        }

        if (err) {
            hse_elog(HSE_ERR "bn_insert %ld result @@e", err, i);
            break;
        }

        if (stop_producer_threads)
            break;
    }

exit:

#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_UNREGISTER();
#endif
#endif

    if (key)
        free(key);
    if (val)
        free(val);

    pthread_exit((void *)(long)merr_errno(err));
}

struct lcp_test_arg {
    pthread_barrier_t *fbarrier;
    uint               tid;
};

static void *
bonsai_client_lcp_test(void *arg)
{
    int                i;
    uint               tid;
    merr_t             err = 0;
    char               key[KI_DLEN_MAX + 36];
    unsigned long      val;
    pthread_barrier_t *fbarrier;

    struct lcp_test_arg *p = (struct lcp_test_arg *)arg;

    fbarrier = p->fbarrier;
    tid = p->tid;

    struct bonsai_skey skey = { 0 };
    struct bonsai_sval sval = { 0 };

#ifdef BONSAI_TREE_CLIENT_VERIFY
    uint lcp, bounds;
#endif

    /*
     * Register is not required for BP. For QSBR, it is required only for
     * clients.
     */
#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_REGISTER();
#endif
#endif

    memset(key, 'a', KI_DLEN_MAX + 27);

    /*
     * Insert keys of the same length (KI_DLEN_MAX + 27).
     * The last byte is replaced with a..z.
     * Each key is inserted with a unique value to identify the keynum, skidx.
     */
    for (i = 0; i < 26; i++) {
        val = (u64)i << 32 | tid;

        key[KI_DLEN_MAX + 26] = 'a' + i;

        bn_skey_init(key, KI_DLEN_MAX + 27, tid, &skey);
        bn_sval_init(&val, sizeof(val), val, &sval);

        pthread_mutex_lock(&mtx);
        err = bn_insert_or_replace(broot, &skey, &sval, false);
        pthread_mutex_unlock(&mtx);

        key[KI_DLEN_MAX + 26] = 'a';

        if (err) {
            hse_elog(HSE_ERR "lcp_test bn_insert %u result @@e", err, i);
            break;
        }
    }

    pthread_barrier_wait(fbarrier);

    while (!stop_producer_threads)
        usleep(1000);

#ifdef BONSAI_TREE_CLIENT_VERIFY
    bounds = atomic_read(&broot->br_bounds);
    if (bounds)
        lcp = bounds - 1;

    for (i = 0; i < 26; i++) {
        struct bonsai_skey          skey = { 0 };
        struct bonsai_kv *          kv = NULL;
        struct bonsai_val *         v;
        unsigned long               val;
        bool                        found;
        const struct key_immediate *ki;

        key[KI_DLEN_MAX + 26] = 'a' + i;

        bn_skey_init(key, KI_DLEN_MAX + 27, tid, &skey);
        ki = &skey.bsk_key_imm;

        rcu_read_lock();
        found = bn_find(broot, &skey, &kv);
        assert(found);

        v = kv->bkv_values;
        memcpy((char *)&val, v->bv_value, sizeof(val));
        assert(val == ((u64)i << 32 | tid));

        rcu_read_unlock();

        key[KI_DLEN_MAX + 26] = 'a';

        if (lcp > 0) {
            assert(key_immediate_cmp(ki, &kv->bkv_key_imm) == S32_MIN);
            assert(memcmp(kv->bkv_key, &kv->bkv_key, lcp) == 0);
        }
    }

    for (i = 1; i < KI_DLEN_MAX + 27; i++) {
        struct bonsai_skey skey = { 0 };
        struct bonsai_kv * kv = NULL;
        bool               found;

        bn_skey_init(key, i, tid, &skey);

        rcu_read_lock();
        found = bn_find(broot, &skey, &kv);
        assert(!found);
    }

    for (i = KI_DLEN_MAX + 28; i < sizeof(key); i++) {
        struct bonsai_skey skey = { 0 };
        struct bonsai_kv * kv = NULL;
        bool               found;

        bn_skey_init(key, i, tid, &skey);

        rcu_read_lock();
        found = bn_find(broot, &skey, &kv);
        assert(!found);
    }
#endif

#ifndef LIBURCU_QSBR
#ifndef LIBURCU_BP
    BONSAI_RCU_UNREGISTER();
#endif
#endif

    pthread_exit((void *)(long)merr_errno(err));
}

static void *
bonsai_client_consumer(void *arg)
{
    struct bonsai_skey skey = { 0 };
    struct bonsai_kv * kv = NULL;

    unsigned long  i;
    unsigned long  key_last;
    unsigned long *key;
    bool           found = true;

    key = calloc(1, key_size);
    if (!key)
        goto exit;

#ifndef LIBURCU_BP
    BONSAI_RCU_REGISTER();
#endif

    while (!stop_consumer_threads) {
        __sync_synchronize();
        key_last = key_current;

        for (i = 1; i <= key_last; i++) {
            *key = i;
            bn_skey_init(key, key_size, 0, &skey);

            rcu_read_lock();
            found = bn_find(broot, &skey, &kv);
            rcu_read_unlock();

            hse_log(HSE_ERR "bn_find %ld result %d", i, found);

            if (stop_consumer_threads)
                break;

            if (!found) {
                hse_log(HSE_ERR "key %ld not found", i);
                break;
            }
        }

#ifdef LIBURCU_QSBR
        BONSAI_RCU_QUIESCE();
#endif

        sched_yield();
    }

#ifdef BONSAI_TREE_DEBUG
    hse_log(HSE_INFO "Stopped consumer ... last key %ld", i);
#endif

exit:

#ifndef LIBURCU_BP
    BONSAI_RCU_UNREGISTER();
#endif

    if (key)
        free(key);

    pthread_exit(found ? (void *)0 : (void *)-1);
}

static int
bonsai_client_multithread_test(void)
{
    int        rc;
    int        i;
    pthread_t *consumer_tids;
    pthread_t *producer_tids;
    void *     ret;

    struct bonsai_skey skey = { 0 };
    struct bonsai_kv * kv = NULL;

#ifdef BONSAI_TREE_CLIENT_VERIFY
    unsigned long *key;
    merr_t         err;
    bool           found;

    key = calloc(1, key_size);
    if (!key) {
        rc = ENOMEM;
        goto exit;
    }
#endif

    cheap = cheap_create(8, 64 * MB);
    if (!cheap)
        return -1;

    err = bn_create(cheap, 32 * MB, bonsai_client_insert_callback, NULL, &broot);
    if (err) {
        hse_log(HSE_ERR "Bonsai tree create failed");
        return err;
    }

    rc = pthread_mutex_init(&mtx, NULL);
    assert(rc == 0);

    consumer_tids = malloc(num_consumers * sizeof(*consumer_tids));
    assert(consumer_tids);

    producer_tids = malloc(num_producers * sizeof(*producer_tids));
    assert(producer_tids);

    rc = create_all_cpu_call_rcu_data(0);
    assert(rc == 0);

    for (i = 0; i < num_producers; i++) {
        rc = pthread_create(&producer_tids[i], NULL, bonsai_client_producer, NULL);
        assert(rc == 0);
    }

    for (i = 0; i < num_consumers; i++) {
        rc = pthread_create(&consumer_tids[i], NULL, bonsai_client_consumer, NULL);
        assert(rc == 0);
    }

    bonsai_client_wait_for_test_completion();

    stop_consumer_threads = 1;
    for (i = 0; i < num_consumers; i++) {
        rc = pthread_join(consumer_tids[i], &ret);
        assert(rc == 0);
    }

    stop_producer_threads = 1;
    for (i = 0; i < num_producers; i++) {
        rc = pthread_join(producer_tids[i], &ret);
        assert(rc == 0);
    }

#ifdef BONSAI_TREE_DEBUG
    hse_log(HSE_INFO "Before teardown noded added %ld removed %ld", client.bnc_add, client.bnc_del);
#endif

#ifdef BONSAI_TREE_CLIENT_VERIFY
    for (i = 1; i < key_current; i++) {
        *key = i;
        bn_skey_init(key, key_size, 0, &skey);

        rcu_read_lock();
        found = bn_find(broot, &skey, &kv);
        rcu_read_unlock();

        if ((random_number == 0) && !found) {
            rc = ENOENT;
            hse_log(HSE_ERR "Key %ld not found", *key);
            break;
        }
    }

    rcu_read_lock();
    bn_traverse(broot);
    rcu_read_unlock();
#endif

    BONSAI_RCU_BARRIER();

#ifdef BONSAI_TREE_DEBUG
    hse_log(HSE_INFO "Tree height %d", bn_height_get(broot.br_root));
#endif

    bn_destroy(broot);

    BONSAI_RCU_BARRIER();

#ifdef BONSAI_TREE_DEBUG_ALLOC
    hse_log(
        HSE_INFO "After teardown noded added %ld removed %ld",
        broot->br_client.bc_add,
        broot->br_client.bc_del);
#if 0
    assert(broot->br_client.bc_add == broot->br_client.bc_del);
    assert(broot->br_client.bc_dup == broot->br_client.bc_dupdel);
#endif
#endif
    free_all_cpu_call_rcu_data();

    cheap_destroy(cheap);
    cheap = NULL;

    pthread_mutex_destroy(&mtx);

    free(producer_tids);
    free(consumer_tids);

#ifdef BONSAI_TREE_CLIENT_VERIFY
    free(key);
#endif

exit:
    return rc;
}

static int
bonsai_client_singlethread_test(enum bonsai_alloc_mode allocm)
{
    merr_t        err;
    int           i;
    unsigned long tmpkey = 9999999;
    size_t        tmpkey_len = sizeof(tmpkey);
    bool          found;

    static unsigned long a[] = { 300, 1,   2,   3,   4,   3,   2,  1,  5,  6,  7,   8,
                                 303, 302, 1,   2,   3,   4,   5,  99, 1,  2,  3,   4,
                                 5,   99,  200, 1,   2,   3,   4,  5,  99, 1,  2,   3,
                                 4,   5,   99,  299, 301, 1,   2,  3,  4,  5,  99,  7,
                                 8,   9,   13,  14,  15,  99,  20, 30, 40, 50, 101, 150,
                                 500, 100, 600, 5,   99,  200, 1,  2,  3,  4,  5,   99 };
    struct bonsai_skey   skey = { 0 };
    struct bonsai_sval   sval = { 0 };
    struct bonsai_kv *   kv = NULL;

    if (allocm == HSE_ALLOC_CURSOR) {
        cheap = cheap_create(8, 64 * MB);
        if (!cheap)
            return -1;
    }

    err = bn_create(cheap, 32 * MB, bonsai_client_insert_callback, NULL, &broot);
    if (err) {
        hse_log(HSE_ERR "Bonsai tree create failed");
        return err;
    }

    bn_reset(broot);

    assert(cheap == bn_get_allocator(broot));
    assert(bonsai_client_insert_callback == bn_get_iorcb(broot));
    assert(NULL == bn_get_rock(broot));
    assert(32 * MB == bn_get_slabsz(broot));

    i = 0;
    do {
        for (; i < sizeof(a) / sizeof(long); i++) {
            /* Initialize Key */
            bn_skey_init(&a[i], sizeof(a[i]), 0, &skey);
            /* Initialize Value */
            bn_sval_init(&a[i], sizeof(a[i]), a[i], &sval);

            err = bn_insert_or_replace(broot, &skey, &sval, false);
            if (merr_errno(err) == EEXIST)
                err = 0;

            if (err) {
                hse_log(HSE_ERR "Inserting %ld result %d", a[i], merr_errno(err));
                break;
            }
        }

    } while (merr_errno(err) == ENOMEM);

    for (i = i - 1; i >= 0; i--) {
        struct bonsai_val *v;
        u64                val;

        /* Initialize Key */
        bn_skey_init(&a[i], sizeof(a[i]), 0, &skey);

        rcu_read_lock();
        found = bn_find(broot, &skey, &kv);
        if (!found) {
            hse_log(HSE_ERR "Finding %ld result %d", a[i], found);
            rcu_read_unlock();
            break;
        }
        v = kv->bkv_values;
        memcpy((char *)&val, v->bv_value, sizeof(val));
        assert(a[i] == val);

        rcu_read_unlock();
    }

    bn_skey_init(&tmpkey, tmpkey_len, 0, &skey);

    rcu_read_lock();
    found = bn_find(broot, &skey, &kv);
    rcu_read_unlock();
    assert(found == false);

    rcu_read_lock();
    bn_traverse(broot);
    rcu_read_unlock();
    BONSAI_RCU_BARRIER();

    bn_destroy(broot);
    BONSAI_RCU_BARRIER();

#if 0
    assert(broot->br_client.bc_add == broot->br_client.bc_del);
    assert(broot->br_client.bc_dup == broot->br_client.bc_dupdel);
#endif

    cheap_destroy(cheap);
    cheap = NULL;

    return 0;
}

MTF_MODULE_UNDER_TEST(bonsai_tree_test);

MTF_BEGIN_UTEST_COLLECTION_PREPOST(
    bonsai_tree_test,
    test_collection_setup,
    test_collection_teardown);

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, basic_single_threaded, no_fail_pre, no_fail_post)
{
    ASSERT_EQ(0, bonsai_client_singlethread_test(HSE_ALLOC_CURSOR));
    ASSERT_EQ(0, bonsai_client_singlethread_test(HSE_ALLOC_MALLOC));
}

MTF_DEFINE_UTEST(bonsai_tree_test, misc)
{
    merr_t err;

    err = bn_create(NULL, 1 * MB, NULL, NULL, &broot);
    ASSERT_NE(err, 0);

    err = bn_create(NULL, 1 * MB, bonsai_client_insert_callback, NULL, NULL);
    ASSERT_NE(err, 0);

    err = bn_create(NULL, 1 * MB, bonsai_client_insert_callback, NULL, &broot);
    ASSERT_EQ(err, 0);

    bn_free(broot, NULL);
    bn_node_free(broot, NULL);

    bn_destroy(broot);
    broot = NULL;

    cheap = cheap_create(8, 4 * MB);
    ASSERT_NE(cheap, NULL);

    err = bn_create(cheap, 1 * MB, bonsai_client_insert_callback, NULL, &broot);
    ASSERT_EQ(err, 0);

    bn_free(broot, NULL);
    bn_node_free(broot, NULL);

    bn_destroy(broot);
    bn_destroy(NULL);

    cheap_destroy(cheap);
    cheap = NULL;
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, producer_test, no_fail_pre, no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    num_consumers = 0;
    num_producers = 1;

    ASSERT_EQ(0, bonsai_client_multithread_test());

#ifdef BONSAI_TREE_DEBUG
    hse_log(
        HSE_INFO "Run time %d seconds consumers %d "
                 "producers %d last key %ld",
        runtime_insecs,
        num_consumers,
        num_producers,
        key_current);
#endif
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, lcp_test, no_fail_pre, no_fail_post)
{
    int        rc;
    int        i;
    const int  num_skidx = 64;
    pthread_t *producer_tids;

    void *              ret;
    pthread_barrier_t   final_barrier;
    struct lcp_test_arg args[num_skidx];
    merr_t              err;

    cheap = cheap_create(8, 64 * MB);
    ASSERT_NE(cheap, NULL);

    err = bn_create(cheap, 32 * MB, bonsai_client_insert_callback, NULL, &broot);
    ASSERT_EQ(err, 0);

    rc = pthread_mutex_init(&mtx, NULL);
    ASSERT_EQ(rc, 0);

    producer_tids = malloc(num_skidx * sizeof(*producer_tids));
    ASSERT_NE(producer_tids, NULL);

    rc = create_all_cpu_call_rcu_data(0);
    ASSERT_EQ(rc, 0);

    stop_producer_threads = 0;

    pthread_barrier_init(&final_barrier, NULL, num_skidx + 1);

    for (i = 0; i < num_skidx; i++) {
        args[i].tid = i;
        args[i].fbarrier = &final_barrier;
        rc = pthread_create(&producer_tids[i], NULL, bonsai_client_lcp_test, &args[i]);
        ASSERT_EQ(rc, 0);
    }

    /* Wait until all the skidx threads are done inserting their keys */
    pthread_barrier_wait(&final_barrier);

    bn_finalize(broot);

    /* lcp must be zero since the keys have different skidx values */
    ASSERT_EQ(atomic_read(&broot->br_bounds), 1);

    stop_producer_threads = 1;
    for (i = 0; i < num_producers; i++) {
        rc = pthread_join(producer_tids[i], &ret);
        ASSERT_EQ(rc, 0);
    }

    BONSAI_RCU_BARRIER();

    bn_destroy(broot);

    BONSAI_RCU_BARRIER();

    cheap_destroy(cheap);
    cheap = NULL;
    cheap = cheap_create(8, 64 * MB);
    ASSERT_NE(cheap, NULL);

    err = bn_create(cheap, 32 * MB, bonsai_client_insert_callback, NULL, &broot);
    ASSERT_EQ(err, 0);

    stop_producer_threads = 0;

    /* Repeat the test with a bonsai tree containing keys for just one skidx. */
    pthread_barrier_init(&final_barrier, NULL, 2);

    args[0].tid = num_skidx + 1;
    args[0].fbarrier = &final_barrier;
    rc = pthread_create(&producer_tids[0], NULL, bonsai_client_lcp_test, &args[0]);
    ASSERT_EQ(rc, 0);

    pthread_barrier_wait(&final_barrier);

    bn_finalize(broot);

    /* lcp must be set this time around */
    ASSERT_GT(atomic_read(&broot->br_bounds), 1 + KI_DLEN_MAX);

    stop_producer_threads = 1;
    rc = pthread_join(producer_tids[0], &ret);
    ASSERT_EQ(rc, 0);

    BONSAI_RCU_BARRIER();

    bn_destroy(broot);

    BONSAI_RCU_BARRIER();

    free_all_cpu_call_rcu_data();

    cheap_destroy(cheap);
    cheap = NULL;

    pthread_mutex_destroy(&mtx);

    free(producer_tids);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, producer_manyconsumer_test, no_fail_pre, no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    num_consumers = 32;
    num_producers = 1;

    ASSERT_EQ(0, bonsai_client_multithread_test());
}

MTF_DEFINE_UTEST_PREPOST(
    bonsai_tree_test,
    manyproducer_manyconsumer_test,
    no_fail_pre,
    no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    num_consumers = 32;
    num_producers = 8;

    ASSERT_EQ(0, bonsai_client_multithread_test());
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, random_key_test, no_fail_pre, no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    random_number = 1;
    runtime_insecs = 7;
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    num_consumers = 0;
    num_producers = 1;

    ASSERT_EQ(0, bonsai_client_multithread_test());
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, malloc_failure_test, no_fail_pre, no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    random_number = 1;
    induce_alloc_failure = 1; /* XXX: This needs to be mocked */
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    runtime_insecs = 7;
    num_consumers = 0;
    num_producers = 1;

    ASSERT_EQ(0, bonsai_client_singlethread_test(HSE_ALLOC_CURSOR));
    ASSERT_EQ(0, bonsai_client_singlethread_test(HSE_ALLOC_MALLOC));
    ASSERT_EQ(0, bonsai_client_multithread_test());
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, odd_key_size_test, no_fail_pre, no_fail_post)
{
    key_begin = 1;
    key_current = 0;
    induce_alloc_failure = 0;
    stop_producer_threads = 0;
    stop_consumer_threads = 0;
    key_size = 7;
    random_number = 0;
    num_consumers = 0;
    num_producers = 1;

    ASSERT_EQ(0, bonsai_client_multithread_test());
}

/* Test the key weight algorithms by creating keys of identical bytes
 * of different lengths.  Only tests edge condition bytes that seem
 * most likely to cause problems.
 */
void
bonsai_weight_test(enum bonsai_alloc_mode allocm, struct mtf_test_info *lcl_ti)
{
    u8                  list[] = { 0, 1, 2, 127, 128, 129, 253, 254, 255 };
    const int           maxlen = 37;
    struct bonsai_root *tree;
    uintptr_t           seqno;
    int                 i, j;
    struct bonsai_skey  skey = { 0 };
    struct bonsai_sval  sval = { 0 };
    struct bonsai_kv *  kv = NULL;
    struct bonsai_val * v;
    merr_t              err;

    init_tree(&tree, allocm);

    for (i = 0; i < NELEM(list); ++i) {
        for (j = 1; j < maxlen; ++j) {
            u8 key[maxlen];

            memset(key, list[i], j);
            bn_skey_init(&key, j, 0, &skey);

            seqno = HSE_ORDNL_TO_SQNREF(3);
            bn_sval_init(key, j, seqno, &sval);

            rcu_read_lock();
            err = bn_insert_or_replace(tree, &skey, &sval, false);
            rcu_read_unlock();

            ASSERT_EQ(0, err);
        }
    }

    for (i = 0; i < NELEM(list); ++i) {
        for (j = 1; j < maxlen; ++j) {
            u8 key[maxlen];

            memset(key, list[i], j);

            seqno = HSE_ORDNL_TO_SQNREF(3);

            bn_skey_init(&key, j, 0, &skey);

            rcu_read_lock();
            (void)bn_find(tree, &skey, &kv);
            v = kv->bkv_values;

            ASSERT_NE(NULL, v);
            ASSERT_EQ(seqno, v->bv_seqnoref);
            ASSERT_EQ(j, v->bv_vlen);
            ASSERT_EQ(0, memcmp(key, v->bv_value, j));
            rcu_read_unlock();
        }
    }

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;
}

static void
validate_tombspan(struct bonsai_root *tree)
{
    bool               found = false;
    struct bonsai_kv * head = 0, *tail = 0;
    struct bonsai_kv * curr, *kv, *ekv;
    struct bonsai_skey skey;
    char               key[10];
    u16                index;

    curr = tree->br_kv.bkv_next;

    while (curr != &tree->br_kv) {

        memcpy(key, curr->bkv_key, curr->bkv_key_imm.ki_klen);
        index = key_immediate_index(&curr->bkv_key_imm);
        bn_skey_init(key, curr->bkv_key_imm.ki_klen, index, &skey);

        found = bn_skiptombs_GE(tree, &skey, &kv);

        if (curr->bkv_tomb) {
            struct bonsai_val *val = curr->bkv_values;

            /* Validate that this is a tombstone */
            VERIFY_NE(val, NULL);
            VERIFY_EQ(val->bv_valuep, HSE_CORE_TOMB_REG);
            VERIFY_EQ(val->bv_next, NULL);

            if (curr->bkv_flags & BKV_FLAG_TOMB_HEAD) {
                head = curr;
                tail = curr->bkv_tomb;
                ekv = tail->bkv_next;
                if (head == tail)
                    head = tail = NULL;
            } else {
                if (!head) {
                    /* This tombspan was invalidated */
                    VERIFY_EQ(curr->bkv_tomb->bkv_tomb, NULL);
                    ekv = curr->bkv_next;
                } else {
                    VERIFY_EQ(curr->bkv_tomb, head);
                    ekv = tail->bkv_next;

                    if (tail == curr)
                        head = tail = NULL;
                }
            }
        } else {
            ekv = curr;
        }

        if (ekv != &tree->br_kv) {
            VERIFY_EQ(found, true);
            assert(kv == ekv);
            VERIFY_EQ(kv, ekv);
        } else {
            VERIFY_EQ(found, false);
        }

        curr = curr->bkv_next;
    }
}

void
bonsai_tombspan_test(enum bonsai_alloc_mode allocm, struct mtf_test_info *lcl_ti)
{
    const int           LEN = 256;
    struct bonsai_root *tree;
    int                 i, j;
    u16                 index = xrand() % 256;
    struct bonsai_skey  skey = { 0 };
    struct bonsai_sval  sval = { 0 };
    struct bonsai_kv *  kv = NULL;
    merr_t              err;
    bool                found;
    u64                 key;

    init_tree(&tree, allocm);

    /* Test that the tombstone span does track tombstone keys, which
     * are inserted in increasing order (mongo load balancing behavior) */
    for (i = 0; i < LEN; ++i) {
        key = i << 24;

        bn_skey_init(&key, sizeof(key), index, &skey);
        bn_sval_init(HSE_CORE_TOMB_REG, 0, HSE_ORDNL_TO_SQNREF(xrand() & 0xFFFFFFFFFFFFF), &sval);

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, true);
        rcu_read_unlock();

        ASSERT_EQ(0, err);

        /* Search for one of the inserted keys */
        j = xrand() % (i + 1);
        key = 0x6100000000000000 + j;
        bn_skey_init(&key, sizeof(key), index, &skey);

        rcu_read_lock();
        found = bn_skiptombs_GE(tree, &skey, &kv);
        rcu_read_unlock();

        /* All of them belong to one tombstone span,
         * and no non-tombstone key should be found. */
        ASSERT_EQ(found, false);
    }

    rcu_read_lock();
    validate_tombspan(tree);
    rcu_read_unlock();

    /* Update one of the keys with a value to invalidate the tombspan. */
    j = xrand() % LEN;
    key = i << 24;
    bn_skey_init(&key, sizeof(key), index, &skey);
    bn_sval_init(&skey, sizeof(skey), HSE_ORDNL_TO_SQNREF(xrand() & 0xFFFFFFFFFFFFF), &sval);

    rcu_read_lock();
    err = bn_insert_or_replace(tree, &skey, &sval, false);
    rcu_read_unlock();

    rcu_read_lock();
    validate_tombspan(tree);
    rcu_read_unlock();

    key = 1 << 16;
    bn_skey_init(&key, sizeof(key), index, &skey);

    rcu_read_lock();
    found = bn_skiptombs_GE(tree, &skey, &kv);
    rcu_read_unlock();

    ASSERT_EQ(found, false);

    for (i = 0; i < 1024; ++i) {
        key = xrand();

        bn_skey_init(&key, sizeof(key), i % 256, &skey);
        if (key & 1)
            bn_sval_init(&key, sizeof(key), HSE_ORDNL_TO_SQNREF(key & 0xffff), &sval);
        else
            bn_sval_init(HSE_CORE_TOMB_REG, 0, HSE_ORDNL_TO_SQNREF(key & 0xffff), &sval);

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, !(key & 1));
        rcu_read_unlock();

        ASSERT_EQ(0, err);
    }

    rcu_read_lock();
    validate_tombspan(tree);
    rcu_read_unlock();

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;
}

/* Create a bunch of unique keys, each with three different values (i.e.,
 * with different sequence numbers).  For half the keys, check to see that
 * the higher and lower values still exist and are valid.
 */
void
bonsai_basic_test(enum bonsai_alloc_mode allocm, struct mtf_test_info *lcl_ti)
{
    const int           LEN = 128 * 1024;
    struct bonsai_root *tree;
    uintptr_t           op_seqno;
    uintptr_t           seqnoref;
    int                 i;
    struct bonsai_skey  skey = { 0 };
    struct bonsai_sval  sval = { 0 };
    struct bonsai_kv *  kv = NULL;
    merr_t              err;
    bool                found;

    init_tree(&tree, allocm);

    for (i = 0; i < LEN; ++i) {
        u64 key = i % 2 ? i : -i;

        op_seqno = 3;
        seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);

        bn_skey_init(&key, sizeof(key), 234, &skey);
        bn_sval_init(&key, sizeof(key), seqnoref, &sval);

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, false);
        rcu_read_unlock();

        ASSERT_EQ(0, err);

        op_seqno = 1;
        seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);
        sval.bsv_seqnoref = seqnoref;

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, false);
        rcu_read_unlock();

        op_seqno = 2;
        seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);
        sval.bsv_seqnoref = seqnoref;

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, false);
        rcu_read_unlock();
    }

    for (i = 0; i < LEN / 2; ++i) {
        struct bonsai_val *v;
        u64                key;
        u64                val;

        key = i % 2 ? i : -i;
        v = NULL;
        op_seqno = 1;
        bn_skey_init(&key, sizeof(key), 234, &skey);

        rcu_read_lock();
        found = bn_find(tree, &skey, &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        v = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, v);
        ASSERT_EQ(op_seqno, HSE_SQNREF_TO_ORDNL(v->bv_seqnoref));
        ASSERT_EQ(sizeof(key), v->bv_vlen);
        memcpy((char *)&val, v->bv_value, sizeof(val));
        ASSERT_EQ(key, val);
        rcu_read_unlock();

        op_seqno = 3;

        rcu_read_lock();
        found = bn_find(tree, &skey, &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        v = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, v);
        ASSERT_EQ(op_seqno, HSE_SQNREF_TO_ORDNL(v->bv_seqnoref));
        ASSERT_EQ(sizeof(key), v->bv_vlen);
        memcpy((char *)&val, v->bv_value, sizeof(val));
        ASSERT_EQ(key, val);
        rcu_read_unlock();
    }

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;
}

/* Update each value of a single multi-valued key many times, then verify
 * the final result.
 */
void
bonsai_update_test(enum bonsai_alloc_mode allocm, struct mtf_test_info *lcl_ti)
{
    const int           MAX_VALUES = 17;
    const int           LEN = 4003 * MAX_VALUES;
    u64                 op_seqno;
    uintptr_t           seqnoref;
    u64                 value;
    u64                 key;
    int                 i;
    merr_t              err;
    bool                found;
    struct bonsai_root *tree;
    struct bonsai_skey  skey = { 0 };
    struct bonsai_sval  sval = { 0 };
    struct bonsai_kv *  kv = NULL;

    init_tree(&tree, allocm);

    key = 0x900dcafe;
    value = 0;

    for (i = 0; i < LEN; ++i) {
        op_seqno = i % MAX_VALUES;
        seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);
        ++value;

        bn_skey_init(&key, sizeof(key), 23, &skey);
        bn_sval_init(&value, sizeof(value), seqnoref, &sval);

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skey, &sval, false);
        rcu_read_unlock();

        ASSERT_EQ(0, err);
    }

    for (i = 0; i < MAX_VALUES; ++i) {
        struct bonsai_val *v;
        u64                val;

        op_seqno = i;

        rcu_read_lock();

        found = bn_find(tree, &skey, &kv);

        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);

        v = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, v);

        ASSERT_EQ(op_seqno, HSE_SQNREF_TO_ORDNL(v->bv_seqnoref));
        ASSERT_EQ(sizeof(value), v->bv_vlen);
        memcpy((char *)&val, v->bv_value, sizeof(val));
        ASSERT_EQ(LEN - MAX_VALUES + i + 1, val);
        rcu_read_unlock();
    }

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;
}

void
bonsai_original_test(enum bonsai_alloc_mode allocm, struct mtf_test_info *lcl_ti)
{
    enum { LEN = 5 };
    struct bonsai_root *tree;
    u64                 keys[LEN];
    struct bonsai_skey  skeys[LEN];
    struct bonsai_skey  skey = { 0 };
    struct bonsai_sval  sval = { 0 };
    int                 i;
    int                 numeric = 0;
    u64                 op_seqno = 343;
    uintptr_t           seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);
    merr_t              err;
    bool                finalize = false;
    bool                found;

    init_tree(&tree, allocm);

    for (i = 0; i < LEN; ++i) {
        u64 key;

        /* Ensure keys are unique and non-consecutive. */
        key = (i << 16) | (xrand() & 0xffff);
        if (!numeric)
            key |= 0x6100000000000000;

        keys[i] = key;
        bn_skey_init(&keys[i], sizeof(keys[i]), xrand() % 256, &skeys[i]);
        bn_sval_init(&keys[i], sizeof(keys[i]), seqnoref, &sval);

        rcu_read_lock();
        err = bn_insert_or_replace(tree, &skeys[i], &sval, false);
        rcu_read_unlock();

        ASSERT_EQ(0, err);
    }

    qsort(skeys, LEN, sizeof(struct bonsai_skey), cmpKey);

again:
    for (i = 0; i < LEN; ++i) {
        u64                 key, key0;
        u32                 sz = sizeof(key);
        struct bonsai_kv *  kv;
        struct bonsai_val * pval;
        struct bonsai_skey *next;
        u16                 sid;

        kv = NULL;
        key0 = *(u64 *)skeys[i].bsk_key;

        /* Assumes no two keys are consecutive */
        sid = key_immediate_index(&skeys[i].bsk_key_imm);
        key = decrement_key(key0, numeric);
        bn_skey_init(&key, sizeof(key), sid, &skey);

        rcu_read_lock();
        found = bn_find(tree, &skey, &kv);
        ASSERT_NE(true, found);
        ASSERT_EQ(NULL, kv);

        kv = NULL;
        found = bn_find(tree, &skeys[i], &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        pval = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, pval);
        ASSERT_EQ(0, memcmp(skeys[i].bsk_key, pval->bv_value, sz));

        kv = NULL;
        key = increment_key(key0, numeric);
        bn_skey_init(&key, sizeof(key), sid, &skey);
        found = bn_find(tree, &skey, &kv);
        ASSERT_NE(true, found);
        ASSERT_EQ(NULL, kv);

        kv = NULL;
        key = decrement_key(key0, numeric);
        bn_skey_init(&key, sizeof(key), sid, &skey);
        found = bn_find(tree, &skey, &kv);
        ASSERT_NE(true, found);
        ASSERT_EQ(NULL, kv);

        next = i < LEN - 1 ? (struct bonsai_skey *)&skeys[i + 1] : NULL;

        kv = NULL;
        found = bn_findGE(tree, &skeys[i], &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        pval = findValue(kv, op_seqno, 0);
        ASSERT_EQ(0, memcmp(skeys[i].bsk_key, pval->bv_value, sz));

        kv = NULL;
        key = increment_key(key0, numeric);
        bn_skey_init(&key, sizeof(key), sid, &skey);

        found = bn_findGE(tree, &skey, &kv);
        if (!found) {
            ASSERT_EQ(NULL, next);
        } else {
            ASSERT_EQ(true, found);
            ASSERT_NE(NULL, kv);
            pval = findValue(kv, op_seqno, 0);
            ASSERT_EQ(0, memcmp(next->bsk_key, pval->bv_value, sz));
        }

        next = i > 0 ? (struct bonsai_skey *)&skeys[i - 1] : NULL;

        kv = NULL;
        found = bn_findLE(tree, &skeys[i], &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        pval = findValue(kv, op_seqno, 0);
        ASSERT_EQ(0, memcmp(skeys[i].bsk_key, pval->bv_value, sz));

        kv = NULL;
        key = decrement_key(key0, numeric);
        bn_skey_init(&key, sizeof(key), sid, &skey);

        found = bn_findLE(tree, &skey, &kv);
        if (!found) {
            ASSERT_EQ(NULL, next);
        } else {
            ASSERT_EQ(true, found);
            ASSERT_NE(NULL, kv);
            pval = findValue(kv, op_seqno, 0);
            ASSERT_EQ(0, memcmp(next->bsk_key, pval->bv_value, sz));
        }

        rcu_read_unlock();
    }

    if (!finalize) {
        bn_finalize(tree);
        finalize = true;
        goto again;
    }

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, weight, no_fail_pre, no_fail_post)
{
    bonsai_weight_test(HSE_ALLOC_CURSOR, lcl_ti);
    bonsai_weight_test(HSE_ALLOC_MALLOC, lcl_ti);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, basic, no_fail_pre, no_fail_post)
{
    bonsai_basic_test(HSE_ALLOC_CURSOR, lcl_ti);
    bonsai_basic_test(HSE_ALLOC_MALLOC, lcl_ti);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, tombspan, no_fail_pre, no_fail_post)
{
    bonsai_tombspan_test(HSE_ALLOC_CURSOR, lcl_ti);
    bonsai_tombspan_test(HSE_ALLOC_MALLOC, lcl_ti);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, update, no_fail_pre, no_fail_post)
{
    bonsai_update_test(HSE_ALLOC_CURSOR, lcl_ti);
    bonsai_update_test(HSE_ALLOC_MALLOC, lcl_ti);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, original, no_fail_pre, no_fail_post)
{
    bonsai_original_test(HSE_ALLOC_CURSOR, lcl_ti);
    bonsai_original_test(HSE_ALLOC_MALLOC, lcl_ti);
}

MTF_DEFINE_UTEST_PREPOST(bonsai_tree_test, complicated, no_fail_pre, no_fail_post)
{
    enum { LEN = 349 };
    u64                 keys[LEN], ord_vals[LEN], key, value;
    u64                 op_seqno;
    uintptr_t           seqnoref;
    struct bonsai_root *tree;
    struct bonsai_val * pval;
    u32                 sz = sizeof(key);
    int                 i, j, rand_num;
    int                 MAX_VALUES_PER_KEY;
    merr_t              err;
    bool                found;

    struct bonsai_skey skey = { 0 };
    struct bonsai_sval sval = { 0 };
    struct bonsai_kv * kv;

    xrand_init(time(NULL));

    MAX_VALUES_PER_KEY = 1;

    skey.bsk_key = &key;

again:
    init_tree(&tree, HSE_ALLOC_CURSOR);

    for (i = 0; i < LEN; ++i) {
        rand_num = (i << 16) | (xrand() & 0xffff);
        key = rand_num | 0x6100000000000000;
        keys[i] = key;
        bn_skey_init(&key, sizeof(key), 143, &skey);
        ord_vals[i] = (rand_num >> 2) + MAX_VALUES_PER_KEY;

        for (j = 1; j <= MAX_VALUES_PER_KEY; j++) {
            if (j % 2)
                op_seqno = ord_vals[i] + j;
            else
                op_seqno = ord_vals[i] - j;

            seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);
            sval.bsv_seqnoref = seqnoref;
            sval.bsv_vlen = 0;

            rcu_read_lock();
            if (op_seqno % 200 == 0) {
                sval.bsv_val = HSE_CORE_TOMB_REG;

                err = bn_insert_or_replace(tree, &skey, &sval, true);
                ASSERT_EQ(0, err);
            } else if (op_seqno % 500 == 0) {
                sval.bsv_val = HSE_CORE_TOMB_PFX;

                err = bn_insert_or_replace(tree, &skey, &sval, false);
                ASSERT_EQ(0, err);
            } else {
                value = key - op_seqno;
                sval.bsv_val = (void *)&value;
                sval.bsv_vlen = sizeof(value);

                err = bn_insert_or_replace(tree, &skey, &sval, false);
                ASSERT_EQ(0, err);
            }
            rcu_read_unlock();
        }
    }

    for (i = 0; i < LEN; ++i) {

        key = keys[i];
        bn_skey_init(&key, sizeof(key), 143, &skey);

        for (j = MAX_VALUES_PER_KEY; j >= 1; j--) {
            if (j % 2)
                op_seqno = ord_vals[i] + j;
            else
                op_seqno = ord_vals[i] - j;

            seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno);

            value = key - op_seqno;

            rcu_read_lock();

            kv = NULL;
            found = bn_find(tree, &skey, &kv);
            ASSERT_EQ(true, found);
            ASSERT_NE(NULL, kv);
            pval = findValue(kv, op_seqno, 0);
            ASSERT_NE(NULL, pval);

            if (op_seqno % 200 == 0) {
                ASSERT_EQ(HSE_CORE_TOMB_REG, pval->bv_valuep);
                ASSERT_EQ(0, pval->bv_vlen);
            } else if (op_seqno % 500 == 0) {
                uintptr_t lcl_seqnoref;

                ASSERT_EQ(HSE_CORE_TOMB_PFX, pval->bv_valuep);
                ASSERT_EQ(0, pval->bv_vlen);

                lcl_seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno + 2);
                kv = NULL;
                found = bn_find(tree, &skey, &kv);
                ASSERT_EQ(true, found);
                ASSERT_NE(NULL, kv);
                pval = findPfxValue(kv, lcl_seqnoref);
                ASSERT_NE(NULL, pval);
                ASSERT_EQ(
                    HSE_SQNREF_TO_ORDNL(lcl_seqnoref) - 2, HSE_SQNREF_TO_ORDNL(pval->bv_seqnoref));

                lcl_seqnoref = HSE_ORDNL_TO_SQNREF(op_seqno - 2);
                kv = NULL;
                found = bn_find(tree, &skey, &kv);
                ASSERT_EQ(true, found);
                ASSERT_NE(NULL, kv);
                pval = findPfxValue(kv, lcl_seqnoref);
                ASSERT_EQ(NULL, pval);
            } else {
                ASSERT_EQ(0, memcmp((void *)&value, pval->bv_value, sz));
                ASSERT_EQ(op_seqno, HSE_SQNREF_TO_ORDNL(pval->bv_seqnoref));
                ASSERT_EQ(pval->bv_vlen, sz);
            }
            rcu_read_unlock();
        }

        if (MAX_VALUES_PER_KEY < 8)
            continue;

        rcu_read_lock();
        op_seqno = ord_vals[i];
        kv = NULL;
        found = bn_find(tree, &skey, &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        pval = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, pval);

        /* The insertion loop above produces a collection of values
         * for the key that are almost centered around op_seqno.
         * Those that are larger start at op_seqno + 1 and go up by 2.
         * Those that are smaller start at op_seqno - 2 and go down by
         * 2. As a result, the value we find should have a sequence
         * number that is 2 smaller than op_seqno.
         */
        ASSERT_EQ(op_seqno - 2, HSE_SQNREF_TO_ORDNL(pval->bv_seqnoref));

        if (ord_vals[i] > MAX_VALUES_PER_KEY) {
            /* If we have enough room, then we know that the
             * smallest sequence number for this key is larger
             * than:
             *       ord_vals[i] - MAX_VALUES_PER_KEY - 1
             */
            op_seqno = ord_vals[i] - MAX_VALUES_PER_KEY - 1;
            kv = NULL;
            found = bn_find(tree, &skey, &kv);
            ASSERT_EQ(true, found);
            ASSERT_NE(NULL, kv);
            pval = findValue(kv, op_seqno, 0);
            ASSERT_EQ(NULL, pval);
        }

        /* Similarly we know that the largest sequence number for this
         * key is smaller than:
         *       ord_vals[i] + MAX_VALUES_PER_KEY
         */
        op_seqno = ord_vals[i] + MAX_VALUES_PER_KEY;
        kv = NULL;
        found = bn_find(tree, &skey, &kv);
        ASSERT_EQ(true, found);
        ASSERT_NE(NULL, kv);
        pval = findValue(kv, op_seqno, 0);
        ASSERT_NE(NULL, pval);

        /* W/o changing op_seqno, we know that if the
         * MAX_VALUES_PER_KEY is even then the first delta was on
         * the "+j" side of the insert branch. In that case
         * ord_vals[i] + MAX_VALUES_PER_KEY will be the precise
         * sequence number in the collection of values. Otherwise
         * it will be one larger.
         */
        if (MAX_VALUES_PER_KEY % 2)
            ASSERT_EQ(op_seqno, HSE_SQNREF_TO_ORDNL(pval->bv_seqnoref));
        else
            ASSERT_EQ(op_seqno - 1, HSE_SQNREF_TO_ORDNL(pval->bv_seqnoref));

        rcu_read_unlock();
    }

    bn_destroy(tree);
    cheap_destroy(cheap);
    cheap = NULL;

    if (++MAX_VALUES_PER_KEY < 131)
        goto again;
}

static void
set_kv(struct bonsai_kv *k, void *key, size_t len, bool is_ptomb)
{
    k->bkv_flags = 0;
    k->bkv_key_imm.ki_klen = len;
    memcpy(k->bkv_key, key, k->bkv_key_imm.ki_klen);
    if (is_ptomb)
        k->bkv_flags |= BKV_FLAG_PTOMB;
}

#define max_cmp(key1, key1_is_pt, key2, key2_is_pt, res) \
    do {                                                 \
        struct bonsai_kv *kv1, *kv2;                     \
        int               rc;                            \
                                                         \
        kv1 = calloc(1, sizeof(*kv1) + ALLOC_LEN_MAX);   \
        kv2 = calloc(1, sizeof(*kv2) + ALLOC_LEN_MAX);   \
        set_kv(kv1, key1, strlen(key1), key1_is_pt);     \
        set_kv(kv2, key2, strlen(key2), key2_is_pt);     \
                                                         \
        rc = bn_kv_cmp_rev(kv1, kv2);                    \
                                                         \
        if (res < 0)                                     \
            ASSERT_LT(rc, 0);                            \
        else if (res > 0)                                \
            ASSERT_GT(rc, 0);                            \
        else                                             \
            ASSERT_EQ(rc, 0);                            \
        free(kv1);                                       \
        free(kv2);                                       \
    } while (0)

MTF_DEFINE_UTEST(bonsai_tree_test, bn_kv_cmp_test)
{
    /* result (last arg) -
     *  1 : key2 > key1
     * -1 : key1 > key2
     *  0 : key1 == key2
     */
    /* two keys - normal */
    max_cmp("ab1234", false, "ab34", false, 1);
    max_cmp("ab34", false, "ab1234", false, -1);

    max_cmp("ab1234", false, "ab", false, -1);
    max_cmp("ab", false, "ab1234", false, 1);

    /* key w/ ptomb, where keylen > ptomblen */
    max_cmp("ab1234", false, "ab", true, 1);
    max_cmp("ab", true, "ab1234", false, -1);

    /* key w/ ptomb, where keylen < ptomblen */
    max_cmp("a", false, "ab", true, 1);
    max_cmp("ab", true, "a", false, -1);

    /* two ptombs */
    max_cmp("ab", true, "ac", true, 1);

    /* matching key and ptomb */
    max_cmp("ab", true, "ab", false, -1);
    max_cmp("ab", false, "ab", true, 1);
}

MTF_END_UTEST_COLLECTION(bonsai_tree_test);
