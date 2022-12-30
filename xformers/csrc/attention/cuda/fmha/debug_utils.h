#pragma once
#include <float.h>
#include <stdio.h>
#include <cmath>
#include "cutlass/numeric_conversion.h"

////////////////////////////////////////////////////////////////////////////////
// Debugging functions
////////////////////////////////////////////////////////////////////////////////
// Nans & inf detection
#define NANCHECK(frag)                         \
  {                                            \
    for (int _i = 0; _i < frag.size(); ++_i) { \
      assert(std::isfinite(float(frag[_i])));  \
      assert(!std::isnan(float(frag[_i])));    \
    }                                          \
  }

// Print on the first thread of the first block
#if 1
#define PRINT_WARP_ID 1
#define PRINT_LANE_ID 0
#define PRINT_T0_L0(msg, ...)                                         \
  if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&        \
      threadIdx.x == PRINT_LANE_ID && threadIdx.y == PRINT_WARP_ID && \
      threadIdx.z == 0) {                                             \
    printf(msg "\n", ##__VA_ARGS__);                                  \
  }

#define PRINT_TN_LN(warp_id, lane_id, msg, ...)                                         \
  if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&        \
      threadIdx.x == lane_id && threadIdx.y == warp_id && \
      threadIdx.z == 0) {                                             \
    printf("[warpid=%d, laneid=%d] " msg "\n", warp_id, lane_id, ##__VA_ARGS__);                                  \
  }

#define PRINT_TX_LX(msg, ...)                                                 \
  for (int bx = 0; bx < gridDim.x; ++bx) {                                    \
    for (int by = 0; by < gridDim.y; ++by) {                                  \
      for (int bz = 0; bz < gridDim.z; ++bz) {                                \
        for (int tx = 0; tx < blockDim.x; ++tx) {                             \
          for (int ty = 0; ty < blockDim.y; ++ty) {                           \
            for (int tz = 0; tz < blockDim.z; ++tz) {                         \
              __syncthreads();                                                \
              if (blockIdx.x == bx && blockIdx.y == by && blockIdx.z == bz && \
                  threadIdx.x == tx && threadIdx.y == ty &&                   \
                  threadIdx.z == tz) {                                        \
                printf(                                                       \
                    "[%d,%d,%d][%d,%d,%d]" msg "\n",                          \
                    bx,                                                       \
                    by,                                                       \
                    bz,                                                       \
                    tx,                                                       \
                    ty,                                                       \
                    tz,                                                       \
                    ##__VA_ARGS__);                                           \
              }                                                               \
            }                                                                 \
          }                                                                   \
        }                                                                     \
      }                                                                       \
    }                                                                         \
  }
#else
#define PRINT_T0_L0
#define PRINT_TX_LX
#endif

struct __string_view {
  char const* data;
  std::size_t size;
};
template <class T>
constexpr __string_view __get_type_name() {
  char const* p = __PRETTY_FUNCTION__;
  while (*p++ != '=')
    ;
  for (; *p == ' '; ++p)
    ;
  char const* p2 = p;
  int count = 1;
  for (;; ++p2) {
    switch (*p2) {
      case '[':
        ++count;
        break;
      case ']':
        --count;
        if (!count)
          return {p, std::size_t(p2 - p)};
    }
  }
  return {};
}

// Print a given array
#define PRINT_ACCUM8_T0_L0_START(name, accum, start)  \
  PRINT_T0_L0(                                        \
      "%s[%d:%d] - {%f, %f, %f, %f, %f, %f, %f, %f}", \
      name,                                           \
      int(start),                                     \
      int(start + 8),                                 \
      float(accum[start + 0]),                        \
      float(accum[start + 1]),                        \
      float(accum[start + 2]),                        \
      float(accum[start + 3]),                        \
      float(accum[start + 4]),                        \
      float(accum[start + 5]),                        \
      float(accum[start + 6]),                        \
      float(accum[start + 7]));
#define PRINT_ACCUM8_T0_L0(name, accum) PRINT_ACCUM8_T0_L0_START(name, accum, 0)
#define PRINT_FRAG_T0_L0(name, frag)                          \
  {                                                           \
    auto typeStr = __get_type_name<decltype(frag)>();         \
    PRINT_T0_L0("printing %s (%s)", name, typeStr.data);      \
    for (int _start = 0; _start < frag.size(); _start += 8) { \
      PRINT_ACCUM8_T0_L0_START("  ", frag, _start);           \
    }                                                         \
    /*__syncthreads();                                        \
    NANCHECK(frag); */                                        \
  }
#define PRINT_ARRAY_T0_L0_INCR(name, array, length, incr)   \
  {                                                         \
    PRINT_T0_L0("printing %s (len=%d)", name, int(length)); \
    for (int _start = 0; _start < length; _start += incr) { \
      PRINT_ACCUM8_T0_L0_START("  ", array, _start);        \
    }                                                       \
  }
#define PRINT_ARRAY_T0_L0(name, array, length) \
  PRINT_ARRAY_T0_L0_INCR(name, array, length, 8)

// Print a 4x4 matrix
#define PRINT_TENSOR4x4_T0_L0_START(name, ref, start_x, start_y)                                           \
  PRINT_T0_L0(                                                                                             \
      "%s[%d:%d, %d:%d]:\n    %f, %f, %f, %f\n    %f, %f, %f, %f\n    %f, %f, %f, %f\n    %f, %f, %f, %f", \
      name,                                                                                                \
      int(start_x),                                                                                        \
      int(start_x + 4),                                                                                    \
      int(start_y),                                                                                        \
      int(start_y + 4),                                                                                    \
      float(ref.at({start_x + 0, start_y + 0})),                                                           \
      float(ref.at({start_x + 0, start_y + 1})),                                                           \
      float(ref.at({start_x + 0, start_y + 2})),                                                           \
      float(ref.at({start_x + 0, start_y + 3})),                                                           \
      float(ref.at({start_x + 1, start_y + 0})),                                                           \
      float(ref.at({start_x + 1, start_y + 1})),                                                           \
      float(ref.at({start_x + 1, start_y + 2})),                                                           \
      float(ref.at({start_x + 1, start_y + 3})),                                                           \
      float(ref.at({start_x + 2, start_y + 0})),                                                           \
      float(ref.at({start_x + 2, start_y + 1})),                                                           \
      float(ref.at({start_x + 2, start_y + 2})),                                                           \
      float(ref.at({start_x + 2, start_y + 3})),                                                           \
      float(ref.at({start_x + 3, start_y + 0})),                                                           \
      float(ref.at({start_x + 3, start_y + 1})),                                                           \
      float(ref.at({start_x + 3, start_y + 2})),                                                           \
      float(ref.at({start_x + 3, start_y + 3})));
#define PRINT_TENSOR4x4_T0_L0(name, ref) \
  PRINT_TENSOR4x4_T0_L0_START(name, ref, 0, 0)

#define PRINT_PROBLEM_SIZE(name, ps)            \
  PRINT_T0_L0(                                  \
      "%s.problem_size: {.m=%d, .n=%d, .k=%d}", \
      name,                                     \
      int(ps.m()),                              \
      int(ps.n()),                              \
      int(ps.k()))


CUTLASS_DEVICE bool is_t0_l0() {
  return blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && 
    threadIdx.x == 0 && threadIdx.y == 0 && threadIdx.z == 0;
}

template<typename TensorRef> 
CUTLASS_DEVICE void print_tensor_ref(TensorRef ref, int m, int n) {
  __syncthreads();
  if (is_t0_l0()) {
    cutlass::NumericConverter<float, typename TensorRef::Element> converter{};
    for (int i = 0; i < m; ++i) {
      for (int j = 0; j < n; ++j) {
        printf("%.3f ", converter(ref.at({i, j})));
      }
      printf("\n");
    }
  }
}

template<typename TensorRef>
CUTLASS_DEVICE void print_tensor_ref_layout(TensorRef ref, int m, int n) {
  __syncthreads();
  if (is_t0_l0()) {
    cutlass::NumericConverter<float, typename TensorRef::Element> converter{};

    typename TensorRef::Element* data = ref.data();

    for (int i = 0; i < m; ++i) {
      for (int j = 0; j < n; ++j) {
        printf("%.3f ", converter(data[i * m + j]));
      }
      printf("\n");
    }
  }
}


template<typename Array>
CUTLASS_DEVICE void print_array(Array array) {
  cutlass::NumericConverter<float, typename Array::Element> converter{};
  if (is_t0_l0()) {
    for (int i = 0; i < Array::kElements; ++i) {
      printf("%.3f \n", converter(array[i]));
    }
    printf("\n");
  }
}
