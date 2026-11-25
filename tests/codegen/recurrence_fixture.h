#pragma once
#include "spice/Circuit.h"

// A biased, loaded RLC branch with a floating capacitor, source-relative
// output and a user-selected number of nonlinear ports.
inline wdf::Circuit recurrenceFixture(int variant) {
    wdf::Circuit c;
    c.addSrc("in", "0", .1);
    c.addSrc("out", "mid", .25);
    c.addRes("in", "a", 1200.);
    c.addRes("a", "mid", 330.);
    c.addRes("mid", "0", 2700.);
    c.addCap("a", "mid", 3.3e-7);
    c.addCap("out", "0", 4.7e-8);
    c.addInd("a", "tap", .012);
    c.addRes("tap", "0", 4700.);
    if (variant == 1 || variant == 2) c.addDiode("mid", "0", {2.e-9, 1.3, 2., 5.e6});
    if (variant == 2) c.addDiode("0", "a", {4.e-9, 1.4, .5, 1.e6});
    if (variant == 3) {
        c.addSrc("vdd", "0", 1.);
        c.addSrc("0", "vee", 1.);
        auto amp = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl061);
        amp.setVRails(.7, .7);
        amp.slewRate = .008;
        c.addOpAmp("tap", "tap", "in", "vdd", "vee", amp);
    }
    return c;
}
