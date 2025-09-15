/* SPDX-License-Identifier: unknown license... */

typedef int             int32_t;
typedef unsigned int    uint32_t;
typedef hyper           int64_t;
typedef unsigned hyper  uint64_t;


struct cluster_qos_msg {
        cqos_cmd_type_t cqos_cmd;
        int32_t cqos_ops;
        uint16_t export_id;
        uint64_t export_rbw;
        uint64_t export_wbw;
        uint64_t export_riops;
        uint64_t export_wiops;
        uint64_t client_rbw;
        uint64_t client_wbw;
        uint64_t client_riops;
        uint64_t client_wiops;
        sockaddr_t node_addr;
        sockaddr_t client_addr;
};

program CQOSPROG {
	version CQOS_VERS {
		void CQOSPROC_PUBSUB_NULL(void)           = 0;
		void CQOSPROC_PUBSUB_MSG(cluster_qos_msg) = 1;
	} = 1;
} = 100062;
