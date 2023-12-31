#pragma once

#include <pybind11/pybind11.h>
namespace py = pybind11;

#include <memory>

#include "Tensor.h"
#include "ReshapeOp.h"
#include "TransposeOp.cuh"
#include "utils.h"
#include "cuda_helpers.h"

enum class BinaryOpType {
    ADD,
    SUB,
    MUL,
    DIV,
    POW,
    MATMUL,
};

#define IMPL_POINTWISE_BINARY_OP(__name, __expr)                                              \
    __global__ void kernel_binary_##__name(const scalar_t* left, const scalar_t* right, CudaArrayRef out,  \
                                           size_t left_len, size_t right_len) {               \
        size_t index = (blockIdx.x * blockDim.x) + threadIdx.x;                               \
                                                                                              \
        if (index < out.length) {                                                             \
            scalar_t a;                                                                       \
                if (left_len == 1)                                                            \
                    a = left[0];                                                              \
                else                                                                          \
                    a = left[index];                                                          \
                                                                                              \
                scalar_t b;                                                                   \
                if (right_len == 1)                                                           \
                    b = right[0];                                                             \
                else                                                                          \
                    b = right[index];                                                         \
            out.ptr[index] = (__expr);                                                        \
        }                                                                                     \
    }                                                                                         \
                                                                                              \
    inline void binary_compute_data_##__name(Tensor* self, const scalar_t* left, const scalar_t* right,  \
                                            size_t left_len, size_t right_len) {              \
        if (self->on_gpu) {                                                                   \
            auto& data = self->allocate_data_gpu();                                           \
                                                                                              \
            kernel_binary_##__name<<<num_blocks(data.length), BLOCK_SIZE>>>(left, right, data, left_len, right_len);  \
        } else {                                                                              \
            auto& data = self->allocate_data_cpu();                                           \
                                                                                              \
            _Pragma("omp parallel for")                                                       \
            for (size_t i = 0; i < data.size(); i++) {                                        \
                using std::pow;                                                               \
                scalar_t a;                                                                   \
                if (left_len == 1)                                                            \
                    a = left[0];                                                              \
                else                                                                          \
                    a = left[i];                                                              \
                                                                                              \
                scalar_t b;                                                                   \
                if (right_len == 1)                                                           \
                    b = right[0];                                                             \
                else                                                                          \
                    b = right[i];                                                             \
                data[i] = (__expr);                                                           \
            }                                                                                 \
        }                                                                                     \
    }


__global__ void kernel_matmul_2d(
    const scalar_t* in1, const scalar_t* in2, CudaArrayRef out,
    size_t si, size_t sj, size_t sk  //
) {
    size_t i = (blockIdx.x * blockDim.x) + threadIdx.x;
    size_t j = (blockIdx.y * blockDim.y) + threadIdx.y;

    if (i < si && j < sj) {
        scalar_t sum = 0.0;
        size_t k = 0;
        for (; k + 3 < sk; k += 4) {
            const scalar_t* in1_ptr = in1 + (i * sk + k);
            const scalar_t* in2_ptr = in2 + (j * sk + k);
            #pragma unroll
            for (size_t ku = 0; ku < 4; ++ku) {
                sum += *(in1_ptr + ku) * *(in2_ptr + ku);
            }
        }
        for (; k < sk; ++k) {
            sum += in1[i * sk + k] * in2[j * sk + k];
        }
        out.ptr[i * sj + j] = sum;
    }
}

IMPL_POINTWISE_BINARY_OP(add, a + b)
IMPL_POINTWISE_BINARY_OP(sub, a - b)
IMPL_POINTWISE_BINARY_OP(mul, a * b)
IMPL_POINTWISE_BINARY_OP(div, a / b)
IMPL_POINTWISE_BINARY_OP(pow, pow(a, b))

#undef IMPL_POINTWISE_BINARY_OP

class BinaryOp : public Tensor {
   public:
    BinaryOp(std::shared_ptr<Tensor> arg1, std::shared_ptr<Tensor> arg2, BinaryOpType op_type)
        : Tensor(verify_and_get_dims(*arg1, *arg2, op_type)), leftChild(arg1), rightChild(arg2), op_type(op_type) {
            this->hashValue = tensor_hash();
            this->on_gpu = arg1->on_gpu && arg2->on_gpu;
        }


    size_t tensor_hash(){
        size_t hashValue = 0;
        Tensor::hash_combine(hashValue, Tensor::vector_hash(this->dims));
        Tensor::hash_combine(hashValue, std::hash<std::string>{}("binary"));
        Tensor::hash_combine(hashValue, static_cast<size_t>(this->op_type));
        Tensor::hash_combine(hashValue, this->leftChild->hashValue);
        Tensor::hash_combine(hashValue, this->rightChild->hashValue);
        return hashValue;
    }

    void compute_data() override {
        // Evaluate the child nodes and get their data.
        const scalar_t* left_child_data = this->leftChild->eval();
        const scalar_t* right_child_data = this->rightChild->eval();
        size_t left_size = product(this->leftChild->dims);
        size_t right_size = product(this->rightChild->dims);

        // Get a function to compute each value.
        // scalar_t (*scalar_func)(scalar_t, scalar_t);
        switch (this->op_type) {
            case BinaryOpType::ADD:
                binary_compute_data_add(this, left_child_data, right_child_data, left_size, right_size);
                break;
            case BinaryOpType::SUB:
                binary_compute_data_sub(this, left_child_data, right_child_data, left_size, right_size);
                break;
            case BinaryOpType::MUL:
                binary_compute_data_mul(this, left_child_data, right_child_data, left_size, right_size);
                break;
            case BinaryOpType::DIV:
                binary_compute_data_div(this, left_child_data, right_child_data, left_size, right_size);
                break;
            case BinaryOpType::POW:
                binary_compute_data_pow(this, left_child_data, right_child_data, left_size, right_size);
                break;
            case BinaryOpType::MATMUL:
            {
                // MATMUL doesn't use a scalar_func.
                assert(this->leftChild->dims.size() == 2);
                assert(this->rightChild->dims.size() == 2);
                assert(this->leftChild->dims[1] == this->rightChild->dims[0]);

                size_t width = this->rightChild->dims[0];
                size_t cols = this->rightChild->dims[1];

                if (this->on_gpu){
                    auto& data = this->allocate_data_gpu();

                    // Make a temporary transpose copy of the righ child's data on gpu
                    std::shared_ptr<Tensor> rigth_data_transposed = transpose(this->rightChild, 0, 1);
                    right_child_data = rigth_data_transposed->eval();

                    // kernel_binary_##__name<<<num_blocks(data.length), BLOCK_SIZE>>>
                    
                    // Prepare arguments for kernel
                    int si = this->leftChild->dims[0];
                    int sj = this->rightChild->dims[1];
                    int sk =  this->rightChild->dims[0];

                    auto num_b = dim3(num_blocks(si, MM_BLOCK_SIZE), num_blocks(sj, MM_BLOCK_SIZE));
                    auto b_size = dim3(MM_BLOCK_SIZE, MM_BLOCK_SIZE);

                    kernel_matmul_2d<<<num_b, b_size>>>(left_child_data, right_child_data, data, si, sj, sk);

                } else {
                    // Allocate the data buffer.
                    auto& data = this->allocate_data_cpu();

                    // Make a temporary transposed copy of the right child's data so that the memory
                    // accesses in the tight inner loop are more cache-friendly.
                    std::vector<scalar_t> right_data_transposed(product(this->rightChild->dims));
                    for (size_t i = 0; i < width; i++) {
                        for (size_t j = 0; j < cols; j++) {
                            right_data_transposed[j * width + i] = right_child_data[i * cols + j];
                        }
                    }

                    #pragma omp parallel for
                    for (size_t i = 0; i < data.size(); i++) {
                        size_t r = i / cols;
                        size_t c = i % cols;
                        const scalar_t* left_row = left_child_data + (r * width);
                        const scalar_t* right_col = right_data_transposed.data() + (c * width);

                        scalar_t sum = 0.0;
                        #pragma omp simd reduction(+:sum)
                        for (size_t j = 0; j < width; j++) {
                            sum += left_row[j] * right_col[j];
                        }
                        data[i] = sum;
                    }
                }
                break;
            }
            default:
                throw std::domain_error("bad op_type");
        }

        // Fill the buffer with computed values.
        // switch (this->op_type) {
        //     case BinaryOpType::MATMUL: {
                
        //     }

        //     default: {
        //         #pragma omp parallel for
        //         for (size_t i = 0; i < data.size(); i++) {
        //             if (product(leftChild->dims) == 1) {
        //                 data[i] = scalar_func(left_child_data[0], right_child_data[i]);
        //             } else if (product(rightChild->dims) == 1) {
        //                 data[i] = scalar_func(left_child_data[i], right_child_data[0]);
        //             } else {
        //                 data[i] = scalar_func(left_child_data[i], right_child_data[i]);
        //             }
        //         }
        //     }
        // }
    }

    std::vector<Tensor*> get_children() override {
        return {this->leftChild.get(), this->rightChild.get()};
    }

    void backward_step() override;  // Implementation in Tensor_backward.cc

   protected:
    static std::vector<size_t> verify_and_get_dims(const Tensor& left, const Tensor& right, BinaryOpType op_type) {
        switch (op_type) {
            case BinaryOpType::MATMUL:
                if (left.dims.size() == 2 && right.dims.size() == 2 && left.dims[1] == right.dims[0]) {
                    return {left.dims[0], right.dims[1]};
                } else {
                    std::string error_message = "invalid dims for BinaryOpType::MATMUL: left.dims=";
                    error_message += vector_to_string(left.dims);
                    error_message += ", right.dims=";
                    error_message += vector_to_string(right.dims);
                    throw py::value_error(error_message);
                }

            default:
                bool is_left_scalar = product(left.dims) == 1;
                bool is_right_scalar = product(right.dims) == 1;
                if (left.dims != right.dims && !is_left_scalar && !is_right_scalar) {
                    std::string error_message = "binary op dims mismatch: left.dims=";
                    error_message += vector_to_string(left.dims);
                    error_message += ", right.dims=";
                    error_message += vector_to_string(right.dims);
                    throw py::value_error(error_message);
                } else if (is_left_scalar && is_right_scalar) {
                    if (left.dims.size() > right.dims.size()) {
                        return left.dims;
                    } else {
                        return right.dims;
                    }
                } else if (is_left_scalar) {
                    return right.dims;
                } else {
                    return left.dims;
                }
        }
    }

    std::shared_ptr<Tensor> leftChild;
    std::shared_ptr<Tensor> rightChild;
    BinaryOpType op_type;
};


// Functions:

namespace gg {

#define IMPL_OP_FUNC(func_name, op_type)                                                                      \
    inline static std::shared_ptr<Tensor> func_name(std::shared_ptr<Tensor> t1, std::shared_ptr<Tensor> t2) { \
        return std::shared_ptr<Tensor>(new BinaryOp(t1, t2, BinaryOpType::op_type));                          \
    }

IMPL_OP_FUNC(add, ADD)
IMPL_OP_FUNC(subtract, SUB)
IMPL_OP_FUNC(mul, MUL)
IMPL_OP_FUNC(div, DIV)
IMPL_OP_FUNC(pow, POW)

inline static std::shared_ptr<Tensor> matmul(std::shared_ptr<Tensor> left, std::shared_ptr<Tensor> right) {
    // Reshape left and right into 2D matrices, as needed.
    bool squeeze_left = false, squeeze_right = false;
    if (left->dims.size() == 1) {
        size_t d = left->dims[0];
        left = reshape(left, {1, d});
        squeeze_left = true;
    }
    if (right->dims.size() == 1) {
        size_t d = right->dims[0];
        right = reshape(right, {d, 1});
        squeeze_right = true;
    }

    // Assert correct 2D*2D matmul dimensions.
    if (!(left->dims.size() == 2 && right->dims.size() == 2 && left->dims[1] == right->dims[0])) {
        std::string error_message = "invalid matmul dims: left.dims=";
        error_message += vector_to_string(left->dims);
        error_message += ", right.dims=";
        error_message += vector_to_string(right->dims);
        throw py::value_error(error_message);
    }

    auto result = std::shared_ptr<Tensor>(new BinaryOp(left, right, BinaryOpType::MATMUL));
    if (squeeze_left && squeeze_right) {
        return reshape(result, {});
    } else if (squeeze_left) {
        return reshape(result, {result->dims[1]});
    } else if (squeeze_right) {
        return reshape(result, {result->dims[0]});
    } else {
        return result;
    }
}

}

#undef IMPL_OP_FUNC


// Operator overloads:

#define IMPL_OPERATOR_OVERLOAD(op, func)                                                                        \
    inline static std::shared_ptr<Tensor> operator op(std::shared_ptr<Tensor> t1, std::shared_ptr<Tensor> t2) { \
        return gg::func(t1, t2);                                                                                \
    }

IMPL_OPERATOR_OVERLOAD(+, add)
IMPL_OPERATOR_OVERLOAD(-, subtract)
IMPL_OPERATOR_OVERLOAD(*, mul)
IMPL_OPERATOR_OVERLOAD(/, div)

#undef IMPL_OPERATOR_OVERLOAD
