#include "geozl/gpu.h"

#include <stdio.h>

static void unavailable(char *error, size_t errorCapacity) {
  if (error != NULL && errorCapacity != 0)
    snprintf(error, errorCapacity, "GPU support is not built in");
}

int geozl_gpu_available(int deviceType) {
  (void)deviceType;
  return 0;
}

GeozlGpu *geozl_gpu_create(int deviceType, int device, size_t threads,
                           size_t maxDecodedBytes, int verifyChecksums,
                           char *error, size_t errorCapacity) {
  (void)deviceType;
  (void)device;
  (void)threads;
  (void)maxDecodedBytes;
  (void)verifyChecksums;
  unavailable(error, errorCapacity);
  return NULL;
}

void geozl_gpu_destroy(GeozlGpu *gpu) { (void)gpu; }

int geozl_gpu_decode(GeozlGpu *gpu, const GeozlGpuFrame frames[],
                     size_t frameCount, const GeozlGpuCopy copies[],
                     size_t copyCount, size_t sampleBytes, size_t elementBytes,
                     const GeozlGpuTarget *target, char *error,
                     size_t errorCapacity) {
  (void)gpu;
  (void)frames;
  (void)frameCount;
  (void)copies;
  (void)copyCount;
  (void)sampleBytes;
  (void)elementBytes;
  (void)target;
  unavailable(error, errorCapacity);
  return 1;
}

int geozl_gpu_synchronize(GeozlGpu *gpu, char *error, size_t errorCapacity) {
  (void)gpu;
  unavailable(error, errorCapacity);
  return 1;
}

GeozlGpuMemory *geozl_gpu_alloc(GeozlGpu *gpu, size_t bytes, char *error,
                                size_t errorCapacity) {
  (void)gpu;
  (void)bytes;
  unavailable(error, errorCapacity);
  return NULL;
}

void *geozl_gpu_pointer(const GeozlGpuMemory *memory) {
  (void)memory;
  return NULL;
}

void geozl_gpu_free(GeozlGpuMemory *memory) { (void)memory; }
