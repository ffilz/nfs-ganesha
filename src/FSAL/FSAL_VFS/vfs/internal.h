#ifndef VFS_INTERNAL
#define VFS_INTERNAL

/* Methods for pNFS
 */
nfsstat4 vfs_getdeviceinfo(struct fsal_module *fsal_hdl,
			   XDR *da_addr_body,
			   const layouttype4 type,
			   const struct pnfs_deviceid *deviceid);

size_t vfs_fs_da_addr_size(struct fsal_module *fsal_hdl);
void export_ops_pnfs(struct export_ops *ops);
void handle_ops_pnfs(struct fsal_obj_ops *ops);
void vfs_pnfs_ds_ops_init(struct fsal_pnfs_ds_ops *ops);

#endif	/* VFS_INTERNAL */
