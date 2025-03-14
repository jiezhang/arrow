// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "arrow/buffer.h"
#include "arrow/compute/type_fwd.h"
#include "arrow/memory_pool.h"
#include "arrow/result.h"
#include "arrow/status.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/cpu_info.h"
#include "arrow/util/mutex.h"
#include "arrow/util/thread_pool.h"
#include "arrow/util/type_fwd.h"

#if defined(__clang__) || defined(__GNUC__)
#define BYTESWAP(x) __builtin_bswap64(x)
#define ROTL(x, n) (((x) << (n)) | ((x) >> ((-n) & 31)))
#define ROTL64(x, n) (((x) << (n)) | ((x) >> ((-n) & 63)))
#define PREFETCH(ptr) __builtin_prefetch((ptr), 0 /* rw==read */, 3 /* locality */)
#elif defined(_MSC_VER)
#include <intrin.h>
#define BYTESWAP(x) _byteswap_uint64(x)
#define ROTL(x, n) _rotl((x), (n))
#define ROTL64(x, n) _rotl64((x), (n))
#if defined(_M_X64) || defined(_M_I86)
#include <mmintrin.h>  // https://msdn.microsoft.com/fr-fr/library/84szxsww(v=vs.90).aspx
#define PREFETCH(ptr) _mm_prefetch((const char*)(ptr), _MM_HINT_T0)
#else
#define PREFETCH(ptr) (void)(ptr) /* disabled */
#endif
#endif

namespace arrow {
namespace util {

// Some platforms typedef int64_t as long int instead of long long int,
// which breaks the _mm256_i64gather_epi64 and _mm256_i32gather_epi64 intrinsics
// which need long long.
// We use the cast to the type below in these intrinsics to make the code
// compile in all cases.
//
using int64_for_gather_t = const long long int;  // NOLINT runtime-int

// All MiniBatch... classes use TempVectorStack for vector allocations and can
// only work with vectors up to 1024 elements.
//
// They should only be allocated on the stack to guarantee the right sequence
// of allocation and deallocation of vectors from TempVectorStack.
//
class MiniBatch {
 public:
  static constexpr int kLogMiniBatchLength = 10;
  static constexpr int kMiniBatchLength = 1 << kLogMiniBatchLength;
};

/// Storage used to allocate temporary vectors of a batch size.
/// Temporary vectors should resemble allocating temporary variables on the stack
/// but in the context of vectorized processing where we need to store a vector of
/// temporaries instead of a single value.
class TempVectorStack {
  template <typename>
  friend class TempVectorHolder;

 public:
  Status Init(MemoryPool* pool, int64_t size) {
    num_vectors_ = 0;
    top_ = 0;
    buffer_size_ = PaddedAllocationSize(size) + kPadding + 2 * sizeof(uint64_t);
    ARROW_ASSIGN_OR_RAISE(auto buffer, AllocateResizableBuffer(size, pool));
    // Ensure later operations don't accidentally read uninitialized memory.
    std::memset(buffer->mutable_data(), 0xFF, size);
    buffer_ = std::move(buffer);
    return Status::OK();
  }

 private:
  int64_t PaddedAllocationSize(int64_t num_bytes) {
    // Round up allocation size to multiple of 8 bytes
    // to avoid returning temp vectors with unaligned address.
    //
    // Also add padding at the end to facilitate loads and stores
    // using SIMD when number of vector elements is not divisible
    // by the number of SIMD lanes.
    //
    return ::arrow::bit_util::RoundUp(num_bytes, sizeof(int64_t)) + kPadding;
  }
  void alloc(uint32_t num_bytes, uint8_t** data, int* id);
  void release(int id, uint32_t num_bytes);
  static constexpr uint64_t kGuard1 = 0x3141592653589793ULL;
  static constexpr uint64_t kGuard2 = 0x0577215664901532ULL;
  static constexpr int64_t kPadding = 64;
  int num_vectors_;
  int64_t top_;
  std::unique_ptr<Buffer> buffer_;
  int64_t buffer_size_;
};

template <typename T>
class TempVectorHolder {
  friend class TempVectorStack;

 public:
  ~TempVectorHolder() { stack_->release(id_, num_elements_ * sizeof(T)); }
  T* mutable_data() { return reinterpret_cast<T*>(data_); }
  TempVectorHolder(TempVectorStack* stack, uint32_t num_elements) {
    stack_ = stack;
    num_elements_ = num_elements;
    stack_->alloc(num_elements * sizeof(T), &data_, &id_);
  }

 private:
  TempVectorStack* stack_;
  uint8_t* data_;
  int id_;
  uint32_t num_elements_;
};

class bit_util {
 public:
  static void bits_to_indexes(int bit_to_search, int64_t hardware_flags,
                              const int num_bits, const uint8_t* bits, int* num_indexes,
                              uint16_t* indexes, int bit_offset = 0);

  static void bits_filter_indexes(int bit_to_search, int64_t hardware_flags,
                                  const int num_bits, const uint8_t* bits,
                                  const uint16_t* input_indexes, int* num_indexes,
                                  uint16_t* indexes, int bit_offset = 0);

  // Input and output indexes may be pointing to the same data (in-place filtering).
  static void bits_split_indexes(int64_t hardware_flags, const int num_bits,
                                 const uint8_t* bits, int* num_indexes_bit0,
                                 uint16_t* indexes_bit0, uint16_t* indexes_bit1,
                                 int bit_offset = 0);

  // Bit 1 is replaced with byte 0xFF.
  static void bits_to_bytes(int64_t hardware_flags, const int num_bits,
                            const uint8_t* bits, uint8_t* bytes, int bit_offset = 0);

  // Return highest bit of each byte.
  static void bytes_to_bits(int64_t hardware_flags, const int num_bits,
                            const uint8_t* bytes, uint8_t* bits, int bit_offset = 0);

  static bool are_all_bytes_zero(int64_t hardware_flags, const uint8_t* bytes,
                                 uint32_t num_bytes);

 private:
  inline static uint64_t SafeLoadUpTo8Bytes(const uint8_t* bytes, int num_bytes);
  inline static void SafeStoreUpTo8Bytes(uint8_t* bytes, int num_bytes, uint64_t value);
  inline static void bits_to_indexes_helper(uint64_t word, uint16_t base_index,
                                            int* num_indexes, uint16_t* indexes);
  inline static void bits_filter_indexes_helper(uint64_t word,
                                                const uint16_t* input_indexes,
                                                int* num_indexes, uint16_t* indexes);
  template <int bit_to_search, bool filter_input_indexes>
  static void bits_to_indexes_internal(int64_t hardware_flags, const int num_bits,
                                       const uint8_t* bits, const uint16_t* input_indexes,
                                       int* num_indexes, uint16_t* indexes,
                                       uint16_t base_index = 0);

#if defined(ARROW_HAVE_AVX2)
  static void bits_to_indexes_avx2(int bit_to_search, const int num_bits,
                                   const uint8_t* bits, int* num_indexes,
                                   uint16_t* indexes, uint16_t base_index = 0);
  static void bits_filter_indexes_avx2(int bit_to_search, const int num_bits,
                                       const uint8_t* bits, const uint16_t* input_indexes,
                                       int* num_indexes, uint16_t* indexes);
  template <int bit_to_search>
  static void bits_to_indexes_imp_avx2(const int num_bits, const uint8_t* bits,
                                       int* num_indexes, uint16_t* indexes,
                                       uint16_t base_index = 0);
  template <int bit_to_search>
  static void bits_filter_indexes_imp_avx2(const int num_bits, const uint8_t* bits,
                                           const uint16_t* input_indexes,
                                           int* num_indexes, uint16_t* indexes);
  static void bits_to_bytes_avx2(const int num_bits, const uint8_t* bits, uint8_t* bytes);
  static void bytes_to_bits_avx2(const int num_bits, const uint8_t* bytes, uint8_t* bits);
  static bool are_all_bytes_zero_avx2(const uint8_t* bytes, uint32_t num_bytes);
#endif
};

}  // namespace util
}  // namespace arrow
