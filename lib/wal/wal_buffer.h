/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2021 Micron Technology, Inc.  All rights reserved.
 */

#ifndef WAL_BUFFER_H
#define WAL_BUFFER_H

struct wal_bufset;

struct wal_bufset *
wal_bufset_open(struct wal *wal);

void
wal_bufset_close(struct wal_bufset *wbs);

void *
wal_bufset_alloc(struct wal_bufset *wbs, size_t len);

merr_t
wal_bufset_flush(struct wal_bufset *wbs);

#endif /* WAL_BUFFER_H */
