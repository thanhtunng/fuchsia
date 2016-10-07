// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "escher/shape/mesh.h"

namespace escher {

Mesh::Mesh(MeshSpec spec, uint32_t num_vertices, uint32_t num_indices)
    : spec(std::move(spec)),
      num_vertices(num_vertices),
      num_indices(num_indices) {}

}  // namespace escher
