#pragma once

// Reference transistor cards used by the ngspice oracle matrix.
//
// The electrical contents are from the local LTspice standard libraries:
//
//   C:\Users\PC\AppData\Local\LTspice\lib\cmp\standard.bjt
//   C:\Users\PC\AppData\Local\LTspice\lib\cmp\standard.mos
//   C:\Users\PC\AppData\Local\LTspice\lib\cmp\standard.jft
//
// (accessed 2026-10-25). The cards are embedded so the tests do not depend on a
// particular LTspice installation being present. Only metadata and parameters
// that ngspice reports as unsupported are removed; the notes on each block say
// exactly what was removed. Core electrical parameters are unchanged.
//
// These are characterization cards for the preset-realism tests. The
// equation-matched tests build minimal cards from the LiveWDF model parameters
// instead, so a full-card effect (Early, high injection, series resistance,
// junction capacitance, etc.) is never attributed to the simplified law.

namespace lwdf_test {

// ---------------------------------------------------------------------------
// BJT cards (LTspice standard.bjt). Removed: Vceo, Icrating, mfg.
// ---------------------------------------------------------------------------

inline constexpr const char* kBjt2N3904 =
    ".model 2N3904 NPN(IS=1E-14 VAF=100 Bf=300 IKF=0.4 XTB=1.5 BR=4 "
    "CJC=4E-12 CJE=8E-12 RB=20 RC=0.1 RE=0.1 TR=250E-9 TF=350E-12 ITF=1 "
    "VTF=2 XTF=3)";

inline constexpr const char* kBjt2N2222 =
    ".model 2N2222 NPN(IS=1E-14 VAF=100 BF=200 IKF=0.3 XTB=1.5 BR=3 "
    "CJC=8E-12 CJE=25E-12 TR=100E-9 TF=400E-12 ITF=1 VTF=2 XTF=3 RB=10 "
    "RC=.3 RE=.2)";

inline constexpr const char* kBjtBC547B =
    ".model BC547B NPN(IS=2.39E-14 NF=1.008 ISE=3.545E-15 NE=1.541 BF=294.3 "
    "IKF=0.1357 VAF=63.2 NR=1.004 ISC=6.272E-14 NC=1.243 BR=7.946 IKR=0.1144 "
    "VAR=25.9 RB=1 IRB=1.00E-06 RBM=1 RE=0.4683 RC=0.85 XTB=0 EG=1.11 XTI=3 "
    "CJE=1.358E-11 VJE=0.65 MJE=0.3279 TF=4.391E-10 XTF=120 VTF=2.643 "
    "ITF=0.7495 PTF=0 CJC=3.728E-12 VJC=0.3997 MJC=0.2955 XCJC=0.6193 "
    "TR=1.00E-32 CJS=0 VJS=0.75 MJS=0.333 FC=0.9579)";

inline constexpr const char* kBjt2N3906 =
    ".model 2N3906 PNP(IS=1E-14 VAF=100 BF=200 IKF=0.4 XTB=1.5 BR=4 "
    "CJC=4.5E-12 CJE=10E-12 RB=20 RC=0.1 RE=0.1 TR=250E-9 TF=350E-12 ITF=1 "
    "VTF=2 XTF=3)";

inline constexpr const char* kBjt2N2907 =
    ".model 2N2907 PNP(IS=1E-14 VAF=120 BF=250 IKF=0.3 XTB=1.5 BR=3 "
    "CJC=8E-12 CJE=30E-12 TR=100E-9 TF=400E-12 ITF=1 VTF=2 XTF=3 RB=10 "
    "RC=.3 RE=.2)";

inline constexpr const char* kBjtBC557B =
    ".model BC557B PNP(IS=3.83E-14 NF=1.008 ISE=1.22E-14 NE=1.528 BF=344.4 "
    "IKF=0.08039 VAF=21.11 NR=1.005 ISC=2.85E-13 NC=1.28 BR=14.84 IKR=0.047 "
    "VAR=32.02 RB=1 IRB=1.00E-06 RBM=1 RE=0.6202 RC=0.5713 XTB=0 EG=1.11 "
    "XTI=3 CJE=1.23E-11 VJE=0.6106 MJE=0.378 TF=5.60E-10 XTF=3.414 VTF=5.23 "
    "ITF=0.1483 PTF=0 CJC=1.08E-11 VJC=0.1022 MJC=0.3563 XCJC=0.6288 "
    "TR=1.00E-32 CJS=0 VJS=0.75 MJS=0.333 FC=0.8027)";

inline constexpr const char* kBjt2N5087 =
    ".model 2N5087 PNP(Is=6.734f Xti=3 Eg=1.11 Vaf=45.7 Bf=254.1 Ne=1.741 "
    "Ise=6.734f Ikf=.1962 Xtb=1.5 Br=2.683 Nc=2 Isc=0 Ikr=0 Rc=1.67 Cjc=6.2p "
    "Mjc=.301 Vjc=.75 Fc=.5 Cje=7.5p Mje=.2861 Vje=.75 Tr=10.1n Tf=467.8p "
    "Itf=.17 Vtf=5 Xtf=8 Rb=10)";

// ---------------------------------------------------------------------------
// MOSFET cards (LTspice standard.mos).
//
// BS170: removed Rg, Rb, Rds and N (ngspice's Level-3 model reports these as
// unrecognized). The Level-3 core, geometry and capacitances are unchanged.
//
// 2N7002: removed mfg, Vds, Ron and Qg (LTspice metadata, not VDMOS electrical
// parameters). The VDMOS core is unchanged. VDMOS instances do not take a W/L
// override in ngspice; the model card supplies the geometry.
// ---------------------------------------------------------------------------

inline constexpr const char* kMosBs170 =
    ".model BS170 NMOS (Level=3 W=200u L=1u Tox=0.05u Vto=2.14 Kp=4e-4 "
    "Vmax=0 Uo=700 Phi=0.6 NSUB=1e15 NFS=9e11 Gamma=0.1 Delta=0 Eta=0 Theta=0 "
    "Kappa=0.005 Rs=0 Rd=0.0 Cgso=140n Cgdo=10n Cgbo=10n Cbd=100p Cj=2e-4 "
    "Cjsw=1n Is=10f Pb=.8)";

inline constexpr const char* kMos2N7002 =
    ".model 2N7002 VDMOS(Rg=3 Vto=1.6 Rd=0 Rs=.75 Rb=.14 Kp=.17 mtriode=1.25 "
    "Cgdmax=80p Cgdmin=12p Cgs=50p Cjo=50p Is=.04p ksubthres=.1)";

// ---------------------------------------------------------------------------
// JFET cards (LTspice standard.jft).
//
// LSK170A/B/C: removed Mj (ngspice's NJF model reports it as unrecognized) and
// mfg. M (grading coefficient) and all core parameters are unchanged.
//
// LSK189A/B/C: removed Isr, Alpha and Vk (unrecognized by ngspice's NJF model)
// and Mfg. The square-law core, series resistances and capacitances are
// unchanged.
// ---------------------------------------------------------------------------

inline constexpr const char* kJfetLsk170A =
    ".MODEL LSK170A NJF(Beta=35E-3 Betatce=-0.5 Vto=-0.35 Lambda=0.024 IS=14f "
    "N=1 Rd=4 Rs=1.1 Cgd=19p Cgs=27p Fc=0.5 M=320m Pb=0.6 Kf=0.004f Af=1.0 "
    "Gdsnoi=8 XTI=0 Nlev=3)";

inline constexpr const char* kJfetLsk170B =
    ".MODEL LSK170B NJF(Beta=30E-3 Betatce=-0.5 Vto=-0.54 Lambda=0.02 IS=15f "
    "N=1 Rd=4.9 Rs=5.1 Cgd=20p Cgs=27p Fc=0.5 M=320m Pb=0.6 Kf=0.004f Af=1.0 "
    "Gdsnoi=7 XTI=0 Nlev=3)";

inline constexpr const char* kJfetLsk170C =
    ".MODEL LSK170C NJF(Beta=34E-3 Betatce=-0.5 Vto=-0.83 Lambda=0.011 IS=27f "
    "N=1 Rd=9.9 Rs=9.6 Cgd=15p Cgs=26p Fc=0.5 M=320m Pb=0.6 Kf=0.005f Af=1.0 "
    "Gdsnoi=5 XTI=0 Nlev=3)";

inline constexpr const char* kJfetLsk189A =
    ".model LSK189A NJF(Beta=2.2m Betatce=-0.5 Vto=-1.13 Vtotc=-2.5m "
    "Lambda=4.3m Is=3f Xti=0 N=1 Rd=11 Rs=30 Cgd=3.19p Cgs=2.92p Fc=0.5 "
    "M=320m Pb=0.8 Kf=0.0009f Af=1 Gdsnoi=2.15 Nlev=3)";

inline constexpr const char* kJfetLsk189B =
    ".model LSK189B NJF(Beta=2.1m Betatce=-0.5 Vto=-1.80 Vtotc=-2.5m "
    "Lambda=5.7m Is=3f Xti=0 N=1 Rd=11 Rs=40 Cgd=3.19p Cgs=2.92p Fc=0.5 "
    "M=320m Pb=0.8 Kf=0.0009f Af=1 Gdsnoi=2.15 Nlev=3)";

inline constexpr const char* kJfetLsk189C =
    ".model LSK189C NJF(Beta=2.1m Betatce=-0.5 Vto=-2.78 Vtotc=-2.5m "
    "Lambda=6.3m Is=3f Xti=0 N=1 Rd=11 Rs=55 Cgd=3.19p Cgs=2.92p Fc=0.5 "
    "M=320m Pb=0.8 Kf=0.0009f Af=1 Gdsnoi=2.15 Nlev=3)";

}  // namespace lwdf_test
