#pragma once

#include "fvc_dataset.h"
#include "tiff_image.h"

namespace fvc {

class ImageTransform {
public:
    virtual ~ImageTransform() = default;
    const Condition& condition() const { return condition_; }
    virtual void apply(GrayImage& image, std::uint64_t seed) const = 0;

protected:
    explicit ImageTransform(Condition condition) : condition_(condition) {}

private:
    Condition condition_;
};

class GaussianNoise final : public ImageTransform {
public:
    GaussianNoise(int sigma, std::uint64_t baseSeed);
    void apply(GrayImage& image, std::uint64_t seed) const override;
};

class Rotation final : public ImageTransform {
public:
    // Positive angles rotate counterclockwise about the image center.
    explicit Rotation(int degrees);
    void apply(GrayImage& image, std::uint64_t seed) const override;
};

class Brightness final : public ImageTransform {
public:
    explicit Brightness(int offset);
    void apply(GrayImage& image, std::uint64_t seed) const override;
};

class HorizontalFlip final : public ImageTransform {
public:
    HorizontalFlip();
    void apply(GrayImage& image, std::uint64_t seed) const override;
};

} // namespace fvc
