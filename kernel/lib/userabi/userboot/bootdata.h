// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/types.h>

zx_handle_t bootdata_get_bootfs(zx_handle_t log, zx_handle_t vmar_self,
                                zx_handle_t job,
                                zx_handle_t engine_vmo, zx_handle_t vdso_vmo,
                                zx_handle_t bootdata_vmo);
