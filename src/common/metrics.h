/*
 * metrics.h - render the shared-memory metrics in the Prometheus text format.
 *
 * Used by of-api (GET /api/metrics, localhost only) and by `of-ctl metrics`. Both read
 * the table through a read-only mapping, so scraping never sends a request to of-core
 * and keeps working while of-core is busy.
 */
#ifndef OF_METRICS_H
#define OF_METRICS_H

#include "common/buf.h"
#include "common/stock.h"

void metrics_format(const stock_table_t *t, buf_t *out);

#endif
