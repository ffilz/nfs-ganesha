// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Copyright (C) 2018 Red Hat, Inc.
 * Contributor : Frank Filz <ffilzlnx@mindspring.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

#include "gtest.hh"

extern "C" {
/* Ganesha headers */
#include "nfs_lib.h"
#include "nfs_file_handle.h"
#include "nfs_proto_functions.h"
}

#ifndef GTEST_GTEST_NFS4_HH
#define GTEST_GTEST_NFS4_HH

namespace gtest {

  class GaeshaNFS4BaseTest : public gtest::GaeshaFSALBaseTest {
  protected:

    virtual void SetUp() {
      bool fhres;

      gtest::GaeshaFSALBaseTest::SetUp();

      memset(&data, 0, sizeof(struct compound_data));
      memset(&arg, 0, sizeof(nfs_arg_t));
      memset(&resp, 0, sizeof(struct nfs_resop4));

      ops = (struct nfs_argop4 *) gsh_calloc(1, sizeof(struct nfs_argop4));
      arg.arg_compound4.argarray.argarray_len = 1;
      arg.arg_compound4.argarray.argarray_val = ops;

      /* Setup some basic stuff (that will be overrode) so TearDown works. */
      data.minorversion = 0;
      ops[0].argop = NFS4_OP_PUTROOTFH;

      /* Convert root_obj to a file handle in the args */
      fhres = nfs4_FSALToFhandle(true, &data.currentFH, test_root,
                                 op_ctx->ctx_export);
      EXPECT_EQ(fhres, true);
    }

    virtual void TearDown() {
      bool rc;

      set_current_entry(&data, nullptr);

      nfs4_Compound_FreeOne(&resp);

      /* Free the compound data and response */
      compound_data_Free(&data);

      /* Free the args structure. */
      rc = xdr_free((xdrproc_t) xdr_COMPOUND4args, &arg);
      EXPECT_EQ(rc, true);

      gtest::GaeshaFSALBaseTest::TearDown();
    }

    void setup_lookup(int pos, const char *name) {
      gsh_free(ops[pos].nfs_argop4_u.oplookup.objname.utf8string_val);
      ops[pos].argop = NFS4_OP_LOOKUP;
      ops[pos].nfs_argop4_u.oplookup.objname.utf8string_len = strlen(name);
      ops[pos].nfs_argop4_u.oplookup.objname.utf8string_val = gsh_strdup(name);
    }

    void cleanup_lookup(int pos, const char *name) {
      gsh_free(ops[pos].nfs_argop4_u.oplookup.objname.utf8string_val);
      ops[pos].nfs_argop4_u.oplookup.objname.utf8string_len = 0;
      ops[pos].nfs_argop4_u.oplookup.objname.utf8string_val = nullptr;
    }

    struct compound_data data;
    struct nfs_argop4 *ops;
    nfs_arg_t arg;
    struct nfs_resop4 resp;
  };
} // namespase gtest

#endif /* GTEST_GTEST_NFS4_HH */
