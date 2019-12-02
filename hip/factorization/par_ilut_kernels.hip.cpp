/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2019, the Ginkgo authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include "core/factorization/par_ilut_kernels.hpp"


#include <hip/hip_runtime.h>


#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/matrix/coo.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "hip/base/math.hip.hpp"
#include "hip/components/atomic.hip.hpp"
#include "hip/components/prefix_sum.hip.hpp"
#include "hip/components/sorting.hip.hpp"


namespace gko {
namespace kernels {
namespace hip {
/**
 * @brief The parallel ILUT factorization namespace.
 *
 * @ingroup factor
 */
namespace par_ilut_factorization {


constexpr auto default_block_size = 512;
constexpr auto items_per_thread = 2;


#include "common/factorization/par_ilut_kernels.hpp.inc"


template <typename ValueType, typename IndexType>
void ssss_count(const ValueType *values, IndexType size,
                remove_complex<ValueType> *tree, unsigned char *oracles,
                IndexType *partial_counts, IndexType *total_counts)
{
    constexpr auto bucket_count = kernel::searchtree_width;
    auto num_threads_total = ceildiv(size, items_per_thread);
    auto num_blocks = IndexType(ceildiv(num_threads_total, default_block_size));
    // pick sample, build searchtree
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::build_searchtree), dim3(1),
                       dim3(bucket_count), 0, 0, as_hip_type(values), size,
                       tree);
    // determine bucket sizes
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::count_buckets), dim3(num_blocks),
                       dim3(default_block_size), 0, 0, as_hip_type(values),
                       size, tree, partial_counts, oracles, items_per_thread);
    // compute prefix sum and total sum over block-local values
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::block_prefix_sum),
                       dim3(bucket_count), dim3(default_block_size), 0, 0,
                       partial_counts, total_counts, num_blocks);
    // compute prefix sum over bucket counts
    hipLaunchKernelGGL(HIP_KERNEL_NAME(start_prefix_sum<bucket_count>), dim3(1),
                       dim3(bucket_count), 0, 0, bucket_count, total_counts,
                       total_counts + bucket_count);
}


template <typename ValueType, typename IndexType>
void ssss_filter(const ValueType *values, IndexType size,
                 const unsigned char *oracles, const IndexType *partial_counts,
                 IndexType bucket, remove_complex<ValueType> *out)
{
    auto num_threads_total = ceildiv(size, items_per_thread);
    auto num_blocks = IndexType(ceildiv(num_threads_total, default_block_size));
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::filter_bucket), dim3(num_blocks),
                       dim3(default_block_size), 0, 0, as_hip_type(values),
                       size, bucket, oracles, partial_counts, out,
                       items_per_thread);
}


template <typename ValueType, typename IndexType>
remove_complex<ValueType> threshold_select(
    std::shared_ptr<const HipExecutor> exec, const ValueType *values,
    IndexType size, IndexType rank)
{
    using AbsType = remove_complex<ValueType>;
    constexpr auto bucket_count = kernel::searchtree_width;
    auto max_num_threads = ceildiv(size, items_per_thread);
    auto max_num_blocks = ceildiv(max_num_threads, default_block_size);

    // we use the last entry to store the total element count
    Array<IndexType> total_counts_array(exec, bucket_count + 1);
    Array<IndexType> partial_counts_array(exec, bucket_count * max_num_blocks);
    Array<unsigned char> oracle_array(exec, size);
    Array<AbsType> tree_array(exec, kernel::searchtree_size);
    auto partial_counts = partial_counts_array.get_data();
    auto total_counts = total_counts_array.get_data();
    auto oracles = oracle_array.get_data();
    auto tree = tree_array.get_data();

    ssss_count(values, size, tree, oracles, partial_counts, total_counts);

    // determine bucket with correct rank
    Array<IndexType> splitter_ranks_array(exec->get_master(),
                                          total_counts_array);
    auto splitter_ranks = splitter_ranks_array.get_const_data();
    auto it = std::upper_bound(splitter_ranks,
                               splitter_ranks + bucket_count + 1, rank);
    auto bucket = IndexType(std::distance(splitter_ranks + 1, it));
    auto bucket_size = splitter_ranks[bucket + 1] - splitter_ranks[bucket];
    rank -= splitter_ranks[bucket];

    Array<AbsType> tmp_out_array(exec, bucket_size);
    Array<AbsType> tmp_in_array(exec, bucket_size);
    auto tmp_out = tmp_out_array.get_data();
    auto tmp_in = tmp_in_array.get_const_data();
    // extract target bucket
    ssss_filter(values, size, oracles, partial_counts, bucket, tmp_out);

    // recursively select from smaller buckets
    int step{};
    while (bucket_size > kernel::basecase_size) {
        std::swap(tmp_out_array, tmp_in_array);
        tmp_out = tmp_out_array.get_data();
        tmp_in = tmp_in_array.get_const_data();

        ssss_count(tmp_in, bucket_size, tree, oracles, partial_counts,
                   total_counts);
        splitter_ranks_array = total_counts_array;
        splitter_ranks = splitter_ranks_array.get_const_data();
        auto it = std::upper_bound(splitter_ranks,
                                   splitter_ranks + bucket_count + 1, rank);
        bucket = IndexType(std::distance(splitter_ranks + 1, it));
        ssss_filter(tmp_in, bucket_size, oracles, partial_counts, bucket,
                    tmp_out);

        rank -= splitter_ranks[bucket];
        bucket_size = splitter_ranks[bucket + 1] - splitter_ranks[bucket];
        // we should never need more than 5 recursion steps, this would mean
        // 256^5 = 2^40. fall back to standard library algorithm in that case.
        ++step;
        if (step > 5) {
            Array<AbsType> cpu_out_array{exec->get_master(), tmp_out_array};
            auto begin = cpu_out_array.get_data();
            auto end = begin + bucket_size;
            auto middle = begin + rank;
            std::nth_element(begin, middle, end);
            return kernel::fast_abs_result<ValueType>(*middle);
        }
    }

    // base case
    Array<AbsType> result_array{exec, 1};
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::basecase_select), dim3(1),
                       dim3(kernel::basecase_block_size), 0, 0, tmp_out,
                       bucket_size, rank, result_array.get_data());
    AbsType result{};
    exec->get_master()->copy_from(exec.get(), 1, result_array.get_const_data(),
                                  &result);
    return kernel::fast_abs_result<ValueType>(result);
}


GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_PAR_ILUT_THRESHOLD_SELECT_KERNEL);


template <typename ValueType, typename IndexType>
void threshold_filter(std::shared_ptr<const HipExecutor> exec,
                      const matrix::Csr<ValueType, IndexType> *a,
                      remove_complex<ValueType> threshold,
                      Array<IndexType> &new_row_ptrs_array,
                      Array<IndexType> &new_col_idxs_array,
                      Array<ValueType> &new_vals_array) GKO_NOT_IMPLEMENTED;


GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_PAR_ILUT_THRESHOLD_FILTER_KERNEL);


template <typename ValueType, typename IndexType>
void spgeam(std::shared_ptr<const HipExecutor> exec,
            const matrix::Dense<ValueType> *alpha,
            const matrix::Csr<ValueType, IndexType> *a,
            const matrix::Dense<ValueType> *beta,
            const matrix::Csr<ValueType, IndexType> *b,
            Array<IndexType> &c_row_ptrs_array,
            Array<IndexType> &c_col_idxs_array,
            Array<ValueType> &c_vals_array) GKO_NOT_IMPLEMENTED;


GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_PAR_ILUT_SPGEAM_KERNEL);


}  // namespace par_ilut_factorization
}  // namespace hip
}  // namespace kernels
}  // namespace gko