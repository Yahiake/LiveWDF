#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <algorithm>

namespace wdf::preview {

// One decaying real pole or complex-conjugate pair. re/im give the pole;
// a,b are the real and imaginary basis weights in the impulse response.
struct Mode { double re, im, a, b; };

class ModalPreview {
public:
    static constexpr std::size_t capacity = 32;

    // Call off the audio thread. Preserve a running instance if setup fails.
    bool prepare(double direct, std::span<const Mode> modes) noexcept {
        if (!std::isfinite(direct) || modes.size() > capacity) return false;
        for (auto m : modes) {
            if (!std::isfinite(m.re) || !std::isfinite(m.im) ||
                !std::isfinite(m.a) || !std::isfinite(m.b) ||
                m.re*m.re + m.im*m.im >= 1.0) return false;
        }
        count_ = modes.size();
        direct_ = direct;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto m = modes[i];
            re_[i] = m.re;
            im_[i] = m.im;
            a_[i] = m.a;
            b_[i] = -m.b;
        }
        reset();
        return true;
    }

    void reset() noexcept { real_.fill(0.0); imag_.fill(0.0); }

    double step(double in) noexcept {
        double out = direct_ * in;
        for (std::size_t i = 0; i < count_; ++i) {
            const double r = real_[i], j = imag_[i];
            out += r;
            real_[i] = re_[i]*r - im_[i]*j + a_[i]*in;
            imag_[i] = im_[i]*r + re_[i]*j + b_[i]*in;
        }
        return out;
    }

    // The audio host may alternatively flush denormals itself.
    void finishBlock() noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::abs(real_[i]) < 1e-30) real_[i]=0.;
            if (std::abs(imag_[i]) < 1e-30) imag_[i]=0.;
        }
    }
private:
    std::size_t count_{};
    double direct_{};
    alignas(32) std::array<double,capacity> re_{},im_{},a_{},b_{},real_{},imag_{};
};
}
