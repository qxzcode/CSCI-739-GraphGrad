#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;

#include <cstdio>
#include <cstring>
#include <memory>

#include "BinaryOp.h"
#include "ReductionOp.h"
#include "ReshapeOp.h"
#include "Tensor.h"
#include "TransposeOp.h"
#include "UnaryOp.cuh"
#include "python_data_to_tensor.h"
#include "Tensor_backward.cuh"

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

    return make_sublist(dims, strides, t.eval(), 0);
}

PYBIND11_MODULE(graphgrad, m) {
    auto tensor_class = py::class_<Tensor, std::shared_ptr<Tensor>>(m, "tensor");
    tensor_class
        .def(py::init(&numpy_array_to_tensor))
        .def(py::init(&python_data_to_tensor))
        .def("dims", [](const Tensor& t) { return t.dims; })
        .def("backward", &Tensor::backward)
        .def_readwrite("grad", &Tensor::grad)
        .def("to_list", to_list)
        .def("__repr__", [](Tensor& t) {
            t.eval();
            return t.to_string();
        });
    m.def("rand", &Tensor::rand);
    m.def("zeros", &Tensor::zeros);
    m.def("ones", &Tensor::ones);

#define DEF_TENSOR_FUNC(name, func_lambda) \
    {                                      \
        auto func = (func_lambda);         \
        tensor_class.def(name, func);      \
        m.def(name, func);                 \
    }

#define DEF_UNARY(name, op_type) DEF_TENSOR_FUNC(name, [](std::shared_ptr<Tensor> t) { \
    return std::shared_ptr<Tensor>(new UnaryOp(t, UnaryOpType::op_type));              \
});
    DEF_UNARY("neg", NEG);
    tensor_class.def("__neg__", [](std::shared_ptr<Tensor> t) { return -t; });
    DEF_UNARY("reciprocal", RECIP);
    DEF_UNARY("relu", RELU);
    DEF_UNARY("binilarize", BIN);
    DEF_UNARY("exp", EXP);
    DEF_UNARY("log", LOG);

#define DEF_TRANSPOSE(name) DEF_TENSOR_FUNC(name, [](std::shared_ptr<Tensor> t, int dim0, int dim1) { \
    return std::shared_ptr<Tensor>(new TransposeOp(t, dim0, dim1));                                   \
});
    DEF_TRANSPOSE("transpose");

#define DEF_RESHAPE(name) DEF_TENSOR_FUNC(name, [](std::shared_ptr<Tensor> t, std::vector<size_t> new_dims) { \
    return std::shared_ptr<Tensor>(new ReshapeOp(t, new_dims));                                               \
});
    DEF_RESHAPE("reshape");

#define DEF_BINARY(name, op_type) DEF_TENSOR_FUNC(name, [](std::shared_ptr<Tensor> t1, std::shared_ptr<Tensor> t2) { \
    return std::shared_ptr<Tensor>(new BinaryOp(t1, t2, BinaryOpType::op_type));                                     \
});
#define DEF_BINARY_WITH_OP(name, op_type, op, py_op)                                                                        \
    {                                                                                                                       \
        DEF_BINARY(name, op_type);                                                                                          \
        tensor_class.def("__" py_op "__", [](std::shared_ptr<Tensor> t1, std::shared_ptr<Tensor> t2) { return t1 op t2; }); \
    }
    DEF_BINARY_WITH_OP("add", ADD, +, "add");
    DEF_BINARY_WITH_OP("subtract", SUB, -, "sub");
    DEF_BINARY_WITH_OP("mul", MUL, *, "mul");
    DEF_BINARY_WITH_OP("div", DIV, /, "truediv");
    DEF_BINARY("matmul", MATMUL);
    DEF_BINARY("pow", POW);

#define DEF_REDUCTION(name, op_type) DEF_TENSOR_FUNC(name, [](std::shared_ptr<Tensor> t) { \
    return std::shared_ptr<Tensor>(new ReductionOp(t, ReductionOpType::op_type));          \
});
    DEF_REDUCTION("sum", SUM);
}