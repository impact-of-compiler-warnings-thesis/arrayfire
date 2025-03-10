/*******************************************************
 * Copyright (c) 2022 ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#pragma once

#include <Param.hpp>
#include <common/dispatch.hpp>
#include <debug_oneapi.hpp>
#include <err_oneapi.hpp>
#include <traits.hpp>

#include <string>
#include <vector>

namespace arrayfire {
namespace oneapi {
namespace kernel {

constexpr int TILE_DIM  = 32;
constexpr int THREADS_X = TILE_DIM;
constexpr int THREADS_Y = 256 / TILE_DIM;

template<typename T>
T getConjugate(const T &in) {
    // For non-complex types return same
    return in;
}

template<>
cfloat getConjugate(const cfloat &in) {
    return std::conj(in);
}

template<>
cdouble getConjugate(const cdouble &in) {
    return std::conj(in);
}

template<typename T, int dimensions>
using local_accessor =
    sycl::accessor<T, dimensions, sycl::access::mode::read_write,
                   sycl::access::target::local>;

template<typename T>
class transposeKernel {
   public:
    transposeKernel(sycl::accessor<T> oData, const KParam out,
                    const sycl::accessor<T> iData, const KParam in,
                    const int blocksPerMatX, const int blocksPerMatY,
                    const bool conjugate, const bool IS32MULTIPLE,
                    local_accessor<T, 1> shrdMem, sycl::stream debugStream)
        : oData_(oData)
        , out_(out)
        , iData_(iData)
        , in_(in)
        , blocksPerMatX_(blocksPerMatX)
        , blocksPerMatY_(blocksPerMatY)
        , conjugate_(conjugate)
        , IS32MULTIPLE_(IS32MULTIPLE)
        , shrdMem_(shrdMem)
        , debugStream_(debugStream) {}
    void operator()(sycl::nd_item<2> it) const {
        const int shrdStride = TILE_DIM + 1;

        const int oDim0 = out_.dims[0];
        const int oDim1 = out_.dims[1];
        const int iDim0 = in_.dims[0];
        const int iDim1 = in_.dims[1];

        // calculate strides
        const int oStride1 = out_.strides[1];
        const int iStride1 = in_.strides[1];

        const int lx = it.get_local_id(0);
        const int ly = it.get_local_id(1);

        // batch based block Id
        sycl::group g        = it.get_group();
        const int batchId_x  = g.get_group_id(0) / blocksPerMatX_;
        const int blockIdx_x = (g.get_group_id(0) - batchId_x * blocksPerMatX_);

        const int batchId_y  = g.get_group_id(1) / blocksPerMatY_;
        const int blockIdx_y = (g.get_group_id(1) - batchId_y * blocksPerMatY_);

        const int x0 = TILE_DIM * blockIdx_x;
        const int y0 = TILE_DIM * blockIdx_y;

        // calculate global in_dices
        int gx = lx + x0;
        int gy = ly + y0;

        // offset in_ and out_ based on batch id
        // also add the subBuffer offsets
        T *iDataPtr = iData_.get_pointer(), *oDataPtr = oData_.get_pointer();
        iDataPtr += batchId_x * in_.strides[2] + batchId_y * in_.strides[3] +
                    in_.offset;
        oDataPtr += batchId_x * out_.strides[2] + batchId_y * out_.strides[3] +
                    out_.offset;

        for (int repeat = 0; repeat < TILE_DIM; repeat += THREADS_Y) {
            int gy_ = gy + repeat;
            if (IS32MULTIPLE_ || (gx < iDim0 && gy_ < iDim1))
                shrdMem_[(ly + repeat) * shrdStride + lx] =
                    iDataPtr[gy_ * iStride1 + gx];
        }
        it.barrier();

        gx = lx + y0;
        gy = ly + x0;

        for (int repeat = 0; repeat < TILE_DIM; repeat += THREADS_Y) {
            int gy_ = gy + repeat;
            if (IS32MULTIPLE_ || (gx < oDim0 && gy_ < oDim1)) {
                const T val = shrdMem_[lx * shrdStride + ly + repeat];
                oDataPtr[gy_ * oStride1 + gx] =
                    conjugate_ ? getConjugate(val) : val;
            }
        }
    }

   private:
    sycl::accessor<T> oData_;
    KParam out_;
    sycl::accessor<T> iData_;
    KParam in_;
    int blocksPerMatX_;
    int blocksPerMatY_;
    bool conjugate_;
    bool IS32MULTIPLE_;
    local_accessor<T, 1> shrdMem_;
    sycl::stream debugStream_;
};

template<typename T>
void transpose(Param<T> out, const Param<T> in, const bool conjugate,
               const bool IS32MULTIPLE) {
    auto local = sycl::range{THREADS_X, THREADS_Y};

    const int blk_x = divup(in.info.dims[0], TILE_DIM);
    const int blk_y = divup(in.info.dims[1], TILE_DIM);

    auto global = sycl::range{blk_x * local[0] * in.info.dims[2],
                              blk_y * local[1] * in.info.dims[3]};

    getQueue().submit([&](sycl::handler &h) {
        auto r = in.data->get_access(h);
        auto q = out.data->get_access(h);
        sycl::stream debugStream(128, 128, h);

        auto shrdMem = local_accessor<T, 1>(TILE_DIM * (TILE_DIM + 1), h);

        h.parallel_for(
            sycl::nd_range{global, local},
            transposeKernel<T>(q, out.info, r, in.info, blk_x, blk_y, conjugate,
                               IS32MULTIPLE, shrdMem, debugStream));
    });
    ONEAPI_DEBUG_FINISH(getQueue());
}

}  // namespace kernel
}  // namespace oneapi
}  // namespace arrayfire
