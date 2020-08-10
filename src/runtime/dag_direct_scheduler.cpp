/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2020 Aksel Alpay
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


#include <algorithm>

#include "hipSYCL/runtime/dag_direct_scheduler.hpp"
#include "hipSYCL/runtime/error.hpp"
#include "hipSYCL/runtime/executor.hpp"
#include "hipSYCL/runtime/util.hpp"
#include "hipSYCL/runtime/dag_manager.hpp"
#include "hipSYCL/runtime/generic/multi_event.hpp"
#include "hipSYCL/runtime/serialization/serialization.hpp"
#include "hipSYCL/runtime/allocator.hpp"

namespace hipsycl {
namespace rt {

namespace {

void abort_submission(dag_node_ptr node) {
  for (auto req : node->get_requirements()) {
    if (!req->is_submitted()) {
      req->cancel();
    }
  }
  node->cancel();
}

template <class Handler>
void execute_if_buffer_requirement(dag_node_ptr node, Handler h) {
  if (node->get_operation()->is_requirement()) {
    if (cast<requirement>(node->get_operation())->is_memory_requirement()) {
      if (cast<memory_requirement>(node->get_operation())
              ->is_buffer_requirement()) {
        h(cast<buffer_memory_requirement>(node->get_operation()));
      }
    }
  }
}

void assign_devices_or_default(dag_node_ptr node, device_id default_device) {
  if (!node->get_execution_hints().has_hint<hints::bind_to_device>()) {
    node->assign_to_device(default_device);
  } else {
    node->assign_to_device(node->get_execution_hints()
                               .get_hint<hints::bind_to_device>()
                               ->get_device_id());
  }
}

// Initialize memory accesses for requirements
void initialize_memory_access(buffer_memory_requirement *bmem_req,
                              device_id target_dev) {
  assert(bmem_req);

  void *device_pointer = bmem_req->get_data_region()->get_memory(target_dev);
  bmem_req->initialize_device_data(device_pointer);
  HIPSYCL_DEBUG_INFO << "dag_scheduler: Preparing deferred pointer of "
                        "requirement node "
                     << dump(bmem_req) << std::endl;
}

result ensure_allocation_exists(buffer_memory_requirement *bmem_req,
                              device_id target_dev) {
  assert(bmem_req);
  if (!bmem_req->get_data_region()->has_allocation(target_dev)) {
    std::size_t num_bytes =
        bmem_req->get_data_region()->get_num_elements().size() *
        bmem_req->get_data_region()->get_element_size();
    
    void *ptr = application::get_backend(target_dev.get_backend())
                    .get_allocator(target_dev)
                    ->allocate(128, num_bytes);

    if(!ptr)
      return register_error(
                 __hipsycl_here(),
                 error_info{
                     "dag_direct_scheduler: Lazy memory allocation has failed.",
                     error_type::memory_allocation_error});

    bmem_req->get_data_region()->add_empty_allocation(target_dev, ptr);
  }

  return make_success();
}

void for_each_explicit_operation(
    dag_node_ptr node, std::function<void(operation *)> explicit_op_handler) {
  if (node->is_submitted())
    return;
  
  if (!node->get_operation()->is_requirement()) {
    explicit_op_handler(node->get_operation());
    return;
  } else {
    execute_if_buffer_requirement(
        node, [&](buffer_memory_requirement *bmem_req) {
          device_id target_device = node->get_assigned_device();

          std::vector<range_store::rect> outdated_regions;
          bmem_req->get_data_region()->get_outdated_regions(
              target_device, bmem_req->get_access_offset3d(),
              bmem_req->get_access_range3d(), outdated_regions);

          for (range_store::rect region : outdated_regions) {
            std::vector<std::pair<device_id, range_store::rect>> update_sources;

            bmem_req->get_data_region()->get_update_source_candidates(
                target_device, region, update_sources);

            if (update_sources.empty()) {
              register_error(
                  __hipsycl_here(),
                  error_info{"dag_direct_scheduler: Could not obtain data "
                             "update sources when trying to materialize "
                             "implicit requirement"});
              node->cancel();
              return;
            }

            // Just use first source for now:
            memory_location src{update_sources[0].first,
                                update_sources[0].second.first,
                                bmem_req->get_data_region()};
            memory_location dest{target_device, region.first,
                                 bmem_req->get_data_region()};
            memcpy_operation op{src, dest, region.second};

            explicit_op_handler(&op);
          }
        });
  }
}

backend_executor *select_executor(dag_node_ptr node, operation *op) {
  device_id dev = node->get_assigned_device();

  assert(!op->is_requirement());
  backend_id executor_backend;
  if (op->has_preferred_backend(executor_backend))
    return application::get_backend(executor_backend).get_executor(dev);
  else {
    return application::get_backend(dev.get_backend()).get_executor(dev);
  }
}

void submit(backend_executor *executor, dag_node_ptr node, operation *op) {
  std::vector<dag_node_ptr> reqs;
  node->for_each_nonvirtual_requirement([&](dag_node_ptr req) {
    reqs.push_back(req);
  });

    // Compress requirements by removing double entries and complete requirements
  reqs.erase(
      std::remove_if(reqs.begin(), reqs.end(),
                     [](dag_node_ptr elem) { return elem->is_complete(); }),
      reqs.end());
  std::sort(reqs.begin(), reqs.end());
  reqs.erase(std::unique(reqs.begin(), reqs.end()), reqs.end());
  // TODO we can even eliminate more requirements, e.g.
  // node -> A -> B
  // node -> B
  // the dependency on B can be eliminated because it is already covered by A.
  // TODO: This might be better implemented in the dag_builder
  node->assign_to_executor(executor);
  executor->submit_directly(node, op, reqs);
}

result submit_requirement(dag_node_ptr req) {
  if (!req->get_operation()->is_requirement() || req->is_submitted())
    return make_success();

  sycl::access::mode access_mode = sycl::access::mode::read_write;

  // Make sure that all required allocations exist
  // (they must exist when we try initialize device pointers!)
  bool allocation_failed = false;
  result res = make_success();
  execute_if_buffer_requirement(req, [&](buffer_memory_requirement *bmem_req) {
    res = ensure_allocation_exists(bmem_req, req->get_assigned_device());
    access_mode = bmem_req->get_access_mode();
  });
  if (!res.is_success())
    return res;
  
  // Then initialize memory accesses
  execute_if_buffer_requirement(
    req, [&](buffer_memory_requirement *bmem_req) {
      initialize_memory_access(bmem_req, req->get_assigned_device());
  });

  // If access is discard, don't create memcopies
  if (access_mode != sycl::access::mode::discard_write &&
      access_mode != sycl::access::mode::discard_read_write) {
    for_each_explicit_operation(req, [&](operation *op) {
      if (!op->is_data_transfer()) {
        res = make_error(
            __hipsycl_here(),
            error_info{
                "dag_direct_scheduler: only data transfers are supported "
                "as operations generated from implicit requirements.",
                error_type::feature_not_supported});
      } else {
        backend_executor *executor = select_executor(req, op);
        // TODO What if we need to copy between two device backends through
        // host?
        submit(executor, req, op);
      }
    });
  }
  if (!res.is_success())
    return res;

  // If the requirement did not result in any operations...
  if (!req->get_event()) {
    // create dummy event
    req->mark_virtually_submitted();
  }
  else {
    execute_if_buffer_requirement(
        req, [&](buffer_memory_requirement *bmem_req) {
          if (access_mode == sycl::access::mode::read) {
            bmem_req->get_data_region()->mark_range_valid(
                req->get_assigned_device(), bmem_req->get_access_offset3d(),
                bmem_req->get_access_range3d());
          } else {
            bmem_req->get_data_region()->mark_range_current(
                req->get_assigned_device(), bmem_req->get_access_offset3d(),
                bmem_req->get_access_range3d());
          }
        });
  }
  
  return make_success();
}
}

void dag_direct_scheduler::submit(dag_node_ptr node) {

  if (!node->get_execution_hints().has_hint<hints::bind_to_device>()) {
    register_error(__hipsycl_here(),
                   error_info{"dag_direct_scheduler: Direct scheduler does not "
                              "support DAG nodes not bound to devices.",
                              error_type::feature_not_supported});
    abort_submission(node);
    return;
  }

  device_id target_device = node->get_execution_hints()
                                .get_hint<hints::bind_to_device>()
                                ->get_device_id();
  node->assign_to_device(target_device);
  for (auto req : node->get_requirements())
    assign_devices_or_default(req, target_device);

  for (auto req : node->get_requirements()) {
    if (!req->get_operation()->is_requirement()) {
      if (!req->is_submitted()) {
        register_error(__hipsycl_here(),
                   error_info{"dag_direct_scheduler: Direct scheduler does not "
                              "support processing multiple unsubmitted nodes",
                              error_type::feature_not_supported});
        abort_submission(node);
        return;
      }
    } else {
      result res = submit_requirement(req);

      if (!res.is_success()) {
        register_error(res);
        abort_submission(node);
        return;
      }
    }
  }


  if (node->get_operation()->is_requirement()) {
    result res = submit_requirement(node);
    
    if (!res.is_success()) {
      register_error(res);
      abort_submission(node);
      return;
    }
  } else {
    // TODO What if this is an explicit copy between two device backends through
    // host?
    backend_executor *exec = select_executor(node, node->get_operation());
    rt::submit(exec, node, node->get_operation());
  }
  // Register node as submitted with the runtime
  // (only relevant for queue::wait() operations)
  application::dag().register_submitted_ops(node);
}

}
}