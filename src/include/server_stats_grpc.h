#ifndef SERVER_STATS_GRPC_H
#define SERVER_STATS_GRPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Opaque forward declarations; full structs are private to server_stats.c */
struct nfsv3_stats;
struct nfsv40_stats;
struct nfsv41_stats;
struct nfsv42_stats;

/*
 * Portable I/O stats snapshot passed from C to C++ before protobuf fill.
 * Layout matches nfsProtoUtil.IoStats and D-Bus (tttttt) IOSTATS_REPLY.
 */
struct grpc_iostats {
	uint64_t requested;
	uint64_t transferred;
	uint64_t total_ops;
	uint64_t errors;
	uint64_t latency;
	uint64_t queue_wait;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Extract read/write stats from the per-version nfsv*_stats structure. */
void server_grpc_fill_v3_iostats(struct nfsv3_stats *v3p,
				 struct grpc_iostats *read_out,
				 struct grpc_iostats *write_out);

void server_grpc_fill_v40_iostats(struct nfsv40_stats *v40p,
				  struct grpc_iostats *read_out,
				  struct grpc_iostats *write_out);

void server_grpc_fill_v41_iostats(struct nfsv41_stats *v41p,
				  struct grpc_iostats *read_out,
				  struct grpc_iostats *write_out);

void server_grpc_fill_v42_iostats(struct nfsv41_stats *v42p,
				  struct grpc_iostats *read_out,
				  struct grpc_iostats *write_out);

/*
 * Per-version entry points called from nfsServiceServer.cc.
 *
 * Each function mirrors the corresponding D-Bus handler in client_mgr.c
 * (e.g. get_nfsv41_stats_io). On success, read_out/write_out and time_out
 * are filled and *success is set true with errmsg "OK". On failure,
 * *success is false and errmsg carries the D-Bus-equivalent message.
 * The return value is always true (errors are encoded in *success/errmsg,
 * matching D-Bus behaviour where the method call itself succeeds).
 */
bool grpc_cltmgr_get_v3_io(const char *ipaddr, struct grpc_iostats *read_out,
			   struct grpc_iostats *write_out,
			   struct timespec *time_out, bool *success,
			   char *errmsg, size_t errmsg_len);

bool grpc_cltmgr_get_v40_io(const char *ipaddr, struct grpc_iostats *read_out,
			    struct grpc_iostats *write_out,
			    struct timespec *time_out, bool *success,
			    char *errmsg, size_t errmsg_len);

bool grpc_cltmgr_get_v41_io(const char *ipaddr, struct grpc_iostats *read_out,
			    struct grpc_iostats *write_out,
			    struct timespec *time_out, bool *success,
			    char *errmsg, size_t errmsg_len);

bool grpc_cltmgr_get_v42_io(const char *ipaddr, struct grpc_iostats *read_out,
			    struct grpc_iostats *write_out,
			    struct timespec *time_out, bool *success,
			    char *errmsg, size_t errmsg_len);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_STATS_GRPC_H */
