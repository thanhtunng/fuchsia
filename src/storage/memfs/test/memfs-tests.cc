// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/memfs/cpp/vnode.h>
#include <sys/stat.h>

#include <zxtest/zxtest.h>

namespace memfs {
namespace {

TEST(MemfsTest, DirectoryLifetime) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  std::unique_ptr<Vfs> vfs;
  fbl::RefPtr<VnodeDir> root;
  ASSERT_OK(Vfs::Create(loop.dispatcher(), "<tmp>", &vfs, &root));
}

TEST(MemfsTest, CreateFile) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  std::unique_ptr<Vfs> vfs;
  fbl::RefPtr<VnodeDir> root;
  ASSERT_OK(Vfs::Create(loop.dispatcher(), "<tmp>", &vfs, &root));
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_OK(root->Create("foobar", S_IFREG, &file));
  auto directory = static_cast<fbl::RefPtr<fs::Vnode>>(root);
  fs::VnodeAttributes directory_attr, file_attr;
  ASSERT_OK(directory->GetAttributes(&directory_attr));
  ASSERT_OK(file->GetAttributes(&file_attr));

  // Directory created before file.
  ASSERT_LE(directory_attr.creation_time, file_attr.creation_time);

  // Observe that the modify time of the directory is larger than the file.
  // This implies "the file is created, then the directory is updated".
  ASSERT_GE(directory_attr.modification_time, file_attr.modification_time);
}

TEST(MemfsTest, SubdirectoryUpdateTime) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  std::unique_ptr<Vfs> vfs;
  fbl::RefPtr<VnodeDir> root;
  ASSERT_OK(Vfs::Create(loop.dispatcher(), "<tmp>", &vfs, &root));
  fbl::RefPtr<fs::Vnode> index;
  ASSERT_OK(root->Create("index", S_IFREG, &index));
  fbl::RefPtr<fs::Vnode> subdirectory;
  ASSERT_OK(root->Create("subdirectory", S_IFDIR, &subdirectory));

  // Write a file at "subdirectory/file".
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_OK(subdirectory->Create("file", S_IFREG, &file));
  file->DidModifyStream();

  // Overwrite a file at "index".
  index->DidModifyStream();

  fs::VnodeAttributes subdirectory_attr, index_attr;
  ASSERT_OK(subdirectory->GetAttributes(&subdirectory_attr));
  ASSERT_OK(index->GetAttributes(&index_attr));

  // "index" was written after "subdirectory".
  ASSERT_LE(subdirectory_attr.modification_time, index_attr.modification_time);
}

TEST(MemfsTest, SubPageContentSize) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  std::unique_ptr<Vfs> vfs;
  fbl::RefPtr<VnodeDir> root;
  ASSERT_OK(Vfs::Create(loop.dispatcher(), "<tmp>", &vfs, &root));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));

  // Set the content size to a non page aligned value.
  uint64_t content_size = zx_system_get_page_size() / 2;
  EXPECT_OK(vmo.set_property(ZX_PROP_VMO_CONTENT_SIZE, &content_size, sizeof(content_size)));

  // Duplicate the handle to create the VMO file so we can use the original handle for testing.
  zx::vmo vmo_dup;
  ASSERT_OK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dup));
  // Create a VMO file sized to its content.
  EXPECT_OK(root->CreateFromVmo("vmo", vmo_dup.release(), 0, content_size));

  // Lookup the VMO and request its representation.
  fbl::RefPtr<fs::Vnode> vmo_vnode;
  ASSERT_OK(root->Lookup("vmo", &vmo_vnode));
  fs::VnodeRepresentation vnode_info;
  ASSERT_OK(vmo_vnode->GetNodeInfo(fs::Rights::ReadOnly(), &vnode_info));
  EXPECT_TRUE(vnode_info.is_memory());

  // We expect no cloning to have happened, this means we should have a handle to our original VMO.
  // We can verify this by comparing koids.
  zx_info_handle_basic_t original_vmo_info;
  EXPECT_OK(vmo.get_info(ZX_INFO_HANDLE_BASIC, &original_vmo_info, sizeof(original_vmo_info),
                         nullptr, nullptr));
  zx_info_handle_basic_t vnode_vmo_info;
  EXPECT_OK(vnode_info.memory().vmo.get_info(ZX_INFO_HANDLE_BASIC, &vnode_vmo_info,
                                             sizeof(vnode_vmo_info), nullptr, nullptr));
  EXPECT_EQ(original_vmo_info.koid, vnode_vmo_info.koid);
}

}  // namespace
}  // namespace memfs
