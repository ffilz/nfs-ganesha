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
#include "fsal_types.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_init.h"

#include "proxyv3_fsal_methods.h"

static const char* kFilestoreHost = "192.168.1.4"; // nfsd by hand
//static const char* kFilestoreHost = "10.150.103.122"; // My filestore instance

// The little struct we want Ganesha to hold for us.
struct proxyv3_obj_handle {
   struct fsal_obj_handle obj;
   nfs_fh3 fh3;
   fattr3 attrs;
   // Optional pointer to the parent of this object, NULL for the root.
   const struct proxyv3_obj_handle *parent;
};

static struct proxyv3_obj_handle *kRootObjHandle;


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
         .supported_attrs = ((const attrmask_t) ATTRS_NFS3),
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


// Given a filehandle and corresponding attributes for a given export, produce a
// new object handle (and optionally fill-in fsal_attrs_out).
static struct proxyv3_obj_handle *
proxyv3_alloc_handle(struct fsal_export *export_handle,
                     const nfs_fh3 *fh3,
                     const fattr3 *attrs,
                     const struct proxyv3_obj_handle *parent,
                     struct attrlist *fsal_attrs_out) {
   // Fill the attributes first to avoid an alloc on failure.
   struct attrlist local_attributes;
   struct attrlist *attrs_out;

   // If we aren't given a destination, make up our own.
   if (fsal_attrs_out != NULL) {
      attrs_out = fsal_attrs_out;
   } else {
      // Pretend we are just requesting the NFSv3 attributes we can fill in.
      memset(&local_attributes, 0, sizeof(struct attrlist));
      attrs_out = &local_attributes;
      FSAL_SET_MASK(attrs_out->request_mask, ATTRS_NFS3);
   }

   if (!fattr3_to_fsalattr(attrs, attrs_out)) {
      // NOTE(boulos): The callee already warned, no need for a repeat.
      return NULL;
   }

   // Alright, ready to go. Instead of being fancy like the NFSv4 proxy, we'll
   // allocate the nested fh3 with an additional calloc call.
   struct proxyv3_obj_handle *result = gsh_calloc(1, sizeof(*result));

   // Copy the fh3 struct.
   result->fh3.data.data_len = fh3->data.data_len;
   result->fh3.data.data_val = gsh_calloc(1, fh3->data.data_len);
   memcpy(result->fh3.data.data_val, fh3->data.data_val, fh3->data.data_len);

   // Copy the NFSv3 attrs.
   memcpy(&result->attrs, attrs, sizeof(*attrs));

   fsal_obj_handle_init(&result->obj, export_handle, attrs_out->type);
   // Just doing what pxy_alloc_handle does...
   result->obj.fs = NULL;
   result->obj.state_hdl = NULL;
   result->obj.fsid = attrs_out->fsid;
   result->obj.fileid = attrs_out->fileid;
   result->obj.obj_ops = &PROXY_V3.handle_ops;

   result->parent = parent;

   return result;
}

// Given a path, parent handle, and so on, do a *single* object lookup.
static fsal_status_t proxyv3_lookup_internal(struct fsal_export *export_handle,
                                             const char *path,
                                             struct fsal_obj_handle *parent,
                                             struct fsal_obj_handle **handle,
                                             struct attrlist *attrs_out) {
   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Doing a lookup of '%s'",
            path);

   if (parent == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Error, expected a parent handle.");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (parent->type != DIRECTORY) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Error, expected parent to be a directory. Got %u",
              parent->type);
      return fsalstat(ERR_FSAL_NOTDIR, 0);
   }

   if (handle == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Error, expected an output handle.");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (path == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Error, received garbage path");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (*path == '\0') {
      // TODO(boulos): What does an empty path mean? We shouldn't have gotten
      // here...
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Error. Path is NUL. Should have exited earlier.");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (strchr(path, '/') != NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Path contains embedded forward slash.");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   struct proxyv3_obj_handle *parent_obj =
      container_of(parent, struct proxyv3_obj_handle, obj);

   if (strcmp(path, ".") == 0 ||
       strcmp(path, "..") == 0) {
      // They just want the current/parent directory. Give it to
      // them. TODO(boulos): Should this force a copy? Should this force a
      // LOOKUP to the server to re-request the attributes?
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got a lookup for '%s' returning the directory handle",
               path);
      struct proxyv3_obj_handle *which_dir;
      if (strcmp(path, ".") == 0) {
         which_dir = parent_obj;
      } else {
         // Sigh, cast away the const here. FSAL shouldn't be asking to edit
         // parent handles...
         which_dir = (struct proxyv3_obj_handle* ) parent_obj->parent;
         // Check that there was a valid parent directory.
         if (which_dir == NULL) {
            LogCrit(COMPONENT_FSAL,
                    "PROXY_V3: Asked for '..' but no parent directory exists");
            return fsalstat(ERR_FSAL_FAULT, 0);
         }
      }

      *handle = &which_dir->obj;

      if (attrs_out != NULL) {
         if (!fattr3_to_fsalattr(&which_dir->attrs, attrs_out)) {
            LogCrit(COMPONENT_FSAL,
                    "PROXY_V3: failed to copy attributes");
            return fsalstat(ERR_FSAL_FAULT, 0);
         }
      }

      return fsalstat(ERR_FSAL_NO_ERROR, 0);
   }

   LOOKUP3args args;
   LOOKUP3res result;

   // The directory is the parent's fh3 handle.
   args.what.dir = parent_obj->fh3;
   // TODO(boulos): Is it actually safe to const cast this away?
   args.what.name = (char*) path;

   if (!proxyv3_nfs_call(kFilestoreHost, op_ctx->creds,
                         NFSPROC3_LOOKUP,
                         (xdrproc_t) xdr_LOOKUP3args, &args,
                         (xdrproc_t) xdr_LOOKUP3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: LOOKUP3 failed");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   if (result.status != NFS3_OK) {
      // Okay, let's see what we got.
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: LOOKUP3 failed, got %u", result.status);
      return nfsstat3_to_fsalstat(result.status);
   }

   // We really need the attributes.
   if (!result.LOOKUP3res_u.resok.obj_attributes.attributes_follow) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: LOOKUP3 didn't return attributes");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   const nfs_fh3* obj_fh =
      &result.LOOKUP3res_u.resok.object;

   const fattr3* obj_attrs =
      &result.LOOKUP3res_u.resok.obj_attributes.post_op_attr_u.attributes;

   struct proxyv3_obj_handle *result_handle =
      proxyv3_alloc_handle(export_handle,
                           obj_fh,
                           obj_attrs,
                           parent_obj,
                           attrs_out);

   if (result_handle == NULL) {
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   *handle = &result_handle->obj;

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Do a specialized lookup for the root of an export via GETATTR3.
fsal_status_t proxyv3_lookup_root(struct fsal_export *export_handle,
                                  struct fsal_obj_handle **handle,
                                  struct attrlist *attrs_out) {
   // Doing a "lookup" on the root, do a GETATTR3 instead.
   GETATTR3args args;
   GETATTR3res result;

   struct proxyv3_export *export =
      container_of(export_handle, struct proxyv3_export, export);

   char *fh_copy = gsh_calloc(1, NFS3_FHSIZE);
   memcpy(fh_copy, export->root_handle, export->root_handle_len);
   args.object.data.data_val = fh_copy;
   args.object.data.data_len = export->root_handle_len;

   memset(&result, 0, sizeof(result));

   // If the call fails for any reason, exit.
   if (!proxyv3_nfs_call(kFilestoreHost, op_ctx->creds,
                         NFSPROC3_GETATTR,
                         (xdrproc_t) xdr_GETATTR3args, &args,
                         (xdrproc_t) xdr_GETATTR3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      gsh_free(fh_copy);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // If we didn't get back NFS3_OK, return the appropriate error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: GETATTR failed. %u",
               result.status);
      gsh_free(fh_copy);
      return nfsstat3_to_fsalstat(result.status);
   }

   // Bundle up the result into a new object handle.
   struct proxyv3_obj_handle *result_handle =
      proxyv3_alloc_handle(export_handle,
                           &args.object,
                           &result.GETATTR3res_u.resok.obj_attributes,
                           NULL /* no parent */,
                           attrs_out);

   // No matter what, clean up the fh_copy we made for args.
   gsh_free(fh_copy);

   // If we couldn't allocate the handle, fail.
   if (result_handle == NULL) {
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   // Make a copy for future lookups.
   kRootObjHandle = result_handle;
   *handle = &(result_handle->obj);

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Given an existing export and a path, try to lookup the file or directory.
fsal_status_t proxyv3_lookup_path(struct fsal_export *export_handle,
                                  const char *path,
                                  struct fsal_obj_handle **handle,
                                  struct attrlist *attrs_out) {
   LogDebug(COMPONENT_FSAL, "PROXY_V3: Looking up path '%s'", path);

   // Check that the first part of the path matches our root.
   const char *kRootPath = "/testy";
   const size_t root_len = strlen(kRootPath);

   const char *p = path;

   // Check that the path matches our root prefix.
   if (strncmp(path, kRootPath, root_len) != 0) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: path ('%s') doesn't match our root ('%s')",
               path, kRootPath);
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   // The prefix matches our root path, move forward.
   p += root_len;

   if (*p == '\0') {
      // Nothing left. Must have been just the root.
      LogDebug(COMPONENT_FSAL, "PROXY_V3: Root Lookup. Doing GETATTR instead");
      return proxyv3_lookup_root(export_handle, handle, attrs_out);
   }

   // Okay, we've got a potential path with slashes. TODO(boulos): Split it up,
   // calling our lookup internal function on each segment.
   return proxyv3_lookup_internal(export_handle, p,
                                  &kRootObjHandle->obj, handle, attrs_out);

}

static fsal_status_t
proxyv3_lookup_handle(struct fsal_obj_handle *parent,
                      const char *path,
                      struct fsal_obj_handle **handle,
                      struct attrlist *attrs_out)
{
   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: lookup_handle for path '%s'", path);
   return proxyv3_lookup_internal(op_ctx->fsal_export, path,
                                  parent, handle, attrs_out);
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
   PROXY_V3.handle_ops.lookup = proxyv3_lookup_handle;
}
