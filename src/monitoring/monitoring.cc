/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2021
 * Author: Bjorn Leffler leffler@google.com
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <memory>
#include <string>
#include <thread>  // NOLINT

#include "absl/base/call_once.h"
#include "absl/container/btree_map.h"
#include "absl/time/time.h"
#include "absl/time/clock.h"
#include "metrics/metrics.h"

// These imports MUST come after the C++ standard imports to avoid conflicts.
#include "monitoring.h"  // NOLINT
#include "./nfs_names.h"

using metrics::Counter;
using metrics::Gauge;
using metrics::GetExportLabel;
using metrics::RegisterExportLabel;
using metrics::RequestMetrics;

using std::string;
using std::thread;
using std::unique_ptr;
using std::to_string;

static absl::once_flag once;

static absl::btree_map<string, absl::Time> clientActivity;
static unique_ptr<thread> monitoringThread;
static uint32_t activeThresholdSeconds = 60;

static unique_ptr<Counter> mdcacheCacheHitsTotal;
static unique_ptr<Counter> mdcacheCacheMissesTotal;
static unique_ptr<Counter> mdcacheCacheHitsByExportTotal;
static unique_ptr<Counter> mdcacheCacheMissesByExportTotal;
static unique_ptr<Counter> rpcsReceivedTotal;
static unique_ptr<Counter> rpcsProcessedTotal;

static unique_ptr<Gauge> rpcsInFlight;
static unique_ptr<Gauge> workerThreads;
static unique_ptr<Gauge> lastClientUpdate;
static unique_ptr<Gauge> activeClients;
static unique_ptr<Gauge> activeClientsThresholdSeconds;

// Per client metrics.
// Only track request and throughput rates to reduce memory overhead.
// RequestMetrics generates more metrics: latency percentiles, etc.
static unique_ptr<Counter> clientRequestsTotal;
static unique_ptr<Counter> clientTransferredBytesTotal;

// Global NFS metrics.
static unique_ptr<RequestMetrics> nfsMetrics;
static unique_ptr<Counter> errorsByVersionOperationStatus;

static void createCounters() {
  mdcacheCacheHitsTotal.reset(new Counter(
      "mdcache_cache_hits_total",
      "Counter for total cache hits in mdcache.",
      {"operation"}));
  mdcacheCacheMissesTotal.reset(new Counter(
    "mdcache_cache_misses_total",
    "Counter for total cache misses in mdcache.",
    {"operation"}));
  mdcacheCacheHitsByExportTotal.reset(new Counter(
    "mdcache_cache_hits_by_export_total",
    "Counter for total cache hits in mdcache, by export.",
    {"export", "operation"}));
  mdcacheCacheMissesByExportTotal.reset(new Counter(
    "mdcache_cache_misses_by_export_total",
    "Counter for total cache misses in mdcache, by export.",
    {"export", "operation"}));
  rpcsReceivedTotal.reset(new Counter("rpcs_received_total",
                                      "Counter for total RPCs received.",
                                      {}));
  rpcsProcessedTotal.reset(new Counter("rpcs_processed_total",
                                       "Counter for total RPCs processed.",
                                       {}));
  clientRequestsTotal.reset(new Counter("client_requests_total",
                                        "Total requests by client.",
                                        {"client", "operation"}));
  clientTransferredBytesTotal.reset(new Counter(
    "client_transferred_bytes_total",
    "Total bytes transferred by client.",
    {"client", "operation"}));
  errorsByVersionOperationStatus.reset(new Counter(
    "nfs_errors_total",
    "Error count by version, operation and status.",
    {"version", "operation", "status"}));
}

static void createGauges() {
  rpcsInFlight.reset(new Gauge("rpcs_in_flight",
                               "Number of NFS requests received or in flight.",
                               {}));
  workerThreads.reset(new Gauge("worker_threads",
                                "NFS worker threads.",
                                {"label"}));
  lastClientUpdate.reset(new Gauge("last_client_update",
                                   "Last update timestamp, per client.",
                                   {"client"}));
  activeClients.reset(new Gauge("active_clients", "Total active clients.", {}));
  activeClientsThresholdSeconds.reset(
    new Gauge("active_clients_threshold_seconds",
              "Timeout in seconds for a client to be considered active.",
              {}));
}

static void createRequestMetrics() {
  nfsMetrics = unique_ptr<RequestMetrics>(new RequestMetrics("nfs"));
}

void monitoringLoop() {
  while (true) {
    // Counting active clients is tricky.
    // Estimate as "clients active in the last XX seconds"
    int active = 0;
    const auto now = absl::Now();
    for (auto const& [key, val] : clientActivity) {
      const double durationSeconds = absl::ToDoubleSeconds(now - val);
      if (durationSeconds < activeThresholdSeconds) {
        active++;
      }
    }
    activeClients->Set(active);
    activeClientsThresholdSeconds->Set(activeThresholdSeconds);
    sleep (10);
  }
}

static string trimIPv6Prefix(const string input) {
  const string prefix("::ffff:");
  if (input.find(prefix) == 0) {
    return input.substr(prefix.size());
  }
  return input;
}

static void init(const uint16_t port) {
  metrics::InitMonitoring(port);
  createCounters();
  createGauges();
  createRequestMetrics();
  monitoringThread = unique_ptr<thread>(new thread(monitoringLoop));
}

extern "C" {

void monitoring_register_export_label(const export_id_t export_id,
                                      const char* label) {
  RegisterExportLabel(export_id, label);
}

const char *monitoring_get_export_label(uint16_t export_id) {
  const string &label = GetExportLabel(export_id);
  return label.c_str();
}

void monitoring_init(const uint16_t port) {
  absl::call_once(once, init, port);
}

static void observeNfsRequest(const char *operation,
                              const nsecs_elapsed_t request_time,
                              const string version,
                              const string statusLabel,
                              const export_id_t export_id,
                              string client) {
  uint64_t latency_ms = request_time / NS_PER_MSEC;
  const string exportLabel = GetExportLabel(export_id);
  errorsByVersionOperationStatus->Increment({version, operation, statusLabel});
  nfsMetrics->ObserveRequest(latency_ms, statusLabel, operation, exportLabel);
  if (client != "") {
    client = trimIPv6Prefix(client);
    clientRequestsTotal->Increment({client, operation});
  }
}
void monitoring_nfs3_request(const uint32_t proc,
                             const nsecs_elapsed_t request_time,
                             const nfsstat3 fsal_status,
                             const export_id_t export_id,
                             const char* client_ip) {
  const string version("nfs3");
  const char *operation = nfs3_proc_name(proc);
  const char *statusLabel = nfsstat3_name(fsal_status);
  const string client(client_ip == NULL ? "" : client_ip);
  observeNfsRequest(operation, request_time, version, statusLabel, export_id,
                    client);
}

void monitoring_nfs4_request(const uint32_t proc,
                             const nsecs_elapsed_t request_time,
                             const nfsstat4 status,
                             const export_id_t export_id,
                             const char* client_ip) {
  const string version("nfs4");
  const char *operation = nfs4_proc_name(proc);
  const char *statusLabel = nfsstat4_name(status);
  const string client(client_ip == NULL ? "" : client_ip);
  observeNfsRequest(operation, request_time, version, statusLabel, export_id,
                    client);
}

void monitoring_nfs_io(const size_t requested,
                       const size_t transferred,
                       const bool success,
                       const bool is_write,
                       const export_id_t export_id,
                       const char* client_ip) {
  const string exportLabel = GetExportLabel(export_id);
  const string operation(is_write ? "write" : "read");
  nfsMetrics->ObserveIO(requested, transferred, transferred, operation,
                        exportLabel);
  string client(client_ip == NULL ? "" : client_ip);
  if (client != "") {
    client = trimIPv6Prefix(client);
    clientTransferredBytesTotal->Increment({client, operation}, transferred);
  }
}

void monitoring_mdcache_cache_hit(const char *operation,
                                  const export_id_t export_id) {
  mdcacheCacheHitsTotal->Increment({operation});
  mdcacheCacheHitsByExportTotal->Increment({to_string(export_id), operation});
}

void monitoring_mdcache_cache_miss(const char *operation,
                                   const export_id_t export_id) {
  mdcacheCacheMissesTotal->Increment({operation});
  mdcacheCacheMissesByExportTotal->Increment({to_string(export_id), operation});
}

void monitoring_rpcs_in_flight(const uint64_t value) {
  rpcsInFlight->Set(value);
}
void monitoring_rpc_received() {
  rpcsReceivedTotal->Increment();
}
void monitoring_rpc_processed() {
  rpcsProcessedTotal->Increment();
}

void monitoring_client_activity(const char *ip) {
  const auto now = absl::Now();
  const int64_t epoch = absl::ToUnixSeconds(now);
  clientActivity.try_emplace(ip, now);
  lastClientUpdate->Set({ip}, epoch);
}

void monitoring_worker_thread_min(const uint64_t value) {
  workerThreads->Set({"min"}, value);
}
void monitoring_worker_thread_max(const uint64_t value) {
  workerThreads->Set({"max"}, value);
}
void monitoring_worker_thread_start() {
  workerThreads->Increment({"total"});
}
void monitoring_worker_thread_exit() {
  workerThreads->Decrement({"total"});
}

}  // extern "C"
