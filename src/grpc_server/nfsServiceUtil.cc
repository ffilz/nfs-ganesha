/* SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2026, IBM
 * Contributor : Avani Rateria <arateria@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

#include <fstream>
#include <log.h>
#include <nfsServiceUtil.h>

// Utility function for reading key files
std::string read_cert_file(const char *filepath)
{
	if (filepath == nullptr || filepath[0] == '\0') {
		return "";
	}

	std::ifstream file_stream(filepath, std::ios::in | std::ios::binary);
	std::ostringstream buffer;

	if (!file_stream.is_open()) {
		LogWarn(COMPONENT_GRPC, "Failed to open file: %s", filepath);

		return "";
	}

	buffer << file_stream.rdbuf();

	return buffer.str();
}
