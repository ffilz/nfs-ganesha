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
#include <nfsService.pb.h>

// Utility function for reading key files
std::string read_cert_file(std::string_view filepath)
{
	std::ifstream file_stream(std::string(filepath),
				  std::ios::in | std::ios::binary);
	std::ostringstream buffer;

	if (!file_stream.is_open()) {
		LogWarn(COMPONENT_GRPC, "Failed to open file: %.*s",
			(int)filepath.size(), filepath.data());

		return "";
	}

	buffer << file_stream.rdbuf();

	return buffer.str();
}

// Read drc info to grpc
void drc_to_grpc(drc_t *drc, void *state)
{
	nfsService::DrcInfoResponse *response =
		static_cast<nfsService::DrcInfoResponse *>(state);
	char client_ip[SOCK_NAME_MAX] = { 0 };
	char *ipaddr = client_ip;
	int port;

	if (!drc) {
		LogEvent(COMPONENT_GRPC, "Skipping NULL drc");
		return;
	}

	if (!sprint_sockip(&drc->d_u.tcp.addr, ipaddr, SOCK_NAME_MAX))
		(void)strlcpy(ipaddr, "<unknown>", SOCK_NAME_MAX);

	port = get_sockport(&drc->d_u.tcp.addr);

	response->add_drc_info()->set_client_addr_label("Client Address:");
	response->add_drc_info()->set_client_addr(ipaddr);
	response->add_drc_info()->set_client_port_label("Client Port:");
	response->add_drc_info()->set_client_port(port);
	response->add_drc_info()->set_drc_entry_label("Number of DRC Entries:");
	response->add_drc_info()->set_num_of_drc_entries(drc->size);
}
