/*
 * Copyright 2020 Google LLC
 * Author: Solomon Boulos <boulos@google.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

#include "config.h"

#include "fsal.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_init.h"

#include "proxyv3_fsal_methods.h"

static const char* kFilestoreHost = "192.168.1.4"; // nfsd by hand
//static const char* kFilestoreHost = "10.150.103.122"; // My filestore instance

struct proxyv3_fsal_module PROXY_V3 = {
   .module = {
      .fs_info = {
         .maxfilesize = INT64_MAX,
         .maxlink = _POSIX_LINK_MAX,
         .maxnamelen = 1024,
         .maxpathlen = 1024,
         .no_trunc = true,
         .chown_restricted = true,
         .case_preserving = true,
         .lock_support = false,
         .named_attr = true,
         .unique_handles = true,
         .acl_support = FSAL_ACLSUPPORT_ALLOW,
         .homogenous = true,
         .supported_attrs = ((const attrmask_t) ATTRS_POSIX),
         .link_supports_permission_checks = true,
         .expire_time_parent = -1,
      }
   }
};

// Global/server-wide parameters for NFSv3 proxying.
static struct config_item proxy_params[] = {
   // Maximum read/write size in bytes
   CONF_ITEM_UI64("maxwread", 1024, FSAL_MAXIOSIZE,
                  1048576,
                  proxyv3_fsal_module,
                  module.fs_info.maxwrite),

   CONF_ITEM_UI64("maxwrite", 1024, FSAL_MAXIOSIZE,
                  1048576,
                  proxyv3_fsal_module,
                  module.fs_info.maxwrite),

   CONFIG_EOL
};

// Per-export parameters
static struct config_item proxy_export_params[] = {
   CONF_ITEM_NOOP("name"),
   CONF_MAND_IP_ADDR("Srv_Addr", "127.0.0.1",
                     proxyv3_client_params, srv_addr),
   CONFIG_EOL
};

struct config_block proxy_param = {
   .dbus_interface_name = "org.ganesha.nfsd.config.fsal.proxyv3",
   .blk_desc.name = "PROXY_V3",
   .blk_desc.type = CONFIG_BLOCK,
   .blk_desc.u.blk.init = noop_conf_init,
   .blk_desc.u.blk.params = proxy_params,
   .blk_desc.u.blk.commit = noop_conf_commit
};

struct config_block proxy_export_param = {
   .dbus_interface_name = "org.ganesha.nfsd.config.fsal.proxyv3-export%d",
   .blk_desc.name = "FSAL",
   .blk_desc.type = CONFIG_BLOCK,
   .blk_desc.u.blk.init = noop_conf_init,
   .blk_desc.u.blk.params = proxy_export_params,
   .blk_desc.u.blk.commit = noop_conf_commit
};


// Load our configuration from the config file and do any validation we need to.
static fsal_status_t proxyv3_init_config(struct fsal_module *fsal_handle,
                                         config_file_t config_file,
                                         struct config_error_type *error_type) {
   struct proxyv3_fsal_module *proxy_v3 =
      container_of(fsal_handle, struct proxyv3_fsal_module, module);

   (void) load_config_from_parse(config_file,
                                 &proxy_param,
                                 proxy_v3,
                                 true,
                                 error_type);
   if (!config_error_is_harmless(error_type)) {
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   display_fsinfo(&(proxy_v3->module));
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Given an existing export and a path, try to lookup the file or directory.
fsal_status_t proxyv3_lookup_path(struct fsal_export *export_handle,
                                  const char *path,
                                  struct fsal_obj_handle **handle,
                                  struct attrlist *attrs_out) {
   LOOKUP3args args;
   LOOKUP3res result;

   struct proxyv3_export *export =
      container_of(export_handle, struct proxyv3_export, export);

   LogDebug(COMPONENT_FSAL, "PROXY_V3: Looking up path '%s'", path);

   // For the *root*, pass in the root handle as the "what".
   args.what.dir.data.data_val = export->root_handle;
   args.what.dir.data.data_len = export->root_handle_len;

   // Remove the const via cast, which is okay as args is actually
   // const LOOKUP3arg and the callee won't modify it. TODO(boulos):
   // Is it actually right that we have to first strip the mount point
   // for NFSv3?
   args.what.name = (char*) (path + strlen("/testy"));

   if (!proxyv3_nfs_call(kFilestoreHost, op_ctx->creds,
                         NFSPROC3_LOOKUP,
                         (xdrproc_t) xdr_LOOKUP3args, &args,
                         (xdrproc_t) xdr_LOOKUP3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_call failed");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // Okay, let's see what we got.
   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Got back status %u", result.status);

   // TODO(boulos): Fill in the attrs_out appropriately.

   return fsalstat(ERR_FSAL_INVAL, 0);
}

// Setup our NFSv3 Proxy for a given NFS Export.
static fsal_status_t proxyv3_create_export(struct fsal_module *fsal_handle,
                                           void *parse_node,
                                           struct config_error_type *error_type,
                                           const struct fsal_up_vector *up_ops) {
   struct proxyv3_export *export = gsh_calloc(1, sizeof(*export));
   int ret;

   // NOTE(boulos): fsal_export_init sets the export ops to defaults.
   fsal_export_init(&export->export);
   export->export.exp_ops.lookup_path = proxyv3_lookup_path;

   // Try to load the config. If it fails (say they didn't provide
   // Srv_Addr), exit early and free the allocated export.
   ret = load_config_from_node(parse_node,
                               &proxy_export_param,
                               &export->params,
                               true,
                               error_type);
   if (ret != 0) {
      LogCrit(COMPONENT_FSAL,
              "Bad params for export %s",
              op_ctx->ctx_export->fullpath);
      gsh_free(export);
      return fsalstat(ERR_FSAL_INVAL, ret);
   }

   export->export.fsal = fsal_handle;
   export->export.up_ops = up_ops;
   op_ctx->fsal_export = &export->export;

   // Attempt to "attach" our FSAL to the export. (I think this just always works...).
   ret = fsal_attach_export(fsal_handle, &export->export.exports);
   if (ret != 0) {
      LogCrit(COMPONENT_FSAL,
              "Failed to attach export %s",
              op_ctx->ctx_export->fullpath);
      gsh_free(export);
      return fsalstat(ERR_FSAL_INVAL, ret);
   }

   // Try to mount. TODO(boulos): Grab this from the parameters.
   mnt3_dirpath dirpath = "/testy";
   mountres3 result;
   memset(&result, 0, sizeof(result));

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Going to try to issue a NULL MOUNT at %s",
            kFilestoreHost);

   // Be nice and try a MOUNT NULL first.
   if (!proxyv3_mount_call(kFilestoreHost, op_ctx->creds,
                           MOUNTPROC3_NULL,
                           (xdrproc_t) xdr_void, NULL,
                           (xdrproc_t) xdr_void, NULL)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_mount_call for NULL failed");
      gsh_free(export);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Going to try to mount '%s' on %s",
            dirpath, kFilestoreHost);

   if (!proxyv3_mount_call(kFilestoreHost, op_ctx->creds,
                           MOUNTPROC3_MNT,
                           (xdrproc_t) xdr_dirpath, &dirpath,
                           (xdrproc_t) xdr_mountres3, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_mount_call for path '%s' failed", dirpath);
      gsh_free(export);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (result.fhs_status != MNT3_OK) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Mount failed. Got back %u for path '%s'",
              result.fhs_status, dirpath);
      gsh_free(export);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Mount successful. Got back a %u len fhandle",
            result.mountres3_u.mountinfo.fhandle.fhandle3_len);

   // Copy the result for later use.
   export->root_handle_len = result.mountres3_u.mountinfo.fhandle.fhandle3_len;
   memcpy(export->root_handle,
          result.mountres3_u.mountinfo.fhandle.fhandle3_val,
          export->root_handle_len);

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}


MODULE_INIT void proxy_v3_init(void) {
   // Try to register our FSAL. If it fails, exit.
   if (register_fsal(&PROXY_V3.module, "PROXY_V3", FSAL_MAJOR_VERSION,
                     FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS) != 0) {
      return;
   }

   if (!proxyv3_rpc_init()) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: RPC system failed to initialize");
      return;
   }

   PROXY_V3.module.m_ops.init_config = proxyv3_init_config;
   PROXY_V3.module.m_ops.create_export = proxyv3_create_export;

   // Fill in the objecting handling ops with the default "Hey! NOT IMPLEMENTED!!" ones.
   fsal_default_obj_ops_init(&PROXY_V3.handle_ops);
}
