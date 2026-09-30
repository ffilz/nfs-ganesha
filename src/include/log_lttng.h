/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (c) 2024 NFS-Ganesha Project
 */

#ifndef _LOG_LTTNG_H
#define _LOG_LTTNG_H

#ifdef USE_LTTNG

#include "log.h"

/**
 * @brief Initialize and register the LTTng log facility.
 *
 * This function registers the "LTTNG" logging facility into the active log
 * facilities list, allowing log messages to be emitted as LTTng tracepoints.
 *
 * @param[in] log_path    Optional log path
 * @param[in] trace_dir   Optional trace directory
 *
 * @return 0 on success, negative errno on failure.
 */
int lttng_log_facility_init(const char *log_path, const char *trace_dir);

/**
 * @brief Return true if the LTTng log facility is the active logger.
 */
bool lttng_log_is_active(void);

/**
 * @brief Shutdown and unregister the LTTng log facility.
 */
void lttng_log_facility_shutdown(void);

#endif /* USE_LTTNG */

#endif /* _LOG_LTTNG_H */
