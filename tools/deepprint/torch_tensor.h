#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// Stable C linkage of the TorchSharp 0.105.0 native bridge (LibTorch CPU 2.5.1).
// Only native DLLs participate; the CLI owns tensor and optimizer lifetimes.
namespace studytorch
{
using Handle = void *;
using I = std::int64_t;
extern "C"
{
    const char *THSTorch_get_and_reset_last_err();
    void THSTorch_set_num_threads(int);
    void THSTorch_manual_seed(I);
    Handle THSTensor_empty(const I *, int, std::int8_t, int, int, bool);
    void THSTensor_dispose(Handle);
    Handle THSTensor_contiguous(Handle);
    Handle THSTensor_detach(Handle);
    Handle THSTensor_clone(Handle);
    Handle THSTensor_grad(Handle);
    void THSTensor_backward(Handle);
    void THSTensor_set_requires_grad(Handle, bool);
    void *THSTensor_data(Handle);
    I THSTensor_numel(Handle);
    I THSTensor_size(Handle, I);
    Handle THSTensor_slice(Handle, I, I, I, I);
    Handle THSTensor_index_select(Handle, I, Handle);
    Handle THSTensor_reshape(Handle, const I *, int);
    Handle THSTensor_cat(const Handle *, int, I);
    Handle THSTensor_add(Handle, Handle, Handle);
    Handle THSTensor_sub(Handle, Handle);
    Handle THSTensor_mul(Handle, Handle);
    Handle THSTensor_mean(Handle);
    Handle THSTensor_sum_along_dimensions(Handle, const I *, int, bool, bool, std::int8_t);
    Handle THSTensor_mm(Handle, Handle);
    Handle THSTensor_transpose(Handle, I, I);
    Handle THSTensor_logsumexp(Handle, long, bool);
    Handle THSTensor_relu(Handle);
    Handle THSNN_functional_linear(Handle, Handle, Handle);
    Handle THSNN_normalize(Handle, double, I, double);
    Handle THSNN_batch_norm(Handle, Handle, Handle, Handle, Handle, bool, double, double);
    Handle THSTensor_conv2d(Handle, Handle, Handle, const I *, int, const I *, int, const I *, int, I);
    Handle THSTensor_avg_pool2d(Handle, const I *, int, const I *, int, const I *, int, bool, bool, I);
    Handle THSTensor_max_pool2d(Handle, const I *, int, const I *, int, const I *, int, const I *, int, bool);
    Handle THSTorch_float32_to_scalar(float);
    void THSTorch_dispose_scalar(Handle);
    Handle THSNN_AdamW_ctor(const Handle *, int, double, double, double, double, double, bool);
    void THSNN_Optimizer_zero_grad(Handle);
    Handle THSNN_Optimizer_step(Handle, Handle (*)());
    void THSNN_Optimizer_dispose(Handle);
}
inline void check()
{
    if (auto e = THSTorch_get_and_reset_last_err())
        throw std::runtime_error(e);
}

class Tensor
{
    std::shared_ptr<void> ptr;

  public:
    Tensor() = default;
    explicit Tensor(Handle h) : ptr(h, THSTensor_dispose)
    {
        check();
        if (!h)
            throw std::runtime_error("LibTorch returned an empty handle");
    }
    Handle h() const
    {
        return ptr.get();
    }
    explicit operator bool() const
    {
        return h() != nullptr;
    }
    I size(int dim = 0) const
    {
        return THSTensor_size(h(), dim);
    }
    std::vector<float> values() const
    {
        Tensor v(THSTensor_contiguous(h()));
        auto n = THSTensor_numel(v.h());
        auto p = static_cast<const float *>(THSTensor_data(v.h()));
        check();
        return {p, p + n};
    }
    Tensor snapshot() const
    {
        Tensor v(THSTensor_detach(h()));
        return Tensor(THSTensor_clone(v.h()));
    }
    void trainable()
    {
        THSTensor_set_requires_grad(h(), true);
        check();
    }
    Tensor slice(I dim, I a, I b) const
    {
        return Tensor(THSTensor_slice(h(), dim, a, b, 1));
    }
    Tensor reshape(std::vector<I> dims) const
    {
        return Tensor(THSTensor_reshape(h(), dims.data(), int(dims.size())));
    }
};

template <class T> Tensor tensor(const std::vector<T> &v, const std::vector<I> &shape)
{
    I n = 1;
    for (auto d : shape)
    {
        if (d <= 0)
            throw std::runtime_error("Invalid tensor shape");
        n *= d;
    }
    if (static_cast<size_t>(n) != v.size())
        throw std::runtime_error("Tensor size mismatch");
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, I>);
    Tensor x(
        THSTensor_empty(shape.data(), int(shape.size()), std::is_same_v<T, float> ? 6 : 4, 0, -1, false));
    std::memcpy(THSTensor_data(x.h()), v.data(), sizeof(T) * v.size());
    return x;
}
inline Tensor number(float f)
{
    return tensor(std::vector<float>{f}, {});
}
inline Tensor operator+(const Tensor &a, const Tensor &b)
{
    static std::shared_ptr<void> one(THSTorch_float32_to_scalar(1), THSTorch_dispose_scalar);
    return Tensor(THSTensor_add(a.h(), b.h(), one.get()));
}
inline Tensor operator-(const Tensor &a, const Tensor &b)
{
    return Tensor(THSTensor_sub(a.h(), b.h()));
}
inline Tensor operator*(const Tensor &a, const Tensor &b)
{
    return Tensor(THSTensor_mul(a.h(), b.h()));
}
inline Tensor operator*(const Tensor &a, float b)
{
    return a * number(b);
}
inline Tensor mean(const Tensor &v)
{
    return Tensor(THSTensor_mean(v.h()));
}
inline Tensor sum1(const Tensor &v)
{
    I dim = 1;
    return Tensor(THSTensor_sum_along_dimensions(v.h(), &dim, 1, false, false, 0));
}
inline Tensor norm(const Tensor &v)
{
    return Tensor(THSNN_normalize(v.h(), 2, 1, 1e-12));
}
inline Tensor relu(const Tensor &v)
{
    return Tensor(THSTensor_relu(v.h()));
}
inline Tensor mm(const Tensor &a, const Tensor &b)
{
    return Tensor(THSTensor_mm(a.h(), b.h()));
}
inline Tensor transpose(const Tensor &a)
{
    return Tensor(THSTensor_transpose(a.h(), 0, 1));
}
inline Tensor linear(const Tensor &x, const Tensor &w, const Tensor &b = {})
{
    return Tensor(THSNN_functional_linear(x.h(), w.h(), b.h()));
}
inline Tensor select(const Tensor &x, const std::vector<I> &indices)
{
    auto ids = tensor(indices, {I(indices.size())});
    return Tensor(THSTensor_index_select(x.h(), 0, ids.h()));
}
inline Tensor cat(const std::vector<Tensor> &tensors, I dim)
{
    std::vector<Handle> h;
    for (auto &t : tensors)
        h.push_back(t.h());
    return Tensor(THSTensor_cat(h.data(), int(h.size()), dim));
}
inline Tensor similarity(const Tensor &a, const Tensor &b, float w)
{
    auto d = a * b;
    return (number(1) + sum1(d.slice(1, 0, 256)) * w + sum1(d.slice(1, 256, 512)) * (1 - w)) * .5f;
}
class AdamW
{
    std::shared_ptr<void> optimizer;

  public:
    AdamW(const std::vector<Tensor> &params, double lr)
    {
        std::vector<Handle> h;
        for (auto &t : params)
            h.push_back(t.h());
        optimizer.reset(THSNN_AdamW_ctor(h.data(), int(h.size()), lr, .9, .999, 1e-8, 1e-4, false),
                        THSNN_Optimizer_dispose);
        check();
    }
    void clear()
    {
        THSNN_Optimizer_zero_grad(optimizer.get());
        check();
    }
    void step()
    {
        auto v = THSNN_Optimizer_step(optimizer.get(), nullptr);
        check();
        if (v)
            THSTensor_dispose(v);
    }
};
} // namespace studytorch
