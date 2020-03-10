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
#include "nfs_file_handle.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_init.h"

#include "proxyv3_fsal_methods.h"

// The little struct we want Ganesha to hold for us.
struct proxyv3_obj_handle {
   struct fsal_obj_handle obj;
   nfs_fh3 fh3;
   fattr3 attrs;
   // Optional pointer to the parent of this object, NULL for the root.
   const struct proxyv3_obj_handle *parent;
};

// TODO(boulos): I should probably shove this into the module or something...
static struct proxyv3_obj_handle *kRootObjHandle;

// This struct tells Ganesha which things we can handle or not. Some of the
// fields are filled in *later* with an FSINFO call.
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
         .named_attr = false,
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
   CONF_ITEM_UI64("maxread", 1024, FSAL_MAXIOSIZE,
                  1048576,
                  proxyv3_fsal_module,
                  module.fs_info.maxread),

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

// Grab the sockaddr from our params via op_ctx.
static const struct sockaddr* proxyv3_sockaddr() {
   struct proxyv3_export *export =
      container_of(op_ctx->fsal_export, struct proxyv3_export, export);

   return export->params.sockaddr;
}

// Grab the socklen from our params via op_ctx.
static const socklen_t proxyv3_socklen() {
   struct proxyv3_export *export =
      container_of(op_ctx->fsal_export, struct proxyv3_export, export);

   return export->params.socklen;
}

// Grab the debugging sockname from our params via op_ctx.
static const char* proxyv3_sockname() {
   struct proxyv3_export *export =
      container_of(op_ctx->fsal_export, struct proxyv3_export, export);

   return export->params.sockname;
}

// Get the current mountd port.
static const uint proxyv3_mountd_port() {
   struct proxyv3_export *export =
      container_of(op_ctx->fsal_export, struct proxyv3_export, export);

   return export->params.mountd_port;
}

// Get the current nfsd port.
static const uint proxyv3_nfsd_port() {
   struct proxyv3_export *export =
      container_of(op_ctx->fsal_export, struct proxyv3_export, export);

   return export->params.nfsd_port;
}


// Load our configuration from the config file and do any validation we need to.
static fsal_status_t proxyv3_init_config(struct fsal_module *fsal_handle,
                                         config_file_t config_file,
                                         struct config_error_type *error_type) {
   struct proxyv3_fsal_module *proxy_v3 =
      container_of(fsal_handle, struct proxyv3_fsal_module, module);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Handling our config");

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

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Making handle from fh3 %p with parent %p",
            fh3, parent);

   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 handle is %s", LEN_FH_STR,
                      fh3->data.data_val, fh3->data.data_len);


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
   struct proxyv3_obj_handle *result =
      gsh_calloc(1, sizeof(struct proxyv3_obj_handle));

   // Copy the fh3 struct.
   result->fh3.data.data_len = fh3->data.data_len;
   result->fh3.data.data_val = gsh_calloc(1, fh3->data.data_len);
   memcpy(result->fh3.data.data_val, fh3->data.data_val, fh3->data.data_len);

   // Copy the NFSv3 attrs.
   memcpy(&result->attrs, attrs, sizeof(fattr3));

   fsal_obj_handle_init(&result->obj, export_handle, attrs_out->type);

   result->obj.fsid = attrs_out->fsid;
   result->obj.fileid = attrs_out->fileid;
   result->obj.obj_ops = &PROXY_V3.handle_ops;

   result->parent = parent;

   return result;
}

// Clean up a handle.
static void proxyv3_handle_release(struct fsal_obj_handle *obj_hdl)
{
   struct proxyv3_obj_handle *handle =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Cleaning up handle %p", handle);

   // Free the underlying filehandle bytes.
   gsh_free(handle->fh3.data.data_val);

   // Finish the outer object.
   fsal_obj_handle_fini(obj_hdl);

   // Free our allocated handle.
   gsh_free(handle);
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

   // Mark as NULL in case we fail along the way.
   *handle = NULL;

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
              "PROXY_V3: Path (%s) contains embedded forward slash.", path);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   struct proxyv3_obj_handle *parent_obj =
      container_of(parent, struct proxyv3_obj_handle, obj);

   // Small optimization to avoid a round-trip: if we know the answer, hand it back.
   if (true && /* TODO(boulos): Turn this into a flag */
       (strcmp(path, ".") == 0 ||
       // We may not have the parent pointer information (could be from a
       // create_handle from key thing, so let the backend respond)
        (strcmp(path, "..") == 0 && parent_obj->parent != NULL))) {
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
         which_dir = (struct proxyv3_obj_handle*) parent_obj->parent;
      }

      // Make a copy for the result.
      struct proxyv3_obj_handle *result_handle =
         proxyv3_alloc_handle(export_handle,
                              &which_dir->fh3,
                              &which_dir->attrs,
                              which_dir->parent,
                              attrs_out);

      if (result_handle == NULL) {
         return fsalstat(ERR_FSAL_FAULT, 0);
      }

      *handle = &result_handle->obj;

      return fsalstat(ERR_FSAL_NO_ERROR, 0);
   }

   LOOKUP3args args;
   LOOKUP3res result;

   // The directory is the parent's fh3 handle.
   args.what.dir = parent_obj->fh3;
   // TODO(boulos): Is it actually safe to const cast this away?
   args.what.name = (char*) path;

   memset(&result, 0, sizeof(result));

   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
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

// The core "Do a GETATTR3" routine.
static fsal_status_t proxyv3_getattr_from_fh3(struct nfs_fh3 *fh3,
                                              struct attrlist *attrs_out) {
   GETATTR3args args;
   GETATTR3res result;

   LogDebug(COMPONENT_FSAL,
            "Doing a getattr on fh3 (%p) with len %u",
            fh3->data.data_val, fh3->data.data_len);

   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 handle is %s", LEN_FH_STR,
                      fh3->data.data_val, fh3->data.data_len);

   args.object.data.data_val = fh3->data.data_val;
   args.object.data.data_len = fh3->data.data_len;

   memset(&result, 0, sizeof(result));

   // If the call fails for any reason, exit.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_GETATTR,
                         (xdrproc_t) xdr_GETATTR3args, &args,
                         (xdrproc_t) xdr_GETATTR3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // If we didn't get back NFS3_OK, return the appropriate error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: GETATTR failed. %u",
               result.status);
      // If the request wants to know about errors, let them know.
      if (FSAL_TEST_MASK(attrs_out->request_mask, ATTR_RDATTR_ERR)) {
         FSAL_SET_MASK(attrs_out->valid_mask, ATTR_RDATTR_ERR);
      }

      return nfsstat3_to_fsalstat(result.status);
   }

   if (!fattr3_to_fsalattr(&result.GETATTR3res_u.resok.obj_attributes,
                           attrs_out)) {
      // NOTE(boulos): The callee already complained, just exit.
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Do just GETATTR3 for an object.
static fsal_status_t proxyv3_getattrs(struct fsal_obj_handle *obj_hdl,
                                      struct attrlist *attrs_out) {
   struct proxyv3_obj_handle *handle =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Responding to GETATTR request for handle %p",
            handle);

   return proxyv3_getattr_from_fh3(&handle->fh3, attrs_out);
}

// Do a SETATTR3 for obj_hdl of the attributes in attrib_set.
static fsal_status_t
proxyv3_setattr2(struct fsal_obj_handle *obj_hdl,
                 bool bypass /* ignored, since we'll happily "bypass" */,
                 struct state_t *state,
                 struct attrlist *attrib_set) {
   struct proxyv3_obj_handle *handle =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   SETATTR3args args;
   SETATTR3res result;

   memset(&result, 0, sizeof(result));

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Responding to SETATTR request for handle %p",
            handle);

   if (state != NULL) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Asked for a stateful SETATTR2, probably a mistake");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   nfs_fh3 *fh3 = &handle->fh3;
   args.object.data.data_val = fh3->data.data_val;
   args.object.data.data_len = fh3->data.data_len;
   // NOTE(boulos): Ganesha's NFSD handles this above us in nfs3_setattr.
   args.guard.check = false;
   if (!fsalattr_to_sattr3(attrib_set, &args.new_attributes)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: SETATTR3() with invalid attributes");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // If the call fails for any reason, exit.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_SETATTR,
                         (xdrproc_t) xdr_SETATTR3args, &args,
                         (xdrproc_t) xdr_SETATTR3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // If we didn't get back NFS3_OK, return the appropriate error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: SETATTR failed. %u", result.status);
      return nfsstat3_to_fsalstat(result.status);
   }

   // Must have worked :).
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Do a specialized lookup for the root of an export via GETATTR3.
fsal_status_t proxyv3_lookup_root(struct fsal_export *export_handle,
                                  struct fsal_obj_handle **handle,
                                  struct attrlist *attrs_out) {
   struct proxyv3_export *export =
      container_of(export_handle, struct proxyv3_export, export);

   nfs_fh3 fh3;
   fh3.data.data_val = export->root_handle;
   fh3.data.data_len = export->root_handle_len;

   struct attrlist tmp_attrs;
   memset(&tmp_attrs, 0, sizeof(tmp_attrs));
   if (attrs_out != NULL) {
      FSAL_SET_MASK(tmp_attrs.request_mask, attrs_out->request_mask);
   }

   fsal_status_t rc = proxyv3_getattr_from_fh3(&fh3, &tmp_attrs);
   if (FSAL_IS_ERROR(rc)) {
      return rc;
   }

   // Bundle up the result into a new object handle.
   struct proxyv3_obj_handle *result_handle =
      proxyv3_alloc_handle(export_handle,
                           &fh3,
                           &tmp_attrs,
                           NULL /* no parent */,
                           attrs_out);

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
   const char *root_path = op_ctx->ctx_export->fullpath;
   const size_t root_len = strlen(root_path);

   const char *p = path;

   // Check that the path matches our root prefix.
   if (strncmp(path, root_path, root_len) != 0) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: path ('%s') doesn't match our root ('%s')",
               path, root_path);
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

// Issue a CREATE3/MKDIR3/SYMLINK style operation, handling all the "make sure
// we got back the attributes" and so on.
static fsal_status_t
proxyv3_issue_createlike(struct proxyv3_obj_handle *parent_obj,
                         const rpcproc_t nfsProc, const char* procName,
                         xdrproc_t encFunc, const void *encArgs,
                         xdrproc_t decFunc, void *decArgs,
                         nfsstat3 *status,
                         post_op_fh3 *op_f3,
                         post_op_attr *op_attr,
                         struct fsal_obj_handle **new_obj,
                         struct attrlist *attrs_out) {
   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Issuing a %s", procName);

   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         nfsProc,
                         encFunc, encArgs,
                         decFunc, decArgs)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: %s failed", procName);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // Okay, let's see what we got.
   if (*status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: %s failed, got %u", procName, *status);
      return nfsstat3_to_fsalstat(*status);
   }

   // We need both the handle and attributes to fill in the results.
   if (!op_attr->attributes_follow ||
       !op_f3->handle_follows) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: %s didn't return obj attributes (%s) or handle (%s)",
               procName,
               op_attr->attributes_follow ? "T" : "F",
               op_f3->handle_follows ? "T" : "F");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   const nfs_fh3 *obj_fh   = &op_f3->post_op_fh3_u.handle;
   const fattr3 *obj_attrs = &op_attr->post_op_attr_u.attributes;

   struct proxyv3_obj_handle *result_handle =
      proxyv3_alloc_handle(op_ctx->fsal_export,
                           obj_fh,
                           obj_attrs,
                           parent_obj,
                           attrs_out);

   if (result_handle == NULL) {
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   *new_obj = &result_handle->obj;

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Perform an "open" (really CREATE3).
static fsal_status_t
proxyv3_open2(struct fsal_obj_handle *fsal_hdl,
              struct state_t *state,
              fsal_openflags_t openflags,
              enum fsal_create_mode createmode,
              const char *name,
              struct attrlist *attrib_set,
              fsal_verifier_t verifier,
              struct fsal_obj_handle **new_obj,
              struct attrlist *attrs_out,
              bool *caller_perm_check) {
   struct proxyv3_obj_handle *parent_obj =
      container_of(fsal_hdl, struct proxyv3_obj_handle, obj);

   if (state != NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Asked for a stateful open2(). Probably a mistake");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   if (name == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Asked for an open by handle, rather than name. NOTYET");
      return fsalstat(ERR_FSAL_NOTSUPP, 0);
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: open2 of parent %p, name %s with flags %x and mode %u",
            fsal_hdl, name, openflags, createmode);

   CREATE3args args;
   CREATE3res result;
   CREATE3resok *resok = &result.CREATE3res_u.resok;

   memset(&result, 0, sizeof(result));

   // The passed in handle is our parent dir. TODO(boulos): Change this for the
   // mode where name == NULL to include a lookup by handle (or something).
   args.where.dir.data.data_val = parent_obj->fh3.data.data_val;
   args.where.dir.data.data_len = parent_obj->fh3.data.data_len;
   // We can safely const-cast away, this is an input.
   args.where.name = (char*) name;

   switch (createmode) {
   case FSAL_NO_CREATE:
   case FSAL_EXCLUSIVE_41:
   case FSAL_EXCLUSIVE_9P:
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Invalid createmode (%u) for NFSv3. Must be one of "
              "UNCHECKED, GUARDED, or EXCLUSIVE",
              createmode);
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   case FSAL_UNCHECKED:
      args.how.mode = UNCHECKED;
      break;
   case FSAL_GUARDED:
      args.how.mode = GUARDED;
      break;
   case FSAL_EXCLUSIVE:
      args.how.mode = EXCLUSIVE;
      break;
   }

   if (createmode == FSAL_EXCLUSIVE) {
      // Set the verifier
      memcpy(&args.how.createhow3_u.verf, verifier, sizeof(fsal_verifier_t));
   } else {
      // Otherwise, set the attributes for the file.
      if (attrib_set == NULL) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: Non-exclusive CREATE() without attributes.");
         return fsalstat(ERR_FSAL_SERVERFAULT, 0);
      }

      if (!fsalattr_to_sattr3(attrib_set, &args.how.createhow3_u.obj_attributes)) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: CREATE() with invalid attributes");
         return fsalstat(ERR_FSAL_INVAL, 0);
      }
   }

   // Issue the CREATE3 call.
   return proxyv3_issue_createlike(parent_obj,
                                   NFSPROC3_CREATE, "CREATE3",
                                   (xdrproc_t) xdr_CREATE3args, &args,
                                   (xdrproc_t) xdr_CREATE3res, &result,
                                   &result.status,
                                   &resok->obj,
                                   &resok->obj_attributes,
                                   new_obj,
                                   attrs_out);
}

// Make a new symlink from dir/name => link_path.
static fsal_status_t
proxyv3_symlink(struct fsal_obj_handle *dir_hdl,
                const char *name,
                const char *link_path,
                struct attrlist *attrs_in,
                struct fsal_obj_handle **new_obj,
                struct attrlist *attrs_out) {
   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: symlink of parent %p, name %s to => %s",
            dir_hdl, name, link_path);

   SYMLINK3args args;
   SYMLINK3res result;
   SYMLINK3resok *resok = &result.SYMLINK3res_u.resok;
   memset(&result, 0, sizeof(result));

   struct proxyv3_obj_handle *parent_obj =
      container_of(dir_hdl, struct proxyv3_obj_handle, obj);

   args.where.dir.data.data_val = parent_obj->fh3.data.data_val;
   args.where.dir.data.data_len = parent_obj->fh3.data.data_len;
   // We can safely const-cast away, this is an input.
   args.where.name = (char*) name;

   if (attrs_in == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: symlink called without attributes. Unexpected");
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   if (!fsalattr_to_sattr3(attrs_in, &args.symlink.symlink_attributes)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: SYMLINK3 with invalid attributes");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // Again, we can safely const-cast away, because this is an input.
   args.symlink.symlink_data = (char*) link_path;

   // Issue the SYMLINK3 call.
   return proxyv3_issue_createlike(parent_obj,
                                   NFSPROC3_SYMLINK, "SYMLINK3",
                                   (xdrproc_t) xdr_SYMLINK3args, &args,
                                   (xdrproc_t) xdr_SYMLINK3res, &result,
                                   &result.status,
                                   &resok->obj,
                                   &resok->obj_attributes,
                                   new_obj,
                                   attrs_out);
}

// Let Ganesha tell us to "close" a file. This should always be stateless for
// NFSv3, therefore nothing to do but check that and say "Sure!".
static fsal_status_t
proxyv3_close(struct fsal_obj_handle *obj_hdl) {
   LogDebug(COMPONENT_FSAL,
            "Asking for stateless CLOSE of handle %p. Say its not 'opened'!",
            obj_hdl);

   return fsalstat(ERR_FSAL_NOT_OPENED, 0);
}


static fsal_status_t
proxyv3_close2(struct fsal_obj_handle *obj_hdl,
               struct state_t *state) {
   LogDebug(COMPONENT_FSAL,
            "Asking for CLOSE of handle %p (state is %p)",
            obj_hdl, state);

   if (state != NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Received stateful CLOSE request. Likely NFSv4.");
      return fsalstat(ERR_FSAL_NOTSUPP, 0);
   }

   // Stateless close through the other door, say it's not opened (avoid's the
   // decref in fsal_close).
   return fsalstat(ERR_FSAL_NOT_OPENED, 0);
}


// Issue a MKDIR.
static fsal_status_t
proxyv3_mkdir(struct fsal_obj_handle *dir_hdl,
              const char *name, struct attrlist *attrs_in,
              struct fsal_obj_handle **new_obj,
              struct attrlist *attrs_out) {
   struct proxyv3_obj_handle *parent_obj =
      container_of(dir_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: mkdir of %s in parent %p",
            name, dir_hdl);

   // In case we fail along the way.
   *new_obj = NULL;

   MKDIR3args args;
   MKDIR3res result;
   MKDIR3resok *resok = &result.MKDIR3res_u.resok;

   memset(&result, 0, sizeof(result));

   args.where.dir.data.data_val = parent_obj->fh3.data.data_val;
   args.where.dir.data.data_len = parent_obj->fh3.data.data_len;
   args.where.name = (char*) name;

   if (!fsalattr_to_sattr3(attrs_in, &args.attributes)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: MKDIR() with invalid attributes");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // Issue the MKDIR3 call.
   return proxyv3_issue_createlike(parent_obj,
                                   NFSPROC3_MKDIR, "MKDIR3",
                                   (xdrproc_t) xdr_MKDIR3args, &args,
                                   (xdrproc_t) xdr_MKDIR3res, &result,
                                   &result.status,
                                   &resok->obj,
                                   &resok->obj_attributes,
                                   new_obj,
                                   attrs_out);
}


// Do a readdir for the given directory (dir_hdl), possibly picking up where
// `whence` left off.
static fsal_status_t
proxyv3_readdir(struct fsal_obj_handle *dir_hdl,
                fsal_cookie_t *whence, void *cbarg,
                fsal_readdir_cb cb, attrmask_t attrmask,
                bool *eof) {
   struct proxyv3_obj_handle *dir =
      container_of(dir_hdl, struct proxyv3_obj_handle, obj);

   // "This should be set to 0 on the first request to read a directory."
   cookie3 cookie = (whence == NULL) ? 0 : *whence;
   // TODO(boulos): Ganesha doesn't seem to have any way to pass this in
   // alongside whence... The comments in the Ganesha NFSD implementation for
   // READDIRPLUS suggest that most clients just ignore it / expect 0s.
   cookieverf3 cookie_verf;
   memset(&cookie_verf, 0, sizeof(cookie_verf));

   LogDebug(COMPONENT_FSAL,
            "Doing READDIR for dir %p (cookie = %lu)",
            dir, cookie);

   // Check that attrmask is at most NFSv3
   if (!attrmask_is_nfs3(attrmask)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: readdir asked for incompatible output attrs");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   *eof = false;

   while (!(*eof)) {
      READDIRPLUS3args args;
      READDIRPLUS3res result;

      memset(&result, 0, sizeof(result));

      args.dir.data.data_val = dir->fh3.data.data_val;
      args.dir.data.data_len = dir->fh3.data.data_len;
      args.cookie = cookie;
      memcpy(&args.cookieverf, &cookie_verf, sizeof(args.cookieverf));
      // We need to let the server know how much data to return per chunk. The
      // V4 proxy uses 4KB and 16KB.
      args.dircount = 4096;
      args.maxcount = 16384;

      LogDebug(COMPONENT_FSAL,
               "Calling READDIRPLUS with cookie %lu",
               cookie);

      if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                            proxyv3_socklen(),
                            proxyv3_nfsd_port(),
                            op_ctx->creds,
                            NFSPROC3_READDIRPLUS,
                            (xdrproc_t) xdr_READDIRPLUS3args, &args,
                            (xdrproc_t) xdr_READDIRPLUS3res, &result))  {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: proxyv3_nfs_call for READDIRPLUS failed (%u)",
                 result.status);
         return fsalstat(ERR_FSAL_SERVERFAULT, 0);
      }

      if (result.status != NFS3_OK) {
         LogDebug(COMPONENT_FSAL,
                  "PROXY_V3: READDIRPLUS failed. %u",
                  result.status);
         return nfsstat3_to_fsalstat(result.status);
      }

      LogDebug(COMPONENT_FSAL,
               "READDIRPLUS succeeded, looping over dirents");

      READDIRPLUS3resok* resok = &result.READDIRPLUS3res_u.resok;

      // Mark EOF now, if true.
      *eof = resok->reply.eof;
      // Update the cookie verifier for the next iteration.
      memcpy(&cookie_verf, &resok->cookieverf, sizeof(cookie_verf));

      // Loop over all the entries, making fsal objects from the results and
      // calling the given callback.
      for (entryplus3 *entry = resok->reply.entries;
           entry != NULL;
           entry = entry->nextentry) {
         // Don't forget to update the cookie (we *could* do this just at the
         // end, but why bother?).
         cookie = entry->cookie;
         if (strcmp(entry->name, ".") == 0 ||
             strcmp(entry->name, "..") == 0) {
            LogDebug(COMPONENT_FSAL,
                     "Skipping special dir value of '%s'",
                     entry->name);
            continue;
         }

         if (!entry->name_handle.handle_follows) {
            LogCrit(COMPONENT_FSAL,
                    "PROXY_V3: READDIRPLUS didn't return a handle for '%s'",
                    entry->name);
            return fsalstat(ERR_FSAL_SERVERFAULT, 0);
         }

         if (!entry->name_attributes.attributes_follow) {
            LogCrit(COMPONENT_FSAL,
                    "PROXY_V3: READDIRPLUS didn't return attributes for '%s'",
                    entry->name);
            return fsalstat(ERR_FSAL_SERVERFAULT, 0);
         }

         nfs_fh3 *fh3 = &entry->name_handle.post_op_fh3_u.handle;
         fattr3 *attrs = &entry->name_attributes.post_op_attr_u.attributes;
         // Tell alloc_handle we just want the requested attributes.

         struct attrlist cb_attrs;
         memset(&cb_attrs, 0, sizeof(cb_attrs));
         FSAL_SET_MASK(cb_attrs.request_mask, attrmask);

         struct proxyv3_obj_handle *result_handle =
            proxyv3_alloc_handle(op_ctx->fsal_export, fh3, attrs, dir,
                                 &cb_attrs);

         if (result_handle == NULL) {
            LogCrit(COMPONENT_FSAL,
                    "PROXY_V3: Failed to make a handle for READDIRPLUS result "
                    "for file '%s'",
                    entry->name);
            return fsalstat(ERR_FSAL_FAULT, 0);
         }

         enum fsal_dir_result cb_rc =
            cb(entry->name, &result_handle->obj, &cb_attrs, cbarg, entry->cookie);

         // Other FSALs do this as >= DIR_READAHEAD, but I prefer an explicit
         // switch with no default.
         switch (cb_rc) {
         case DIR_CONTINUE: continue; // Next entry.
         case DIR_READAHEAD: break; // We don't do read-ahead, just exit.
         case DIR_TERMINATE: break; // Okay, all done.
         }
      }
   }

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
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

// Handle a read from `obj_hdl` at offset read_arg->offset. When done, let
// done_cb know how it went. NOTE(boulos): This function allows for lots of
// fancy read options like NFSv4 delegations and so on, but as we only allow v3
// callers none of that should apply.
static void
proxyv3_read2(struct fsal_obj_handle *obj_hdl,
              bool bypass /* unused */,
              fsal_async_cb done_cb,
              struct fsal_io_arg *read_arg,
              void *cb_arg) {
   struct proxyv3_obj_handle *obj =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Doing read2 at offset %zu in handle %p of len %zu",
            read_arg->offset, obj_hdl, read_arg->iov[0].iov_len);

   // Signal that we've read 0 bytes.
   read_arg->io_amount = 0;

   // Like Ceph, we don't handle READ_PLUS.
   if (read_arg->info != NULL) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got a READPLUS request. Not supported");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), read_arg, cb_arg);
      return;
   }

   // Since we're just a V3 proxy, we are stateless. If we get a stateful
   // request, something bad must have happened.
   if (read_arg->state != NULL) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got a stateful READ request. Not supported");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), read_arg, cb_arg);
      return;
   }

   // NOTE(boulos): Ganesha doesn't actually have a useful readv() equivalent,
   // since it only allows a single offset (read_arg->offset), so read2
   // implementations can just uselessly fill in different amounts at an
   // offset. NFSv3 doesn't have a readv() equivalent, and Ganesha's NFSD won't
   // generate it from clients anyway, but warn here.
   if (read_arg->iov_count > 1) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got asked for multiple reads at once. Unexpected.");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), read_arg, cb_arg);
      return;
   }

   char *dst = read_arg->iov[0].iov_base;
   uint64_t offset = read_arg->offset;
   size_t bytes_to_read = read_arg->iov[0].iov_len;
   // TODO(boulos): Clamp read size against maxRead (but again, Ganesha's NFSD
   // layer will have already done so).

   READ3args args;
   READ3res result;
   READ3resok *resok = &result.READ3res_u.resok;

   memset(&result, 0, sizeof(result));

   args.file.data.data_val = obj->fh3.data.data_val;
   args.file.data.data_len = obj->fh3.data.data_len;
   args.offset = offset;
   args.count = bytes_to_read;

   // Issue the read.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_READ,
                         (xdrproc_t) xdr_READ3args, &args,
                         (xdrproc_t) xdr_READ3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      done_cb(obj_hdl, fsalstat(ERR_FSAL_SERVERFAULT, 0), read_arg, cb_arg);
      return;
   }

   // If the read failed, tell the callback about the error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: READ failed: %u", result.status);
      done_cb(obj_hdl, nfsstat3_to_fsalstat(result.status), read_arg, cb_arg);
      return;
   }

   // NOTE(boulos): data_len is not part of the NFS spec, but Ganesha should be
   // getting the same number of bytes in the result.
   if (resok->count != resok->data.data_len) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Did a read of len %u (resok.count) but buf says %u",
              resok->count, resok->data.data_len);
      done_cb(obj_hdl, fsalstat(ERR_FSAL_SERVERFAULT, 0), read_arg, cb_arg);
      return;
   }

   read_arg->end_of_file = resok->eof;
   read_arg->io_amount = resok->count;

   // Copy the bytes into the output buffer.
   memcpy(dst, resok->data.data_val, resok->data.data_len);

   // Let the caller know that we're done.
   done_cb(obj_hdl, fsalstat(ERR_FSAL_NO_ERROR, 0), read_arg, cb_arg);
}

// Handle a write to `obj_hdl` at offset writte_arg->offset. When done, let
// done_cb know how it went. NOTE(boulos): This function allows for lots of
// fancy options like NFSv4 delegations and so on, but as we only allow v3
// callers none of that should apply.
static void
proxyv3_write2(struct fsal_obj_handle *obj_hdl,
               bool bypass /* unused */,
               fsal_async_cb done_cb,
               struct fsal_io_arg *write_arg,
               void *cb_arg) {
   struct proxyv3_obj_handle *obj =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Doing write2 at offset %zu in handle %p of len %zu",
            write_arg->offset, obj_hdl, write_arg->iov[0].iov_len);

   // Signal that we've written 0 bytes so far.
   write_arg->io_amount = 0;

   // If into is only for READPLUS, it should definitely be NULL.
   if (write_arg->info != NULL) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Write had 'readplus' info. Something went wrong");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_SERVERFAULT, 0), write_arg, cb_arg);
      return;
   }

   // Since we're just a V3 proxy, we are stateless. If we get a stateful
   // request, something bad must have happened.
   if (write_arg->state != NULL) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got a stateful WRITE request. Not supported");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), write_arg, cb_arg);
      return;
   }

   // NOTE(boulos): Ganesha doesn't actually have a useful readv() equivalent,
   // since it only allows a single offset (read_arg->offset), so read2
   // implementations can just uselessly fill in different amounts at an
   // offset. NFSv3 doesn't have a readv() equivalent, and Ganesha's NFSD won't
   // generate it from clients anyway, but warn here.
   if (write_arg->iov_count > 1) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: Got asked for multiple writes at once. Unexpected.");
      done_cb(obj_hdl, fsalstat(ERR_FSAL_NOTSUPP, 0), write_arg, cb_arg);
      return;
   }

   char *src = write_arg->iov[0].iov_base;
   uint64_t offset = write_arg->offset;
   size_t bytes_to_write = write_arg->iov[0].iov_len;
   // TODO(boulos): Clamp read size against maxRead (but again, Ganesha's NFSD
   // layer will have already done so).

   WRITE3args args;
   WRITE3res result;
   WRITE3resok *resok = &result.WRITE3res_u.resok;

   memset(&result, 0, sizeof(result));

   args.file.data.data_val = obj->fh3.data.data_val;
   args.file.data.data_len = obj->fh3.data.data_len;
   args.offset = offset;
   args.count = bytes_to_write;
   args.data.data_len = bytes_to_write;
   args.data.data_val = src;
   // If the request is for a stable write, ask for FILE_SYNC (rather than just
   // DATA_SYNC), like nfs3_write.c does.
   args.stable = (write_arg->fsal_stable) ? FILE_SYNC : UNSTABLE;

   // Issue the write.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_WRITE,
                         (xdrproc_t) xdr_WRITE3args, &args,
                         (xdrproc_t) xdr_WRITE3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      done_cb(obj_hdl, fsalstat(ERR_FSAL_SERVERFAULT, 0), write_arg, cb_arg);
      return;
   }

   // If the write failed, tell the callback about the error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: WRITE failed: %u", result.status);
      done_cb(obj_hdl, nfsstat3_to_fsalstat(result.status), write_arg, cb_arg);
      return;
   }

   // Signal that we wrote resok->count bytes.
   write_arg->io_amount = resok->count;

   // Let the caller know that we're done.
   done_cb(obj_hdl, fsalstat(ERR_FSAL_NO_ERROR, 0), write_arg, cb_arg);
}

// Handle COMMIT requests.
static fsal_status_t
proxyv3_commit2(struct fsal_obj_handle *obj_hdl,
                off_t offset,
                size_t len) {
   struct proxyv3_obj_handle *obj =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Doing commit at offset %zu in handle %p of len %zu",
            offset, obj_hdl, len);
   COMMIT3args args;
   COMMIT3res result;

   memset(&result, 0, sizeof(result));

   args.file.data.data_val = obj->fh3.data.data_val;
   args.file.data.data_len = obj->fh3.data.data_len;
   args.offset = offset;
   args.count = len;

   // Issue the COMMIT.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_COMMIT,
                         (xdrproc_t) xdr_COMMIT3args, &args,
                         (xdrproc_t) xdr_COMMIT3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              result.status);
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   // If the commit failed, report the error upwards.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: COMMIT failed: %u", result.status);
      return nfsstat3_to_fsalstat(result.status);
   }

   // Commit happened, no problems to report.
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Handle REMOVE3/RMDIR3 requests.
static fsal_status_t
proxyv3_unlink(struct fsal_obj_handle *dir_hdl,
               struct fsal_obj_handle *obj_hdl,
               const char* name) {
   struct proxyv3_obj_handle *dir =
      container_of(dir_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: REMOVE request for dir %p of %s %s",
            dir_hdl, (obj_hdl->type == DIRECTORY) ? "directory" : "file", name);

   /*
    * NOTE(boulos): While the NFSv3 spec says:
    *
    *  In general, REMOVE is intended to remove non-directory file
    *  objects and RMDIR is to be used to remove directories.  However, REMOVE
    *  can be used to remove directories, subject to restrictions imposed by
    *  either the client or server interfaces."
    *
    *  It seems that in practice, Linux's kNFSd at least does not go in for
    *  using REMOVE3 for directories and returns NFS3_ISDIR.
    */

   bool is_rmdir = obj_hdl->type == DIRECTORY;

   REMOVE3args regular_args;
   REMOVE3res regular_result;

   RMDIR3args dir_args;
   RMDIR3res dir_result;

   diropargs3 *diropargs = (is_rmdir) ? &dir_args.object : &regular_args.object;

   memset(&regular_result, 0, sizeof(regular_result));
   memset(&dir_result, 0, sizeof(dir_result));

   diropargs->dir.data.data_val = dir->fh3.data.data_val;
   diropargs->dir.data.data_len = dir->fh3.data.data_len;
   diropargs->name = (char*) name;

   rpcproc_t method = (is_rmdir) ? NFSPROC3_RMDIR : NFSPROC3_REMOVE;
   xdrproc_t enc = (is_rmdir) ? (xdrproc_t) xdr_RMDIR3args :
      (xdrproc_t) xdr_REMOVE3args;
   xdrproc_t dec = (is_rmdir) ? (xdrproc_t) xdr_RMDIR3res :
      (xdrproc_t) xdr_REMOVE3res;

   void *args   = (is_rmdir) ? (void*) &dir_args : (void*) &regular_args;
   void *result = (is_rmdir) ? (void*) &dir_result : (void*) &regular_result;

   nfsstat3 *status = (is_rmdir) ? &dir_result.status : &regular_result.status;

   // Issue the REMOVE.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         method,
                         enc, args,
                         dec, result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call failed (%u)",
              *status);
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   // If the REMOVE/RMDIR failed, report the error upwards.
   if (*status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: %s failed: %u",
               (is_rmdir) ? "RMDIR" : "REMOVE",
               *status);
      return nfsstat3_to_fsalstat(*status);
   }

   // Remove happened, no problems to report.
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}


// Run FSSTAT to learn about how mch space the volume has available.
static fsal_status_t
proxyv3_get_dynamic_info(struct fsal_export *exp_hdl,
                         struct fsal_obj_handle *obj_hdl,
                         fsal_dynamicfsinfo_t *infop) {
   struct proxyv3_obj_handle *obj =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   // FSSTAT is supposed to be called with the root handle.
   if (obj != kRootObjHandle) {
      // Let's just check if the handles actually match.
      if (obj->fh3.data.data_len != kRootObjHandle->fh3.data.data_len ||
          memcmp(obj->fh3.data.data_val,
                 kRootObjHandle->fh3.data.data_val,
                 obj->fh3.data.data_len)) {
         LogCrit(COMPONENT_FSAL,
                 "PROXY_V3: fsinfo called w/ handle %p != root (%p)",
                 obj, kRootObjHandle);
         // Didn't match, exit now.
         return fsalstat(ERR_FSAL_INVAL, 0);
      } else {
         LogDebug(COMPONENT_FSAL,
                  "PROXY_V3: fsinfo called w/ handle %p != root (%p),"
                  "but data matches",
                  obj, kRootObjHandle);
         // Continue onwards.
      }
   }

   FSSTAT3args args;
   FSSTAT3res result;

   args.fsroot.data.data_val = obj->fh3.data.data_val;
   args.fsroot.data.data_len = obj->fh3.data.data_len;

   memset(&result, 0, sizeof(result));
   // If the call fails for any reason, exit.
   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_FSSTAT,
                         (xdrproc_t) xdr_FSSTAT3args, &args,
                         (xdrproc_t) xdr_FSSTAT3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: proxyv3_nfs_call for FSSTAT3 failed (%u)",
              result.status);
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // If we didn't get back NFS3_OK, return the appropriate error.
   if (result.status != NFS3_OK) {
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: FSSTAT3 failed. %u",
               result.status);
      return nfsstat3_to_fsalstat(result.status);
   }


   infop->total_bytes = result.FSSTAT3res_u.resok.tbytes;
   infop->free_bytes = result.FSSTAT3res_u.resok.fbytes;
   infop->avail_bytes = result.FSSTAT3res_u.resok.abytes;
   infop->total_files = result.FSSTAT3res_u.resok.tfiles;
   infop->free_files = result.FSSTAT3res_u.resok.ffiles;
   infop->avail_files = result.FSSTAT3res_u.resok.afiles;
   // maxread/maxwrite are *static* not dynamic info
   infop->time_delta.tv_sec = result.FSSTAT3res_u.resok.invarsec;
   infop->time_delta.tv_nsec = 0;

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Take our FSAL Object handle and fill in an nfs_fh3 equivalent.
static fsal_status_t
proxyv3_handle_to_wire(const struct fsal_obj_handle *obj_hdl,
                       fsal_digesttype_t output_type,
                       struct gsh_buffdesc *fh_desc) {
   struct proxyv3_obj_handle *handle =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   if (fh_desc == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: received null output buffer");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   if (output_type != FSAL_DIGEST_NFSV3) {
      // The MDCACHE has an explicit FSAL_DIGEST_V4 hard coded into it
      // (mdc_get_parent_handle) that my op_ctx->nfs_vers == 4 check there
      // doesn't handle: the case of starting the export (there is no
      // "op"). Just warn about this and move on.
      LogWarn(COMPONENT_FSAL,
              "PROXY_V3: Asked for an NFSv4 rather NFSv3 handle! Proceeding.");
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: handle_to_wire %p, with len %u",
            handle->fh3.data.data_val, handle->fh3.data.data_len);
   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 value is %s", LEN_FH_STR,
                      handle->fh3.data.data_val, handle->fh3.data.data_len);

   size_t len = handle->fh3.data.data_len;
   const char* bytes = handle->fh3.data.data_val;

   // Make sure the output buffer can handle our filehandle.
   if (fh_desc->len < len) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: not given enough buffer (%zu) for fh (%zu)",
              fh_desc->len, len);
      return fsalstat(ERR_FSAL_TOOSMALL, 0);
   }

   memcpy(fh_desc->addr, bytes, len);
   fh_desc->len = len;
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Take an input NFSv3 fh3 and tell Ganesha we're okay with that.
static fsal_status_t
proxyv3_wire_to_host(struct fsal_export *exp_hdl,
                     fsal_digesttype_t in_type,
                     struct gsh_buffdesc *fh_desc,
                     int flags) {
   if (fh_desc == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Got NULL input pointers");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: wire_to_host of %p, with len %zu",
            fh_desc->addr, fh_desc->len);
   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 handle is %s", LEN_FH_STR,
                      fh_desc->addr, fh_desc->len);

   if (fh_desc->addr == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: wire_to_host received NULL address");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   if (in_type != FSAL_DIGEST_NFSV3) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: Asked to convert an NFSv4 handle");
      return fsalstat(ERR_FSAL_INVAL, 0);
   }

   // Otherwise fh_desc->addr and fh_desc->len are already the nfs_fh3 we want.
   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Given a handle (an nfs_fh3 for us), do a GETATTR to make an object.
static fsal_status_t
proxyv3_create_handle(struct fsal_export *export_handle,
                      struct gsh_buffdesc *hdl_desc,
                      struct fsal_obj_handle **handle,
                      struct attrlist *attrs_out) {
   nfs_fh3 fh3;

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Creating handle from %p with len %zu",
            hdl_desc->addr, hdl_desc->len);

   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 handle is %s", LEN_FH_STR,
                      hdl_desc->addr, hdl_desc->len);

   // In case we die along the way.
   *handle = NULL;

   fh3.data.data_val = hdl_desc->addr;
   fh3.data.data_len = hdl_desc->len;

   struct attrlist tmp_attrs;
   memset(&tmp_attrs, 0, sizeof(tmp_attrs));
   if (attrs_out != NULL) {
      FSAL_SET_MASK(tmp_attrs.request_mask, attrs_out->request_mask);
   }

   fsal_status_t rc = proxyv3_getattr_from_fh3(&fh3, &tmp_attrs);
   if (FSAL_IS_ERROR(rc)) {
      return rc;
   }

   // Bundle up the result into a new object handle.
   struct proxyv3_obj_handle *result_handle =
      proxyv3_alloc_handle(export_handle,
                           &fh3,
                           &tmp_attrs,
                           NULL /* XXX(boulos): How do we fill in parent? */,
                           attrs_out);

   // If we couldn't allocate the handle, fail.
   if (result_handle == NULL) {
      return fsalstat(ERR_FSAL_FAULT, 0);
   }

   *handle = &(result_handle->obj);

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

// Given our FSAL object, point to the fh3 data as a hash input for MDCACHE.
static void
proxyv3_handle_to_key(struct fsal_obj_handle *obj_hdl,
                      struct gsh_buffdesc *fh_desc) {
   struct proxyv3_obj_handle *handle =
      container_of(obj_hdl, struct proxyv3_obj_handle, obj);

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: handle to key for %p", handle);

   if (fh_desc == NULL) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: received null output buffer");
      return;
   }

   LogFullDebugOpaque(COMPONENT_FSAL, " fh3 handle is %s", LEN_FH_STR,
                      handle->fh3.data.data_val, handle->fh3.data.data_len);

   fh_desc->addr = handle->fh3.data.data_val;
   fh_desc->len = handle->fh3.data.data_len;
}


// Fill in various static paramters from the given root file handle.
static fsal_status_t
proxyv3_fill_fsinfo(nfs_fh3 *fh3) {
   // Now issue an FSINFO to ask the server about its max read/write sizes.
   FSINFO3args args;
   FSINFO3res result;
   FSINFO3resok *resok = &result.FSINFO3res_u.resok;
   fsal_staticfsinfo_t *fsinfo = &PROXY_V3.module.fs_info;

   memcpy(&args.fsroot, fh3, sizeof(*fh3));
   memset(&result, 0, sizeof(result));

   if (!proxyv3_nfs_call(proxyv3_sockaddr(),
                         proxyv3_socklen(),
                         proxyv3_nfsd_port(),
                         op_ctx->creds,
                         NFSPROC3_FSINFO,
                         (xdrproc_t) xdr_FSINFO3args, &args,
                         (xdrproc_t) xdr_FSINFO3res, &result)) {
      LogCrit(COMPONENT_FSAL,
              "PROXY_V3: FSINFO failed");
      return fsalstat(ERR_FSAL_SERVERFAULT, 0);
   }

   if (result.status != NFS3_OK) {
      // Okay, let's see what we got.
      LogDebug(COMPONENT_FSAL,
               "PROXY_V3: FSINFO failed, got %u", result.status);
      return nfsstat3_to_fsalstat(result.status);
   }

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: FSINFO3 returned maxread %u maxwrite %u maxfilesize %zu",
            resok->rtmax, resok->wtmax, resok->maxfilesize);

   // Lower any values we need to. NOTE(boulos): The export manager code reads
   // fsinfo->maxread/maxwrite/maxfilesize, but the *real* values are the
   // op_ctx->ctx_export->MaxRead/MaxWrite/PrefRead/PrefWrite fields (which it
   // feels gross to go writing into...).
   if (resok->rtmax != 0 && fsinfo->maxread > resok->rtmax) {
      LogWarn(COMPONENT_FSAL,
              "Changing maxread from %zu to %u",
              fsinfo->maxread, resok->rtmax);
      fsinfo->maxread = resok->rtmax;
   }

   if (resok->wtmax != 0 && fsinfo->maxwrite > resok->wtmax) {
      LogWarn(COMPONENT_FSAL,
              "Reducing maxwrite from %zu to %u",
              fsinfo->maxwrite, resok->wtmax);
      fsinfo->maxwrite = resok->wtmax;
   }

   if (resok->maxfilesize != 0 && fsinfo->maxfilesize > resok->maxfilesize) {
      LogWarn(COMPONENT_FSAL,
              "Changing maxfilesize from %zu to %zu",
              fsinfo->maxfilesize, resok->maxfilesize);
      fsinfo->maxfilesize = resok->maxfilesize;
   }

   return fsalstat(ERR_FSAL_NO_ERROR, 0);
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
   export->export.exp_ops.get_fs_dynamic_info = proxyv3_get_dynamic_info;
   export->export.exp_ops.wire_to_host = proxyv3_wire_to_host;
   export->export.exp_ops.create_handle = proxyv3_create_handle;

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

   // Setup the pointer and socklen arguments.
   sockaddr_t *sockaddr = &export->params.srv_addr;
   export->params.sockaddr = (struct sockaddr*) sockaddr;
   if (sockaddr->ss_family == AF_INET) {
      export->params.socklen = sizeof(struct sockaddr_in);
   } else {
      export->params.socklen = sizeof(struct sockaddr_in6);
   }

   // String-ify the "name" for debugging statements.
   struct display_buffer dspbuf = {
      sizeof(export->params.sockname),
      export->params.sockname,
      export->params.sockname
   };
   display_sockaddr(&dspbuf, &export->params.srv_addr);

   LogDebug(COMPONENT_FSAL,
            "Got sockaddr %s", export->params.sockname);

   u_int mountd_port = 0;
   u_int nfsd_port = 0;
   if (!proxyv3_find_ports(proxyv3_sockaddr(),
                           proxyv3_socklen(),
                           &mountd_port,
                           &nfsd_port)) {
      LogDebug(COMPONENT_FSAL,
               "Failed to find mountd/nfsd, oh well");
   }
   // Copy into our param struct.
   export->params.mountd_port = mountd_port;
   export->params.nfsd_port = nfsd_port;

   mnt3_dirpath dirpath = op_ctx->ctx_export->fullpath;
   mountres3 result;
   memset(&result, 0, sizeof(result));

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Going to try to issue a NULL MOUNT at %s",
            proxyv3_sockname());

   // Be nice and try a MOUNT NULL first.
   if (!proxyv3_mount_call(proxyv3_sockaddr(),
                           proxyv3_socklen(),
                           proxyv3_mountd_port(),
                           op_ctx->creds,
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
            dirpath, proxyv3_sockname());

   if (!proxyv3_mount_call(proxyv3_sockaddr(),
                           proxyv3_socklen(),
                           proxyv3_mountd_port(),
                           op_ctx->creds,
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

   nfs_fh3 *fh3 = (nfs_fh3*) &result.mountres3_u.mountinfo.fhandle;

   LogDebug(COMPONENT_FSAL,
            "PROXY_V3: Mount successful. Got back a %u len fhandle",
            fh3->data.data_len);

   // Copy the result for later use.
   export->root_handle_len = fh3->data.data_len;
   memcpy(export->root_handle, fh3->data.data_val, fh3->data.data_len);

   // Now fill in the fsinfo and we're done.
   return proxyv3_fill_fsinfo(fh3);
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
   PROXY_V3.handle_ops.handle_to_wire = proxyv3_handle_to_wire;
   PROXY_V3.handle_ops.handle_to_key = proxyv3_handle_to_key;
   PROXY_V3.handle_ops.release = proxyv3_handle_release;
   PROXY_V3.handle_ops.getattrs = proxyv3_getattrs;
   PROXY_V3.handle_ops.setattr2 = proxyv3_setattr2;
   PROXY_V3.handle_ops.mkdir = proxyv3_mkdir;
   PROXY_V3.handle_ops.readdir = proxyv3_readdir;
   PROXY_V3.handle_ops.symlink = proxyv3_symlink;
   PROXY_V3.handle_ops.read2 = proxyv3_read2;
   PROXY_V3.handle_ops.open2 = proxyv3_open2;
   PROXY_V3.handle_ops.close = proxyv3_close;
   PROXY_V3.handle_ops.close2 = proxyv3_close2;
   PROXY_V3.handle_ops.write2 = proxyv3_write2;
   PROXY_V3.handle_ops.commit2 = proxyv3_commit2;
   PROXY_V3.handle_ops.unlink = proxyv3_unlink;
}
