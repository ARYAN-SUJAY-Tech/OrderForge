#include "common/metrics.h"

#include <inttypes.h>
#include <time.h>

static const char *const mode_names[] = {"naive", "db-lock", "lockfree-shm"};

static void metric(buf_t *b, const char *name, const char *type, const char *help, uint64_t v)
{
    buf_printf(b, "# HELP %s %s\n# TYPE %s %s\n%s %" PRIu64 "\n", name, help, name, type, name, v);
}

#define C(name, field, help) metric(out, "orderforge_" name, "counter", help, atomic_load(&m->field))
#define G(name, value, help) metric(out, "orderforge_" name, "gauge", help, (uint64_t)(value))

void metrics_format(const stock_table_t *t, buf_t *out)
{
    const stock_shm_t *h = t->shm;
    const stock_metrics_t *m = &h->metrics;
    int64_t started = atomic_load(&h->started_unix);

    G("core_up", started != 0, "1 while of-core has finished starting.");
    G("core_start_time_seconds", started, "Unix time of the last of-core start.");
    buf_printf(out, "# HELP orderforge_stock_mode_info Stock protection mode.\n"
                    "# TYPE orderforge_stock_mode_info gauge\n"
                    "orderforge_stock_mode_info{mode=\"%s\"} 1\n",
               h->stock_mode < 3 ? mode_names[h->stock_mode] : "unknown");

    C("requests_total", requests_total, "Requests received by of-core.");
    C("busy_rejects_total", busy_rejects, "Requests refused because the job queue was full.");
    G("queue_depth", atomic_load(&m->queue_depth), "Jobs waiting for a worker.");
    G("active_workers", atomic_load(&m->active_workers), "Workers running a request.");
    C("orders_total", orders_total, "Orders placed.");
    C("orders_failed_total", orders_failed, "Checkouts refused because stock ran out.");
    C("cas_retries_total", cas_retries, "Lost compare-and-swap races in stock reservation.");
    C("payments_sent_total", payments_sent, "Payment requests handed to of-pay.");
    C("payments_succeeded_total", payments_succeeded, "Payments approved.");
    C("payments_declined_total", payments_declined, "Payments declined.");
    C("payments_ignored_total", payments_ignored, "Duplicate or replayed payment replies ignored.");
    C("payments_rejected_total", payments_rejected, "Payment messages with a bad MAC or nonce.");
    C("late_refunds_total", late_refunds, "Approvals that arrived after expiry, refunded.");
    C("reservations_expired_total", reservations_expired, "Unpaid orders cancelled by the reaper.");

    /* Histogram: buckets are stored per bucket and emitted cumulatively. */
    static const int bounds[OF_LAT_BUCKETS] = OF_LAT_BOUNDS_MS;
    buf_puts(out, "# HELP orderforge_request_duration_seconds Time from queueing to response "
                  "in of-core.\n# TYPE orderforge_request_duration_seconds histogram\n");
    uint64_t cum = 0;
    for (int i = 0; i < OF_LAT_BUCKETS; i++) {
        cum += atomic_load(&m->latency_bucket[i]);
        if (bounds[i] < 0)
            buf_printf(out, "orderforge_request_duration_seconds_bucket{le=\"+Inf\"} %" PRIu64 "\n",
                       cum);
        else
            buf_printf(out, "orderforge_request_duration_seconds_bucket{le=\"%g\"} %" PRIu64 "\n",
                       bounds[i] / 1000.0, cum);
    }
    buf_printf(out, "orderforge_request_duration_seconds_sum %.6f\n"
                    "orderforge_request_duration_seconds_count %" PRIu64 "\n",
               atomic_load(&m->latency_sum_us) / 1e6, atomic_load(&m->latency_count));

    C("wal_appends_total", wal.appends, "Records appended to the write-ahead log.");
    C("wal_flushes_total", wal.flushes, "write+fdatasync rounds (group commits).");
    C("wal_bytes_total", wal.bytes, "Bytes made durable in the write-ahead log.");
    C("wal_flush_microseconds_total", wal.flush_us_total, "Time spent in write+fdatasync.");
    G("wal_max_batch", atomic_load(&m->wal.max_batch), "Most records made durable by one fsync.");
    C("wal_compactions_total", wal.compactions, "Write-ahead log compactions.");

    C("core_starts_total", starts, "of-core starts.");
    C("core_recoveries_total", recoveries, "Starts that recovered from a crashed of-core.");
    G("recovery_last_milliseconds", atomic_load(&m->recovery_ms_last), "Duration of the last start-up recovery.");
    C("recovered_committed_total", recovered_committed, "In-flight reservations found committed in MySQL.");
    C("recovered_released_total", recovered_released, "In-flight reservations found rolled back.");
    C("reconcile_drift_products_total", drift_products, "Products corrected by reconciliation.");
    C("reconcile_drift_units_total", drift_units, "Units corrected by reconciliation.");
    C("wal_torn_bytes_total", wal_torn_bytes, "Torn log tails discarded during recovery.");

    G("stock_products", atomic_load(&h->count), "Products in the shared-memory stock table.");
    buf_puts(out, "# HELP orderforge_stock_available Units available per product (live).\n"
                  "# TYPE orderforge_stock_available gauge\n");
    for (uint32_t i = 0; i < h->capacity; i++) {
        uint64_t id = atomic_load(&h->slots[i].product_id);
        if (id)
            buf_printf(out, "orderforge_stock_available{product_id=\"%" PRIu64 "\"} %d\n", id,
                       (int)atomic_load(&h->slots[i].available));
    }
    buf_puts(out, "# HELP orderforge_stock_reserved Units held by checkouts in flight.\n"
                  "# TYPE orderforge_stock_reserved gauge\n");
    for (uint32_t i = 0; i < h->capacity; i++) {
        uint64_t id = atomic_load(&h->slots[i].product_id);
        if (id)
            buf_printf(out, "orderforge_stock_reserved{product_id=\"%" PRIu64 "\"} %d\n", id,
                       (int)atomic_load(&h->slots[i].reserved));
    }
}
