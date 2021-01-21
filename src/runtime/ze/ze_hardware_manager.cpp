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

#include <cassert>
#include <cstdint>
#include <string>

#include "hipSYCL/runtime/ze/ze_hardware_manager.hpp"
#include "hipSYCL/runtime/device_id.hpp"
#include "hipSYCL/runtime/error.hpp"

namespace hipsycl {
namespace rt {


ze_context_manager::ze_context_manager(ze_driver_handle_t driver)
 : _driver{driver} {

  ze_context_desc_t desc;
  desc.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
  desc.pNext = nullptr;
  desc.flags = 0;
  ze_result_t err = zeContextCreate(driver, &desc, &_handle);

  if(err != ZE_RESULT_SUCCESS) {
    register_error(__hipsycl_here(),
                   error_info{"ze_context_manager: Could not create context",
                              error_code{"ze", static_cast<int>(err)}});
  }
}

ze_context_handle_t ze_context_manager::get() const {
  return _handle;
}

ze_driver_handle_t ze_context_manager::get_driver() const {
  return _driver;
}

ze_context_manager::~ze_context_manager() {
  ze_result_t err = zeContextDestroy(_handle);

  if(err != ZE_RESULT_SUCCESS) {
    register_error(__hipsycl_here(),
                   error_info{"ze_context_manager: Could not destroy context",
                              error_code{"ze", static_cast<int>(err)}});
  }
}

ze_hardware_context::ze_hardware_context(ze_driver_handle_t driver,
                                         ze_device_handle_t device,
                                         ze_context_handle_t ctx)
    : _driver{driver}, _device{device}, _ctx{ctx} {

  ze_result_t err = zeDeviceGetProperties(_device, &_props);

  if(err != ZE_RESULT_SUCCESS) {
    print_error(__hipsycl_here(),
                  error_info{"ze_hardware_context: Could not query device properties",
                             error_code{"ze", static_cast<int>(err)}});
  }

  err = zeDeviceGetComputeProperties(_device, &_compute_props);

  if(err != ZE_RESULT_SUCCESS) {
    print_error(__hipsycl_here(),
                  error_info{"ze_hardware_context: Could not query device compute properties",
                             error_code{"ze", static_cast<int>(err)}});
  }

  uint32_t num_memory_properties = 0;
  err = zeDeviceGetMemoryProperties(_device, &num_memory_properties, nullptr);

  if(err != ZE_RESULT_SUCCESS) {
    print_error(__hipsycl_here(),
                  error_info{"ze_hardware_context: Could not query number of memory properties",
                             error_code{"ze", static_cast<int>(err)}});
  }
  if(num_memory_properties > 0) {
    _memory_props.resize(num_memory_properties);

    err = zeDeviceGetMemoryProperties(_device, &num_memory_properties, _memory_props.data());

    if(err != ZE_RESULT_SUCCESS) {
      print_error(__hipsycl_here(),
                  error_info{"ze_hardware_context: Could not query memory properties",
                             error_code{"ze", static_cast<int>(err)}});
    }
  }
}

bool ze_hardware_context::is_cpu() const {
  return _props.type == ZE_DEVICE_TYPE_CPU;
}

bool ze_hardware_context::is_gpu() const {
  return _props.type == ZE_DEVICE_TYPE_GPU;
}

std::size_t ze_hardware_context::get_max_kernel_concurrency() const {
  // TODO
  return 1;
}
  
std::size_t ze_hardware_context::get_max_memcpy_concurrency() const {
  // TODO
  return 1;
}

std::string ze_hardware_context::get_device_name() const {
  return std::string{_props.name};
}

std::string ze_hardware_context::get_vendor_name() const {
  return std::string{"pci:"}+std::to_string(_props.vendorId);
}

bool ze_hardware_context::has(device_support_aspect aspect) const {
  switch (aspect) {
  case device_support_aspect::emulated_local_memory:
    return false;
    break;
  case device_support_aspect::host_unified_memory:
    return _props.flags & ZE_DEVICE_PROPERTY_FLAG_INTEGRATED;
    break;
  case device_support_aspect::error_correction:
    return _props.flags & ZE_DEVICE_PROPERTY_FLAG_ECC;
    break;
  case device_support_aspect::global_mem_cache:
    return true;
    break;
  case device_support_aspect::global_mem_cache_read_only:
    return false;
    break;
  case device_support_aspect::global_mem_cache_write_only:
    return false;
    break;
  case device_support_aspect::images:
    return false;
    break;
  case device_support_aspect::little_endian:
    return true;
    break;
  }
  assert(false && "Unknown device aspect");
  std::terminate();
}

std::size_t ze_hardware_context::get_property(device_uint_property prop) const {
  switch (prop) {
  case device_uint_property::max_compute_units:
    return _props.numSlices * _props.numSubslicesPerSlice;
    break;
  case device_uint_property::max_global_size0:
    return _compute_props.maxGroupSizeX * _compute_props.maxGroupCountX;
    break;
  case device_uint_property::max_global_size1:
    return _compute_props.maxGroupSizeY * _compute_props.maxGroupCountY;
    break;
  case device_uint_property::max_global_size2:
    return _compute_props.maxGroupSizeZ * _compute_props.maxGroupCountZ;
    break;
  case device_uint_property::max_group_size:
    return _compute_props.maxTotalGroupSize;
    break;
  case device_uint_property::preferred_vector_width_char:
    return 4;
    break;
  case device_uint_property::preferred_vector_width_double:
    return 1;
    break;
  case device_uint_property::preferred_vector_width_float:
    return 1;
    break;
  case device_uint_property::preferred_vector_width_half:
    return 2;
    break;
  case device_uint_property::preferred_vector_width_int:
    return 1;
    break;
  case device_uint_property::preferred_vector_width_long:
    return 1;
    break;
  case device_uint_property::preferred_vector_width_short:
    return 2;
    break;
  case device_uint_property::native_vector_width_char:
    return 4;
    break;
  case device_uint_property::native_vector_width_double:
    return 1;
    break;
  case device_uint_property::native_vector_width_float:
    return 1;
    break;
  case device_uint_property::native_vector_width_half:
    return 2;
    break;
  case device_uint_property::native_vector_width_int:
    return 1;
    break;
  case device_uint_property::native_vector_width_long:
    return 1;
    break;
  case device_uint_property::native_vector_width_short:
    return 2;
    break;
  case device_uint_property::max_clock_speed:
    return _props.coreClockRate / 1000;
    break;
  case device_uint_property::max_malloc_size:
    return _props.maxMemAllocSize;
    break;
  case device_uint_property::address_bits:
    return 64;
    break;
  case device_uint_property::max_read_image_args:
    return 0;
    break;
  case device_uint_property::max_write_image_args:
    return 0;
    break;
  case device_uint_property::image2d_max_width:
    return 0;
    break;
  case device_uint_property::image2d_max_height:
    return 0;
    break;
  case device_uint_property::image3d_max_width:
    return 0;
    break;
  case device_uint_property::image3d_max_height:
    return 0;
    break;
  case device_uint_property::image3d_max_depth:
    return 0;
    break;
  case device_uint_property::image_max_buffer_size:
    return 0;
    break;
  case device_uint_property::image_max_array_size:
    return 0;
    break;
  case device_uint_property::max_samplers:
    return 0;
    break;
  case device_uint_property::max_parameter_size:
    return std::numeric_limits<std::size_t>::max();
    break;
  case device_uint_property::mem_base_addr_align:
    return 8; // TODO
    break;
  case device_uint_property::global_mem_cache_line_size:
    return 128; //TODO
    break;
  case device_uint_property::global_mem_cache_size:
    return 128; // TODO
    break;
  case device_uint_property::global_mem_size:
    return _props.maxMemAllocSize; // TODO Is this correct?
    break;
  case device_uint_property::max_constant_buffer_size:
    return 0; // TODO
    break;
  case device_uint_property::max_constant_args:
    return std::numeric_limits<std::size_t>::max();
    break;
  case device_uint_property::local_mem_size:
    return _compute_props.maxSharedLocalMemory;
    break;
  case device_uint_property::printf_buffer_size:
    return std::numeric_limits<std::size_t>::max();
    break;
  case device_uint_property::partition_max_sub_devices:
    return 0;
    break;
  }
  assert(false && "Invalid device property");
  std::terminate();
}
  
std::string ze_hardware_context::get_driver_version() const {
  ze_driver_properties_t props;
  ze_result_t err = zeDriverGetProperties(_driver, &props);

  if(err != ZE_RESULT_SUCCESS) {
    register_error(__hipsycl_here(),
                  error_info{"ze_hardware_context: Could not query driver properties",
                             error_code{"ze", static_cast<int>(err)}});
    return "<unknown>";
  } else {
    return std::to_string(props.driverVersion);
  }
}

std::string ze_hardware_context::get_profile() const {
  return "FULL_PROFILE";
}

ze_hardware_context::~ze_hardware_context(){}

uint32_t ze_hardware_context::get_ze_global_memory_ordinal() const {
  uint32_t result = 0;

  std::size_t max_found_mem = 0;
  for(std::size_t i = 0; i < _memory_props.size(); ++i) {
    if(_memory_props[i].totalSize > max_found_mem) {
      max_found_mem = _memory_props[i].totalSize;
      result = i;
    }
  }

  return result;
}

ze_hardware_manager::ze_hardware_manager() {

  uint32_t num_drivers = 0;
  ze_result_t err = zeDriverGet(&num_drivers, nullptr);

  if (err != ZE_RESULT_SUCCESS) {
    print_warning(__hipsycl_here(),
                  error_info{"ze_hardware_manager: Could not get number of drivers, "
                             "assuming no drivers available.",
                             error_code{"ze", static_cast<int>(err)}});
  }
  
  if(num_drivers > 0) {
    std::vector<ze_driver_handle_t> drivers(num_drivers);
    err = zeDriverGet(&num_drivers, drivers.data());

    if (err != ZE_RESULT_SUCCESS) {
      print_error(__hipsycl_here(),
                    error_info{"ze_hardware_manager: Could not obtain driver handles",
                              error_code{"ze", static_cast<int>(err)}});
      num_drivers = 0;
    }

    for(int i = 0; i < num_drivers; ++i) {
      
      _drivers.push_back(drivers[i]);
      _contexts.push_back(ze_context_manager{_drivers[i]});

      uint32_t num_devices = 0;
      err = zeDeviceGet(drivers[i], &num_devices, nullptr);

      if (err != ZE_RESULT_SUCCESS) {
        print_error(__hipsycl_here(),
                    error_info{"ze_hardware_manager: Could not obtain number of devices",
                              error_code{"ze", static_cast<int>(err)}});
        num_devices = 0;
      }

      if(num_devices > 0) {
        std::vector<ze_device_handle_t> devices(num_devices);
        
        err = zeDeviceGet(drivers[i], &num_devices, devices.data());

        for(int dev = 0; dev < num_devices; ++dev) {
          _devices.push_back(ze_hardware_context{drivers[i], devices[dev],
                                                 _contexts.back().get()});
        }
      }
    }
  }
}


std::size_t ze_hardware_manager::get_num_devices() const {
  return _devices.size();
}

hardware_context *ze_hardware_manager::get_device(std::size_t index) {
  assert(index < _devices.size());
  return &(_devices[index]);
}

device_id ze_hardware_manager::get_device_id(std::size_t index) const {
  return device_id{backend_descriptor{
    hardware_platform::level_zero, api_platform::level_zero}, 
    static_cast<int>(index)
  };
}

ze_context_handle_t ze_hardware_manager::get_ze_context(std::size_t device_index) const {
  assert(device_index < _devices.size());

  return _devices[device_index].get_ze_context();
}

result ze_hardware_manager::device_handle_to_device_id(ze_device_handle_t d, device_id &out) const {

  for(std::size_t i = 0; i < _devices.size(); ++i) {
    if(_devices[i].get_ze_device() == d) {
      out = get_device_id(i);
      return make_success();
    }
  }
  return make_error(__hipsycl_here(),
                    error_info{"ze_hardware_manager: Could not convert "
                               "ze_device_handle_t to hipSYCL device id"});
}

}
}