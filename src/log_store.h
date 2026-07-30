/*
 * log_store.h - NVS flash ring buffer with backfill cursor
 *
 * Sampling never blocks on connectivity. The device records first and
 * transmits opportunistically. Oldest records are evicted when full
 * and the drop is counted so loss is never silent.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LOG_STORE_H__
#define LOG_STORE_H__

#include <stddef.h>
#include "record.h"

/**
 * @brief Mount the NVS partition and recover state.
 * @return 0 on success, negative errno on failure.
 */
int log_store_init(void);

/**
 * @brief Append one record to the ring buffer.
 *
 * If the buffer is full the oldest unconsumed record is evicted and
 * REC_FLAG_DROPPED is set in the new record so the gap is visible
 * in the cloud audit trail.
 *
 * @param rec  Record to store.
 * @return 0 on success, negative errno on failure.
 */
int log_store_put(const struct sample_record *rec);

/**
 * @brief Peek at the next batch of unconsumed records.
 *
 * Does NOT advance the backfill cursor. Call log_store_consume() after
 * a confirmed cloud delivery (PUBACK) to advance.
 *
 * @param recs   Output array.
 * @param max    Maximum number of records to return.
 * @param count  Actual number placed in recs.
 * @return 0 on success, negative errno on failure.
 */
int log_store_get_batch(struct sample_record *recs, size_t max,
                        size_t *count);

/**
 * @brief Advance the backfill cursor by count records.
 *
 * Call only after confirmed PUBACK. Never call on failed delivery
 * or the records are permanently lost.
 *
 * @param count  Number of records to mark consumed.
 */
void log_store_consume(size_t count);

/**
 * @brief Return the number of unconsumed records waiting to be sent.
 * @return Count, or negative errno on failure.
 */
int log_store_pending(void);

#endif /* LOG_STORE_H__ */
