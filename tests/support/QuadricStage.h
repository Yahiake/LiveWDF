#pragma once

#include "devices/QuadricTriode.h"

namespace wdf {

/// The paper's Sec. 8 common-cathode gain stage, ported line-for-line from the
/// authors' own GNU Octave reference
/// (https://dangelo.audio/assets/code/triode_stage_quadric.m), with the paper's
/// Table 4 component values as defaults.
///
///     R. Giampiccolo, S. D'Angelo, A. Bernardini, and A. Sarti,
///     "A Quadric Surface Model of Vacuum Tubes for Virtual Analog
///     Applications," Proc. 26th Int. Conf. on Digital Audio Effects
///     (DAFx23), Copenhagen, Denmark, Sept. 2023, pp. 296-303.
///
/// Because this is a line-for-line port of that script, the upstream MIT
/// notice is reproduced here as its license requires, and the same notice is
/// recorded in `THIRD-PARTY-NOTICES.md`:
///
///     Copyright (C) 2023 Stefano D'Angelo
///
///     Permission is hereby granted, free of charge, to any person obtaining a
///     copy of this software and associated documentation files (the
///     "Software"), to deal in the Software without restriction, including
///     without limitation the rights to use, copy, modify, merge, publish,
///     distribute, sublicense, and/or sell copies of the Software, and to
///     permit persons to whom the Software is furnished to do so, subject to
///     the following conditions:
///
///     The above copyright notice and this permission notice shall be included
///     in all copies or substantial portions of the Software.
///
///     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
///     OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
///     MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
///     IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
///     CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
///     TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
///     SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///
/// This exists to validate the triode element inside a real network -- it is
/// not part of the Mark IIC+ signal path. It is a single triode at the root of
/// a WDF tree whose three triode ports are all reflection-free, so the whole
/// stage is one forward scatter plus one backward pass per sample: no Newton,
/// no iteration. That is the structure the paper benchmarks at 4.6x, and the
/// structure our six-triode preamp *cannot* use unchanged.
///
///     Vi --( )--+-- Ci --+-- Ri --+
///               |        |        |
///               |       GND      ...
///              x(i)   |-]  |-   grid junction -- Rg -- tube grid
///                                        ...
///
/// Every junction value below carries the same name as the Octave script, so
/// the two can be diffed by eye.
struct QuadricCommonCathode {
    // ---- circuit (paper Table 4) ----
    double E = 250.0;     // V   DC supply
    double Rp = 100e3;    // ohm plate resistor
    double Ri = 1e6;      // ohm input resistor
    double Rg = 20e3;     // ohm grid resistor
    double Rk = 1e3;      // ohm cathode resistor
    double Ck = 10e-6;    // F   cathode bypass
    double Ci = 100e-9;   // F   input coupling
    double Co = 10e-9;    // F   output coupling
    double Ro = 1e6;      // ohm load resistor

    QuadricTriodeParams params{};
    QuadricProjection projection = QuadricProjection::Reference;
    bool dcInit = true;

    // ---- WDF resistances and adaptor/filter coefficients ----
    double fs = 44100.0;
    double wCi_R = 0.0, wCk_R = 0.0, wCo_R = 0.0;
    double wsi_kl = 0.0, wsi_R = 0.0, wpg_kt = 0.0, wpg_R = 0.0, wsg_kl = 0.0, wsg_R = 0.0;
    double wpk_kt = 0.0, wpk_R = 0.0, wsp_kl = 0.0, wsp_R = 0.0, wpp_kt = 0.0, wpp_R = 0.0;
    double kTxCi = 0.0, kTCk = 0.0, kTCo = 0.0, kT0 = 0.0;
    double kyT = 0.0, kyCo = 0.0, ky0 = 0.0;
    double kCiT = 0.0, kCixCi = 0.0, kCoCo = 0.0, kCo0 = 0.0;

    // ---- state ----
    double wCi_s = 0.0, wCk_s = 0.0, wCo_s = 0.0;
    QuadricTriodeWdf tube;
    QuadricCommonCathodeBias bias;
    bool ready = false;

    /// The ideal voltage source's port resistance, as in the reference. Kept
    /// non-zero because a WDF port resistance must be positive.
    static constexpr double kWviR = 1e-6;

    bool prepare(double sampleRate) noexcept {
        fs = sampleRate;
        if (!(fs > 0.0) || !params.valid() || !(E > 0.0) || !(Rp > 0.0) || !(Ri > 0.0) || !(Rg > 0.0) ||
            !(Rk > 0.0) || !(Ro > 0.0) || !(Ck > 0.0) || !(Ci > 0.0) || !(Co > 0.0))
            return false;

        wCi_R = 1.0 / (2.0 * fs * Ci);
        wCk_R = 1.0 / (2.0 * fs * Ck);
        wCo_R = 1.0 / (2.0 * fs * Co);

        wsi_kl = wCi_R / (wCi_R + kWviR);
        wsi_R = wCi_R + kWviR;
        wpg_kt = wsi_R / (wsi_R + Ri);
        wpg_R = (wsi_R * Ri) / (wsi_R + Ri);
        wsg_kl = Rg / (Rg + wpg_R);
        wsg_R = Rg + wpg_R;
        wpk_kt = wCk_R / (Rk + wCk_R);
        wpk_R = (Rk * wCk_R) / (Rk + wCk_R);
        wsp_kl = wCo_R / (wCo_R + Ro);
        wsp_R = wCo_R + Ro;
        wpp_kt = wsp_R / (wsp_R + Rp);
        wpp_R = (wsp_R * Rp) / (wsp_R + Rp);

        kTxCi = 1.0 - wpg_kt;
        kTCk = 1.0 - wpk_kt;
        kTCo = 1.0 - wpp_kt;
        kT0 = wpp_kt * E;
        kyT = 0.5 * (1.0 - wsp_kl);
        kyCo = -0.5 * (1.0 - wsp_kl) * (1.0 + wpp_kt);
        ky0 = 0.5 * (1.0 - wsp_kl) * wpp_kt * E;
        kCiT = wsi_kl * (1.0 - wsg_kl);
        kCixCi = wsi_kl * ((1.0 - wpg_kt) * (wsg_kl + 1.0) - 2.0);
        kCoCo = 1.0 - wsp_kl * (1.0 + wpp_kt);
        kCo0 = wsp_kl * wpp_kt * E;

        tube.p = params;
        tube.Zg = wsg_R;
        tube.Zk = wpk_R;
        tube.Zp = wpp_R;
        tube.projection = projection;
        ready = tube.prepare();
        if (!ready)
            return false;

        bias = quadricCommonCathodeBias(params, E, Rp, Rk);
        reset();
        return true;
    }

    /// Sets the reactive state to the stage's DC operating point, which is what
    /// the authors' reference does (Vk0 on Ck, Vp0 on Co, zero on Ci).
    void reset() noexcept {
        wCi_s = 0.0;
        wCk_s = dcInit ? bias.vk : 0.0;
        wCo_s = dcInit ? bias.vp : 0.0;
    }

    double process(double x) noexcept {
        const double xCi = x + wCi_s;
        const double ag = kTxCi * xCi;             // grid port incident wave
        const double ak = kTCk * wCk_s;            // cathode
        const double ap = kTCo * wCo_s + kT0;      // plate
        double bg, bk, bp;
        tube.reflect(ag, ak, ap, bg, bk, bp);
        const double y = kyT * bp + kyCo * wCo_s + ky0;
        wCi_s = kCiT * bg + kCixCi * xCi + wCi_s;
        wCk_s = bk - wpk_kt * wCk_s;
        wCo_s = wsp_kl * bp + kCoCo * wCo_s + kCo0;
        return y;
    }

    /// Convenience: run a block in place-free style, writing `n` outputs.
    void processBlock(const double* in, double* out, int n) noexcept {
        for (int i = 0; i < n; ++i)
            out[i] = process(in[i]);
    }
};

}  // namespace wdf
