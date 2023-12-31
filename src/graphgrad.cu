#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;

#include <cstdio>
#include <cstring>
#include <memory>

#include "BinaryOp.h"
#include "ExpandOp.cuh"
#include "ReductionOp.cuh"
#include "ReshapeOp.h"
#include "Tensor.h"
#include "TransposeOp.cuh"
#include "UnaryOp.cuh"
#include "python_data_to_tensor.h"
#include "Tensor_backward.cuh"

std::mt19937 global_rng(std::random_device{}());

bool use_gpu = false;

static py::object make_sublist(const std::vector<size_t>& dims, const std::vector<size_t>& strides, const scalar_t* data, size_t dim) {
    if (dim == dims.size()) {
        return py::float_(*data);
    } else {
        py::list list;
        for (size_t i = 0; i < dims[dim]; i++, data += strides[dim]) {
            list.append(make_sublist(dims, strides, data, dim + 1));
        }
        return list;
    }
}

static py::object to_list(Tensor& t) {
    const std::vector<size_t>& dims = t.dims;
    const size_t num_dims = dims.size();

    std::vector<size_t> strides(num_dims);
    if (num_dims > 0) {
        strides[num_dims - 1] = 1;
        for (int i = int(num_dims) - 2; i >= 0; i--) {
            strides[i] = strides[i + 1] * dims[i + 1];
        }
    }

    std::vector<scalar_t> data = t.eval_to_cpu();
    return make_sublist(dims, strides, data.data(), 0);
}

static std::string to_string(Tensor& t) {
    std::string result = "<Tensor: dims=";
    result += vector_to_string(t.dims);

    result += ", data=";
    result += vector_to_string(t.eval_to_cpu());

    result += ", on_gpu=";
    result += t.on_gpu ? "true" : "false";

    result += ">";
    return result;
}

PYBIND11_MODULE(graphgrad, m) {
    m.def("use_gpu", [](bool new_use_gpu) { use_gpu = new_use_gpu; });
    m.def("set_cuda_device", [](int device) {
        cudaSetDevice(device);
        assert_no_cuda_error();
    });
    m.def("eval", [](std::shared_ptr<Tensor> t) {
        // Create a new leaf Tensor.
        auto new_t = std::make_shared<Tensor>(t->dims);
        new_t->on_gpu = t->on_gpu;

        // Assign the tensor a random hashValue.
        std::uniform_int_distribution<size_t> hash_dis(0, std::numeric_limits<size_t>::max());
        new_t->hashValue = hash_dis(global_rng);

        // The tensor's data with the data from t->eval().
        const scalar_t* data = t->eval();
        size_t data_len = product(t->dims);
        if (new_t->on_gpu) {
            CudaArray cuda_array(data_len);
            cudaMemcpy(cuda_array.ptr, data, data_len * sizeof(scalar_t), cudaMemcpyDefault);
            assert_no_cuda_error();
            new_t->data.emplace(std::move(cuda_array));
        } else {
            new_t->data.emplace(std::vector<scalar_t>(data, data + data_len));
        }

        return new_t;
    });
    m.def("clear_cache", []() {
        Tensor::clear_cache();
    });

    auto tensor_class = py::class_<Tensor, std::shared_ptr<Tensor>>(m, "tensor");
    tensor_class
        .def(py::init(&numpy_array_to_tensor))
        .def(py::init(&python_data_to_tensor))
        .def("dims", [](const Tensor& t) { return t.dims; })
        .def("backward", &Tensor::backward)
        .def_readwrite("grad", &Tensor::grad)
        .def("to_list", to_list)
        .def("__repr__", to_string);
    m.def("rand", &Tensor::rand);
    m.def("zeros", &Tensor::zeros);
    m.def("ones", &Tensor::ones);

    auto def_tensor_func = [&](const char* name, auto func_lambda) {
        auto func = (func_lambda);
        tensor_class.def(name, func);
        m.def(name, func);
    };

#define DEF_UNARY(name, op_type) def_tensor_func(name, [](std::shared_ptr<Tensor> t) { \
    return std::shared_ptr<Tensor>(new UnaryOp(t, UnaryOpType::op_type));              \
});
    DEF_UNARY("neg", NEG);
    tensor_class.def("__neg__", neg);
    DEF_UNARY("reciprocal", RECIP);
    DEF_UNARY("relu", RELU);
    DEF_UNARY("binilarize", BIN);
    DEF_UNARY("exp", EXP);
    DEF_UNARY("log", LOG);

    def_tensor_func("transpose", transpose);

    def_tensor_func("reshape", reshape);

    auto def_binary = [&](const char* name, auto op_func, std::optional<const char*> py_op, bool allow_scalars) {
        auto concat = [](auto p1, auto p2, auto p3) {
            return std::string(p1) + p2 + p3;
        };
        std::string py_op_func = concat("__", py_op.value_or(""), "__");
        std::string py_op_func_rev = concat("__r", py_op.value_or(""), "__");

        // tensor, tensor
        def_tensor_func(name, op_func);
        if (py_op) {
            tensor_class.def(py_op_func.c_str(), op_func);
        }

        if (allow_scalars) {
            // tensor, scalar
            auto ts_op_func = [=](std::shared_ptr<Tensor> t1, scalar_t t2) { return op_func(t1, Tensor::from_scalar(t2)); };
            def_tensor_func(name, ts_op_func);
            if (py_op) {
                tensor_class.def(py_op_func.c_str(), ts_op_func);
            }

            // scalar, tensor
            m.def(name, [=](scalar_t t1, std::shared_ptr<Tensor> t2) { return op_func(Tensor::from_scalar(t1), t2); });
            if (py_op) {
                tensor_class.def(py_op_func_rev.c_str(), [=](std::shared_ptr<Tensor> t2, scalar_t t1) { return op_func(Tensor::from_scalar(t1), t2); });
            }
        }
    };
    def_binary("add", gg::add, "add", true);
    def_binary("subtract", gg::subtract, "sub", true);
    def_binary("mul", gg::mul, "mul", true);
    def_binary("div", gg::div, "truediv", true);
    def_binary("matmul", gg::matmul, "matmul", false);
    def_binary("pow", gg::pow, std::nullopt, true);

#define DEF_REDUCTION(name, op_type) def_tensor_func(name, [](std::shared_ptr<Tensor> t) { \
    return std::shared_ptr<Tensor>(new ReductionOp(t, ReductionOpType::op_type));          \
});
    DEF_REDUCTION("sum", SUM);
    DEF_REDUCTION("sum_dim0", SUM_DIM0);

    def_tensor_func("expand", expand);

    m.add_object("_cleanup", py::capsule([]() {
        Tensor::clear_cache();
    }));
}
