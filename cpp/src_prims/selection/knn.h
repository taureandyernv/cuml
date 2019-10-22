/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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

#pragma once

#include "cuda_utils.h"

#include "distance/distance.h"

#include <faiss/Heap.h>
#include <faiss/gpu/GpuDistance.h>
#include <faiss/gpu/GpuIndexFlat.h>
#include <faiss/gpu/GpuResources.h>
#include <faiss/gpu/StandardGpuResources.h>

#include <iostream>

namespace MLCommon {
namespace Selection {

/** Merge results from several shards into a single result set.
   * @param n number of elements in search array
   * @param k number of neighbors returned
   * @param distances output distance array
   * @param labels output index array
   * @param all_distances  row-wise stacked array of intermediary knn output distances size nshard * n * k
   * @param all_labels     row-wise stacked array of intermediary knn output indices size nshard * n * k
   * @param translations  label translations to apply, size nshard
   */
template <class C>
void merge_tables(int64_t n, int64_t k, int64_t nshard, float *distances,
                  int64_t *labels, float *all_distances, int64_t *all_labels,
                  int64_t *translations) {
  if (k == 0) {
    return;
  }

  size_t stride = n * k;
#pragma omp parallel
  {
    std::vector<int> buf(2 * nshard);
    int *pointer = buf.data();
    int *shard_ids = pointer + nshard;
    std::vector<float> buf2(nshard);
    float *heap_vals = buf2.data();
#pragma omp for
    for (int64_t i = 0; i < n; i++) {
      // the heap maps values to the shard where they are
      // produced.
      const float *D_in = all_distances + i * k;
      const int64_t *I_in = all_labels + i * k;
      int heap_size = 0;

      for (int64_t s = 0; s < nshard; s++) {
        pointer[s] = 0;
        if (I_in[stride * s] >= 0)
          faiss::heap_push<C>(++heap_size, heap_vals, shard_ids,
                              D_in[stride * s], s);
      }

      float *D = distances + i * k;
      int64_t *I = labels + i * k;

      for (int j = 0; j < k; j++) {
        if (heap_size == 0) {
          I[j] = -1;
          D[j] = C::neutral();
        } else {
          // pop best element
          int s = shard_ids[0];
          int &p = pointer[s];
          D[j] = heap_vals[0];
          I[j] = I_in[stride * s + p] + translations[s];

          faiss::heap_pop<C>(heap_size--, heap_vals, shard_ids);
          p++;
          if (p < k && I_in[stride * s + p] >= 0)
            faiss::heap_push<C>(++heap_size, heap_vals, shard_ids,
                                D_in[stride * s + p], s);
        }
      }
    }
  }
};

/**
   * Search the kNN for the k-nearest neighbors of a set of query vectors
   * @param input device memory to search as an array of device pointers
   * @param sizes array of memory sizes
   * @param n_params size of input and sizes arrays
   * @param D number of cols in input and search_items
   * @param search_items set of vectors to query for neighbors
   * @param n        number of items in search_items
   * @param res_I      pointer to device memory for returning k nearest indices
   * @param res_D      pointer to device memory for returning k nearest distances
   * @param k        number of neighbors to query
   * @param s the cuda stream to use
   * @param translations translation ids for indices when index rows represent
   *        non-contiguous partitions
   */
template <typename IntType = int,
          Distance::DistanceType DistanceType = Distance::EucUnexpandedL2>
void brute_force_knn(float **input, int *sizes, int n_params, IntType D,
                     float *search_items, IntType n, int64_t *res_I,
                     float *res_D, IntType k, cudaStream_t s,
                     std::vector<int64_t> *translations = nullptr) {
  // TODO: Also pass internal streams down from handle.

  ASSERT(DistanceType == Distance::EucUnexpandedL2 ||
           DistanceType == Distance::EucUnexpandedL2Sqrt,
         "Only EucUnexpandedL2Sqrt and EucUnexpandedL2 metrics are supported "
         "currently.");

  std::vector<int64_t> *id_ranges = translations;
  if (translations == nullptr) {
    std::cout << "Translations was NULL!" << std::endl;

    id_ranges = new std::vector<int64_t>();
    int64_t total_n = 0;
    for (int i = 0; i < n_params; i++) {
      if (i < n_params)  // if i < sizes[i]
        id_ranges->push_back(total_n);
      total_n += sizes[i];
    }
  } else {

    std::cout << "Using translations: [" << std::endl;
    for (int i = 0; i < translations->size(); i++) {
      std::cout << translations[i] << ", ";
    }

    std::cout << "]" << std::endl;
  }

  float *result_D = new float[k * n];
  long *result_I = new int64_t[k * n];

  float *all_D = new float[n_params * k * n];
  long *all_I = new int64_t[n_params * k * n];

  ASSERT_DEVICE_MEM(search_items, "search items");
  ASSERT_DEVICE_MEM(res_I, "output index array");
  ASSERT_DEVICE_MEM(res_D, "output distance array");

  CUDA_CHECK(cudaStreamSynchronize(s));

#pragma omp parallel
  {
#pragma omp for
    for (int i = 0; i < n_params; i++) {
      const float *ptr = input[i];
      IntType size = sizes[i];

      cudaPointerAttributes att;
      cudaError_t err = cudaPointerGetAttributes(&att, ptr);

      if (err == 0 && att.device > -1) {
        CUDA_CHECK(cudaSetDevice(att.device));
        CUDA_CHECK(cudaPeekAtLastError());

        try {
          faiss::gpu::StandardGpuResources gpu_res;

          cudaStream_t stream;
          CUDA_CHECK(cudaStreamCreate(&stream));

          gpu_res.noTempMemory();
          gpu_res.setCudaMallocWarning(false);
          gpu_res.setDefaultStream(att.device, stream);

          faiss::gpu::bruteForceKnn(&gpu_res, faiss::METRIC_L2, ptr, true, size,
                                    search_items, true, n, D, k,
                                    all_D + (i * k * n), all_I + (i * k * n));

          CUDA_CHECK(cudaPeekAtLastError());
          CUDA_CHECK(cudaStreamSynchronize(stream));

          CUDA_CHECK(cudaStreamDestroy(stream));

        } catch (const std::exception &e) {
          std::cout << "Exception occurred: " << e.what() << std::endl;
        }

      } else {
        std::stringstream ss;
        ss << "Input memory for " << ptr
           << " failed. isDevice?=" << att.devicePointer << ", N=" << sizes[i];
        std::cout << "Exception: " << ss.str() << std::endl;
      }
    }
  }

  merge_tables<faiss::CMin<float, IntType>>(n, k, n_params, result_D, result_I,
                                            all_D, all_I, id_ranges->data());

  if (DistanceType == Distance::EucUnexpandedL2Sqrt) {
    MLCommon::LinAlg::unaryOp<float>(
      res_D, res_D, n * k, [] __device__(float input) { return sqrt(input); },
      s);
  }

  MLCommon::updateDevice(res_D, result_D, k * n, s);
  MLCommon::updateDevice(res_I, result_I, k * n, s);

  delete all_D;
  delete all_I;

  delete result_D;
  delete result_I;

  if (translations == nullptr) delete id_ranges;
};

};  // namespace Selection
};  // namespace MLCommon
