#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <cfloat>
#include <cmath>
#include <cassert>
#include <memory>

#include <cuda_runtime_api.h>

#include "private.h"


static int check_args(
    float tolerance, float yinyang_t, uint32_t samples_size, uint16_t features_size,
    uint32_t clusters_size, uint32_t device, const float *samples, float *centroids,
    uint32_t *assignments) {
  if (clusters_size < 2 || clusters_size == UINT32_MAX) {
    return kmcudaInvalidArguments;
  }
  if (features_size == 0) {
    return kmcudaInvalidArguments;
  }
  if (samples_size < clusters_size) {
    return kmcudaInvalidArguments;
  }
  if (device == 0) {
    return kmcudaNoSuchDevice;
  }
  int devices = 0;
  cudaGetDeviceCount(&devices);
  if (device > (1u << devices)) {
    return kmcudaNoSuchDevice;
  }
  if (samples == nullptr || centroids == nullptr || assignments == nullptr) {
    return kmcudaInvalidArguments;
  }
  if (tolerance < 0 || tolerance > 1) {
    return kmcudaInvalidArguments;
  }
  if (yinyang_t < 0 || yinyang_t > 0.5) {
    return kmcudaInvalidArguments;
  }
  return kmcudaSuccess;
}

static std::vector<int> setup_devices(uint32_t device, int verbosity) {
  std::vector<int> devs;
  for (int dev = 0; device; dev++) {
    if (device & 1) {
      devs.push_back(dev);
      if (cudaSetDevice(dev) != cudaSuccess) {
        INFO("failed to validate device %d", dev);
        devs.pop_back();
      }
    }
    device >>= 1;
  }
  return std::move(devs);
}

static KMCUDAResult print_memory_stats() {
  size_t free_bytes, total_bytes;
  if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess) {
    return kmcudaRuntimeError;
  }
  printf("GPU memory: used %zu bytes (%.1f%%), free %zu bytes, total %zu bytes\n",
         total_bytes - free_bytes, (total_bytes - free_bytes) * 100.0 / total_bytes,
         free_bytes, total_bytes);
  return kmcudaSuccess;
}

extern "C" {

KMCUDAResult kmeans_init_centroids(
    KMCUDAInitMethod method, uint32_t samples_size, uint16_t features_size,
    uint32_t clusters_size, uint32_t seed, int32_t verbosity,
    const std::vector<int> &devs, const udevptrs<float> &samples,
    udevptrs<float> *dists, udevptrs<float> *dev_sums,
    udevptrs<float> *centroids) {
  uint32_t ssize = features_size * sizeof(float);
  srand(seed);
  switch (method) {
    case kmcudaInitMethodRandom:
      INFO("randomly picking initial centroids...\n");
      for (uint32_t c = 0; c < clusters_size; c++) {
        if ((c + 1) % 1000 == 0 || c == clusters_size - 1) {
          INFO("\rcentroid #%" PRIu32, c + 1);
          fflush(stdout);
          CUMEMCPY_D2D(
              *centroids, c * features_size, samples,
              (rand() % samples_size) * features_size, features_size);
        } else {
          CUMEMCPY_D2D_ASYNC(
              *centroids, c * features_size, samples,
              (rand() % samples_size) * features_size,
              features_size);
        }
      }
      break;
    case kmcudaInitMethodPlusPlus:
      INFO("performing kmeans++...\n");
      CUMEMCPY_D2D_ASYNC(
          *centroids, 0, samples, (rand() % samples_size) * features_size,
          features_size);
      std::unique_ptr<float[]> host_dists(new float[samples_size]);
      for (uint32_t i = 1; i < clusters_size; i++) {
        if (verbosity > 1 || (verbosity > 0 && (
              clusters_size < 100 || i % (clusters_size / 100) == 0))) {
          printf("\rstep %d", i);
          fflush(stdout);
        }
        float dist_sum = 0;
        RETERR(kmeans_cuda_plus_plus(
            samples_size, features_size, i, verbosity, devs, samples, centroids,
            dists, dev_sums, host_dists.get(), &dist_sum),
               DEBUG("\nkmeans_cuda_plus_plus failed\n"));
        assert(dist_sum == dist_sum);
        double choice = ((rand() + .0) / RAND_MAX);
        uint32_t choice_approx = choice * samples_size;
        double choice_sum = choice * dist_sum;
        uint32_t j;
        {
          double dist_sum2 = 0;
          for (j = 0; j < samples_size && dist_sum2 < choice_sum; j++) {
            dist_sum2 += host_dists[j];
          }
        }
        if (choice_approx < 100) {
          double dist_sum2 = 0;
          for (j = 0; j < samples_size && dist_sum2 < choice_sum; j++) {
            dist_sum2 += host_dists[j];
          }
        } else {
          double dist_sum2 = 0;
          #pragma omp simd reduction(+:dist_sum2)
          for (uint32_t t = 0; t < choice_approx; t++) {
            dist_sum2 += host_dists[t];
          }
          if (dist_sum2 < choice_sum) {
            for (j = choice_approx; j < samples_size && dist_sum2 < choice_sum; j++) {
              dist_sum2 += host_dists[j];
            }
          } else {
            for (j = choice_approx; j > 1 && dist_sum2 >= choice_sum; j--) {
              dist_sum2 -= host_dists[j];
            }
            j++;
          }
        }
        assert(j > 0);
        FOR_ALL_DEVSI(
          CUMEMCPY_D2D_ASYNC(*centroids, i * features_size,
                             samples, (j - 1) * features_size,
                             ssize);
        );
      }
      break;
  }

  INFO("\rdone            \n");
  return kmcudaSuccess;
}

int kmeans_cuda(bool kmpp, float tolerance, float yinyang_t, uint32_t samples_size,
                uint16_t features_size, uint32_t clusters_size, uint32_t seed,
                uint32_t device, int32_t verbosity, int device_ptrs,
                const float *samples, float *centroids, uint32_t *assignments) {
  DEBUG("arguments: %d %.3f %.2f %" PRIu32 " %" PRIu16 " %" PRIu32 " %" PRIu32
        " %" PRIu32 " %" PRIi32 " %p %p %p\n",
        kmpp, tolerance, yinyang_t, samples_size, features_size, clusters_size,
        seed, device, verbosity, samples, centroids, assignments);
  RETERR(check_args(
      tolerance, yinyang_t, samples_size, features_size, clusters_size,
      device, samples, centroids, assignments));
  auto devs = setup_devices(device, verbosity);
  if (devs.empty()) {
    return kmcudaNoSuchDevice;
  }
  udevptrs<float> device_samples;
  size_t device_samples_size = static_cast<size_t>(samples_size) * features_size;
  if (device_ptrs < 0) {
    CUMALLOC(device_samples, device_samples_size);
    CUMEMCPY_H2D_ASYNC(device_samples, 0, samples, device_samples_size);
  } else {
    device_samples.emplace_back(const_cast<float*>(samples), true);
  }
  udevptrs<float> device_centroids;
  size_t centroids_size = static_cast<size_t>(clusters_size) * features_size;
  bool must_copy_result = true;
  FOR_ALL_DEVS(
    if (dev == device_ptrs) {
      device_centroids.emplace_back(centroids, true);
      must_copy_result = false;
    } else {
      CUMALLOC_ONE(device_centroids, centroids_size);
    }
  );
  udevptrs<uint32_t> device_assignments;
  FOR_ALL_DEVS(
    if (dev == device_ptrs) {
      device_assignments.emplace_back(assignments, true);
    } else {
      CUMALLOC_ONE(device_assignments, samples_size);
    }
  );
  udevptrs<uint32_t> device_assignments_prev;
  CUMALLOC(device_assignments_prev, samples_size);
  udevptrs<uint32_t> device_ccounts;
  CUMALLOC(device_ccounts, clusters_size);

  uint32_t yinyang_groups = yinyang_t * clusters_size;
  DEBUG("yinyang groups: %" PRIu32 "\n", yinyang_groups);
  udevptrs<uint32_t> device_assignments_yy, device_passed_yy;
  udevptrs<float> device_bounds_yy, device_drifts_yy, device_centroids_yy;
  if (yinyang_groups >= 1) {
    CUMALLOC(device_assignments_yy, clusters_size);
    size_t yyb_size = static_cast<size_t>(samples_size) * (yinyang_groups + 1);
    CUMALLOC(device_bounds_yy, yyb_size);
    CUMALLOC(device_drifts_yy, centroids_size + clusters_size);
    CUMALLOC(device_passed_yy, samples_size);
    size_t yyc_size = yinyang_groups * features_size;
    if (yyc_size + (clusters_size + yinyang_groups) <= samples_size) {
      for (auto &p : device_passed_yy) {
        device_centroids_yy.emplace_back(
            reinterpret_cast<float*>(p.get()), true);
      }
    } else {
      CUMALLOC(device_centroids_yy, yyc_size);
    }
  }

  if (verbosity > 1) {
    RETERR(print_memory_stats());
  }
  RETERR(kmeans_cuda_setup(samples_size, features_size, clusters_size,
                           yinyang_groups, devs, verbosity),
         DEBUG("kmeans_cuda_setup failed: %s\n",
               cudaGetErrorString(cudaGetLastError())));
  RETERR(kmeans_init_centroids(
      static_cast<KMCUDAInitMethod>(kmpp), samples_size, features_size,
      clusters_size, seed, verbosity, devs, device_samples,
      reinterpret_cast<udevptrs<float>*>(&device_assignments),
      reinterpret_cast<udevptrs<float>*>(&device_assignments_prev),
      &device_centroids),
         DEBUG("kmeans_init_centroids failed: %s\n",
               cudaGetErrorString(cudaGetLastError())));
  RETERR(kmeans_cuda_yy(
      tolerance, yinyang_groups, samples_size, clusters_size, features_size,
      verbosity, devs, device_samples, &device_centroids,
      &device_ccounts, &device_assignments_prev,
      &device_assignments, &device_assignments_yy,
      &device_centroids_yy, &device_bounds_yy,
      &device_drifts_yy, &device_passed_yy),
         DEBUG("kmeans_cuda_internal failed: %s\n",
               cudaGetErrorString(cudaGetLastError())));
  if (must_copy_result) {
    if (device_ptrs < 0) {
      CUCH(cudaMemcpy(centroids, device_centroids[0].get(),
                      centroids_size * sizeof(float), cudaMemcpyDeviceToHost),
           kmcudaMemoryCopyError);
      CUCH(cudaMemcpy(assignments, device_assignments[0].get(),
                      samples_size * sizeof(uint32_t), cudaMemcpyDeviceToHost),
           kmcudaMemoryCopyError);
    } else {
      CUCH(cudaMemcpyPeer(centroids, device_ptrs, device_centroids[0].get(),
                          devs[0], centroids_size * sizeof(float)),
           kmcudaMemoryCopyError);
      CUCH(cudaMemcpyPeer(assignments, device_ptrs, device_assignments[0].get(),
                          devs[0], samples_size * sizeof(uint32_t)),
           kmcudaMemoryCopyError);
    }
  }
  DEBUG("return kmcudaSuccess\n");
  return kmcudaSuccess;
}
}