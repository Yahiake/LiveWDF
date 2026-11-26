#define DOD250_GAIN_MAX 16
#define DOD250_LEVEL_MAX 100
#define DOD250_LEVEL_DEFAULT 73
#define LIVEWDF_LEVEL_MAX 200

enum { MODEL_DOD250, MODEL_MICROAMP };

#define GP5_PARAMS(P, C) \
    P(GAIN, "Gain", "Circuit", 0, DOD250_GAIN_MAX, 12, GP5_U_CUSTOM, GP5_P_DSP) \
    P(LEVEL, "Level", "Circuit", 0, LIVEWDF_LEVEL_MAX, DOD250_LEVEL_DEFAULT, GP5_U_PERCENT, GP5_P_DSP) \
    C(ON, "Effect", "Circuit", 2, "Bypass|On", 1, GP5_P_DSP) \
    C(MODEL, "Model", "Circuit", 2, "DOD 250|Micro Amp", MODEL_DOD250, GP5_P_DSP) \
    C(CAB, "Cab", "Output", 2, "Off|On", 0, GP5_P_DSP) \
    P(DELAY, "Delay", "Output", 0, 100, 0, GP5_U_PERCENT, GP5_P_DSP) \
    P(REVERB, "Reverb", "Output", 0, 100, 0, GP5_U_PERCENT, GP5_P_DSP)
