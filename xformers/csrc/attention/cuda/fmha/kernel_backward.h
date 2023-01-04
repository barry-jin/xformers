#pragma once

#include <cmath>
#include <type_traits>
#include <vector>

#include <cuda_fp16.h>
#include <curand_kernel.h>

#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAGeneratorImpl.h>
#include <c10/cuda/CUDAGuard.h>
#include <ATen/cuda/CUDAGraphsUtils.cuh>

#include "cutlass/cutlass.h"
#include "cutlass/epilogue/thread/linear_combination.h"
#include "cutlass/epilogue/thread/scale_type.h"
#include "cutlass/fast_math.h"
#include "cutlass/functional.h"
#include "cutlass/gemm/gemm.h"
#include "cutlass/half.h"
#include "cutlass/layout/matrix.h"
#include "cutlass/layout/vector.h"
#include "cutlass/numeric_conversion.h"
#include "cutlass/numeric_types.h"
#include "cutlass/tensor_ref.h"

#include "debug_utils.h"
#include "gemm_kernel_utils.h"

#include "cutlass/epilogue/thread/linear_combination_relu.h"
#include "cutlass/epilogue/threadblock/epilogue_smem_accumulator.h"
#include "cutlass/epilogue/warp/fragment_iterator_tensor_op.h"
#include "cutlass/epilogue/warp/tile_iterator_tensor_op.h"
#include "cutlass/gemm/device/default_gemm_configuration.h"
#include "cutlass/gemm/kernel/default_gemm.h"
#include "cutlass/gemm/threadblock/default_mma.h"
#include "cutlass/gemm/threadblock/default_mma_core_simt.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm70.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm75.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm80.h"
#include "cutlass/matrix_shape.h"
#include "cutlass/platform/platform.h"
#include "cutlass/transform/threadblock/predicated_tile_iterator.h"
#include "cutlass/transform/threadblock/vector_iterator.h"
#include "epilogue_pipelined.h"
#include "iterators/epilogue_predicated_tile_iterator.h"

#include "find_default_mma.h"
#include "gemm/custom_mma.h"
#include "mma_from_smem.h"
#include "transform/tile_smem_loader.h"

#include <inttypes.h>

using namespace gemm_kernel_utils;

namespace {

template <typename FragmentType, int32_t kNumThreads>
struct GmemTile {
  /*
    Helper functions to efficient store/load RF to gmem

    GEMM accumulators have a particular format on A100, and
    it takes some compute/shared-memory to rearrange them to
    a RowMajor or ColumnMajor format in global memory through
    an Epilogue. The same complexity goes for loading into RF.

    This class loads/stores RF as they are, and can be used for
    efficient accumulation across gemms for instance:

    ```
    GmemTile tile;
    for (int i = 0; i < N; ++i) {
      // ...

      Fragment accum;
      if (i == 0) {
        accum.clear();
      } else {
        tile.load(accum);
      }
      mma(accum, ...);
      if (i < N-1) {
        // Store for next GEMM
        tile.store(accum);
      } else {
        // Store in tensor (eg RowMajor)
        epilogue(accum);
      }

      // ...
    }
    ```
  */

  // 128bits per thread
  using AccessType = cutlass::Array<float, 4>;
  static constexpr int32_t kBytes = sizeof(AccessType);
  static constexpr int32_t kStride = kNumThreads * AccessType::kElements;
  static constexpr int32_t kNumIters =
      FragmentType::kElements / AccessType::kElements;
  static constexpr int32_t kElementsStored =
      kNumThreads * FragmentType::kElements;
  static_assert(
      FragmentType::kElements % AccessType::kElements == 0,
      "fragment not aligned on 128 bits");

  float* ptr;

  CUTLASS_DEVICE void load(FragmentType& fragment, int thread_id) {
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < kNumIters; ++i) {
      AccessType* __restrict__ gmem_ptr = reinterpret_cast<AccessType*>(
          ptr + thread_id * AccessType::kElements + i * kStride);
      AccessType sub_fragment;
      cutlass::arch::global_load<AccessType, kBytes>(
          sub_fragment, gmem_ptr, true);
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < AccessType::kElements; ++j) {
        fragment[i * AccessType::kElements + j] = sub_fragment[j];
      }
    }
  }

  CUTLASS_DEVICE void store(FragmentType const& fragment, int thread_id) {
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < kNumIters; ++i) {
      AccessType* __restrict__ gmem_ptr = reinterpret_cast<AccessType*>(
          ptr + thread_id * AccessType::kElements + i * kStride);
      AccessType sub_fragment;
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < AccessType::kElements; ++j) {
        sub_fragment[j] = fragment[i * AccessType::kElements + j];
      }
      cutlass::arch::global_store<AccessType, kBytes>(
          sub_fragment, gmem_ptr, true);
    }
  }
};

template <typename scalar_t, typename Arch>
constexpr int getWarpsPerSm() {
  bool is_half = !std::is_same<scalar_t, float>::value;
  if (Arch::kMinComputeCapability >= 80) {
    return is_half ? 12 : 8;
  }
  return 8;
}
} // namespace

template <
    // which arch we target (eg `cutlass::arch::Sm80`)
    typename ArchTag_,
    // input/output type
    typename scalar_t__,
    // run optimized kernel because memory accesses will be aligned
    bool kIsAligned_,
    // use dropout if enabled
    bool kApplyDropout,
    // upperbound on `max(value.shape[-1], query.shape[-1])`
    int kMaxK_ = std::numeric_limits<int>::max()>
struct AttentionBackwardKernel {
  // TODO. hack for avoiding compiling versions that won't work
  // using scalar_t_ = cutlass::half_t;
  using scalar_t_ = typename cutlass::platform::conditional<
      cutlass::sizeof_bits<scalar_t__>::value <= 16,
      scalar_t__,
      cutlass::half_t>::type;
  static constexpr int kMaxK = cutlass::const_min(kMaxK_, 128);

  using scalar_t = scalar_t_;
  using output_t = scalar_t;
  using output_accum_t = float;
  using lse_scalar_t = float;
  using accum_t = float;
  using ArchTag = ArchTag_;
  static constexpr bool kIsAligned = kIsAligned_;

  struct Params {
    // Input tensors
    scalar_t* query_ptr; // [Mq, nH, K]
    scalar_t* key_ptr; // [Mk, nH, K]
    scalar_t* value_ptr; // [Mk, nH, Kv]
    scalar_t* bias_ptr = nullptr;
    lse_scalar_t* logsumexp_ptr; // [nH, Mq]
    scalar_t* output_ptr; // [Mq, nH, Kv]
    scalar_t* grad_output_ptr; // [Mq, nH, Kv]
    accum_t* delta_ptr; // [Mq, nH]

    // Output tensors
    output_t* grad_query_ptr; //  [Mq, nH, K]
    output_t* grad_key_ptr; //    [Mk, nH, K]
    output_t* grad_value_ptr; //  [Mk, nH, Kv]
    output_t* grad_bias_ptr = nullptr;

    // Accumulators
    union {
      output_accum_t* workspace = nullptr; // [Mq, Kq] + [Mkv, Kq] + [Mkv, Kv]
      output_accum_t* workspace_gk;
    };
    output_accum_t* workspace_gv;
    output_accum_t* workspace_gq;

    // Scale
    accum_t scale;

    // Dimensions/strides
    int32_t head_dim;
    int32_t head_dim_value;
    int32_t num_queries;
    int32_t num_keys;
    int32_t num_heads;
    bool causal;

    int32_t q_strideM;
    int32_t k_strideM;
    int32_t v_strideM;
    int32_t bias_strideM;
    int32_t gO_strideM;
    int32_t gB_strideM;
    int8_t gQKV_strideM_multiplier; // 3 for packed, 1 otherwise

    // dropout
    at::PhiloxCudaState rng_engine_inputs;
    // RNG sequence offset based on batch_id and head_id
    unsigned long long dropout_batch_head_rng_offset;
    float dropout_prob;

    CUTLASS_HOST_DEVICE int32_t o_strideM() const {
      return head_dim_value * num_heads;
    }
    CUTLASS_HOST_DEVICE int32_t gQ_strideM() const {
      return gQKV_strideM_multiplier * num_heads * head_dim;
    }
    CUTLASS_HOST_DEVICE int32_t gK_strideM() const {
      return gQKV_strideM_multiplier * num_heads * head_dim;
    }
    CUTLASS_HOST_DEVICE int32_t gV_strideM() const {
      return gQKV_strideM_multiplier * num_heads * head_dim_value;
    }

    // Everything below is only used in `advance_to_block`
    // and shouldn't use registers
    int64_t o_strideH;
    int32_t q_strideH;
    int32_t k_strideH;
    int32_t v_strideH;
    int32_t bias_strideH;
    int64_t o_strideB;
    int64_t q_strideB;
    int64_t k_strideB;
    int64_t v_strideB;
    int64_t bias_strideB;
    int64_t lse_strideM;
    int32_t num_batches;

    int64_t gO_strideB;
    int64_t gQ_strideB;
    int64_t gK_strideB;
    int64_t gV_strideB;
    int64_t gB_strideB;
    int64_t gO_strideH;
    int64_t gQ_strideH;
    int64_t gK_strideH;
    int64_t gV_strideH;
    int64_t gB_strideH;

    CUTLASS_DEVICE void advance_to_block() {
      int64_t batch_id = blockIdx.z;
      int32_t head_id = blockIdx.y;

      query_ptr += batch_id * q_strideB + head_id * q_strideH;
      key_ptr += batch_id * k_strideB + head_id * k_strideH;
      value_ptr += batch_id * v_strideB + head_id * v_strideH;
      logsumexp_ptr += (batch_id * num_heads + head_id) * lse_strideM;
      if (bias_ptr != nullptr) {
        bias_ptr += batch_id * bias_strideB + head_id * bias_strideH;
      }
      output_ptr += batch_id * o_strideB + head_id * o_strideH;
      grad_output_ptr += batch_id * gO_strideB + head_id * gO_strideH;
      delta_ptr += (batch_id * num_heads + head_id) * num_queries;

      grad_query_ptr += batch_id * gQ_strideB + head_id * gQ_strideH;
      grad_key_ptr += batch_id * gK_strideB + head_id * gK_strideH;
      grad_value_ptr += batch_id * gV_strideB + head_id * gV_strideH;
      if (grad_bias_ptr != nullptr) {
        grad_bias_ptr += batch_id * gB_strideB + head_id * gB_strideH;
      }

      dropout_batch_head_rng_offset =
          batch_id * (num_heads * num_queries * num_keys) +
          head_id * (num_queries * num_keys);

      head_dim = warp_uniform(head_dim);
      head_dim_value = warp_uniform(head_dim_value);
      num_queries = warp_uniform(num_queries);
      num_keys = warp_uniform(num_keys);
      num_heads = warp_uniform(num_heads);

      gO_strideM = warp_uniform(gO_strideM);
      gQKV_strideM_multiplier = warp_uniform(gQKV_strideM_multiplier);
      q_strideM = warp_uniform(q_strideM);
      k_strideM = warp_uniform(k_strideM);
      v_strideM = warp_uniform(v_strideM);

      query_ptr = warp_uniform(query_ptr);
      key_ptr = warp_uniform(key_ptr);
      value_ptr = warp_uniform(value_ptr);
      bias_ptr = warp_uniform(bias_ptr);
      logsumexp_ptr = warp_uniform(logsumexp_ptr);
      output_ptr = warp_uniform(output_ptr);
      grad_output_ptr = warp_uniform(grad_output_ptr);
      delta_ptr = warp_uniform(delta_ptr);

      grad_query_ptr = warp_uniform(grad_query_ptr);
      grad_key_ptr = warp_uniform(grad_key_ptr);
      grad_value_ptr = warp_uniform(grad_value_ptr);
      grad_bias_ptr = warp_uniform(grad_bias_ptr);

      if (kNeedsAccumGradQ || kNeedsAccumGradK || kNeedsAccumGradV) {
        assert(workspace_size() == 0 || workspace != nullptr);

        workspace += (batch_id * num_heads + head_id) * workspace_strideBH();
        workspace = warp_uniform(workspace);
        workspace_gv = workspace + workspace_elements_gk();
        workspace_gq = workspace_gv + workspace_elements_gv();
      } else {
        workspace = nullptr;
      }
    }

    __host__ dim3 getBlocksGrid() const {
      return dim3(1, num_heads, num_batches);
    }
    __host__ dim3 getThreadsGrid() const {
      return dim3(kWarpSize, kNumWarpsPerBlock, 1);
    }
    CUTLASS_HOST_DEVICE int64_t workspace_elements_gk() const {
      if (!kNeedsAccumGradK) {
        return 0;
      }
      return align_up(num_keys, (int32_t)kBlockSizeJ) *
          align_up(head_dim, (int32_t)kBlockSizeI);
    }
    CUTLASS_HOST_DEVICE int64_t workspace_elements_gv() const {
      if (!kNeedsAccumGradV) {
        return 0;
      }
      return align_up(num_keys, (int32_t)kBlockSizeJ) *
          align_up(head_dim_value, (int32_t)kBlockSizeI);
    }
    CUTLASS_HOST_DEVICE int64_t workspace_elements_gq() const {
      if (!kNeedsAccumGradQ) {
        return 0;
      }
      if (num_keys <= kBlockSizeJ) {
        return 0;
      }
      return align_up(num_queries, (int32_t)kBlockSizeI) *
          align_up(head_dim, (int32_t)kBlockSizeJ);
    }
    CUTLASS_HOST_DEVICE int64_t workspace_strideBH() const {
      // Aligned on 128bits
      return align_up(
          workspace_elements_gk() + workspace_elements_gv() +
              workspace_elements_gq(),
          int64_t(4));
    }
    CUTLASS_HOST_DEVICE int64_t workspace_size() const {
      // Returns size of buffer we need to run this kernel
      return num_batches * num_heads * workspace_strideBH() * sizeof(float);
    }
  };

  // Block I
  static constexpr bool kSupports64x128 =
      ArchTag::kMinComputeCapability >= 80 ||
      (ArchTag::kMinComputeCapability >= 70 &&
       cutlass::sizeof_bits<scalar_t>::value <= 16);
  static constexpr int64_t kWarpSize = 32;
  static constexpr int64_t kBlockSizeI =
      kSupports64x128 && kMaxK > 64 ? 128 : 64;

  // If this is true, we store and accumulate dK/dV in RF
  // rather than going back to gmem everytime
  static constexpr bool kIsHalf = cutlass::sizeof_bits<scalar_t>::value <= 16;
  static constexpr bool kOutputInRF = kIsHalf && kMaxK <= kBlockSizeI;
  static constexpr bool kPreloadMmas =
      kIsHalf && ArchTag::kMinComputeCapability >= 80 && kOutputInRF;
  static constexpr bool kPrologueQK = kPreloadMmas;
  static constexpr bool kPrologueGV = kPreloadMmas;
  static constexpr bool kPrologueDOV = kPreloadMmas;
  static constexpr bool kPrologueGQ = kPreloadMmas;
  static constexpr bool kPrologueGK = kPreloadMmas;
  static constexpr bool kReuseDOi = kIsHalf && kMaxK <= 128;

  // Block J
  static constexpr int64_t kBlockSizeJ = kPreloadMmas && kMaxK > 64 ? 128 : 64;
  static constexpr int64_t kNumWarpsPerBlock =
      (kBlockSizeI * kBlockSizeJ) / (32 * 32);

  // Compute delta for the f16 kernels
  // TODO: Figure out why it's slower on the f32 kernels
  // (something due to RF pressure?)
  // TODO: Remove condition on `kOutputInRF` - this is needed to work
  // around a compiler bug on V100, not exactly sure why but I spent
  // too much time on this already. Reproducible with
  // (B, Mq, Mkv, K) = (1, 1, 1, 136) for instance
  static constexpr bool kKernelComputesDelta =
      kIsHalf && (kOutputInRF || ArchTag::kMinComputeCapability != 70);

  static constexpr bool kNeedsAccumGradQ =
      !std::is_same<output_accum_t, output_t>::value;
  static constexpr bool kNeedsAccumGradK =
      !kOutputInRF && !std::is_same<output_accum_t, output_t>::value;
  static constexpr bool kNeedsAccumGradV =
      !kOutputInRF && !std::is_same<output_accum_t, output_t>::value;

  // Launch bounds
  static constexpr int64_t kNumThreads = kWarpSize * kNumWarpsPerBlock;
  static constexpr int64_t kMinBlocksPerSm =
      getWarpsPerSm<scalar_t, ArchTag>() / kNumWarpsPerBlock;

  using GemmType = DefaultGemmType<ArchTag, scalar_t>;
  using DefaultConfig =
      typename cutlass::gemm::device::DefaultGemmConfiguration<
          typename GemmType::OpClass,
          ArchTag,
          scalar_t,
          scalar_t,
          scalar_t, // ElementC
          accum_t // ElementAccumulator
          >;
  static constexpr auto kOptimalAlignement =
      std::max(DefaultConfig::kAlignmentA, DefaultConfig::kAlignmentB);
  static constexpr auto kMinimumAlignment = GemmType::kMinimumAlignment;

  struct MatmulQK {
    /*
    attn_T = k_j @ q_i.transpose(-2, -1) # matmul
    attn_T = (attn_T - logsumexp[i_start:i_end].unsqueeze(1).transpose(-2,
    -1)).exp() # epilogue

    with attn_T.shape = (kBlockSizeJ, kBlockSizeI)
    */
    using ThreadblockShape =
        cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using DefaultMma = typename cutlass::gemm::threadblock::DefaultMma<
        scalar_t, // ElementA
        cutlass::layout::RowMajor, // LayoutA
        kIsAligned ? DefaultConfig::kAlignmentA : GemmType::kMinimumAlignment,
        scalar_t, // ElementB
        cutlass::layout::ColumnMajor, // LayoutB
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
        accum_t, // ElementC
        cutlass::layout::RowMajor, // LayoutC
        typename GemmType::OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        DefaultConfig::kStages,
        typename GemmType::Operator,
        false, // AccumulatorsInRowMajor = false,
        cutlass::gemm::SharedMemoryClearOption::kNone>;
    using MmaCore = typename DefaultMma::MmaCore;
    using Mma =
        typename MakeCustomMma<typename DefaultMma::ThreadblockMma, kMaxK>::Mma;

    // used for efficient load of bias tile (Bij) from global memory to shared
    // memory
    using BiasLoader = TileSmemLoader<
        scalar_t,
        // Bij is applied to transposed attn matrix tile (Pij.T). Bij is loaded
        // row-major but needs to have transposed shape so we get the same
        // elements.
        cutlass::MatrixShape<ThreadblockShape::kN, ThreadblockShape::kM>,
        MmaCore::kThreads,
        // input restriction: kv_len has to be a multiple of this value
        128 / cutlass::sizeof_bits<scalar_t>::value>;

    // Epilogue to store to shared-memory in a format that we can use later for
    // the second matmul
    using B2bGemm = typename cutlass::gemm::threadblock::B2bGemm<
        typename Mma::Operator::IteratorC,
        typename Mma::Operator,
        scalar_t,
        WarpShape,
        ThreadblockShape>;
    using ScalingCoefsUpdater = typename DefaultAttentionScalingCoefsUpdater<
        typename Mma::Operator::IteratorC,
        accum_t,
        kWarpSize>::Updater;
    using AccumulatorSharedStorage = typename B2bGemm::AccumulatorSharedStorage;
  };

  struct MatmulGradV {
    /*
    grad_v[j_start:j_end] += attn_T @ do_i # matmul

    Dimensions: (kBlockSizeJ * kNumWarpsPerBlock, kBlockSizeI, K)
    (we might need to iterate multiple times on K)
    */
    using ThreadblockShape =
        cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using InstructionShape = typename GemmType::InstructionShape;

    using DefaultGemm = cutlass::gemm::kernel::DefaultGemm<
        scalar_t, // ElementA,
        cutlass::layout::RowMajor, // LayoutA,
        DefaultConfig::kAlignmentA,
        scalar_t, // ElementB,
        cutlass::layout::RowMajor, // LayoutB,
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
        output_t,
        cutlass::layout::RowMajor, // LayoutC,
        accum_t,
        typename GemmType::OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        typename DefaultConfig::EpilogueOutputOp,
        void, // ThreadblockSwizzle - not used
        DefaultConfig::kStages,
        false, // SplitKSerial
        typename GemmType::Operator>;

    // if dropout:
    //   for computing dVj += (Pij.T * Zij) @ dOi
    //   Pij_dropped.T = Pij.T * Zij is computed on the fly as fragments of
    //   Pij.T are loaded in. The reason we do it this way is because Pij.T and
    //   Zij are reused in later steps, while Pij_dropped.T is only needed in
    //   this step. computing Pij_dropped.T on the fly allows us to avoid
    //   keeping all 3 of Pij_dropped.T, Pij.T, and Zij in shared memory at the
    //   same time.
    // if no dropout:
    //   for computing dVj += Pij.T @ dOi
    using WarpIteratorA = typename cutlass::gemm::threadblock::DefaultWarpIteratorAFromSharedMemory<
        typename DefaultGemm::Mma::Operator::Shape,  // WarpShape
        typename DefaultGemm::Mma::Operator::InstructionShape,  // InstructionShape
        typename DefaultGemm::Mma::Operator::IteratorA,  // RegularWarpIterator
        typename DefaultGemm::Mma::Policy  // Policy
    >::WarpIterator; 
    using DefaultMmaFromSmem =
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            MatmulQK::AccumulatorSharedStorage::Shape::kN,
            WarpIteratorA,
            kApplyDropout,  // kScaleOperandA
            kReuseDOi>;  // kForceSmemHoldEntireB

    using Mma = typename DefaultMmaFromSmem::Mma;
    using IteratorB = typename Mma::IteratorB;
    using WarpCount = typename Mma::WarpCount;

    // Epilogue
    using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
    using DefaultEpilogue = typename DefaultGemm::Epilogue;
    using OutputTileIterator =
        typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
            typename DefaultEpilogue::OutputTileIterator>::Iterator;
    using AccumTileGmem = GmemTile<typename Mma::FragmentC, kNumThreads>;
  };

  struct MatmulDOIVJ {
    /*
    doi_t_vj = do_i @ v_j.transpose(-2, -1) # matmul
    tmp = (doi_t_vj - Di.unsqueeze(1)) * attn # inplace / epilogue?
    */
    using ThreadblockShape =
        cutlass::gemm::GemmShape<kBlockSizeI, kBlockSizeJ, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;

    using ElementC = output_t;
    using ElementAccum = accum_t;

    // no-op output op - epilogue just stores result to global memory
    using BiasGradEpilogueOutputOp =
        typename cutlass::epilogue::thread::LinearCombination<
            ElementC,
            DefaultConfig::EpilogueOutputOp::kCount,
            typename DefaultConfig::EpilogueOutputOp::ElementAccumulator,
            typename DefaultConfig::EpilogueOutputOp::ElementCompute,
            cutlass::epilogue::thread::ScaleType::Nothing>;

    using DefaultGemm = typename cutlass::gemm::kernel::DefaultGemm<
        scalar_t, // ElementA
        cutlass::layout::RowMajor, // LayoutA
        kIsAligned ? DefaultConfig::kAlignmentA : GemmType::kMinimumAlignment,
        scalar_t, // ElementB
        cutlass::layout::ColumnMajor, // LayoutB
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
        ElementC, // ElementC
        cutlass::layout::RowMajor, // LayoutC
        ElementAccum, // ElementAccumulator
        typename GemmType::OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        BiasGradEpilogueOutputOp, // EpilogueOutputOp
        void, // ThreadblockSwizzle (not used)
        DefaultConfig::kStages, // Stages
        false, // SplitKSerial
        typename GemmType::Operator,
        cutlass::gemm::SharedMemoryClearOption::kNone>;

    using Mma = typename cutlass::platform::conditional<
        kReuseDOi,
        // if we are reusing dOi from dVj computation, then do an MMA from shared
        // memory where we expect the entirety of dOi to be loaded in shared memory
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
                typename DefaultGemm::Mma,  // Mma
                kMaxK,
                typename cutlass::gemm::threadblock::DefaultWarpIteratorAFromBSharedMemory<
                    typename DefaultGemm::Mma::Operator::Shape,  // WarpShape
                    typename DefaultGemm::Mma::Operator::InstructionShape,
                    typename MatmulGradV::Mma::WarpIteratorB,  // RegularWarpIterator
                    typename DefaultGemm::Mma::Policy
                >::WarpIterator,
                false, // kScaleOperandA,
                false, // kForceSmemHoldEntireB
                false // kTransposeA
            >::Mma,
        // if we don't reuse dOi from dVj computation, then do a normal MMA
        // where dOi is reloaded from global memory
        typename MakeCustomMma<typename DefaultGemm::Mma, kMaxK>::Mma
    >::type;

    // epilogue used to write bias gradient, which is just the output of this
    // matmul with some operations applied to the fragment
    using BiasGradEpilogue = typename DefaultGemm::Epilogue;

    // Epilogue to store to shared-memory in a format that we can use later for
    // the second matmul
    using B2bGemm = typename cutlass::gemm::threadblock::B2bGemm<
        typename DefaultGemm::Mma::Operator::IteratorC,
        typename DefaultGemm::Mma::Operator,
        scalar_t,
        WarpShape,
        ThreadblockShape>;
    using AccumulatorSharedStorage = typename B2bGemm::AccumulatorSharedStorage;
  };

  struct MatmulGradQ {
    // grad_q <- tmp @ k_j
    using ThreadblockShape =
        cutlass::gemm::GemmShape<kBlockSizeI, kBlockSizeJ, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using InstructionShape = typename GemmType::InstructionShape;

    using DefaultGemm = cutlass::gemm::kernel::DefaultGemm<
        scalar_t, // ElementA,
        cutlass::layout::RowMajor, // LayoutA,
        DefaultConfig::kAlignmentA,
        scalar_t, // ElementB,
        cutlass::layout::RowMajor, // LayoutB,
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
        output_t,
        cutlass::layout::RowMajor, // LayoutC,
        accum_t,
        typename GemmType::OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        typename DefaultConfig::EpilogueOutputOp,
        void, // ThreadblockSwizzle - not used
        DefaultConfig::kStages,
        false, // SplitKSerial
        typename GemmType::Operator>;

    using WarpIteratorA = typename cutlass::gemm::threadblock::DefaultWarpIteratorAFromSharedMemory<
        typename DefaultGemm::Mma::Operator::Shape,
        typename DefaultGemm::Mma::Operator::InstructionShape,
        typename DefaultGemm::Mma::Operator::IteratorA,
        typename DefaultGemm::Mma::Policy>::WarpIterator;
    using DefaultMmaFromSmem =
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            MatmulDOIVJ::AccumulatorSharedStorage::Shape::kN,
            WarpIteratorA,
            false,  // kScaleOperandA
            false>; // kForceSmemHoldEntireB
    using Mma = typename DefaultMmaFromSmem::Mma;
    using IteratorB = typename Mma::IteratorB;
    using WarpCount = typename Mma::WarpCount;

    // Epilogue
    using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
    using DefaultEpilogue = typename DefaultGemm::Epilogue;
    using OutputTileIterator =
        typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
            typename DefaultEpilogue::OutputTileIterator>::Iterator;
    using AccumTileGmem = GmemTile<typename Mma::FragmentC, kNumThreads>;
  };
  struct MatmulGradK {
    // grad_k <- tmp.transpose(-2, -1) @ q_i
    using ThreadblockShape =
        cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using InstructionShape = typename GemmType::InstructionShape;

    using DefaultGemm = cutlass::gemm::kernel::DefaultGemm<
        scalar_t, // ElementA,
        cutlass::layout::RowMajor, // LayoutA,
        DefaultConfig::kAlignmentA,
        scalar_t, // ElementB,
        cutlass::layout::RowMajor, // LayoutB,
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
        output_t,
        cutlass::layout::RowMajor, // LayoutC,
        accum_t,
        typename GemmType::OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        typename DefaultConfig::EpilogueOutputOp,
        void, // ThreadblockSwizzle - not used
        DefaultConfig::kStages,
        false, // SplitKSerial
        typename GemmType::Operator>;

    using WarpIteratorA = typename cutlass::gemm::threadblock::DefaultWarpIteratorAFromSharedMemory<
        typename DefaultGemm::Mma::Operator::Shape,
        typename DefaultGemm::Mma::Operator::InstructionShape,
        typename DefaultGemm::Mma::Operator::IteratorA,
        typename DefaultGemm::Mma::Policy>::WarpIterator;
    using DefaultMmaFromSmemN =
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            MatmulQK::AccumulatorSharedStorage::Shape::kN, // kMaxK
            WarpIteratorA,
            false,  // kScaleOperandA
            false>; // kForceSmemHoldEntireB
    using DefaultMmaFromSmemT =
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            MatmulDOIVJ::AccumulatorSharedStorage::Shape::kM,  // kMaxK
            WarpIteratorA,
            false, // kScaleOperandA
            false, // kForceSmemHoldEntireB
            kPreloadMmas>; // kTransposeA
    using DefaultMmaFromSmem = typename cutlass::platform::conditional<
        DefaultMmaFromSmemT::kIsTransposedA,
        DefaultMmaFromSmemT,
        DefaultMmaFromSmemN>::type;
    using Mma = typename DefaultMmaFromSmem::Mma;
    using IteratorB = typename Mma::IteratorB;
    using WarpCount = typename Mma::WarpCount;

    // Epilogue
    using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
    using DefaultEpilogue = typename DefaultGemm::Epilogue;
    using OutputTileIterator =
        typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
            typename DefaultEpilogue::OutputTileIterator>::Iterator;
    using AccumTileGmem = GmemTile<typename Mma::FragmentC, kNumThreads>;
  };

  // shared storage for keeping Zij matrix. not needed if we aren't using
  // dropout, in which case we use an empty array to save shared memory
  using ZijSharedStorage = typename cutlass::platform::conditional<
      kApplyDropout,
      typename MatmulQK::AccumulatorSharedStorage,
      // dummy shared storage object that takes up no space.
      typename cutlass::gemm::threadblock::AccumulatorSharedStorage<
          typename cutlass::gemm::GemmShape<0, 0, 0>,
          typename MatmulQK::AccumulatorSharedStorage::Element,
          typename MatmulQK::AccumulatorSharedStorage::Layout,
          typename cutlass::MatrixShape<0, 0>>>::type;

  // See https://fburl.com/gsheet/l5bltspl
  // for an illustration of how smem is used
  struct SharedStoragePrologue {
    struct {
      cutlass::Array<accum_t, kBlockSizeI> di; // (do_i * o_i).sum(-1)
      typename MatmulQK::Mma::SharedStorageA mm_qk_k;
    } persistent;
    union {
      struct {
        // p1 - after Q.K / dV / dO.V
        union {
          // 1. efficient load of bias tile Bij, which is then applied to Pij
          typename MatmulQK::BiasLoader::SmemTile bias;
          // 4. store Pij. it is needed:
          // - in dVj += (Pij.T * Zij) @ dOi
          // - in dSij = Pij * (dPij - Di)
          // 6. dVj += (Pij.T * Zij) @ dOi
          // 10. write to fragment
          typename MatmulQK::AccumulatorSharedStorage attn_shared_storage;
        };
        // 5. store Zij. it is needed:
        // - to compute Pij_dropped = Pij * Zij on the fly as fragments of Pij
        // are loaded for the computation of dVj.
        // - to compute dPij = (dOi @ Vj.T) * Zij
        // 6. used in dVj += (Pij.T * Zij) @ dOi
        // 9. used in dPij = dPij_dropped * Zij
        ZijSharedStorage zij;

        union {
          // has shape (n_queries_block, head_dim)
          // 2. prologue for dVj (preload dOi)
          // 6. workspace for dVj += (Pij.T * Zij) @ dOi
          typename MatmulGradV::Mma::SharedStorage mm_gradV;
          // 7. dVj epilogue
          typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue;
        };

        // 3. prologue for dPij_dropped
        // 8. used in dPij_dropped = dOi @ Vj.T
        typename MatmulDOIVJ::Mma::SharedStorage mm_doivj;
      } p1;

      struct {
        // p2 - dQ
        union {
          typename MatmulQK::AccumulatorSharedStorage
              tmpT_shared_storage; // (from p1)
          typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
        };
        typename MatmulGradK::Mma::SharedStorage mm_gradK; // (preload)
        typename MatmulGradQ::Mma::SharedStorage mm_gradQ; // (preload)
        union {
          // store dB = dSij to global memory
          typename MatmulDOIVJ::BiasGradEpilogue::SharedStorage gradB_epilogue;
          typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue;
        };
      } p2;

      struct {
        // p3 - after last iteration on dQ's epilogue / dK
        union {
          typename MatmulQK::AccumulatorSharedStorage
              tmpT_shared_storage; // (from p1)
          typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
        };
        typename MatmulGradK::Mma::SharedStorage mm_gradK; // (preload)
        typename MatmulGradQ::DefaultEpilogue::SharedStorage
            gradQ_epilogue_lastIter;

        typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue;
      } p3;

      struct {
        // p4 - after last iteration on dK's epilogue / preload next K.Q_t
        typename MatmulQK::Mma::SharedStorageB mm_qk_q;

        // If we reach end of current key, dump RF->gmem with "final" epilogues
        typename MatmulGradK::DefaultEpilogue::SharedStorage
            gradK_epilogue_final;
        typename MatmulGradV::DefaultEpilogue::SharedStorage
            gradV_epilogue_final;
      } p4;
    };
    static void print_size() {
      // Field size
#define FSZ(f) int((sizeof(((SharedStorage*)0)->f)))

      printf("Total smem: %d bytes\n", int(sizeof(SharedStorage)));
      printf("  persistent: %db\n", FSZ(persistent));
      printf("    mm_qk_k: %db\n", FSZ(persistent.mm_qk_k));
      printf("  p1: %db\n", FSZ(p1));
      printf("    bias: %db\n", FSZ(p1.bias));
      printf("    attn_shared_storage: %db\n", FSZ(p1.attn_shared_storage));
      printf("    zij: %db\n", FSZ(p1.zij));
      printf("    mm_gradV: %db\n", FSZ(p1.mm_gradV));
      printf("    gradV_epilogue: %db\n", FSZ(p1.gradV_epilogue));
      printf("    mm_doivj: %db\n", FSZ(p1.mm_doivj));
      printf("  p2: %db\n", FSZ(p2));
      printf("    tmpT_shared_storage: %db\n", FSZ(p2.tmpT_shared_storage));
      printf("    tmp_shared_storage: %db\n", FSZ(p2.tmp_shared_storage));
      printf("    mm_gradK: %db\n", FSZ(p2.mm_gradK));
      printf("    mm_gradQ: %db\n", FSZ(p2.mm_gradQ));
      printf("    gradB_epilogue: %db\n", FSZ(p2.gradB_epilogue));
      printf("    gradQ_epilogue: %db\n", FSZ(p2.gradQ_epilogue));
      printf("  p3: %db\n", FSZ(p3));
      printf("    tmpT_shared_storage: %db\n", FSZ(p3.tmpT_shared_storage));
      printf("    tmp_shared_storage: %db\n", FSZ(p3.tmp_shared_storage));
      printf("    mm_gradK: %db\n", FSZ(p3.mm_gradK));
      printf("    gradQ_epilogue_lastIter: %db\n", FSZ(p3.gradQ_epilogue_lastIter));
      printf("    gradK_epilogue: %db\n", FSZ(p3.gradK_epilogue));
      printf("  p4: %db\n", FSZ(p4));
      printf("    mm_qk_q: %db\n", FSZ(p4.mm_qk_q));
      printf("    gradK_epilogue_final: %db\n", FSZ(p4.gradK_epilogue_final));
      printf("    gradV_epilogue_final: %db\n", FSZ(p4.gradV_epilogue_final));
    }
// ===========================================
#define FIELD(INSIDE_STRUCT, FIELDNAME) \
  CUTLASS_DEVICE auto& FIELDNAME() {    \
    return INSIDE_STRUCT.FIELDNAME;     \
  }

    FIELD(persistent, di)
    FIELD(persistent, mm_qk_k)
    FIELD(p1, bias)
    FIELD(p1, attn_shared_storage)
    FIELD(p1, zij)
    FIELD(p1, mm_gradV)
    FIELD(p1, gradV_epilogue)
    FIELD(p1, mm_doivj)
    FIELD(p2, mm_gradK)
    FIELD(p2, mm_gradQ)
    FIELD(p2, gradB_epilogue)
    FIELD(p2, gradQ_epilogue)
    FIELD(p2, tmp_shared_storage)
    FIELD(p3, tmpT_shared_storage)
    FIELD(p3, gradQ_epilogue_lastIter)
    FIELD(p3, gradK_epilogue)
    FIELD(p4, mm_qk_q)
    FIELD(p4, gradK_epilogue_final)
    FIELD(p4, gradV_epilogue_final)
  };

  struct SharedStorageNoPrologue {
    struct {
      cutlass::Array<accum_t, kBlockSizeI> di; // (do_i * o_i).sum(-1)
    } persistent;
    union {
      struct {
        // p1 - Q.K matmul
        typename MatmulQK::Mma::SharedStorageA mm_qk_k;
        typename MatmulQK::Mma::SharedStorageB mm_qk_q;
      } p1;

      struct {
        // p2 - compute dVj and dPij_dropped = dOi @ Vj.T
        union {
          // 1. efficient load of bias tile Bij, which is then applied to Pij
          typename MatmulQK::BiasLoader::SmemTile bias;
          // 2. store Pij to shared memory. it is needed:
          // - in this step, where it is used in dVj += (Pij.T * Zij) @ dOi
          // - in next step where it is used in dSij = Pij * (dPij - Di)
          typename MatmulQK::AccumulatorSharedStorage attn_shared_storage;
        };
        // 3. store Zij. it is needed:
        // - in this step, where it is used to compute Pij_dropped = Pij * Zij
        // on the
        //   fly as fragments of Pij are loaded for the computation of dVj.
        // - later to compute dPij = (dOi @ Vj.T) * Zij
        ZijSharedStorage zij;

        // 4. load (all of) dOi. needed:
        // - in this step: dVj += (Pij.T * Zij.T) @ dOi
        // - later step: dPij_dropped = dOi @ Vj.T
        typename MatmulGradV::Mma::SharedStorage mm_gradV;
        union {
          // 5. efficient write of dVj to global memory
          typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue;
          // 6. staging memory for loading Vj during dOi @ Vj
          typename MatmulDOIVJ::Mma::SharedStorage mm_doivj;
        };
      } p2;

      struct {
        // for efficient store of dB = dSij to global memory
        typename MatmulDOIVJ::BiasGradEpilogue::SharedStorage gradB_epilogue;
      } p3;

      struct {
        // p4 - compute gradQ
        typename MatmulQK::AccumulatorSharedStorage
            tmpT_shared_storage; // (from p2)
        typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
        union {
          typename MatmulGradQ::Mma::SharedStorage mm_gradQ;
          typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue;
          typename MatmulGradQ::DefaultEpilogue::SharedStorage
              gradQ_epilogue_lastIter;
        };
      } p4;

      struct {
        // p5 - compute gradK
        typename MatmulQK::AccumulatorSharedStorage
            tmpT_shared_storage; // (from p2)
        typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
        union {
          typename MatmulGradK::Mma::SharedStorage mm_gradK;
          typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue;
        };
      } p5;

      struct {
        // p6 - store RF accumulated into gmem
        typename MatmulGradK::DefaultEpilogue::SharedStorage
            gradK_epilogue_final;
        typename MatmulGradV::DefaultEpilogue::SharedStorage
            gradV_epilogue_final;
      } p6;
    };
    static void print_size() {
#define FIELD_SIZEOF(f) int((sizeof(((SharedStorage*)0)->f)))
      printf("Total smem: %d bytes\n", int(sizeof(SharedStorage)));
      printf("  persistent: %db\n", FIELD_SIZEOF(persistent));
      printf("  p1: %db\n", FIELD_SIZEOF(p1));
      printf("  p2: %db\n", FIELD_SIZEOF(p2));
      printf("  p3: %db\n", FIELD_SIZEOF(p3));
      printf("  p4: %db\n", FIELD_SIZEOF(p4));
      printf("  p5: %db\n", FIELD_SIZEOF(p5));
      printf("  p6: %db\n", FIELD_SIZEOF(p6));
    }
// ===========================================
#define FIELD(INSIDE_STRUCT, FIELDNAME) \
  CUTLASS_DEVICE auto& FIELDNAME() {    \
    return INSIDE_STRUCT.FIELDNAME;     \
  }

    FIELD(persistent, di)
    FIELD(p1, mm_qk_k)
    FIELD(p1, mm_qk_q)
    FIELD(p2, bias)
    FIELD(p2, attn_shared_storage)
    FIELD(p2, zij)
    FIELD(p2, mm_gradV)
    FIELD(p2, gradV_epilogue)
    FIELD(p2, mm_doivj)
    FIELD(p3, gradB_epilogue)
    FIELD(p4, tmpT_shared_storage)
    FIELD(p4, tmp_shared_storage)
    FIELD(p4, mm_gradQ)
    FIELD(p4, gradQ_epilogue)
    FIELD(p4, gradQ_epilogue_lastIter)
    FIELD(p5, mm_gradK)
    FIELD(p5, gradK_epilogue)
    FIELD(p6, gradK_epilogue_final)
    FIELD(p6, gradV_epilogue_final)
  };

  using SharedStorage = typename std::conditional<
      kPreloadMmas,
      SharedStoragePrologue,
      SharedStorageNoPrologue>::type;

  struct OutputFragments {
    typename MatmulGradV::Mma::FragmentC gradV;
    typename MatmulGradK::Mma::FragmentC gradK;

    CUTLASS_DEVICE void clear() {
      gradV.clear();
      gradK.clear();
    }
  };

  static bool __host__ check_supported(Params const& p) {
    CHECK_ALIGNED_PTR(p.query_ptr, kMinimumAlignment);
    CHECK_ALIGNED_PTR(p.key_ptr, kMinimumAlignment);
    CHECK_ALIGNED_PTR(p.value_ptr, kMinimumAlignment);
    CHECK_ALIGNED_PTR(p.output_ptr, kMinimumAlignment);
    CHECK_ALIGNED_PTR(p.grad_output_ptr, kMinimumAlignment);
    XFORMERS_CHECK(p.lse_strideM % 8 == 0, "LSE is not correctly aligned");
    XFORMERS_CHECK(
        p.q_strideH % kMinimumAlignment == 0, "query is not correctly aligned");
    XFORMERS_CHECK(
        p.k_strideH % kMinimumAlignment == 0, "key is not correctly aligned");
    XFORMERS_CHECK(
        p.v_strideH % kMinimumAlignment == 0, "value is not correctly aligned");
    return true;
  }

  static CUTLASS_DEVICE void kernel(Params const& p) {
    extern __shared__ char smem_buffer[];
    SharedStorage& shared_storage = *((SharedStorage*)smem_buffer);

    if (kPrologueQK) {
      prologueQkNextIteration<true>(shared_storage, p, 0, 0);
    }

    // Computes (dO*out).sum(-1) and writes it to `p.delta_ptr`
    if (kKernelComputesDelta) {
      constexpr int kOptimalElements =
          128 / cutlass::sizeof_bits<scalar_t>::value;
      if (p.head_dim_value % kOptimalElements == 0) {
        for (int query_start = 0; query_start < p.num_queries;
             query_start += kBlockSizeI) {
          computeDelta<kOptimalElements>(p, query_start);
        }
      } else {
        for (int query_start = 0; query_start < p.num_queries;
             query_start += kBlockSizeI) {
          computeDelta<1>(p, query_start);
        }
      }
      __syncthreads();
    }

    OutputFragments output_frags;

    curandStatePhilox4_32_10_t rng_state_init;
    if (kApplyDropout) {
      auto seeds = at::cuda::philox::unpack(p.rng_engine_inputs);
      // each element of the attention matrix P with shape
      // (batch_sz, n_heads, n_queries, n_keys) is associated with a single
      // offset in RNG sequence. we initialize the RNG state with offset that
      // starts at the beginning of a (n_queries, n_keys) matrix for this
      // block's batch_id and head_id
      // initializing rng state is very expensive, so we run once per kernel,
      // rather than once per iteration. each iteration takes a copy of the
      // initialized RNG state and offsets it as needed.
      curand_init(
          std::get<0>(seeds),
          0,
          std::get<1>(seeds) + p.dropout_batch_head_rng_offset,
          &rng_state_init);
    }

    int32_t key_start = 0;
    int32_t key_end = p.num_keys / kBlockSizeJ * kBlockSizeJ;
    for (; key_start < key_end; key_start += kBlockSizeJ) {
      output_frags.clear();
      int32_t query_start = getQueryStart(p, key_start);
      int32_t query_end = query_start +
          (p.num_queries - query_start) / kBlockSizeI * kBlockSizeI;
      for (; query_start < query_end; query_start += kBlockSizeI) {
        processBlockIJ<true>(
            shared_storage,
            output_frags,
            p,
            query_start,
            key_start,
            rng_state_init);
      }
      // last (partial) query
      if (query_start < p.num_queries) {
        processBlockIJ<false>(
            shared_storage,
            output_frags,
            p,
            query_start,
            key_start,
            rng_state_init);
      }
      if (kOutputInRF) {
        writeFragsToGmem<true>(shared_storage, output_frags, p, key_start);
      }
      __syncthreads();
    }
    // Last (partial) key
    if (key_start != p.num_keys) {
      output_frags.clear();
      for (int32_t query_start = getQueryStart(p, key_start);
           query_start < p.num_queries;
           query_start += kBlockSizeI) {
        processBlockIJ<false>(
            shared_storage,
            output_frags,
            p,
            query_start,
            key_start,
            rng_state_init);
      }
      if (kOutputInRF) {
        writeFragsToGmem<false>(shared_storage, output_frags, p, key_start);
      }
    }
  }

  static CUTLASS_DEVICE void loadDi(
      cutlass::Array<accum_t, kBlockSizeI>& di,
      Params const& p,
      int32_t query_start) {
    int32_t thread_id = threadIdx.x + threadIdx.y * blockDim.x;
    if (thread_id < kBlockSizeI) {
      accum_t di_rf = accum_t(0);
      if (query_start + thread_id < p.num_queries) {
        di_rf = p.delta_ptr[query_start + thread_id];
      }
      di[thread_id] = di_rf;
    }
  }

  template <bool skipBoundsChecks>
  static CUTLASS_DEVICE void processBlockIJ(
      SharedStorage& shared_storage,
      OutputFragments& output_frags,
      Params const& p,
      int32_t query_start,
      int32_t key_start,
      const curandStatePhilox4_32_10_t& curand_state_init) {
    cutlass::MatrixCoord no_offset{0, 0};
    accum_t scale = p.scale;
    int16_t thread_id = threadIdx.x + threadIdx.y * blockDim.x;
    int8_t warp_id = warp_uniform(threadIdx.y);
    int8_t lane_id = threadIdx.x;

    bool isFirstQuery =
        query_start == 0 || (p.causal && query_start <= key_start);
    int32_t next_query, next_key;
    incrIteration(p, query_start, key_start, next_query, next_key);
    bool isLastQuery = next_key != key_start;
    __syncthreads();
    loadDi(shared_storage.di(), p, query_start);

    int32_t num_queries_in_block = skipBoundsChecks
        ? MatmulQK::Mma::Shape::kN
        : std::min(
              (int32_t)MatmulQK::Mma::Shape::kN, p.num_queries - query_start);
    int32_t num_keys_in_block = skipBoundsChecks
        ? MatmulQK::Mma::Shape::kM
        : std::min((int32_t)MatmulQK::Mma::Shape::kM, p.num_keys - key_start);

    auto prologueGradV = [&](int col) {
      typename MatmulGradV::Mma::IteratorB iterator_dO(
          {int32_t(p.gO_strideM)},
          p.grad_output_ptr + query_start * p.gO_strideM + col,
          {num_queries_in_block, p.head_dim_value - col},
          thread_id,
          no_offset);
      MatmulGradV::Mma::prologue(
          shared_storage.mm_gradV(),
          iterator_dO,
          thread_id,
          num_queries_in_block);
    };
    auto prologueGradQ = [&](int col) {
      typename MatmulGradQ::Mma::IteratorB iterator_K(
          {int32_t(p.k_strideM)},
          p.key_ptr + key_start * p.k_strideM + col,
          {num_keys_in_block, p.head_dim - col},
          thread_id,
          no_offset);
      MatmulGradQ::Mma::prologue(
          shared_storage.mm_gradQ(), iterator_K, thread_id, num_keys_in_block);
    };
    auto prologueGradK = [&](int col) {
      typename MatmulGradK::Mma::IteratorB iterator_Q(
          {int32_t(p.q_strideM)},
          p.query_ptr + query_start * p.q_strideM + col,
          {num_queries_in_block, p.head_dim - col},
          thread_id,
          no_offset);
      MatmulGradK::Mma::prologue(
          shared_storage.mm_gradK(),
          iterator_Q,
          thread_id,
          num_queries_in_block);
    };
    auto prologueDOV = [&]() {
      typename MatmulDOIVJ::Mma::IteratorB iterator_B(
          {int32_t(p.v_strideM)},
          p.value_ptr + key_start * p.v_strideM,
          {p.head_dim_value, num_keys_in_block},
          thread_id,
          no_offset);
      MatmulDOIVJ::Mma::prologue(
          shared_storage.mm_doivj(),
          iterator_B,
          thread_id,
          p.head_dim_value);
    };

    /////////////////////////////////////////////////////////////////////////////////////////////////
    // MatmulQK
    /////////////////////////////////////////////////////////////////////////////////////////////////
    {
      using Mma = typename MatmulQK::Mma;

      cutlass::gemm::GemmCoord problem_size(
          num_keys_in_block,
          num_queries_in_block,
          p.head_dim // k
      );

      // k_j
      typename Mma::IteratorA iterator_A(
          {int32_t(p.k_strideM)},
          p.key_ptr + key_start * p.k_strideM,
          {problem_size.m(), problem_size.k()},
          thread_id,
          no_offset);

      // q_i.transpose(-2, -1)
      typename Mma::IteratorB iterator_B(
          {int32_t(p.q_strideM)},
          p.query_ptr + query_start * p.q_strideM,
          {problem_size.k(), problem_size.n()},
          thread_id,
          no_offset);

      Mma mma(
          shared_storage.mm_qk_k(),
          shared_storage.mm_qk_q(),
          thread_id,
          warp_id,
          lane_id);

      typename Mma::FragmentC accum;

      accum.clear();

      auto gemm_k_iterations =
          (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

      // Compute threadblock-scoped matrix multiply-add
      mma.set_prologue_done(kPrologueQK);
      mma.set_zero_outside_bounds(!skipBoundsChecks);
      mma(gemm_k_iterations, accum, iterator_A, iterator_B, accum);
      accum = cutlass::multiplies<typename Mma::FragmentC>()(scale, accum);

      // Epilogue: add LSE + exp and store that to our shared memory buffer
      // shmem <- (matmul_result -
      // logsumexp[i_start:i_end].unsqueeze(1)).exp()
      int warp_idx_mn_0 =
          warp_id % (Mma::Base::WarpCount::kM * Mma::Base::WarpCount::kN);
      auto output_tile_coords = cutlass::MatrixCoord{
          warp_idx_mn_0 % Mma::Base::WarpCount::kM,
          warp_idx_mn_0 / Mma::Base::WarpCount::kM};

      // apply bias if applicable
      if (p.bias_ptr != nullptr) {
        // load bias tile Bij into shared memory
        typename MatmulQK::BiasLoader::GmemTileIterator bias_iter(
            {cutlass::layout::RowMajor(p.bias_strideM)},
            p.bias_ptr + query_start * p.bias_strideM + key_start,
            {num_queries_in_block, num_keys_in_block},
            thread_id);
        cutlass::TensorRef<scalar_t, cutlass::layout::RowMajor> bias_tensor_ref(
            shared_storage.bias().data(),
            cutlass::layout::RowMajor(MatmulQK::ThreadblockShape::kM));
        typename MatmulQK::BiasLoader::SmemTileIterator smem_tile_iter(
            bias_tensor_ref, thread_id);
        MatmulQK::BiasLoader::load(bias_iter, smem_tile_iter);

        // Pij += Bij, where Pij is in register fragment and Bij is in shmem
        auto lane_offset = MatmulQK::ScalingCoefsUpdater::get_lane_offset(
            lane_id, warp_id, output_tile_coords);
        MatmulQK::ScalingCoefsUpdater::iterateRows(
            lane_offset,
            [&](int accum_n) {},
            [&](int accum_m, int accum_n, int idx) {
              // remember we are transposed
              if (skipBoundsChecks ||
                  (accum_n < num_queries_in_block &&
                   accum_m < num_keys_in_block)) {
                accum[idx] += bias_tensor_ref.at({accum_n, accum_m});
              }
            },
            [&](int accum_n) {});
      }

      // Apply mask
      if (p.causal) {
        auto lane_offset = MatmulQK::ScalingCoefsUpdater::get_lane_offset(
            lane_id, warp_id, output_tile_coords);
        MatmulQK::ScalingCoefsUpdater::iterateRows(
            lane_offset,
            [&](int accum_m) {},
            [&](int accum_m, int accum_n, int idx) {
              // (don't forget we are transposed!)
              if (accum_m > accum_n + query_start - key_start) {
                accum[idx] = -std::numeric_limits<accum_t>::infinity();
              }
            },
            [&](int accum_m) {});
      }

      __syncthreads();
      if (kPrologueGV) {
        prologueGradV(0);
      }
      if (kPrologueDOV) {
        prologueDOV();
      }
      MatmulQK::B2bGemm::accumApplyLSEToSmem(
          shared_storage.attn_shared_storage(),
          accum,
          p.logsumexp_ptr + query_start,
          problem_size.n(),
          thread_id,
          warp_id,
          lane_id,
          output_tile_coords);

      // if we are using dropout, compute Zij, writing it to shared memory.
      // each element of Zij is:
      // - 0 with probability dropout_p
      // - 1 / (1 - dropout_p) with probability 1 - dropout_p
      if (kApplyDropout) {
        auto zij = shared_storage.zij().accum_ref();
        // each thread generates a contiguous sequence of elements in Zij, all
        // in the same row. the reason they have to come from the same row is
        // that sampling random numbers from a contiguous random number sequence
        // is much more efficient than jumping around, and the linear offset of
        // each element of Z (the global matrix) maps to an offset in a random
        // number sequence. for Z, the end of a row and the beginning of the
        // next have adjacent offsets, but for Zij (tile of global matrix), this
        // is not necessarily the case.
        const int num_threads = blockDim.x * blockDim.y * blockDim.z;
        const int threads_per_row = cutlass::fast_min(
            num_threads / num_queries_in_block, num_keys_in_block);
        const int elts_per_thread = cutlass::round_nearest(
            cutlass::ceil_div(num_keys_in_block, threads_per_row), 4);

        const int thread_i = thread_id / threads_per_row;
        const int thread_start_j =
            (thread_id % threads_per_row) * elts_per_thread;

        if (thread_i < num_queries_in_block &&
            thread_start_j < num_keys_in_block) {
          curandStatePhilox4_32_10_t curand_state = curand_state_init;
          skipahead(
              (query_start + thread_i) * p.num_keys +
                  (key_start + thread_start_j),
              &curand_state);
          const float dropout_scale = 1.0 / (1.0 - p.dropout_prob);

          // generate elements of Zij, 4 elements at a time
          for (int zij_start_col_idx = thread_start_j; zij_start_col_idx <
               cutlass::fast_min(thread_start_j + elts_per_thread,
                                 num_keys_in_block);
               zij_start_col_idx += 4) {
            const float4 rand_uniform_quad = curand_uniform4(&curand_state);

            CUTLASS_PRAGMA_UNROLL
            for (int quad_idx = 0; quad_idx < 4; ++quad_idx) {
              // we'll write Zij transposed since attention is also transposed
              // during the matmul to compute dV.
              zij.at({zij_start_col_idx + quad_idx, thread_i}) =
                  static_cast<scalar_t>(
                      dropout_scale *
                      ((&rand_uniform_quad.x)[quad_idx] > p.dropout_prob));
            }
          }
        }
      }
      __syncthreads();
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////
    // GradV matmul
    //
    // grad_v[j_start:j_end] += attn_T @ do_i
    /////////////////////////////////////////////////////////////////////////////////////////////////
    for (int col = 0; col < (kOutputInRF ? 1 : p.head_dim_value);
         col += MatmulGradV::ThreadblockShape::kN) {
      using Mma = typename MatmulGradV::Mma;
      using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

      cutlass::gemm::GemmCoord problem_size(
          num_keys_in_block, p.head_dim_value - col, num_queries_in_block);
      auto createEpilogueIter = [&]() {
        return typename MatmulGradV::OutputTileIterator(
            typename MatmulGradV::OutputTileIterator::Params{p.gV_strideM()},
            p.grad_value_ptr + key_start * p.gV_strideM() + col,
            {num_keys_in_block, p.head_dim_value - col},
            thread_id);
      };
      typename Mma::IteratorB iterator_B(
          {int32_t(p.gO_strideM)},
          p.grad_output_ptr + query_start * p.gO_strideM + col,
          {num_queries_in_block, p.head_dim_value - col},
          thread_id,
          no_offset);
      // PRINT_T0_L0("dVj matmul setup for col %d", col);
      // PRINT_T0_L0("  TbShape: %s", __get_type_name<MatmulGradV::ThreadblockShape>().data);
      // PRINT_T0_L0("  MaxK: %d", Mma::Base::kMaxK);
      // PRINT_T0_L0("  kK: %d", Mma::Base::Shape::kK);
      // PRINT_T0_L0("  kStages: %d", Mma::Base::kStages);
      // PRINT_T0_L0("  SmemShape: %s", __get_type_name<Mma::Base::SharedStorage::ShapeB>().data);
      // PRINT_T0_L0("  kSmemContainsEntireB: %d", Mma::Base::kSmemContainsEntireB);

      // if dropout: dVj += (Pij.T * Zij) @ dOi
      // otherwise:  dVj += Pij.T @ dOi
      Mma mma(
          // operand A: Pij.T
          shared_storage.attn_shared_storage().accum_ref(),
          // operand A_scale Zij.T:
          // if we're using dropout, operand A is Pij_dropped.T = Pij.T * Zij.T
          // which is computed on the fly as fragments of Pij.T are loaded in
          shared_storage.zij().accum_ref(),
          // operand B: dOi - which was loaded into shared memory previously
          // when we computed dVj
          shared_storage.mm_gradV().operand_B_ref(),
          thread_id,
          warp_id,
          lane_id);

      int storage_id = col / MatmulGradV::ThreadblockShape::kN;
      AccumTileGmem gmem_tile{
          p.workspace_gv + storage_id * AccumTileGmem::kElementsStored};
      if (!kOutputInRF) {
        if (isFirstQuery || !kNeedsAccumGradV) {
          output_frags.gradV.clear();
        } else {
          gmem_tile.load(output_frags.gradV, thread_id);
        }
      }
      mma.set_prologue_done(kPrologueGV);

      auto gemm_k_iterations =
          (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

      // Compute threadblock-scoped matrix multiply-add
      __syncthreads();

      mma(gemm_k_iterations,
          output_frags.gradV,
          iterator_B,
          output_frags.gradV);
      __syncthreads();
      if (kPrologueGV &&
          col + MatmulGradV::ThreadblockShape::kN < p.head_dim_value) {
        prologueGradV(col + MatmulGradV::ThreadblockShape::kN);
      }

      if (!kOutputInRF) {
        if (kNeedsAccumGradV && !isLastQuery) {
          gmem_tile.store(output_frags.gradV, thread_id);
        } else {
          accumulateInGmem<MatmulGradV>(
              shared_storage.gradV_epilogue(),
              output_frags.gradV,
              createEpilogueIter(),
              isFirstQuery || kNeedsAccumGradV);
        }
      }
    }
    __syncthreads();
    /////////////////////////////////////////////////////////////////////////////////////////////////
    // MatmulDOIVJ
    /////////////////////////////////////////////////////////////////////////////////////////////////
    {
      using Mma = typename MatmulDOIVJ::Mma;
      // v_j.transpose(-2, -1)
      typename Mma::IteratorB iterator_B(
          {int32_t(p.v_strideM)},
          p.value_ptr + key_start * p.v_strideM,
          {p.head_dim_value, num_keys_in_block},
          thread_id,
          no_offset);

      // PRINT_T0_L0("dOi before use in dOi @ Vj");
      // print_tensor_ref(shared_storage.mm_gradV().operand_B_ref(), num_queries_in_block, p.head_dim);

      // PRINT_T0_L0("dOi @ Vj matmul setup");
      // PRINT_T0_L0("  MaxK: %d", Mma::Base::kMaxK);
      // PRINT_T0_L0("  kK: %d", Mma::Base::Shape::kK);
      // PRINT_T0_L0("  kStages: %d", Mma::Base::kStages);
      // PRINT_T0_L0("  kSmemContainsEntireB: %d", Mma::Base::kSmemContainsEntireB);
      Mma mma(
          // holds dOi (loaded during dVj matmul)
          shared_storage.mm_gradV().operand_B_ref(),
          // used for loading tiles of Vj to shared memory
          shared_storage.mm_doivj().operand_B_ref(),
          thread_id,
          warp_id,
          lane_id);
      mma.set_prologue_done(kPrologueDOV);

      typename Mma::FragmentC accum;

      accum.clear();

      auto gemm_k_iterations =
          (p.head_dim_value + Mma::Shape::kK - 1) / Mma::Shape::kK;

      // PRINT_T0_L0("mma specs:");
      // PRINT_T0_L0("Shape::kK: %d", Mma::Base::Shape::kK);
      // PRINT_T0_L0("kStages: %d", Mma::Base::kStages);
      // PRINT_T0_L0("Shmem: %s", __get_type_name<decltype(shared_storage.mm_gradV().operand_B_ref())>().data);

      // PRINT_T0_L0("WarpIter: %s", __get_type_name<typename Mma::WarpIteratorA>().data);
      // PRINT_T0_L0("WarpIter::InstructionShape: %s", __get_type_name<typename Mma::WarpIteratorA::InstructionShape>().data);
      // PRINT_T0_L0("WarpIter::InstructionCount: %s", __get_type_name<typename Mma::WarpIteratorA::InstructionCount>().data);

      // Compute threadblock-scoped matrix multiply-add
      mma(gemm_k_iterations, accum, iterator_B, accum);
      __syncthreads();
      if (kPrologueGQ) {
        prologueGradQ(0);
      }
      if (kPrologueGK) {
        prologueGradK(0);
      }

      int warp_idx_mn_0 =
          warp_id % (Mma::Base::WarpCount::kM * Mma::Base::WarpCount::kN);
      auto output_tile_coords = cutlass::MatrixCoord{
          warp_idx_mn_0 % Mma::Base::WarpCount::kM,
          warp_idx_mn_0 / Mma::Base::WarpCount::kM};
      // TODO: This must be terribly inefficient. There must be a better way
      // tmp [RF] <- (accum [RF] - Di [smem] ) * attn_T.T [smem]
      // attn_shared_storage  [smem] <- tmp.T
      // tmp_shared_storage [smem] <- tmp
      {
        using RegistersIter = typename DefaultAttentionScalingCoefsUpdater<
            typename Mma::Operator::IteratorC,
            typename MatmulDOIVJ::ElementAccum,
            kWarpSize>::Updater;
        auto lane_offset = RegistersIter::get_lane_offset(
            lane_id, warp_id, output_tile_coords);

        // if dropout was used, compute dPij = dPij_dropped * Zij
        // Zij was written to shared memory earlier, and the elementwise
        // multiplication occurs on a fragment of dPij_dropped
        if (kApplyDropout) {
          const auto zij = shared_storage.zij().accum_ref();

          RegistersIter::iterateRows(
              lane_offset,
              [&](int accum_m) {},
              [&](int accum_m, int accum_n, int idx) {
                const int global_query_idx = query_start + accum_m;
                const int global_key_idx = key_start + accum_n;

                if (skipBoundsChecks ||
                    (global_query_idx < p.num_queries &&
                     global_key_idx < p.num_keys)) {
                  accum[idx] *= zij.at({accum_n, accum_m});
                }
              },
              [&](int accum_m) {});
        }

        auto attn_T = shared_storage.attn_shared_storage().accum_ref();
        accum_t current_di;
        typename Mma::FragmentC fragment_attn, fragment_di;
        RegistersIter::iterateRows(
            lane_offset,
            [&](int accum_m) { current_di = shared_storage.di()[accum_m]; },
            [&](int accum_m, int accum_n, int idx) {
              // TODO: Otherwise we can get nans as we
              // might have infs here (only seen on f16 tho)
              if (skipBoundsChecks ||
                  (accum_m < num_queries_in_block &&
                   accum_n < num_keys_in_block)) {
                fragment_attn[idx] = attn_T.at({accum_n, accum_m});
              } else {
                fragment_attn[idx] = 0;
              }
              fragment_di[idx] = current_di;
            },
            [&](int accum_m) {

            });
        // dSij = (dPij - Di) * Pij
        accum = (accum - fragment_di) * fragment_attn;

        // store bias gradient tile dBij to global memory,
        // where dBij = dSij = Pij * (dPij - Di)
        if (p.grad_bias_ptr != nullptr) {
          typename MatmulDOIVJ::BiasGradEpilogue::OutputTileIterator
              output_iter(
                  typename MatmulDOIVJ::BiasGradEpilogue::OutputTileIterator::
                      Params{p.gB_strideM},
                  // grad_bias_ptr is offset to point at beginning of
                  // matrix of shape (queries, keys) for a given
                  // (batch_id, head_id) the pointer arithmetic here produces
                  // a pointer to the start of the current tile within that
                  // matrix
                  p.grad_bias_ptr + query_start * p.gB_strideM + key_start,
                  {num_queries_in_block, num_keys_in_block},
                  thread_id);

          // no-op epilogue operator - just casting and storing contents of
          // accum to global memory
          typename MatmulDOIVJ::BiasGradEpilogue::OutputOp output_op({1, 1});
          typename MatmulDOIVJ::BiasGradEpilogue epilogue(
              shared_storage.gradB_epilogue(), thread_id, warp_id, lane_id);
          epilogue(output_op, output_iter, accum, output_iter);
        }

        accum = accum * scale;

        __syncthreads();
        if (!MatmulGradK::DefaultMmaFromSmem::kIsTransposedA) {
          auto tmpT = shared_storage.tmpT_shared_storage().accum_ref();
          // attn <- attn_T.T
          RegistersIter::iterateRows(
              lane_offset,
              [&](int accum_m) {},
              [&](int accum_m, int accum_n, int idx) {
                tmpT.at({accum_n, accum_m}) = scalar_t(accum[idx]);
              },
              [&](int accum_m) {});
        }
      }

      MatmulDOIVJ::B2bGemm::accumToSmem(
          shared_storage.tmp_shared_storage(),
          accum,
          lane_id,
          output_tile_coords);
      __syncthreads();
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////
    // GradQ matmul
    //
    // grad_q[i_start:i_end] += tmp @ k_j
    /////////////////////////////////////////////////////////////////////////////////////////////////
    for (int col = 0; col < p.head_dim;
         col += MatmulGradQ::ThreadblockShape::kN) {
      using Mma = typename MatmulGradQ::Mma;
      using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

      cutlass::gemm::GemmCoord problem_size(
          num_queries_in_block,
          false ? MatmulGradQ::ThreadblockShape::kN : p.head_dim - col,
          num_keys_in_block);

      // k_j
      typename Mma::IteratorB iterator_B(
          {int32_t(p.k_strideM)},
          p.key_ptr + key_start * p.k_strideM + col,
          {problem_size.k(), problem_size.n()},
          thread_id,
          no_offset);

      Mma mma(
          // operand A: dSij
          shared_storage.tmp_shared_storage().accum_ref(),
          // operand B: Kj
          shared_storage.mm_gradQ().operand_B_ref(),
          thread_id,
          warp_id,
          lane_id);

      typename Mma::FragmentC accum;

      bool isFirst = key_start == 0;
      int col_id = col / MatmulGradQ::ThreadblockShape::kN;
      int storage_id =
          (col_id +
           query_start / kBlockSizeI *
               ceil_div(p.head_dim, MatmulGradQ::ThreadblockShape::kN));
      AccumTileGmem gmem_tile{
          p.workspace_gq + storage_id * AccumTileGmem::kElementsStored};
      if (isFirst || !kNeedsAccumGradQ) {
        accum.clear();
      } else {
        gmem_tile.load(accum, thread_id);
      }

      auto gemm_k_iterations =
          (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

      // Compute threadblock-scoped matrix multiply-add
      __syncthreads();
      mma.set_prologue_done(kPrologueGQ);
      mma(gemm_k_iterations, accum, iterator_B, accum);
      __syncthreads();
      bool isLastColumn = col + MatmulGradQ::ThreadblockShape::kN >= p.head_dim;
      if (kPrologueGQ && !isLastColumn) {
        prologueGradQ(col + MatmulGradQ::ThreadblockShape::kN);
      }

      // Output results
      int32_t next_query, next_key;
      incrIteration(p, p.num_queries, key_start, next_query, next_key);
      bool isLast =
          (p.causal && next_query > query_start) || next_key >= p.num_keys;
      if (kNeedsAccumGradQ && !isLast) {
        gmem_tile.store(accum, thread_id);
      } else {
        typename MatmulGradQ::OutputTileIterator output_it(
            typename MatmulGradQ::OutputTileIterator::Params{p.gQ_strideM()},
            p.grad_query_ptr + query_start * p.gQ_strideM() + col,
            {problem_size.m(), problem_size.n()},
            thread_id);
        accumulateInGmem<MatmulGradQ>(
            isLastColumn ? shared_storage.gradQ_epilogue_lastIter()
                         : shared_storage.gradQ_epilogue(),
            accum,
            output_it,
            isFirst || kNeedsAccumGradQ);
      }
    }
    /////////////////////////////////////////////////////////////////////////////////////////////////
    // GradK matmul
    //
    // grad_k[i_start:i_end] += tmp.transpose(-2, -1) @ q_i
    /////////////////////////////////////////////////////////////////////////////////////////////////
    for (int col = 0; col < (kOutputInRF ? 1 : p.head_dim);
         col += MatmulGradK::ThreadblockShape::kN) {
      using Mma = typename MatmulGradK::Mma;
      using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

      cutlass::gemm::GemmCoord problem_size(
          num_keys_in_block,
          false ? MatmulGradK::ThreadblockShape::kN : p.head_dim - col,
          num_queries_in_block);
      auto createEpilogueIter = [&]() {
        return typename MatmulGradK::OutputTileIterator(
            typename MatmulGradK::OutputTileIterator::Params{p.gK_strideM()},
            p.grad_key_ptr + key_start * p.gK_strideM() + col,
            {num_keys_in_block,
             false ? MatmulGradK::ThreadblockShape::kN : p.head_dim - col},
            thread_id);
      };

      // q_i
      typename Mma::IteratorB iterator_B(
          {int32_t(p.q_strideM)},
          p.query_ptr + query_start * p.q_strideM + col,
          {problem_size.k(), problem_size.n()},
          thread_id,
          no_offset);

      auto getTmp = [&](int) { return &shared_storage.tmp_shared_storage(); };
      auto getTmpT = [&](int) { return &shared_storage.tmpT_shared_storage(); };
      // this is basically:
      // opA = kIsTransposedA ? getTmp() : getTmpT();
      bool constexpr kIsTransposedA =
          MatmulGradK::DefaultMmaFromSmem::kIsTransposedA;
      auto& opA = *call_conditional<
          kIsTransposedA,
          decltype(getTmp),
          decltype(getTmpT)>::apply(getTmp, getTmpT, 0);
      Mma mma(
          // operand A: dSij.T
          opA.accum_ref(),
          // operand B: Qi
          shared_storage.mm_gradK().operand_B_ref(),
          thread_id,
          warp_id,
          lane_id);

      int storage_id = col / MatmulGradK::ThreadblockShape::kN;
      AccumTileGmem gmem_tile{
          p.workspace_gk + storage_id * AccumTileGmem::kElementsStored};
      if (!kOutputInRF) {
        if (isFirstQuery || !kNeedsAccumGradK) {
          output_frags.gradK.clear();
        } else {
          gmem_tile.load(output_frags.gradK, thread_id);
        }
      }
      mma.set_prologue_done(kPrologueGK);

      auto gemm_k_iterations =
          (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

      // Compute threadblock-scoped matrix multiply-add
      __syncthreads();

      mma(gemm_k_iterations,
          output_frags.gradK,
          iterator_B,
          output_frags.gradK);
      __syncthreads();
      bool isLastColumn = col + MatmulGradK::ThreadblockShape::kN >= p.head_dim;
      if (kPrologueGK && !isLastColumn) {
        prologueGradK(col + MatmulGradK::ThreadblockShape::kN);
      }

      if (kPrologueQK && isLastColumn) {
        int32_t next_query, next_key;
        incrIteration(p, query_start, key_start, next_query, next_key);
        DISPATCH_BOOL(next_key != key_start, kForceReloadK, ([&]() {
                        prologueQkNextIteration<kForceReloadK>(
                            shared_storage, p, next_query, next_key);
                      }));
      }

      // Output results
      if (!kOutputInRF) {
        if (kNeedsAccumGradK && !isLastQuery) {
          gmem_tile.store(output_frags.gradK, thread_id);
        } else {
          accumulateInGmem<MatmulGradK>(
              isLastColumn ? shared_storage.gradK_epilogue_final()
                           : shared_storage.gradK_epilogue(),
              output_frags.gradK,
              createEpilogueIter(),
              isFirstQuery || kNeedsAccumGradK);
        }
      }
    }
  }

  static CUTLASS_DEVICE int32_t
  getQueryStart(Params const& p, int32_t key_start) {
    if (p.causal) {
      return (key_start / kBlockSizeI) * kBlockSizeI;
    }
    return 0;
  };

  static CUTLASS_DEVICE void incrIteration(
      Params const& p,
      int32_t query_start,
      int32_t key_start,
      int32_t& next_query,
      int32_t& next_key) {
    next_query = query_start + kBlockSizeI;
    next_key = key_start;
    if (next_query >= p.num_queries) {
      next_key = key_start + kBlockSizeJ;
      next_query = getQueryStart(p, next_key);
    }
  }

  template <bool kForceReloadK>
  static CUTLASS_DEVICE void prologueQkNextIteration(
      SharedStorage& shared_storage,
      Params const& p,
      int32_t query_start,
      int32_t key_start) {
    if (query_start >= p.num_queries || key_start >= p.num_keys) {
      return;
    }

    static constexpr bool kReloadK =
        kForceReloadK || !MatmulQK::Mma::kSmemContainsEntireMat;
    auto thread_id = get_thread_id();
    typename MatmulQK::Mma::IteratorA iterator_A(
        {int32_t(p.k_strideM)},
        p.key_ptr + key_start * p.k_strideM,
        {p.num_keys - key_start, p.head_dim},
        thread_id,
        cutlass::MatrixCoord{0, 0});

    typename MatmulQK::Mma::IteratorB iterator_B(
        {int32_t(p.q_strideM)},
        p.query_ptr + query_start * p.q_strideM,
        {p.head_dim, p.num_queries - query_start},
        thread_id,
        cutlass::MatrixCoord{0, 0});

    MatmulQK::Mma::prologue<kReloadK, true>(
        shared_storage.mm_qk_k(),
        shared_storage.mm_qk_q(),
        iterator_A,
        iterator_B,
        thread_id,
        p.head_dim);
  }

  template <bool skipBoundsChecks>
  static CUTLASS_DEVICE void writeFragsToGmem(
      SharedStorage& shared_storage,
      OutputFragments& output_frags,
      Params const& p,
      int32_t key_start) {
    int32_t num_keys_in_block = skipBoundsChecks
        ? MatmulQK::Mma::Shape::kM
        : std::min((int32_t)MatmulQK::Mma::Shape::kM, p.num_keys - key_start);
    typename MatmulGradV::OutputTileIterator outputV_it(
        typename MatmulGradV::OutputTileIterator::Params{p.gV_strideM()},
        p.grad_value_ptr + key_start * p.gV_strideM(),
        {num_keys_in_block, p.head_dim_value},
        get_thread_id());
    accumulateInGmem<MatmulGradV>(
        shared_storage.gradV_epilogue_final(),
        output_frags.gradV,
        outputV_it,
        true);

    typename MatmulGradK::OutputTileIterator outputK_it(
        typename MatmulGradK::OutputTileIterator::Params{p.gK_strideM()},
        p.grad_key_ptr + key_start * p.gK_strideM(),
        {num_keys_in_block,
         false ? MatmulGradK::ThreadblockShape::kN : p.head_dim},
        get_thread_id());
    accumulateInGmem<MatmulGradK>(
        shared_storage.gradK_epilogue_final(),
        output_frags.gradK,
        outputK_it,
        true);
  }

  template <typename MatmulT>
  static CUTLASS_DEVICE void accumulateInGmem(
      typename MatmulT::DefaultEpilogue::SharedStorage& epilogue_smem,
      typename MatmulT::Mma::FragmentC const& accum,
      typename MatmulT::OutputTileIterator output_it,
      bool first) {
    using DefaultEpilogue = typename MatmulT::DefaultEpilogue;
    using DefaultOutputOp = typename MatmulT::DefaultOutputOp;
    using Mma = typename MatmulT::Mma;
    DISPATCH_BOOL(
        first, kIsFirst, ([&]() {
          static constexpr auto ScaleType = kIsFirst
              ? cutlass::epilogue::thread::ScaleType::Nothing
              : cutlass::epilogue::thread::ScaleType::NoBetaScaling;
          using EpilogueOutputOp =
              typename cutlass::epilogue::thread::LinearCombination<
                  typename DefaultOutputOp::ElementOutput,
                  DefaultOutputOp::kCount,
                  typename DefaultOutputOp::ElementAccumulator,
                  typename DefaultOutputOp::ElementCompute,
                  ScaleType>;
          using Epilogue =
              typename cutlass::epilogue::threadblock::EpiloguePipelined<
                  typename DefaultEpilogue::Shape,
                  typename Mma::Operator,
                  DefaultEpilogue::kPartitionsK,
                  typename MatmulT::OutputTileIterator,
                  typename DefaultEpilogue::AccumulatorFragmentIterator,
                  typename DefaultEpilogue::WarpTileIterator,
                  typename DefaultEpilogue::SharedLoadIterator,
                  EpilogueOutputOp,
                  typename DefaultEpilogue::Padding,
                  DefaultEpilogue::kFragmentsPerIteration,
                  true // IterationsUnroll
                  >;
          EpilogueOutputOp rescale({1, 1});
          Epilogue epilogue(
              epilogue_smem, get_thread_id(), get_warp_id(), get_lane_id());
          epilogue(rescale, output_it, accum, output_it);
        }));
  }

  template <int kElementsPerAccess>
  static CUTLASS_DEVICE void computeDelta(
      Params const& p,
      int32_t query_start) {
    // Each thread computes one value for Delta
    // Depending on warp configuration, we might have multiple
    // threads of the same warp working on the same row
    using AccessType = cutlass::Array<scalar_t, kElementsPerAccess>;
    static_assert(kNumThreads >= kBlockSizeI, "");
    static constexpr int kNumThreadsPerLine = kNumThreads / kBlockSizeI;
    int16_t thread_id = get_thread_id();

    int16_t laneFirstCol =
        kElementsPerAccess * (get_lane_id() % kNumThreadsPerLine);
    int16_t laneRow = thread_id / kNumThreadsPerLine;
    bool rowPred = (query_start + laneRow) < p.num_queries;
    bool pred = rowPred;

    // on windows, previous syntax __restrict__ AccessType*
    // resulted in error: "restrict" is not allowed
    const AccessType* __restrict__ grad_output_ptr =
        reinterpret_cast<const AccessType*>(
            p.grad_output_ptr + (query_start + laneRow) * p.gO_strideM +
            laneFirstCol);
    const AccessType* __restrict__ output_ptr =
        reinterpret_cast<const AccessType*>(
            p.output_ptr + (query_start + laneRow) * p.o_strideM() +
            laneFirstCol);

    static constexpr int64_t kMaxIters =
        kMaxK / (kElementsPerAccess * kNumThreadsPerLine);
    constexpr int kPipelineStages = 2;
    accum_t delta_value = accum_t(0);
    using GlobalLoad =
        cutlass::arch::global_load<AccessType, sizeof(AccessType)>;
    AccessType frag_grad_output[kPipelineStages];
    AccessType frag_output[kPipelineStages];

    auto loadAndIncrement = [&](int ld_pos, bool is_valid) {
      frag_grad_output[ld_pos].clear();
      frag_output[ld_pos].clear();
      GlobalLoad(frag_grad_output[ld_pos], grad_output_ptr, is_valid);
      GlobalLoad(frag_output[ld_pos], output_ptr, is_valid);
      grad_output_ptr += kNumThreadsPerLine;
      output_ptr += kNumThreadsPerLine;
    };

    CUTLASS_PRAGMA_UNROLL
    for (int iter = 0; iter < kPipelineStages - 1; ++iter) {
      int ld_pos = iter % kPipelineStages;
      pred = pred &&
          (laneFirstCol + iter * kElementsPerAccess * kNumThreadsPerLine) <
              p.head_dim_value;
      loadAndIncrement(ld_pos, pred);
    }
    auto columnIteration = [&](int iter) {
      // Load for next iter
      int ld_pos = (iter + kPipelineStages - 1) % kPipelineStages;
      pred = pred &&
          (laneFirstCol +
           (iter + kPipelineStages - 1) * kElementsPerAccess *
               kNumThreadsPerLine) < p.head_dim_value;
      loadAndIncrement(ld_pos, pred);
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < AccessType::kElements; ++i) {
        delta_value += accum_t(frag_output[iter % kPipelineStages][i]) *
            accum_t(frag_grad_output[iter % kPipelineStages][i]);
      }
    };

    // If we have a small lower-bound for K, we can unroll the loop
    if (kMaxK <= 256) {
      CUTLASS_PRAGMA_UNROLL
      for (int iter = 0; iter < kMaxIters; ++iter) {
        columnIteration(iter);
      }
    } else {
      int num_iters =
          ceil_div(p.head_dim_value, kElementsPerAccess * kNumThreadsPerLine) *
          (kElementsPerAccess * kNumThreadsPerLine);
      for (int iter = 0; iter < num_iters; ++iter) {
        columnIteration(iter);
      }
    }

    // Reduce between workers
    static_assert(
        kNumThreadsPerLine == 1 || kNumThreadsPerLine == 2 ||
            kNumThreadsPerLine == 4,
        "");
    CUTLASS_PRAGMA_UNROLL
    for (int i = 1; i < kNumThreadsPerLine; i *= 2) {
      delta_value = delta_value + __shfl_xor_sync(0xffffffff, delta_value, i);
    }

    // Store in gmem
    if (rowPred) {
      p.delta_ptr[query_start + laneRow] = delta_value;
    }
  }

  static CUTLASS_DEVICE int8_t get_lane_id() {
    return threadIdx.x;
  }
  static CUTLASS_DEVICE int8_t get_warp_id() {
    return threadIdx.y;
  }
  static CUTLASS_DEVICE int16_t get_thread_id() {
    return threadIdx.x + threadIdx.y * blockDim.x;
  }
};

template <typename AK>
__global__ void __launch_bounds__(AK::kNumThreads, AK::kMinBlocksPerSm)
    attention_kernel_backward_batched(typename AK::Params params);

#define _ATTENTION_KERNEL_BACKWARD_BEGIN(...)                 \
  template <>                                                 \
  __global__ void __launch_bounds__(                          \
      __VA_ARGS__::kNumThreads, __VA_ARGS__::kMinBlocksPerSm) \
      attention_kernel_backward_batched<__VA_ARGS__>(         \
          typename __VA_ARGS__::Params p) {                   \
    using Kernel = __VA_ARGS__;
#define _ATTENTION_KERNEL_BACKWARD_END() }

#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD(ARCH, ...)             \
  _ATTENTION_KERNEL_BACKWARD_BEGIN(                                  \
      AttentionBackwardKernel<cutlass::arch::Sm##ARCH, __VA_ARGS__>) \
  p.advance_to_block();                                              \
  Kernel::kernel(p);                                                 \
  _ATTENTION_KERNEL_BACKWARD_END();

#ifdef __CUDA_ARCH__
#define __CUDA_ARCH_OR_ZERO__ __CUDA_ARCH__
#else
#define __CUDA_ARCH_OR_ZERO__ 0
#endif

#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_DISABLED(ARCH, ...)                \
  _ATTENTION_KERNEL_BACKWARD_BEGIN(                                              \
      AttentionBackwardKernel<cutlass::arch::Sm##ARCH, __VA_ARGS__>)             \
  printf(                                                                        \
      "FATAL: this function is for sm%d, but was built with __CUDA_ARCH__=%d\n", \
      int(ARCH),                                                                 \
      int(__CUDA_ARCH_OR_ZERO__));                                               \
  _ATTENTION_KERNEL_BACKWARD_END();

// All kernels are disabled by default
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM50(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD_DISABLED(50, __VA_ARGS__)
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM70(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD_DISABLED(70, __VA_ARGS__)
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM75(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD_DISABLED(75, __VA_ARGS__)
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM80(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD_DISABLED(80, __VA_ARGS__)

// Enable the right one based on __CUDA_ARCH__
#ifndef __CUDA_ARCH__
#elif __CUDA_ARCH__ < 500
#error "Need cuda arch at least 5.0"
#elif __CUDA_ARCH__ < 700
#undef INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM50
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM50(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD(50, __VA_ARGS__)
#elif __CUDA_ARCH__ < 750
#undef INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM70
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM70(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD(70, __VA_ARGS__)
#elif __CUDA_ARCH__ < 800
#undef INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM75
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM75(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD(75, __VA_ARGS__)
#elif __CUDA_ARCH__ >= 800
#undef INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM80
#define INSTANTIATE_ATTENTION_KERNEL_BACKWARD_SM80(...) \
  INSTANTIATE_ATTENTION_KERNEL_BACKWARD(80, __VA_ARGS__)
#endif
