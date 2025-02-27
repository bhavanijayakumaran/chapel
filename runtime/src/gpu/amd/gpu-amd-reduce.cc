/*
 * Copyright 2020-2023 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.  *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAS_GPU_LOCALE


#include <hip/hip_common.h>
#include "../common/rocm-version.h"

#if ROCM_VERSION_MAJOR >= 5
// if we include this all the time, we get unused function errors
#include "../common/rocm-utils.h"
#include <hipcub/hipcub.hpp>
#endif


#include "chpl-gpu.h"
#include "chpl-gpu-impl.h"
#include "gpu/chpl-gpu-reduce-util.h"

#if ROCM_VERSION_MAJOR >= 5
#define DEF_ONE_REDUCE_RET_VAL(impl_kind, chpl_kind, data_type) \
void chpl_gpu_impl_##chpl_kind##_reduce_##data_type(data_type* data, int n,\
                                                    data_type* val, int* idx,\
                                                    void* stream) {\
  data_type* result; \
  ROCM_CALL(hipMalloc(&result, sizeof(data_type)));\
  void* temp = NULL; \
  size_t temp_bytes = 0; \
  ROCM_CALL(hipcub::DeviceReduce::impl_kind(temp, temp_bytes, data, \
                                            (data_type*)result, n));\
  ROCM_CALL(hipMalloc(((hipDeviceptr_t*)&temp), temp_bytes)); \
  ROCM_CALL(hipcub::DeviceReduce::impl_kind(temp, temp_bytes, data, \
                                            (data_type*)result, n));\
  ROCM_CALL(hipMemcpyDtoHAsync(val, result, sizeof(data_type),\
                              (hipStream_t)stream)); \
  ROCM_CALL(hipFree(result)); \
}
#else
#define DEF_ONE_REDUCE_RET_VAL(impl_kind, chpl_kind, data_type) \
void chpl_gpu_impl_##chpl_kind##_reduce_##data_type(data_type* data, int n,\
                                                    data_type* val, int* idx,\
                                                    void* stream) {\
  chpl_internal_error("Reduction via runtime calls is not supported with AMD GPUs using ROCm version <5\n");\
}
#endif // 1

GPU_IMPL_REDUCE(DEF_ONE_REDUCE_RET_VAL, Sum, sum)
GPU_IMPL_REDUCE(DEF_ONE_REDUCE_RET_VAL, Min, min)
GPU_IMPL_REDUCE(DEF_ONE_REDUCE_RET_VAL, Max, max)

#undef DEF_ONE_REDUCE_RET_VAL

#if ROCM_VERSION_MAJOR >= 5
#define DEF_ONE_REDUCE_RET_VAL_IDX(impl_kind, chpl_kind, data_type) \
void chpl_gpu_impl_##chpl_kind##_reduce_##data_type(data_type* data, int n,\
                                                    data_type* val, int* idx,\
                                                    void* stream) {\
  using kvp = hipcub::KeyValuePair<int,data_type>; \
  kvp* result; \
  ROCM_CALL(hipMalloc(&result, sizeof(kvp))); \
  void* temp = NULL; \
  size_t temp_bytes = 0; \
  hipcub::DeviceReduce::impl_kind(temp, temp_bytes, data, (kvp*)result, n,\
                                  (hipStream_t)stream);\
  ROCM_CALL(hipMalloc(&temp, temp_bytes)); \
  hipcub::DeviceReduce::impl_kind(temp, temp_bytes, data, (kvp*)result, n,\
                                  (hipStream_t)stream);\
  kvp result_host; \
  ROCM_CALL(hipMemcpyDtoHAsync(&result_host, result, sizeof(kvp),\
                               (hipStream_t)stream)); \
  *val = result_host.value; \
  *idx = result_host.key; \
  ROCM_CALL(hipFree(result)); \
}
#else
#define DEF_ONE_REDUCE_RET_VAL_IDX(impl_kind, chpl_kind, data_type) \
void chpl_gpu_impl_##chpl_kind##_reduce_##data_type(data_type* data, int n,\
                                                    data_type* val, int* idx,\
                                                    void* stream) {\
  chpl_internal_error("Reduction via runtime calls is not supported with AMD GPUs using ROCm version <5\n");\
}
#endif // 1

GPU_IMPL_REDUCE(DEF_ONE_REDUCE_RET_VAL_IDX, ArgMin, minloc)
GPU_IMPL_REDUCE(DEF_ONE_REDUCE_RET_VAL_IDX, ArgMax, maxloc)

#undef DEF_ONE_REDUCE_RET_VAL_IDX

#endif // HAS_GPU_LOCALE

