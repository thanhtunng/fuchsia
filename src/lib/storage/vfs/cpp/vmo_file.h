// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_VFS_CPP_VMO_FILE_H_
#define SRC_LIB_STORAGE_VFS_CPP_VMO_FILE_H_

#include <lib/zx/vmo.h>

#include <mutex>

#include "vnode.h"

namespace fs {

// A file node backed by a range of bytes in a VMO.
//
// The file has a fixed size specified at creating time; it does not grow or shrink even when
// written into.
//
// This class is thread-safe.
class VmoFile : public Vnode {
 public:
  // Construct with fbl::MakeRefCounted.

  // Specifies the desired behavior when a client asks for the file's
  // underlying VMO.
  enum class VmoSharing {
    // The VMO is not shared with the client.
    NONE,

    // The VMO handle is duplicated for each client.
    //
    // This is appropriate when it is okay for clients to access the entire contents of the VMO,
    // possibly extending beyond the pages spanned by the file.
    //
    // This mode is significantly more efficient than |CLONE| and |CLONE_COW| and should be
    // preferred when file spans the whole VMO or when the VMO's entire content is safe for clients
    // to read.
    DUPLICATE,

    // The VMO range spanned by the file is cloned on demand, using copy-on-write semantics to
    // isolate modifications of clients which open the file in a writable mode.
    //
    // This is appropriate when clients need to be restricted from accessing portions of the VMO
    // outside of the range of the file and when file modifications by clients should not be visible
    // to each other.
    CLONE_COW,
  };

  // The underlying VMO handle.
  zx_handle_t vmo_handle() const { return vmo_handle_; }

  // The offset of the start of the file within the VMO in bytes.
  size_t offset() const { return offset_; }

  // The length of the file in bytes.
  size_t length() const { return length_; }

  // True if the file is writable.
  // If false, attempts to open the file for write will fail.
  bool is_writable() const { return writable_; }

  // The VMO sharing mode of the file.
  VmoSharing vmo_sharing() const { return vmo_sharing_; }

  // |Vnode| implementation:
  VnodeProtocolSet GetProtocols() const final;
  bool ValidateRights(Rights rights) const final;
  zx_status_t GetAttributes(VnodeAttributes* a) final;
  zx_status_t Read(void* data, size_t length, size_t offset, size_t* out_actual) final;
  zx_status_t Write(const void* data, size_t length, size_t offset, size_t* out_actual) final;
  zx_status_t GetNodeInfoForProtocol(VnodeProtocol protocol, Rights rights,
                                     VnodeRepresentation* info) final;

 protected:
  friend fbl::internal::MakeRefCountedHelper<VmoFile>;
  friend fbl::RefPtr<VmoFile>;

  // Creates a file node backed an VMO owned by the creator. The creator retains ownership of
  // |unowned_vmo| which must outlive this object.
  VmoFile(const zx::vmo& unowned_vmo, size_t offset, size_t length, bool writable = false,
          VmoSharing vmo_sharing = VmoSharing::DUPLICATE);

  ~VmoFile() override;

 private:
  zx_status_t AcquireVmo(zx_rights_t rights, zx::vmo* out_vmo, size_t* out_offset);
  zx_status_t DuplicateVmo(zx_rights_t rights, zx::vmo* out_vmo, size_t* out_offset);
  zx_status_t CloneVmo(zx_rights_t rights, zx::vmo* out_vmo, size_t* out_offset);

  zx_handle_t const vmo_handle_;
  size_t const offset_;
  size_t const length_;
  bool const writable_;
  VmoSharing const vmo_sharing_;

  // Clone of the portion of the VMO which contains the file's data. In |CLONE_COW| mode, this is
  // shared among read-only clients.
  struct {
    std::once_flag once;
    zx::vmo vmo;
  } shared_clone_;

  DISALLOW_COPY_ASSIGN_AND_MOVE(VmoFile);
};

}  // namespace fs

#endif  // SRC_LIB_STORAGE_VFS_CPP_VMO_FILE_H_
