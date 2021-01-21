/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2021 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <level_zero/ze_api.h>

#include "hipSYCL/runtime/ze/ze_backend.hpp"
#include "hipSYCL/runtime/ze/ze_hardware_manager.hpp"
#include "hipSYCL/runtime/device_id.hpp"


namespace hipsycl {
namespace rt {

ze_backend::ze_backend() {
  ze_result_t err = zeInit(0);

  if (err != ZE_RESULT_SUCCESS) {
    print_error(__hipsycl_here(),
                error_info{"ze_backend: Call to zeInit() failed",
                            error_code{"ze", static_cast<int>(err)}});
    return;
  }

  _hardware_manager = std::make_unique<ze_hardware_manager>();
}

api_platform ze_backend::get_api_platform() const {
  return api_platform::level_zero;
}

hardware_platform ze_backend::get_hardware_platform() const {
  return hardware_platform::level_zero;
}

backend_id ze_backend::get_unique_backend_id() const {
  return backend_id::level_zero;
}
  
backend_hardware_manager* ze_backend::get_hardware_manager() const {
  return _hardware_manager.get();
}

backend_executor* ze_backend::get_executor(device_id dev) const {
  return nullptr;
}

backend_allocator *ze_backend::get_allocator(device_id dev) const {
  return nullptr;
}

std::string ze_backend::get_name() const {
  return "Level Zero";
}

}
}