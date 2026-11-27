# Factory presets

The Factory browser ships 111 presets in three collections.

- **Amps (25)** covers complete dry signal paths for 25 amp and channel selections. Use **Power Amp** beside **Circuit** to choose the complete amp, calibrated preamp tap or isolated power stage.
- **Pedals (46)** covers drive, fuzz, delay, modulation, EQ, filters, and dynamics.
- **Studio (40)** covers preamps, EQ, dynamics, and saturation or enhancement.

[Sorted by mean CPU](#mean-cpu-time) · [Sorted by p99 CPU](#p99-cpu-time) · [Benchmark method](#benchmark-method)

## Mean CPU time

Lower is lighter. All timings are in microseconds of CPU time per 128-sample block. Amp rows labeled Preamp measure the selector's Preamp position.

| Preset | Collection | Type | Circuit | Mean µs | Median µs | p99 µs |
| --- | --- | --- | --- | ---: | ---: | ---: |
| [MXR Micro Amp][preset-38] | Pedals | Boost | TL061 boost with the original 50 pF feedback capacitor. | 21.2 | 21.0 | 25.0 |
| [API 312 Preamp VP312 Rev A][preset-89] | Studio | Preamp | VP312 Rev A preamp with an AP2520 model and input and output transformers. | 27.0 | 27.0 | 38.0 |
| [Orange Rockerverb 50w - Clean Channel][preset-4] | Amps | Preamp | Clean-channel preamp modeled with two 12AX7 triode sections. | 30.3 | 31.0 | 37.0 |
| [Ampeg B-15N Portaflex][preset-92] | Amps | Preamp | Rev C Portaflex Channel One Normal jack through the 6SL7 stages and passive Bass/Treble network to the Ext. | 32.5 | 31.0 | 43.0 |
| [Fender Princeton Reverb AA1164][preset-96] | Amps | Preamp | Black-panel Princeton Reverb dry high-input path through its two-band stack and the third recovery triode. | 32.7 | 32.0 | 58.0 |
| [Orange OR120 Graphic MkII][preset-106] | Amps | Preamp | Orange Graphic MkII two-stage preamp from the January 1974 service drawing. | 32.7 | 32.0 | 48.0 |
| [Ibanez TS_808][preset-32] | Pedals | Overdrive | Tube Screamer circuit with JRC4558 op-amps and 1N914 clipping diodes. | 36.1 | 36.0 | 46.0 |
| [Hiwatt DR103][preset-100] | Amps | Preamp | Four-input DR103 Normal high path with the plate-driven three-band stack, master volume and V3A stage. | 36.9 | 35.0 | 46.0 |
| [DOD 250 Overdrive Preamp Gray][preset-21] | Pedals | Overdrive | Gray-version LM741 overdrive with frequency-dependent gain and shunt diode clipping. | 40.1 | 39.0 | 56.0 |
| [ENGL E530 - Lead Hi Gain][preset-93] | Amps | Preamp | Four ECC83 triodes in Lead Hi Gain mode, with its four-band tone network and instrument send divider. | 52.4 | 45.0 | 156.0 |
| [Dan Armstrong Orange Squeezer][preset-22] | Pedals | Compressor | Feedback compressor with a JFET gain cell and germanium envelope detector. | 60.5 | 61.0 | 81.0 |
| [Marshall Guv'nor][preset-42] | Pedals | Distortion | First-generation distortion with red LED clipping and a normalled effects loop. | 61.3 | 55.0 | 112.0 |
| [Marshall JCM800 2203 - High Input][preset-104] | Amps | Preamp | High input through the JCM800 2203 cold clipper, cathode follower, passive EQ and master volume. | 65.8 | 51.0 | 180.0 |
| [Maag EQ4-500 Parallel Circuit Model][preset-78] | Studio | EQ | Parallel RC equalizer with four fixed bells and five selectable Air frequencies. | 66.6 | 68.0 | 75.0 |
| [Vox AC15 No. 3 - EF86 Normal][preset-110] | Amps | Preamp | Normal input 1 from Jennings OA/031 with the EF86, screen decoupling, Brilliance switch and 500 kΩ Volume. | 69.6 | 68.0 | 98.0 |
| [Boss SD-1 Super OverDrive][preset-18] | Pedals | Overdrive | Asymmetric feedback clipping with one silicon junction against two. | 71.5 | 73.0 | 90.0 |
| [Orange Rockerverb 50w - Dirty Channel][preset-5] | Amps | Preamp | Dirty-channel preamp modeled with four 12AX7 triode sections. | 72.4 | 50.0 | 180.0 |
| [Trident Series 80B Four-Band EQ S800][preset-87] | Studio | EQ | Four-band active RC EQ with two swept mids, shared shelf stages, and low cut. | 75.6 | 75.0 | 93.0 |
| [Klon Centaur][preset-33] | Pedals | Overdrive | S 698 circuit with a fixed clean summing feed and retained bypass-buffer loading. | 77.0 | 77.0 | 94.0 |
| [Marshall Bluesbreaker Mk I][preset-41] | Pedals | Overdrive | Black-box overdrive with a TL072, linked gain stages, and series clipping diodes. | 77.0 | 70.0 | 143.0 |
| [Pro Co RAT LM308][preset-47] | Pedals | Distortion | LM308 distortion with silicon clipping and an internal 30 pF compensation model. | 79.0 | 81.0 | 92.0 |
| [Peavey 5150 - Lead][preset-107] | Amps | Preamp | Original 5150 high-input Lead path with the 39k cold clipper, parallel V5 feed, passive EQ and Post control. | 86.0 | 59.0 | 262.0 |
| [Laney GH100L - Drive][preset-101] | Amps | Preamp | High input with Drive engaged, both gain controls, cold clipper and three-band EQ. | 97.5 | 54.0 | 316.0 |
| [Mesa Boogie Mark IIC+ - Lead][preset-105] | Amps | Preamp | Vintage Mark IIC+ Lead mode with the early tone stack, cascaded lead stages, and parallel dry path. | 100.9 | 71.0 | 360.0 |
| [Dunlop Cry Baby GCB-95][preset-24] | Pedals | Wah | Buffered transistor wah with separate AC treadle feedback and DC bias paths. | 106.0 | 104.0 | 167.0 |
| [Mesa Boogie Dual Rectifier][preset-3] | Amps | Preamp | Dual Rectifier preamp modeled with five 12AX7 triode sections. | 113.1 | 82.0 | 281.0 |
| [Origin Effects Cali76 FET Compressor (Circuit Approximation)][preset-46] | Pedals | Compressor | 1176-inspired JFET compressor approximation with independent attack, release, and dry and compressed levels. | 120.3 | 115.0 | 182.0 |
| [Fender Twin Reverb AB763][preset-99] | Amps | Preamp | Black-panel Twin Reverb Normal high input with the 120 pF Bright switch engaged and its three-band tone stack. | 133.2 | 135.0 | 159.0 |
| [Fender Tweed Deluxe 5E3 - Instrument][preset-98] | Amps | Preamp | Input 1 of the Instrument channel on Fender drawing 5E3, with both interactive volume controls, the shared 12AY7 cathode and the 12AX7 gain stage. | 134.3 | 132.0 | 177.0 |
| [Fender Bassman 5F6-A][preset-94] | Amps | Preamp | Tweed Bassman Normal high jack, 12AY7 input and cathode-follower three-band preamp. | 148.8 | 144.0 | 218.0 |
| [Mosrite Fuzzrite Silicon][preset-43] | Pedals | Fuzz | Two-stage silicon fuzz with a 22 kΩ load on the Depth blend. | 149.5 | 153.0 | 199.0 |
| [Fuzz Face][preset-30] | Pedals | Fuzz | Two-transistor fuzz modeled with germanium AC128 devices. | 150.6 | 146.5 | 225.0 |
| [MCI JH-400B Mic Preamp 43E241][preset-90] | Studio | Preamp | Microphone input to patch output with live input pad, gain, and representative 1:10 transformer. | 158.5 | 151.0 | 229.0 |
| [Marshall 1959 Super Lead - Jumped][preset-102] | Amps | Preamp | Marshall 1959 Super Lead from the July 1970 Unicord drawing. Split-cathode V1, 500 pF bright mix, bypassed V2a, 47 kΩ feedback from the 16 Ω tap with the 5 kΩ Presence, four EL34s on fixed rails and bias. JCM800 era switches to the 1981 Lead Series values and its 100 kΩ feedback from the 4 Ω tap with the 22 kΩ Presence. | 160.2 | 138.0 | 272.0 |
| [Harrison 32-Series 3232 Channel EQ][preset-76] | Studio | EQ | Four-band proportional-Q EQ with separate sweepable high-pass and low-pass filters. | 163.0 | 162.0 | 211.0 |
| [Roland JC-120][preset-108] | Amps | Preamp | 1979 Jazz Chorus Channel 1 high input and four 2SK117-GR JFETs, with passive three-band EQ and switchable Bright. | 166.5 | 166.0 | 180.0 |
| [Colorsound Power Boost 18V][preset-20] | Pedals | Boost | Three-transistor 18 V boost with an active Baxandall tone network. | 168.5 | 170.0 | 187.0 |
| [Nobels ODR-1 Natural Overdrive][preset-45] | Pedals | Overdrive | Rev 5 overdrive with a Spectrum gyrator and feedback and shunt clipping. | 177.2 | 175.0 | 226.0 |
| [Boss DS-1A Distortion][preset-11] | Pedals | Distortion | 1994 engaged distortion circuit with silicon clipping diodes and op-amp gain. | 182.3 | 178.0 | 272.0 |
| [Fender Tweed Champ 5F1 - Input 1][preset-97] | Amps | Amplifier | Fender 5F1 high input, two 12AX7 stages and a cathode-biased 6V6 with transformer feedback. | 212.5 | 200.0 | 340.0 |
| [Boss BF-2 Flanger][preset-7] | Pedals | Flanger | MN3207 flanger with diode limiting, adjustable delay bias, and post-filter resonance. | 221.1 | 222.0 | 288.0 |
| [SPL Transient Designer 9842 DET Model][preset-63] | Studio | Transient shaper | Differential envelope model with independent Attack and Sustain driving a THAT2180 gain cell. | 221.2 | 218.0 | 295.0 |
| [Vox AC30 - Top Boost][preset-111] | Amps | Preamp | Brilliant input from Jennings OS/065 with the OS/010 Top Boost add-on. | 221.6 | 216.0 | 395.0 |
| [Roger Mayer Octavia Transistor][preset-48] | Pedals | Fuzz | Transistor octave fuzz with Darlington stages and opposite half-cycle silicon rectifiers. | 222.0 | 219.0 | 288.0 |
| [Helios Type 69 Musicland EQ][preset-77] | Studio | EQ | Switched LC mids, independent bass cut, and discrete 2128 makeup. | 241.9 | 154.0 | 683.0 |
| [EHX Electric Mistress 9V][preset-27] | Pedals | Flanger | SAD1024 flanger model with shaped feedback and a Filter Matrix switch. | 242.6 | 241.0 | 317.0 |
| [Fender Deluxe Reverb AB763][preset-95] | Amps | Preamp | Black-panel Deluxe Reverb dry Vibrato channel, high input, 47 pF volume bright cap and reverb mixer recovery triode. | 255.8 | 251.0 | 336.0 |
| [Sontec MES-432C-9 Circuit Model][preset-84] | Studio | EQ | Three adjustable bridged feedback resonators, two shelves, and a servo-stabilized output. | 263.3 | 263.0 | 313.0 |
| [Neve 1073 Line Preamp EH10023][preset-91] | Studio | Preamp | Transformer-coupled line path with selectable input attenuation and a discrete BA283AV stage. | 263.9 | 154.0 | 657.0 |
| [SSL 4000 E 82E242 Black EQ][preset-83] | Studio | EQ | Black EQ core with four two-op-amp filters and switchable low and high bell bands. | 264.5 | 263.0 | 327.0 |
| [Trident A-Range 1038 Four-Band EQ ED3001][preset-86] | Studio | EQ | Four stepped-frequency EQ bands with two discrete class-A amplifiers. | 267.0 | 147.0 | 535.0 |
| [Boss TR-2 Tremolo][preset-19] | Pedals | Tremolo | VCA tremolo with an analog oscillator and separate Rate, Wave, and Depth. | 275.8 | 269.0 | 367.0 |
| [Boss MT-2 Metal Zone][preset-15] | Pedals | Distortion | Engaged distortion circuit with ganged Mid Frequency controls for swept mids. | 288.7 | 289.0 | 371.0 |
| [Sola Sound Tone Bender Professional Mk II][preset-50] | Pedals | Fuzz | Early positive-ground fuzz with three germanium transistor stages. | 294.1 | 292.0 | 451.0 |
| [Pultec EQP-1A Program Equalizer][preset-81] | Studio | EQ | Passive EQ with interacting bass boost and attenuation, switched resonators, and push-pull tube makeup. | 299.1 | 296.0 | 442.0 |
| [EHX Big Muff Pi Ram's Head Violet][preset-25] | Pedals | Fuzz | 1973 Violet fuzz with two diode-feedback clipping stages and an asymmetric tone network. | 334.7 | 291.0 | 557.0 |
| [Soldano SLO-100 - Overdrive][preset-109] | Amps | Preamp | Classic overdrive channel with the parallel clean branch, cold clipper, effects buffer and recovery, and cathode-follower tone stack. | 336.8 | 269.0 | 739.0 |
| [ZVEX Fuzz Factory][preset-51] | Pedals | Fuzz | Five-control fuzz with an input transistor and two germanium AC128 stages. | 338.4 | 349.0 | 506.0 |
| [API 550A Proportional-Q Circuit Model][preset-73] | Studio | EQ | Early 15-frequency proportional-Q approximation with Twin-T filters and transformer output. | 340.5 | 335.0 | 417.0 |
| [UREI-1176LN-Rev-D][preset-66] | Studio | Compressor | Rev D FET compressor with dual-phase feedback detection and a transformer-loaded class-A output. | 343.9 | 338.0 | 476.0 |
| [Boss HM-2 Heavy Metal][preset-14] | Pedals | Distortion | Germanium and silicon clipping with three active gyrators under two Color Mix controls. | 348.2 | 350.0 | 424.0 |
| [API-2500][preset-53] | Studio | Compressor | Blackmer VCA compressor reconstruction with switchable feedforward or feedback detection and Thrust filtering. | 353.7 | 345.0 | 499.0 |
| [Pultec MEQ-5 Mid-Range Equalizer][preset-82] | Studio | EQ | Passive midrange EQ with switched boost and dip bands and tube amplification. | 355.5 | 355.0 | 468.0 |
| [BBE 482i Sonic Maximizer Channel B][preset-71] | Studio | Enhancer | Channel B with a pseudo state-variable filter and a level-dependent high-band stage. | 356.7 | 352.0 | 481.0 |
| [Maestro FZ-1 Fuzz-Tone 1962][preset-40] | Pedals | Fuzz | Positive-ground 3 V fuzz with three 2N270 germanium transistor models. | 361.8 | 358.5 | 541.0 |
| [dbx 902 De-Esser][preset-68] | Studio | De-esser | Dual RMS detectors and a Blackmer VCA with broadband or high-band-only reduction. | 366.7 | 353.0 | 531.0 |
| [EVH 5150 III 100S Stealth - Channel 3][preset-1] | Amps | Preamp | Channel 3 preamp modeled with seven 12AX7 triode sections. | 374.2 | 374.0 | 505.0 |
| [Boss CE-2 Chorus][preset-8] | Pedals | Chorus | MN3007 chorus with pre-emphasis, reconstruction filtering, and a TL022 modulation oscillator. | 390.7 | 384.0 | 489.0 |
| [Empirical Labs FATSO EL7][preset-57] | Studio | Compressor | Single-channel reconstruction with feedback compression, soft clipping, Warmth, and a switchable transformer model. | 392.4 | 381.0 | 539.0 |
| [Altec-436C][preset-54] | Studio | Compressor | Push-pull 6BC8 variable-mu gain stage with a 6CG7 line output. | 400.4 | 388.5 | 553.0 |
| [GML 8200 Series II Circuit Model][preset-75] | Studio | EQ | Five parallel tracking-notch EQ branches with continuously variable Q. | 402.3 | 405.0 | 462.0 |
| [EHX Small Clone MN3007][preset-28] | Pedals | Chorus | EH-460D chorus with MN3007 delay, switched depth, and the original wet mix. | 403.1 | 393.5 | 544.0 |
| [EMI-RS124][preset-56] | Studio | Compressor | 6BC8 variable-mu compressor with continuous Recovery and a timing Hold switch. | 405.2 | 403.5 | 489.0 |
| [Neve 1081 Gold Label Equalizer][preset-80] | Studio | EQ | Four Gold Label EQ stages with switched presence coils and shelf/peak selectors. | 411.9 | 404.0 | 532.0 |
| [Marshall 2555 Silver Jubilee - Lead][preset-103] | Amps | Preamp | Silver Jubilee Lead path with asymmetric LED and silicon clipping, lead master, normalled effects loop and the ganged bass tone network. | 432.9 | 408.0 | 745.0 |
| [Thermionic-Culture-Vulture][preset-72] | Studio | Tube saturator | Valve distortion reconstruction with selectable 6AS6 triode, pentode, and approximate P2 modes. | 441.6 | 427.0 | 584.0 |
| [API 560 Graphic EQ Gyrator Model][preset-74] | Studio | EQ | Ten octave-spaced active bandpass cells with a shared boost/cut mixer and transformer output. | 450.9 | 446.0 | 612.0 |
| [Aphex Aural Exciter Type B][preset-70] | Studio | Exciter | Tunable high-pass filter feeds a limiter detector and an OTA harmonic generator. | 451.4 | 446.0 | 575.0 |
| [ADR-Compex-F760X-RS][preset-52] | Studio | Compressor | JFET gain control shared by separate compressor, peak limiter, and expander/gate circuits. | 460.5 | 433.0 | 714.0 |
| [Pye-4060][preset-62] | Studio | Compressor | Baseband reconstruction of the transistor-chopper compressor with transformer equivalents and feedback timing. | 466.4 | 464.5 | 601.0 |
| [SSL-4000-G-Bus-Compressor][preset-64] | Studio | Compressor | Single bus-compressor lane with Blackmer VCA equivalents, full-wave detection, and Auto Release. | 470.7 | 460.0 | 716.0 |
| [EHX Deluxe Memory Man][preset-26] | Pedals | Analog delay | Dual-MN3005 echo with NE570 companding and selectable chorus or vibrato. | 476.0 | 466.0 | 618.0 |
| [dbx-160-VU][preset-69] | Studio | Compressor | Hard-knee compression modeled with the dbx 200 VCA and 209 RMS detector. | 476.5 | 460.0 | 670.0 |
| [Neve-33609J][preset-61] | Studio | Compressor | Diode-bridge dynamics with separate feedback paths for the compressor and limiter. | 478.7 | 466.0 | 649.0 |
| [Boss GE-7 Equalizer][preset-13] | Pedals | EQ | Seven-band graphic EQ with six gyrator bands, a high shelf, and Level. | 482.0 | 470.0 | 685.0 |
| [UREI-LA-3A][preset-67] | Studio | Compressor | T4B optical compression with transistor gain and a separate transformer-coupled optical driver. | 486.2 | 475.0 | 660.0 |
| [UREI 545 Parametric Equalizer][preset-88] | Studio | EQ | Four state-variable bands with variable end cuts and a transformer line output. | 486.9 | 473.0 | 764.0 |
| [MXR Dyna Comp CA3080][preset-35] | Pedals | Compressor | CA3080 compressor with a discrete full-wave detector and OTA Balance trim. | 489.1 | 479.5 | 1,135.0 |
| [Manley-Variable-Mu][preset-60] | Studio | Compressor | Transformer-coupled reconstruction with paired 5670 gain stages and 5687 output tubes. | 492.9 | 486.5 | 686.0 |
| [MXR M108S Ten Band EQ (Rev E Reference)][preset-36] | Pedals | EQ | Ten-band EQ with M108S-style dual outputs and a Rev E audio circuit. | 501.0 | 502.0 | 605.0 |
| [Marshall Valvestate 8100 - Boost Channel][preset-2] | Amps | Preamp | Boost-channel preamp with op-amp gain, diode clipping, and a low-voltage ECC83 stage. | 537.6 | 495.5 | 890.0 |
| [Diamond CPR-1 (Quartz Reference)][preset-23] | Pedals | Compressor | Quartz optical compressor with a VTL5C3 model and the original 900 Hz setting. | 555.2 | 534.0 | 817.0 |
| [Boss BD-2 Blues Driver][preset-6] | Pedals | Overdrive | Two JFET differential gain stages with ganged Gain and silicon diode clipping. | 579.9 | 563.0 | 880.0 |
| [Boss PQ-4 Parametric EQ][preset-17] | Pedals | EQ | Parametric EQ with linked tuning pairs for the High and Middle bands. | 587.6 | 586.0 | 762.0 |
| [MXR Phase 90 Script][preset-39] | Pedals | Phaser | Script-era phaser with four matched 2N5952 all-pass stages and an LM741 oscillator. | 627.8 | 607.5 | 917.0 |
| [Soundcraft Series 600 Mono Input][preset-85] | Studio | EQ | Mono line channel with transistor preamp, low cut, swept mids, and a fader amplifier. | 644.0 | 636.0 | 809.0 |
| [Boss NS-2 Noise Suppressor][preset-16] | Pedals | Noise suppressor | Noise suppressor with return emphasis, peak detection, and analog threshold and decay. | 718.1 | 371.0 | 2,136.0 |
| [Gates-Sta-Level-M5167][preset-59] | Studio | Compressor | 6386 broadcast leveler with a 6V6 output and Single or Double timing. | 725.7 | 708.0 | 1,113.0 |
| [Musitronics Mu-Tron III][preset-44] | Pedals | Envelope filter | Envelope filter with paired photoresistors and low-pass, band-pass, or high-pass operation. | 752.3 | 768.0 | 1,000.0 |
| [Boss DM-2 Analog Delay][preset-10] | Pedals | Analog delay | First-edition MN3005 delay with NE570 compression and expansion. | 764.3 | 741.0 | 1,217.0 |
| [Teletronix-LA-2A][preset-65] | Studio | Compressor | Valve-driven T4 optical compression with fast and program-dependent release. | 771.4 | 768.0 | 1,017.0 |
| [Boss DS-2 Turbo Distortion][preset-12] | Pedals | Distortion | Turbo I/II switches pre-emphasis, feedback bypass, and an output notch together. | 784.2 | 760.0 | 1,126.0 |
| [Boss CS-3 Compression Sustainer (August 1986)][preset-9] | Pedals | Compressor | August 1986 compressor circuit with a differential VCA and analog detector. | 790.8 | 757.0 | 1,194.0 |
| [MXR M234 Analog Chorus][preset-37] | Pedals | Chorus | Rev C approximation with 3207-family delay, wet/dry routing, and adjustable filters. | 929.5 | 907.0 | 1,402.0 |
| [ISP Decimator II G String (Noise Terminator Reference)][preset-31] | Pedals | Noise suppressor | Noise Terminator reference with THAT2180 gain control and an internally patched Send and Return. | 993.6 | 976.5 | 1,453.0 |
| [MXR Carbon Copy Analog Delay][preset-34] | Pedals | Analog delay | Four BL3208A delay lines with SA571 companding and switchable modulation. | 1,016.2 | 1,007.5 | 1,322.0 |
| [Fairchild-660][preset-58] | Studio | Compressor | 6386 variable-mu limiter with tube feedback stages and a switchable release tail. | 1,027.3 | 1,003.0 | 1,506.0 |
| [Manley Massive Passive Standard Circuit Model][preset-79] | Studio | EQ | Four parallel passive LC bands with selectable frequencies and tube makeup stages. | 1,072.1 | 1,072.0 | 1,355.0 |
| [Shin-ei Univox Uni-Vibe 915][preset-49] | Pedals | Phaser | Four staggered phase stages driven by a modeled lamp and photocell response. | 1,378.7 | 1,443.0 | 2,101.0 |
| [Drawmer-DS201][preset-55] | Studio | Noise gate | JFET gate with analog key filters, independent timing, and a Duck mode. | 1,577.6 | 1,558.5 | 2,276.0 |
| [EHX Small Stone Issue J][preset-29] | Pedals | Phaser | Four-stage OTA phaser with linked Color feedback and sweep range. | 2,304.3 | 2,217.0 | 3,724.0 |

## p99 CPU time

| Preset | p99 µs |
| --- | ---: |
| [MXR Micro Amp][preset-38] | 25.0 |
| [Orange Rockerverb 50w - Clean Channel][preset-4] | 37.0 |
| [API 312 Preamp VP312 Rev A][preset-89] | 38.0 |
| [Ampeg B-15N Portaflex][preset-92] | 43.0 |
| [Ibanez TS_808][preset-32] | 46.0 |
| [Hiwatt DR103][preset-100] | 46.0 |
| [Orange OR120 Graphic MkII][preset-106] | 48.0 |
| [DOD 250 Overdrive Preamp Gray][preset-21] | 56.0 |
| [Fender Princeton Reverb AA1164][preset-96] | 58.0 |
| [Maag EQ4-500 Parallel Circuit Model][preset-78] | 75.0 |
| [Dan Armstrong Orange Squeezer][preset-22] | 81.0 |
| [Boss SD-1 Super OverDrive][preset-18] | 90.0 |
| [Pro Co RAT LM308][preset-47] | 92.0 |
| [Trident Series 80B Four-Band EQ S800][preset-87] | 93.0 |
| [Klon Centaur][preset-33] | 94.0 |
| [Vox AC15 No. 3 - EF86 Normal][preset-110] | 98.0 |
| [Marshall Guv'nor][preset-42] | 112.0 |
| [Marshall Bluesbreaker Mk I][preset-41] | 143.0 |
| [ENGL E530 - Lead Hi Gain][preset-93] | 156.0 |
| [Fender Twin Reverb AB763][preset-99] | 159.0 |
| [Dunlop Cry Baby GCB-95][preset-24] | 167.0 |
| [Fender Tweed Deluxe 5E3 - Instrument][preset-98] | 177.0 |
| [Orange Rockerverb 50w - Dirty Channel][preset-5] | 180.0 |
| [Marshall JCM800 2203 - High Input][preset-104] | 180.0 |
| [Roland JC-120][preset-108] | 180.0 |
| [Origin Effects Cali76 FET Compressor (Circuit Approximation)][preset-46] | 182.0 |
| [Colorsound Power Boost 18V][preset-20] | 187.0 |
| [Mosrite Fuzzrite Silicon][preset-43] | 199.0 |
| [Harrison 32-Series 3232 Channel EQ][preset-76] | 211.0 |
| [Fender Bassman 5F6-A][preset-94] | 218.0 |
| [Fuzz Face][preset-30] | 225.0 |
| [Nobels ODR-1 Natural Overdrive][preset-45] | 226.0 |
| [MCI JH-400B Mic Preamp 43E241][preset-90] | 229.0 |
| [Peavey 5150 - Lead][preset-107] | 262.0 |
| [Boss DS-1A Distortion][preset-11] | 272.0 |
| [Marshall 1959 Super Lead - Jumped][preset-102] | 272.0 |
| [Mesa Boogie Dual Rectifier][preset-3] | 281.0 |
| [Boss BF-2 Flanger][preset-7] | 288.0 |
| [Roger Mayer Octavia Transistor][preset-48] | 288.0 |
| [SPL Transient Designer 9842 DET Model][preset-63] | 295.0 |
| [Sontec MES-432C-9 Circuit Model][preset-84] | 313.0 |
| [Laney GH100L - Drive][preset-101] | 316.0 |
| [EHX Electric Mistress 9V][preset-27] | 317.0 |
| [SSL 4000 E 82E242 Black EQ][preset-83] | 327.0 |
| [Fender Deluxe Reverb AB763][preset-95] | 336.0 |
| [Fender Tweed Champ 5F1 - Input 1][preset-97] | 340.0 |
| [Mesa Boogie Mark IIC+ - Lead][preset-105] | 360.0 |
| [Boss TR-2 Tremolo][preset-19] | 367.0 |
| [Boss MT-2 Metal Zone][preset-15] | 371.0 |
| [Vox AC30 - Top Boost][preset-111] | 395.0 |
| [API 550A Proportional-Q Circuit Model][preset-73] | 417.0 |
| [Boss HM-2 Heavy Metal][preset-14] | 424.0 |
| [Pultec EQP-1A Program Equalizer][preset-81] | 442.0 |
| [Sola Sound Tone Bender Professional Mk II][preset-50] | 451.0 |
| [GML 8200 Series II Circuit Model][preset-75] | 462.0 |
| [Pultec MEQ-5 Mid-Range Equalizer][preset-82] | 468.0 |
| [UREI-1176LN-Rev-D][preset-66] | 476.0 |
| [BBE 482i Sonic Maximizer Channel B][preset-71] | 481.0 |
| [Boss CE-2 Chorus][preset-8] | 489.0 |
| [EMI-RS124][preset-56] | 489.0 |
| [API-2500][preset-53] | 499.0 |
| [EVH 5150 III 100S Stealth - Channel 3][preset-1] | 505.0 |
| [ZVEX Fuzz Factory][preset-51] | 506.0 |
| [dbx 902 De-Esser][preset-68] | 531.0 |
| [Neve 1081 Gold Label Equalizer][preset-80] | 532.0 |
| [Trident A-Range 1038 Four-Band EQ ED3001][preset-86] | 535.0 |
| [Empirical Labs FATSO EL7][preset-57] | 539.0 |
| [Maestro FZ-1 Fuzz-Tone 1962][preset-40] | 541.0 |
| [EHX Small Clone MN3007][preset-28] | 544.0 |
| [Altec-436C][preset-54] | 553.0 |
| [EHX Big Muff Pi Ram's Head Violet][preset-25] | 557.0 |
| [Aphex Aural Exciter Type B][preset-70] | 575.0 |
| [Thermionic-Culture-Vulture][preset-72] | 584.0 |
| [Pye-4060][preset-62] | 601.0 |
| [MXR M108S Ten Band EQ (Rev E Reference)][preset-36] | 605.0 |
| [API 560 Graphic EQ Gyrator Model][preset-74] | 612.0 |
| [EHX Deluxe Memory Man][preset-26] | 618.0 |
| [Neve-33609J][preset-61] | 649.0 |
| [Neve 1073 Line Preamp EH10023][preset-91] | 657.0 |
| [UREI-LA-3A][preset-67] | 660.0 |
| [dbx-160-VU][preset-69] | 670.0 |
| [Helios Type 69 Musicland EQ][preset-77] | 683.0 |
| [Boss GE-7 Equalizer][preset-13] | 685.0 |
| [Manley-Variable-Mu][preset-60] | 686.0 |
| [ADR-Compex-F760X-RS][preset-52] | 714.0 |
| [SSL-4000-G-Bus-Compressor][preset-64] | 716.0 |
| [Soldano SLO-100 - Overdrive][preset-109] | 739.0 |
| [Marshall 2555 Silver Jubilee - Lead][preset-103] | 745.0 |
| [Boss PQ-4 Parametric EQ][preset-17] | 762.0 |
| [UREI 545 Parametric Equalizer][preset-88] | 764.0 |
| [Soundcraft Series 600 Mono Input][preset-85] | 809.0 |
| [Diamond CPR-1 (Quartz Reference)][preset-23] | 817.0 |
| [Boss BD-2 Blues Driver][preset-6] | 880.0 |
| [Marshall Valvestate 8100 - Boost Channel][preset-2] | 890.0 |
| [MXR Phase 90 Script][preset-39] | 917.0 |
| [Musitronics Mu-Tron III][preset-44] | 1,000.0 |
| [Teletronix-LA-2A][preset-65] | 1,017.0 |
| [Gates-Sta-Level-M5167][preset-59] | 1,113.0 |
| [Boss DS-2 Turbo Distortion][preset-12] | 1,126.0 |
| [MXR Dyna Comp CA3080][preset-35] | 1,135.0 |
| [Boss CS-3 Compression Sustainer (August 1986)][preset-9] | 1,194.0 |
| [Boss DM-2 Analog Delay][preset-10] | 1,217.0 |
| [MXR Carbon Copy Analog Delay][preset-34] | 1,322.0 |
| [Manley Massive Passive Standard Circuit Model][preset-79] | 1,355.0 |
| [MXR M234 Analog Chorus][preset-37] | 1,402.0 |
| [ISP Decimator II G String (Noise Terminator Reference)][preset-31] | 1,453.0 |
| [Fairchild-660][preset-58] | 1,506.0 |
| [Shin-ei Univox Uni-Vibe 915][preset-49] | 2,101.0 |
| [Boss NS-2 Noise Suppressor][preset-16] | 2,136.0 |
| [Drawmer-DS201][preset-55] | 2,276.0 |
| [EHX Small Stone Issue J][preset-29] | 3,724.0 |

## Benchmark method

Release build at [`2f71c20`](https://github.com/Yahiake/LiveWDF/commit/2f71c20e2ce2c802906cf4b9b8bdfcd889007cc1) for the original 91 presets and on the factory amp collection branch for the 20 additions. Both runs used Linux and one logical CPU on an Intel Core Ultra 7 155H. [The circuit renderer](https://github.com/Yahiake/LiveWDF/blob/2f71c20e2ce2c802906cf4b9b8bdfcd889007cc1/tests/regression/render_circuit.cpp) ran Preview at factory control positions in mono, 1×, 48 kHz, with 128-sample blocks.

Three takes used the Guitar-TECHS Sire T7 DI excerpt starting at 6 seconds, normalized to −6 dBFS before preset input calibration. Each take used 1,024 silent warm-up samples and discarded the first 32 audio blocks. Mean, median, and p99 pool the next 512 blocks from each take. [Recording and license](../tests/fixtures/guitar-techs-di/README.md)

Mean is the arithmetic average. Median is the middle block CPU time. p99 is the nearest-rank 99th percentile. The block budget at these settings is 2.67 ms. These figures measure renderer CPU time.

[preset-1]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/EVH%205150%20III%20100S%20Stealth%20-%20Channel%203.livewdf
[preset-2]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Marshall%20Valvestate%208100%20-%20Boost%20Channel.livewdf
[preset-3]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Mesa%20Boogie%20Dual%20Rectifier.livewdf
[preset-4]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Orange%20Rockerverb%2050w%20-%20Clean%20Channel.livewdf
[preset-5]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Orange%20Rockerverb%2050w%20-%20Dirty%20Channel.livewdf
[preset-6]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20BD-2%20Blues%20Driver.livewdf
[preset-7]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20BF-2%20Flanger.livewdf
[preset-8]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20CE-2%20Chorus.livewdf
[preset-9]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20CS-3%20Compression%20Sustainer%20%28August%201986%29.livewdf
[preset-10]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20DM-2%20Analog%20Delay.livewdf
[preset-11]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20DS-1A%20Distortion.livewdf
[preset-12]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20DS-2%20Turbo%20Distortion.livewdf
[preset-13]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20GE-7%20Equalizer.livewdf
[preset-14]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20HM-2%20Heavy%20Metal.livewdf
[preset-15]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20MT-2%20Metal%20Zone.livewdf
[preset-16]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20NS-2%20Noise%20Suppressor.livewdf
[preset-17]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20PQ-4%20Parametric%20EQ.livewdf
[preset-18]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20SD-1%20Super%20OverDrive.livewdf
[preset-19]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Boss%20TR-2%20Tremolo.livewdf
[preset-20]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Colorsound%20Power%20Boost%2018V.livewdf
[preset-21]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/DOD%20250%20Overdrive%20Preamp%20Gray.livewdf
[preset-22]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Dan%20Armstrong%20Orange%20Squeezer.livewdf
[preset-23]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Diamond%20CPR-1%20%28Quartz%20Reference%29.livewdf
[preset-24]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Dunlop%20Cry%20Baby%20GCB-95.livewdf
[preset-25]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/EHX%20Big%20Muff%20Pi%20Ram%27s%20Head%20Violet.livewdf
[preset-26]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/EHX%20Deluxe%20Memory%20Man.livewdf
[preset-27]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/EHX%20Electric%20Mistress%209V.livewdf
[preset-28]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/EHX%20Small%20Clone%20MN3007.livewdf
[preset-29]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/EHX%20Small%20Stone%20Issue%20J.livewdf
[preset-30]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Fuzz%20Face.livewdf
[preset-31]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/ISP%20Decimator%20II%20G%20String%20%28Noise%20Terminator%20Reference%29.livewdf
[preset-32]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Ibanez%20TS_808.livewdf
[preset-33]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Klon%20Centaur.livewdf
[preset-34]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20Carbon%20Copy%20Analog%20Delay.livewdf
[preset-35]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20Dyna%20Comp%20CA3080.livewdf
[preset-36]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20M108S%20Ten%20Band%20EQ%20%28Rev%20E%20Reference%29.livewdf
[preset-37]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20M234%20Analog%20Chorus.livewdf
[preset-38]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20Micro%20Amp.livewdf
[preset-39]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/MXR%20Phase%2090%20Script.livewdf
[preset-40]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Maestro%20FZ-1%20Fuzz-Tone%201962.livewdf
[preset-41]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Marshall%20Bluesbreaker%20Mk%20I.livewdf
[preset-42]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Marshall%20Guv%27nor.livewdf
[preset-43]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Mosrite%20Fuzzrite%20Silicon.livewdf
[preset-44]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Musitronics%20Mu-Tron%20III.livewdf
[preset-45]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Nobels%20ODR-1%20Natural%20Overdrive.livewdf
[preset-46]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Origin%20Effects%20Cali76%20FET%20Compressor%20%28Circuit%20Approximation%29.livewdf
[preset-47]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Pro%20Co%20RAT%20LM308.livewdf
[preset-48]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Roger%20Mayer%20Octavia%20Transistor.livewdf
[preset-49]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Shin-ei%20Univox%20Uni-Vibe%20915.livewdf
[preset-50]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/Sola%20Sound%20Tone%20Bender%20Professional%20Mk%20II.livewdf
[preset-51]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/pedals_fx/ZVEX%20Fuzz%20Factory.livewdf
[preset-52]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/ADR-Compex-F760X-RS.livewdf
[preset-53]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/API-2500.livewdf
[preset-54]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Altec-436C.livewdf
[preset-55]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Drawmer-DS201.livewdf
[preset-56]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/EMI-RS124.livewdf
[preset-57]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Empirical%20Labs%20FATSO%20EL7.livewdf
[preset-58]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Fairchild-660.livewdf
[preset-59]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Gates-Sta-Level-M5167.livewdf
[preset-60]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Manley-Variable-Mu.livewdf
[preset-61]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Neve-33609J.livewdf
[preset-62]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Pye-4060.livewdf
[preset-63]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/SPL%20Transient%20Designer%209842%20DET%20Model.livewdf
[preset-64]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/SSL-4000-G-Bus-Compressor.livewdf
[preset-65]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/Teletronix-LA-2A.livewdf
[preset-66]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/UREI-1176LN-Rev-D.livewdf
[preset-67]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/UREI-LA-3A.livewdf
[preset-68]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/dbx%20902%20De-Esser.livewdf
[preset-69]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/dynamics/dbx-160-VU.livewdf
[preset-70]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/enhancement/Aphex%20Aural%20Exciter%20Type%20B.livewdf
[preset-71]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/enhancement/BBE%20482i%20Sonic%20Maximizer%20Channel%20B.livewdf
[preset-72]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/enhancement/Thermionic-Culture-Vulture.livewdf
[preset-73]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/API%20550A%20Proportional-Q%20Circuit%20Model.livewdf
[preset-74]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/API%20560%20Graphic%20EQ%20Gyrator%20Model.livewdf
[preset-75]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/GML%208200%20Series%20II%20Circuit%20Model.livewdf
[preset-76]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Harrison%2032-Series%203232%20Channel%20EQ.livewdf
[preset-77]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Helios%20Type%2069%20Musicland%20EQ.livewdf
[preset-78]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Maag%20EQ4-500%20Parallel%20Circuit%20Model.livewdf
[preset-79]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Manley%20Massive%20Passive%20Standard%20Circuit%20Model.livewdf
[preset-80]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Neve%201081%20Gold%20Label%20Equalizer.livewdf
[preset-81]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Pultec%20EQP-1A%20Program%20Equalizer.livewdf
[preset-82]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Pultec%20MEQ-5%20Mid-Range%20Equalizer.livewdf
[preset-83]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/SSL%204000%20E%2082E242%20Black%20EQ.livewdf
[preset-84]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Sontec%20MES-432C-9%20Circuit%20Model.livewdf
[preset-85]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Soundcraft%20Series%20600%20Mono%20Input.livewdf
[preset-86]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Trident%20A-Range%201038%20Four-Band%20EQ%20ED3001.livewdf
[preset-87]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/Trident%20Series%2080B%20Four-Band%20EQ%20S800.livewdf
[preset-88]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/eq/UREI%20545%20Parametric%20Equalizer.livewdf
[preset-89]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/preamps/API%20312%20Preamp%20VP312%20Rev%20A.livewdf
[preset-90]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/preamps/MCI%20JH-400B%20Mic%20Preamp%2043E241.livewdf
[preset-91]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/studio/preamps/Neve%201073%20Line%20Preamp%20EH10023.livewdf
[preset-92]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Ampeg%20B-15N%20Portaflex.livewdf
[preset-93]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/ENGL%20E530%20-%20Lead%20Hi%20Gain.livewdf
[preset-94]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Bassman%205F6-A.livewdf
[preset-95]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Deluxe%20Reverb%20AB763.livewdf
[preset-96]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Princeton%20Reverb%20AA1164.livewdf
[preset-97]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Tweed%20Champ%205F1%20-%20Input%201.livewdf
[preset-98]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Tweed%20Deluxe%205E3%20-%20Instrument.livewdf
[preset-99]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Fender%20Twin%20Reverb%20AB763.livewdf
[preset-100]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Hiwatt%20DR103.livewdf
[preset-101]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Laney%20GH100L%20-%20Drive.livewdf
[preset-102]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Marshall%201959%20Super%20Lead%20-%20Jumped.livewdf
[preset-103]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Marshall%202555%20Silver%20Jubilee%20-%20Lead.livewdf
[preset-104]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Marshall%20JCM800%202203%20-%20High%20Input.livewdf
[preset-105]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Mesa%20Boogie%20Mark%20IIC%2B%20-%20Lead.livewdf
[preset-106]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Orange%20OR120%20Graphic%20MkII.livewdf
[preset-107]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Peavey%205150%20-%20Lead.livewdf
[preset-108]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Roland%20JC-120.livewdf
[preset-109]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Soldano%20SLO-100%20-%20Overdrive.livewdf
[preset-110]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Vox%20AC15%20No.%203%20-%20EF86%20Normal.livewdf
[preset-111]: https://github.com/Yahiake/LiveWDF/blob/main/schematics/amps/Vox%20AC30%20-%20Top%20Boost.livewdf
