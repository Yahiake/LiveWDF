# Pedal schematic and SPICE sources

References for the 41-pedal LiveWDF factory expansion. The four earlier factory pedals
are already in the [pedal directory](pedals_fx/). The entries below identify
circuit revisions and flag where only a related circuit or a black-box
reference is available. The accompanying [studio equipment catalog](STUDIO-SOURCES.md)
covers 40 recording devices.

## SPICE references

- [LTspice guitar pedals](https://github.com/cushychicken/ltspice-guitar-pedals)
  includes reference projects for the GE-7, Dyna Comp, and RAT.
- [LiveSPICE examples](https://github.com/dsharlet/LiveSPICE/tree/master/Tests/Examples)
  include the SD-1, Blues Breaker, Big Muff Pi, RAT, and Phase 90.
  These are LiveSPICE schematics. They provide independent topology
  references and can run in LiveSPICE.
- [Pedal Bench](https://bench.swellamps.com/) provides schematic views and
  LTspice exports for many of these effects. Refer to the circuit notes for
  model substitutions and omissions. Its exports are useful for comparison
  under the site's terms.
- [MiSTer-Discrete circuit-compiler](https://github.com/jopdorp/MiSTer-Discrete/tree/692e777112e4a3c05a17578d9cfda1ecbc9b98d6/circuits/parts)
  supplies independent component-level references for diode breakdown,
  op-amp limiting, the CA3080 OTA, and the MN3005/MN3008 clocked delays.
  Its fixed-clock BBD deck can check delay, clipping and loading at a
  constant clock. Clock modulation in a chorus requires a variable-clock
  model.
- The factory scans, traced circuits, approximate simulation models, and
  hardware measurements answer different questions. Verify the revision,
  part values, gain controls, and dynamic behavior before using a model as
  a reference for the original pedal.

## Circuit revision gaps

A complete schematic for the exact MXR M234 or the current Cali76 FET
Compressor remains unverified. Available references for the M108S, Diamond
CPR-1, and Decimator II G String include related or derivative circuits.
The included presets document which revisions and circuit approximations they use.

## Boost and overdrive

### MXR Micro Amp M-133
- Recommended full schematic: https://generalguitargadgets.com/pdf/ggg_mamp_sc.pdf (GGG replica schematic, 2012 revision; full circuit and power section). Source/BOM https://generalguitargadgets.com/effects-projects/boosters/mxr-microamp/. The drawing is copyrighted and explicitly disallows redistribution from other sites; link instead.
- Alternate directly viewable image: https://electrosmash.mas-effects.com/images/mxr-microamp/mxr-microamp-schematic.png; annotated analysis https://electrosmash.mas-effects.com/mxr-microamp.html. The analysis notes historical PCB and minor component differences; avoid claiming an exact original board revision.
- One op-amp non-inverting gain, 500k reverse-log drive control, gain-leg capacitor and feedback capacitor, bias divider and output coupling. Retain supply rail saturation rather than treating it as unlimited linear amplification.

### Colorsound Power Boost
- Recommended later 18 V version complete, inspectable schematic + parts/netlist: https://bench.swellamps.com/boost-3t-1970/. It identifies its stock configuration as later version, has an interactive schematic, component-by-component connections and SPICE netlist download. The active tone circuit is Baxandall feedback around Q3, not a passive tone filter; Q1/Q2 form a feedback pair with emitter drive pot. Three silicon BC109C, 18 V, drive, bass, treble; stock has no separate output volume. Preserve transistor nonlinearities and both feedback loops.
- Alternative documented early-version drawing (schematic and vero layout): https://turretboard.knucklehead.dk/wp-content/uploads/2012/05/powerboostearlyMonkeyxx.pdf. Early and later sourcing descriptions differ on capacitor values and output resistor, so do not combine values across sources without checking diagram. A reconstructed early parts list: https://akashic.cryptideffects.com/circuit/revolutiondeux-2012-02-colorsound-power-boost-overdriver-power-boost-early. Archive marks values read by eye; treat this as an aid, not verified ground truth. Historical discussion https://community.element14.com/members-area/personalblogs/b/blog/posts/colorsound-power-boost-18v-effects-pedal. The 9 V Overdriver is a distinct configuration, not the selected 18 V Power Boost.

### DOD Overdrive Preamp 250
- Factory circuit: [Gray version](pedals_fx/DOD%20250%20Overdrive%20Preamp%20Gray.livewdf).
- Recommended full '77 Gray replica schematic: https://generalguitargadgets.com/pdf/ggg_dist_g250_sc.pdf. Project identifies it as '77 Gray https://generalguitargadgets.com/effects-projects/distortion/dod-overdrive-250/. Copyright notice forbids rehosting; link rather than bundle. Standard variant also linked on project page, do not mix BOM values between variants.
- Alternative original-style model 250 reproduction https://www.guitars-of-love.com/images/ADIP_audio_production_projects/DOD_OD-250/DOD_OD-250_schematic.pdf; 2009 third-party redrawing with 4558, explicitly notes copyright restrictions. Not necessarily same '77 gray revision.
- Single op-amp noninverting drive; output coupling, symmetric silicon diode shunt clipping, and output volume; not feedback diode soft clipping. Include frequency shaping and finite supply/headroom, 9 V bias. Check values against chosen schematic's own labels, not the standard variant's BOM.

### Boss SD-1
- Recommended manufacturer service notes, first edition Mar 3 1981, second printing Jan 1984, complete circuit: https://www.synfo.nl/servicemanuals/Boss/BOSS_SD-1_SERVICE_NOTES.pdf. Service schematics PDF alternate: https://civade.com/AudioPro/Boss/Boss_SD-1_Super_Overdrive_Schematic.pdf.
- Implement JFET/BJT input buffer, op-amp drive with asymmetric diode network in feedback, post-drive tone and level, and output buffer in proper sequence. Asymmetric feedback clipping differs from the DOD shunt clipper. Service schematic includes electronic bypass, power switching, indicator and DC supply; decide explicitly whether simulation scope includes switching or effect signal path only. Manufacturer document identifies this historical service edition; do not claim all later production revisions are identical.

## Overdrive and distortion

These links identify specific published diagrams; reproduce their actual component values and topology rather than treating the product name as a unique circuit. Links are references, not permission to redistribute source diagrams.

### Boss BD-2 Blues Driver
- **Reference:** Roland/Boss **February 1995 BD-2 service notes**, main board assembly 70567645 / PWB 00786489, full circuit diagram plus parts list, wiring, switching, rails, and errata: https://stompboxelectronics.com/wp-content/uploads/2023/01/BOSS-BD2_ServiceNotes.pdf
- **Critical correction:** the appended Roland erratum of 1996-12-03 explicitly corrects Q9 and Q12 from 2SA1335GR to **2SA1049GR** on both schematic and parts list. This document is a specific early production circuit, not a claim about every later board or BD-2W.
- **Implementation:** gain is a mechanically ganged 250kA + 250kA potentiometer, tone 10kB, level 100kA. Preserve the two coupled gain sections and transistor/JFET nonlinear stages, including DC bias and loading. Electronic bypass, LED, and power-input switching are identifiable separately; decide deliberately whether to model the engaged audio path or the entire switching system.
- **Alternative entry:** https://stompboxelectronics.com/resources/guitar-pedal-service-manuals/ (links the same manufacturer service notes).

### Marshall Bluesbreaker Mk I
- **Reference:** General Guitar Gadgets **Vintage BluesBreaker original-wiring** schematic, dated 2019-05-20, a replica of the original black pedal: https://generalguitargadgets.com/pdf/ggg_mbb_orig_sc.pdf; project and BOM: https://generalguitargadgets.com/effects-projects/distortion/bluesbreaker/. This is a redraw, not a factory-issued service schematic.
- **Revision:** Aion identifies two Mk I variants. Its more common standard version uses **3.3k and 4.7k** in the two first-stage gain branches, while an early low-gain version uses **27k and 33k** respectively. GGG’s BOM agrees with the common 3.3k/4.7k set (different reference designators). Document chosen version rather than silently mixing them: https://aionfx.com/project/cerulean-amp-overdrive/. Exclude the different BB-2 pedal and King of Tone/Morning Glory modifications.
- **Alternative:** https://aionfx.com/app/files/docs/cerulean_documentation.pdf (Aion project schematic; compare modifications to stock before adopting). Core topology: interactive gain control across a noninverting gain/filter stage and an inverting feedback-clipping stage, with two series silicon diodes in each feedback direction, followed by tone and level. Preserve loading and the single potentiometer's correlated sections.

### Nobels ODR-1 Natural Overdrive
- **Reference:** Full manufacturer-branded ODR-1 circuit diagram dated **7 Dec 2000**, as archived by PisoTones: https://www.pisotones.com/Nobels_ODR-1/imgs/Nobels%20ODR-1.pdf. Contains engaged circuit, power, and extensive electronically buffered bypass/control circuitry. Archive context confirms this was the original manufacturer diagram: https://www.pisotones.com/Nobels_ODR-1/ODR-1.htm. Do not confuse with ODR-1 Plus, ODR-S, ODR-mini, or modified recreations.
- **Alternative:** https://aionfx.com/app/files/docs/andromeda_documentation.pdf and https://aionfx.com/project/andromeda-natural-overdrive/. This recreation explicitly adds a variable bass control; its full-up setting is intended to reproduce stock. It is not an exact complete factory bypass/power schematic.
- **Caveat:** Nobels states the silver reissue has the same circuit as the original (https://nobels.de/knowledge-base/does-the-odr-1-silver-box-reissue-have-the-exact-same-schematic-circuitry-as-the-old-orignal-odr-1/?lang=en), while Aion reports some silver units shipped with drive and volume pots swapped. Preserve the coupled Spectrum network and its loading, nonlinear diode branches, and active gain stages rather than substituting a conventional single tone filter.

### Pro Co RAT
- **Reference:** complete LM308 RAT schematic (RATS.UTSCH): https://www.dirk-hendrik.com/proco_rat.pdf. Includes input/output and bypass switching, LM308 with 30 pF external compensation, clipping, filter, JFET output, and supply. **Revision/date not identified on sheet**; treat as a particular RAT-like published circuit, not a canonical first-production schematic. It has a 360-ohm lower gain resistor and switching/LED arrangements; verify against the selected physical unit before labeling it vintage original.
- **Alternative:** https://www.gitec-forum-eng.de/wp-content/uploads/2019/05/bcm4_distortion_2_wb_rat_en.pdf includes a schematic and component comparison for Original RAT, RAT 2 and Turbo RAT; https://www.electrosmash.com/proco-rat describes the LM308 circuit and later OP07 substitutions. Establish component set, silicon vs LED clipper, switch/buffer and op-amp before modeling. The 1986 unit trace https://akashic.cryptideffects.com/circuit/revolutiondeux-2012-02-kurt-cobain-nirvana-gear-rat-1986 is another specifically identified historical lead, but verify underlying diagram directly.
- **Implementation:** single-supply bias, frequency-dependent variable gain, op-amp slew/rail behavior and compensation, antiparallel hard-clipping diodes, reverse-acting passive filter and JFET output buffer affect behavior. Do not substitute an ideal static clipper and call it an exact LM308 circuit.

## Distortion

Use exact listed revisions. These are sources for component-level transcription, not completed verified netlists. Schematic artwork must be inspected directly; automatic PDF descriptions can miss wiring and falsely infer values.

### Boss MT-2 Metal Zone
- Revision: Boss/Roland service notes **April 1991, First Edition**, PCB 22930117RT (main 1/2, controls 2/2), 9 V PSA.
- Direct original service-note PDF (4 pages; diagram and test waveforms): https://www.synthxl.com/wp-content/uploads/2020/04/Boss-MT-2-Service-Notes.pdf
- Alternate direct PDF: https://guitar-gear.ru/forum/index.php?app=core&attach_id=50406&module=attach&section=attach
- Index / parts: https://manuals.plus/m/209e10153aa6e216558b52afb493c7370fff8995a7d7487cebfee0ee56936dee
- Full audio path includes buffers, many active stages, diode distortion, active low/high controls, and parametric mid with coupled multi-gang frequency control. Preserve every stage and loading, multi-gang wiper orientation and bias nodes. Service notes supply oscilloscope examples with 200 Hz, 5 mVpp input, useful for gross validation. Parts list identifies M5218AL op-amps, 1SS133 and S5500G diodes. Do not reconstruct connectivity from OCR prose.

### Boss DS-1 Distortion
- Revision: Roland service notes **December 1994, Second Edition** document both DS-1A (PSA) and earlier DS-1; their circuit diagrams are respectively pages 4 and 5. Explicitly choose one; the ICs, power network and boards differ. Choose DS-1A page 4 if using 1994 hardware, DS-1 page 5 for the earlier schematic.
- Direct original service notes: https://civade.com/AudioPro/Boss/DS-1_SERVICE_NOTES.pdf
- Alternate identical notes: https://www.synfo.nl/servicemanuals/Boss/DS-1_SERVICE_NOTES.pdf
- DS-1A board 70562634, PCB 2291732601; DIST 100kB, TONE 20kB, LEVEL 100kB. Include BJT input gain and op-amp distortion gain, D4/D5 diode pair, loaded passive tone network and output buffer. Preserve the tone network interaction instead of substituting independent EQ; transcribe actual diagram for exact connections and DC bias.

### Boss HM-2 Heavy Metal
- Revision: Roland service notes **15 December 1983, First Edition**. Contains schematic and two board/volume-board revisions; choose the circuit as drawn and document board choice, do not infer component equivalence from layouts.
- Direct original service-note PDF: https://civade.com/AudioPro/Boss/Boss_HM-2_Heavy_Metal_Distortion_Service_Manual.pdf
- Alternate service notes mirror: https://manuals.plus/m/047cea1e1dfc7a9c451a1e62ebd52fcd9bae159c30645adfe4aad9c62a5fb2dc
- Four front controls: LEVEL, COLOR MIX L and H, DIST. Preserve transistor/op-amp gain and nonlinear clipping plus active color-mix network; don't replace the distinctive interactive EQ with simple generic tone controls. Include bias and coupling, and board variants where relevant. Verify topology and components visually.

### Marshall The Guv'nor
- Revision: original black **1988-era Guv'nor**, not GV-2/Guv'nor Plus. This is the original-style schematic provided for reference by General Guitar Gadgets, not an independently verified Marshall factory document.
- Direct original-style schematic PDF: https://generalguitargadgets.com/pdf/ggg_mgv_sc_guv.pdf
- Alternate direct image: https://generalguitargadgets.com/wp-content/uploads/ggcp/guvnor/ggg_mgv_sc_guv-1.png
- Source and context: https://generalguitargadgets.com/effects-projects/distortion/guvnor/; analysis https://www.electrosmash.com/marshall-guvnor-analysis
- Diagram has two TL072 stages, gain pot, LED diode clipping, coupled bass/middle/treble passive tone stack, level control, DC bias, and stereo effects loop. Account for the loop jack's normalled path when no cable is inserted. General Guitar Gadgets separately provides a modified project schematic: don't substitute it for original reference without documenting changes. Schematic includes electronic switching; decide explicitly whether modelling engaged path only or bypass too.

## Fuzz and octave fuzz

URLs below point to public schematics and simulation interfaces, not licenses to redistribute. Explicit revision selection and validation against actual hardware remain necessary. No netlist here has been independently run or verified.

### EHX Big Muff Pi Ram's Head

- Select **V2 1973 first Violet** as the specific starting circuit; do not treat Ram's Head as one fixed schematic. Primary visual schematic: https://www.bigmuffpage.com/images/schematics/KR_1973_V2_Violet_Schematic_1st_version.jpg. Schematic variants and provenance: https://www.bigmuffpage.com/Big_Muff_Pi_versions_schematics_part1.html. About 20 V2 variants exist; earliest '47', 1973, first circle face, Violet, later Violet 560 pF and 470 pF revisions differ. Original diode markings are ambiguous; 1N914 is a substitute, not a proven exact part.
- EHX's 2019 reissue was reverse engineered from a particular aged Violet specimen, whose measured values differed from printed nominal components. Thus vintage nominal drawing and reissue are not identical targets. https://www.kitrae.net/music/big_muff_history1RH.html
- Four silicon transistor stages with two anti-parallel diode feedback clipping stages, passive tone stack, sustain and volume. Capture every capacitor, bias resistor and feedback path from the linked image; do not infer all Violet values from neighboring versions.
- A Reddit simulation author reports an ngspice file `BigMuff-Rams-Head/BigMuff-Rams-Head.cir`, but accessible indexing did not provide a reliable complete URL or verified contents: https://www.reddit.com/r/diypedals/comments/1lz9uop. Treat as lead, not a deliverable.

### Sola Sound Tone Bender Professional Mk II

- Reissue schematic PDF, complete connectivity: https://schematicheaven.net/effects/colorsound_tonebender_mkII_pro_reissue.pdf. Aion's practical Mk II schematic/BOM: https://aionfx.com/app/files/docs/deimos_legacy_documentation.pdf. Note protection and bias modifications in the Aion version; reproduce original signal path separately.
- Simulation with schematic, components, variant selector and **SPICE netlist (LTspice)** download button: https://bench.swellamps.com/fuzz-ge-3t-1966/. It identifies a specific OC75-based 1966 variant and an OC81D Q1 alternative; the simulator output has not been downloaded/checked, and license is not stated. Early OC75 Q1 base resistor is 10k; OC81D variant may use 100k. First transistor depends on leakage, which an ordinary silicon PNP model or generic zero-leakage germanium transistor will not replicate. Positive-ground, negative-supply behavior must be retained.
- Circuit variability includes input RF cap values and three transistor gain/leakage selections. Avoid confusing Mk II with Mk 1.5, Mk III or a silicon adaptation.

### Roger Mayer Octavia: specify version

- **Recommended identifiable target: Roger Mayer transistor Octavia, commercial 1980-style (no transformer)**, if the requested target is the commercial Roger Mayer pedal. Simulator with full parts listing and **SPICE netlist (LTspice)** button: https://bench.swellamps.com/fuzz-octave-split-1980/. It models a 2N3906/MPSA13 feedback pair, Darlington phase splitter, diode full-wave rectifier and output follower. Download availability is a UI feature; file not independently inspected, and transistor gain dispersion is omitted per source.
- Complete build schematic and BOM for **2003 Vision-derived** project (not the unmodified 1980 circuit): https://aionfx.com/app/files/docs/octahedron_documentation.pdf. Aion states its tone control can be omitted to approximate stock, and adds an octave disable switch. See https://aionfx.com/project/octahedron-octave-fuzz/. Confirm exact stock connections against another trace before claiming bit-exact 1980.
- 1967–69 Hendrix units were Mayer transformer prototypes with many individual revisions and no single standard circuit. Tycobrahe 1973 is a commercial descendant of a traced transformer unit, not the 1980 transistor circuit. Mayer says Tycobrahe copied a 24 V design and ran it at 9 V: https://www.roger-mayer.co.uk/octavia.htm. History and clear schematic of Tycobrahe derivative: https://aionfx.com/app/files/docs/octagon_documentation.pdf. Aion's edition adds an octave switch. Tycobrahe SPICE-exporting simulator: https://bench.swellamps.com/fuzz-octave-tx-1973/. Transformer model/coupling/core need explicit verification; do not label this as the transistor Mayer circuit or a definitive 1967 Hendrix unit.

### ZVEX Fuzz Factory

- Five controls: Volume, Gate, Comp, Drive, Stab. Common reverse-engineered schematic at https://guitargearfinder.com/guides/fuzz-factory-clone/ and direct schematic link cited by Harvard project https://basicaudio.net/Fuzz%20factory%20schem.PNG (direct availability not independently verified); Harvard project https://archive.blogs.harvard.edu/bbrzycki/2018/04/. Manufacturer's module wiring diagram with pot values https://static1.squarespace.com/static/555e332ce4b0577e788c3a16/t/573e3543f699bb468e2aaf5b/1463694660970/module_instructions.pdf is for the Inventobox module, not itself a full component schematic; cross-check actual wiring.
- Silicon input stage and two germanium PNP fuzz stages, highly interactive bias and power controls. Germanium leakage, gain, temperature and stray parasitics affect gating/oscillation; do not replace with memoryless clipping. Confirm all pot orientations, taper and end-stop behavior; safe SPICE pot endpoint resistance avoids singular matrices but changes nominal ideal short.
- No independently verified complete Fuzz Factory SPICE deck or openly licensed germanium models found. Treat general transistor models as placeholders unless measured or referenced to a specific real part, and validate oscillation and controls against physical recordings/measurements.

### Rights and reproducibility

Aion PDFs explicitly include a licensing section: inspect it before incorporating any drawing or text. No explicit license was established for the Kit Rae drawing, ZVEX manual, community clone diagrams, or Swell Amps generated decks; public access is not permission to redistribute. Cite sources, independently create connectivity where appropriate, record model/provenance and measured differences, and do not imply hardware validation based on a schematic or simulator alone.

## Fuzz and envelope filter

### Musitronics Mu-Tron III (prioritize factory 1975 revision)

- Original factory/ARP service scan, owner manual, parts list, and full schematic: https://el34world.com/charts/Schematics/Files/Effects/Mu_tron_iii_om_sch.pdf. The drawing is explicitly ©1975 Musitronics, “above s/n 02050”; choose and transcribe this specific revision rather than blending with earlier units. Service parts identify P873-13 dual photocell (one LED), selected RC4558 dual op amps, 1M and 150k potentiometers, and 1N4148. Scan is graphical: inspect the drawing itself for every connection, switch pole and value; OCR is inadequate.
- Practical alternative/redraw: https://aionfx.com/project/lumitron-resonant-filter/ and its linked PCB-only documentation, schematic p. 8 (https://akashic.cryptideffects.com/circuit/aionfx-lumitron-resonant-filter). This is an adaptation, not unqualified original; check document revision and differences against factory. Aion discussion: early dual-LDR optocoupler marked 805A, later manual P873-13; two VTL5C3 devices are common substitute, different LDR characteristic may require changed driver. https://www.scribd.com/document/721418081/lumitron-documentation
- Existing runnable interactive approximation with **Download → SPICE netlist (LTspice)**: https://bench.swellamps.com/filter-envelope-mutron-1972/. Download itself is a browser button, not a verified static deck URL; no downloaded deck has been validated. Its own notes explicitly assume the LDR figures and R23, so do not present it as a faithful factory SPICE reference. It implements ±9V, 4558s, coupled envelope/LED/two LDRs and state-variable circuit. C5/C7 1.8 nF, low-range adds 2.2 nF to each, 220k parallel LDR integrator resistors; A2 high-pass, A3 band-pass, A4 low-pass; 1M reverse-audio gain, 150k peak. Verify details against selected factory schematic.
- Implementation: preserve coupled LED intensity to **both** photoresistors, unequal dark/light resistances, nonlinear resistance-vs-light and attack/release memory, envelope rectifier diode behavior, Up/Down inversion and range/mode switching. Test self-oscillation/resonance, note dynamics, reset/initial state and sample-rate stability. Optocoupler static resistor or an OTA model is not the original behavior; precise part response remains an assumption until measured. Original manufacturer overview https://mu-tron.com/vintage-musitronics/mu-tron-iii/.

### Maestro FZ-1, original (not FZ-1A)

- Physical 1962 unit trace with component photos and 2N270 evidence: https://www.guitar-mod.com/rg_pedal_maestro_repair.html. 3 V supply, three PNP germanium RCA 2N270, leakage-sensitive bias; input impedance/source must be represented. Patent original https://grwhitehead.github.io/us3213181/references/US3213181.pdf. Patent drawing contains known discrepancies; do not blindly treat patent as built-unit circuit. https://www.flickr.com/photos/simonm1965/4329619267/ is a corrected independent drawing and warns patent and leaflet are wrong. Cross-check trace before choosing values.
- Existing ngspice transistor-level simulation and 2N270 model: https://github.com/grwhitehead/us3213181; explanation https://grwhitehead.github.io/us3213181/. This implements the patent with assumptions, not guaranteed the traced production unit; author says transistor model fitted to historical data. Confirm model licensing, leakage and deck compatibility. Another interactive model has LTspice export button: https://bench.swellamps.com/fuzz-ge-3t-1962/. No downloaded export has been validated.
- Specific discrepancy: patent prints 470 Ω for Q2 bias where corrected working version uses 470 kΩ; patent leaves volume pot unspecified, simulations assume 500 kΩ. FZ-1 Q2 collector load is 1.5 kΩ, while later FZ-1A has 10 kΩ. Output 3.3 nF and 56 kΩ create severe bass cut. 3 V original differs from 1.5 V FZ-1A: factory FZ-1A / 1B / 1S sheets https://funkwerkes.com/web/wp-content/techdocs/MixedProAudio/Maestroworld/Maestro_FZ-1A-1B-1S_fuzzes.pdf. Avoid merging circuits. Germanium transistor leakage, temperature and device spread are audible, so BF alone does not define realistic behavior.

### Mosrite Fuzzrite — silicon version (choose this revision explicitly)

- Complete detailed schematic: https://homoelectromagneticus.com/fuzzrite/correct_schematic.png; discussion https://homoelectromagneticus.com/projects/fuzzrite.php. Source identifies a 22 kΩ lead-mounted resistor commonly omitted and treats 2N2222 as a substitute due to uncertain original transistor marking. Its figure places 22 kΩ on first-stage branch of depth blend; compare exact topology against it, not just textual shorthand. 350 kΩ depth and ~33 kΩ volume are cited. Photos/traces and revisions https://fuzzboxes.org/features/fuzzrite.
- Separate adaptation/complete build document https://pedalparts.co.uk/docs/FuzzRite.pdf (2.2 nF substitutions, 500k and 50k pots, modern silicon transistor substitutions). Distinguish these alterations from the traced schematic.
- Existing interactive silicon SPICE export via button https://bench.swellamps.com/fuzz-si-2t-1968/ (2N3904 assumed; 2.2 nF branch caps; 350k depth, 33k volume). No exported deck checked. R12=22 kΩ in first-stage blend branch. Two collector-to-base biased cascaded stages, first/second signals of opposite polarity are blended; capture cancellation at intermediate depth and transistor gain/spread.

### Evidence limits

A linked original factory scan or physical trace is a stronger starting point than simulator's derivative schematic. None of the browser-button downloads or models was run or validated here; do not label them production-ready WDF/SPICE ports. Component numbering differs among drawings. Verify all values/connections, model licensing, output/bypass loading, calibration and transistor/op-amp/opto dynamic behavior before committing to a circuit.

## Phaser, vibe, and tremolo

### MXR Phase 90, original script (1974)
- Direct complete schematic: https://generalguitargadgets.com/pdf/ggg_p90_sc.pdf — GGG P90 replica schematic, version 2015June30, not a claim of an original factory drawing. Project https://generalguitargadgets.com/effects-projects/phase-shifters/mxr-phase-90-script-logo/ identifies it as the 1974 script circuit and explicitly says omit the optional R15 feedback resistor. Replica has true-bypass and LED modifications; PNP substituted 2N3906 for 2N4125. Preserve the original audio circuit, do not blindly implement optional R15.
- Four matched 2N5952 JFET variable resistances in four 741 all-pass stages, input 741 buffer, PNP output mixer and LFO/bias; rate and bias trimmer. Capture the gate bias and LFO and mixing/loading as well as four stages; a fixed oscillator can be an approximation rather than a circuit-complete model. JFET matching affects common-gate-control sweep.
- Independent physical model and diagram: https://www.dafx14.fau.de/papers/dafx14_felix_eichas_physical_modeling_of_the_.pdf Figure 4 describes block-logo feedback path R15 and explains omitting it for 1974 script. It measures nonlinear JFETs and models using a lookup table. Alternative https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_13.pdf.

### EHX Small Stone Issue J, 1977–78
- Direct full historical Issue J drawing in EHX schematic collection https://device.report/m/41207c84e0906828459c49ad8f99e928b644047280ff7a9d2788ca855212cfbd (find 'EH Small Stone Phaser Issue J'; 141 pages). An independently hosted drawing is cited at http://www.generalguitargadgets.com/pdf/ggg_smallstj_sc.pdf but could not verify availability. Issue J factory print is listed as dated 10 January 1978 https://www.ronsound.com/index.php?cPath=11_18&main_page=index&page=7. Do not equate Issue J with first 1975 six-chip revision.
- Four CA3094/EH1048 OTA stages plus a fifth for the oscillator/control, input/output BJT and Color-switch feedback network; rate control. OTA transconductance and output-buffer loading are required, not JFET stages. Preserve color-state volume behavior.
- Fully accessible modern adaptation schematic and notes https://aionfx.com/app/files/docs/sunstone_documentation.pdf p.7: LM13700 OTA sections and discrete output buffer substitution; extra gain/level stage means it is not the historical schematic. Notes identify the substitutions and give color-volume changes. A separate redraw https://schematicsonline.com/wp-content/uploads/2022/11/EHX-Small-Stone-EC4800.pdf is CA3094 based, but do not assert its revision matches Issue J without comparing component values.

### Shin-ei / Univox Uni-Vibe, model 915
- Direct original 915 owner's manual schematic reproduction https://schematicsonline.com/wp-content/uploads/2022/11/Univox-U-915-Uni-vibe.pdf; historical context https://www.classicamplification.com/24inf/uvh.html. This is the later U-915 revision, not a claim that every earlier unit matches it.
- Circuit contains 13 BJTs, incandescent lamp, four CdS photocells in four phasing stages, oscillator, lamp driver, chorus/vibrato switch and power supply; use lamp thermal dynamics and LDR resistance/illumination memory for faithful behavior. Keep differing phase-stage capacitors and wet/dry chorus mix. Original cell response and lamp specifications require measurement; no factory cell type can be inferred from drawing alone.
- Caution: historical tracing forum https://www.freestompboxes.org/viewtopic.php?t=921 cites original-unit trace and discrepancies from manual; https://www.freestompboxes.org/viewtopic.php?start=180&t=19321 alleges polarity error in factory C17. Treat as unverified community warnings and check actual operating point and independently traced unit before committing values.

### Boss TR-2 (December 1996 service revision)
- Complete original manufacturer service notes, schematic, board, parts and measurement conditions: https://stompboxelectronics.com/wp-content/uploads/2026/11/BOSS-TR-2-Service-Notes.pdf; Roland 17059864, Dec 1996. Alternative mirror https://manualmachine.com/bossaudio/tr2/8547814-schematic/.
- IC1 M5207L01 VCA, three M5218AL dual op-amps, JFET switching/buffering, BJTs and diodes; 100kB RATE and DEPTH, 100kA WAVE. Model VCA control characteristic and the oscillator, wave shaping and depth control, not simply a sine multiplier. Circuit includes electronic bypass switching and power/bias network; decide scope explicitly. The service document contains output-waveform checks useful for validation. Do not misidentify the VCA as BA662.

## Chorus and flanger

### MXR M234 Analog Chorus, Rev C approximation

- Dunlop's June 2016 Rev C drawing SC93503008195 is visible at https://www.scribd.com/document/928951400/M234-Analog-Chorus-Rev-C-1. The public preview confirms the 3207 delay, BL3102, MC33178 audio stages and TL062CP LFO. The visible schematic and component values support the input buffer, emphasis, transistor-buffered filters and state-variable low cut. Some pot-board connections and small labels remain unreadable.
- Repair photographs independently show the 3207-family delay and BL3102, and the article locates the TL062 on the control board. https://stompboxelectronics.com/2023/07/18/repair-room-20-mxr-analog-chorus-repair/
- The model reconstructs the high-cut and wet/dry routing using Dunlop's manual. Low is ganged and High covers approximately 660 Hz to flat. Thru Connected sends the wet signal to the main output, with the buffered Thru available as a schematic node. https://www.jimdunlop.com/content/manuals/M234.pdf
- The RC oscillator and sampled BBD clock approximate the original TL062 pair and ultrasonic clock. The delay taps use a centered combined output, and the protection and bypass circuitry are outside the engaged signal-path model.

### Boss CE-2 original, ET-50B
- Full Boss CE-2/CE-2B service notes, original circuit diagram and component layout: https://audioschematics.dk/wp-content/uploads/media/schematics/effects/boss-CE-2.pdf. ET-50B / 052-516B, MN3007 1024-stage BBD, MN3101 clock. Select CE-2 page rather than bass CE-2B. More accessible component-numbered analysis https://electrosmash.mas-effects.com/boss-ce-2-analysis.
- Existing downloadable LTspice netlist via **SPICE netlist (LTspice)** button at https://bench.swellamps.com/chorus-bbd-1979/. This is a third-party effect-on approximation, not a complete original factory deck: clock replaced by current-driven model and stand-in resistor RCV, no BBD clock ripple, bypass switching / LED removed. Confirm actual downloaded deck and its licensing/compatibility before reuse.
- Independent KiCad clone https://github.com/Acramonics/CE2Clone adds anti-tick modification and substitutes 2N5088s; not pristine original.

### EHX Small Clone EH-460D
- Full original factory sheet https://civade.com/AudioPro/Electro-Harmonix/Electro-Harmonix-Small-Clone-Delay-Schematic.pdf shows SAD1024 base circuit plus explicit note (A) for MN3007 modifications. CD4047 clock; treat two BBD arrangements as different networks, not a drop-in component swap. Reissues have circuit differences: https://aionfx.com/project/lithium-analog-chorus/. An alternate source describes corrected original schematic as including SAD1024 and MN3007 https://www.ronsound.com/index.php?cPath=11_18&main_page=index&page=7; inspect corrections before claiming exactness.
- Existing approximate MN3007/4047 effect-on LTspice deck via button https://bench.swellamps.com/chorus-bbd-1981/; the 4047 uses a current-driven approximation; delay values calculated without measured actual clock; bypass/footswitch omitted. Not a factory deck.

### Boss BF-2 guitar flanger
- Full Boss service notes https://www.synfo.nl/servicemanuals/Boss/BOSS_BF-2B_SERVICE_NOTES.pdf contain separate BF-2 and BF-2B diagrams and parts. Use BF-2 guitar schematic: MN3207, MN3102, not BF-2B. Device identifiers are explicitly stated in original notes. Parts/adjustments and switch included.
- Downloadable third-party effect-on LTspice model through https://bench.swellamps.com/flanger-bbd-1980/. Treat as approximation and inspect before using as original. Do not confuse MN3207/MN3102 with CE-2 MN3007/MN3101 electrical arrangement.

### EHX 9V Electric Mistress (original family, explicitly non-Deluxe)
- Full 9 V SAD1024 and CD4013 schematic https://guitar-gear.ru/forum/index.php?app=core&attach_id=33076&module=attach&section=attach. Labeled EH 9V Electric Mistress schematic only, rev 2 Nov 2005, tonepad tracing/presentation, original non-true-bypass switching, yellow circles denote corrections to earlier drawing; personal use only and commercial use forbidden. This is a community redraw, not a verified factory schematic for a particular serial number / PCB revision. Do not call exact EH-5200-1A/1B without board comparison.
- Closely related modified project https://www.freestompboxes.org/museum/gaussmarkov.net/layouts/mwelecmiss/mwelecmiss-project.pdf has added send/return, true-bypass accommodations, altered values including C4/C21, R7/R42/R8. Explicitly modified and personal use only; do not silently present as stock.
- There is a **different** downloadable LTspice model for the Deluxe Electric Mistress at https://bench.swellamps.com/flanger-bbd-1978/, not the 9V Electric Mistress: SAD1024 parallel-phase halves represented as one equivalent 1024-stage line, assumed output resistance, not physical two-output/clock cancellation. It is an approximate related reference, not the requested original circuit. Board variations are material: https://www.ehx.com/topic/electric-mistress-eh-5200-1/ and https://www.freestompboxes.org/viewtopic.php?t=8249.

### Simulation caveat
The bench pages provide interactive button-generated LTspice netlists rather than directly linked, independently verified static deck URLs. All are independent third-party models, and clock/BBD approximations plus omitted switching make them starting points, not measurements or device-faithful transistor-level BBD simulations. Check license and download contents before integration. A separate BBD DSP research implementation https://github.com/jatinchowdhury18/BBDDelay is not a SPICE pedal deck and is not evidence of a particular pedal topology.

## Delay

### Boss DM-2
- The NE570 approximation averages the full-wave rectifier current with the original 2.2 ms time constant. On a 440 Hz, 75 mV peak input, the first-repeat harmonic distortion dropped from 4.75% to 0.96% after matching this detector topology. The 44.1, 48 and 96 kHz burst tests kept the approximate 20.7–298.7 ms delay range and had no rejected steps.

- Primary first-edition manufacturer service notes, complete schematic including bypass, switching, power, input/dry/feedback/wet paths and trims: https://civade.com/AudioPro/Boss/Boss_DM-2_Delay_Machine_Service_Manual.pdf. IC3 MN3005, IC4 MN3101, IC2 NE570. Single 4096-stage BBD; two output pins and clock cancel trim; compressor and expander, anti-alias and reconstruction filters. 
- Second-edition service notes dated 25 August 1982, serial 182000 onward: https://manuals.plus/m/a5857b84879179dd0abce591b90261ee889e93c1c329e8d9c990986913f74ad3. MN3205/MN3102 replace MN3005/MN3101; not pin-compatible substitutions; PCB change ET5214-510 to -510A. 
- Existing browser simulation and LTspice netlist download button: https://bench.swellamps.com/delay-bbd-1981/. Partial effect-on model, omits footswitch, flip-flop, FET switch; BBD has combined outputs and no clock feedthrough; clock model fitted, R49 omitted. Site explicitly reserves its models, drawings and presets and forbids redistribution/use to build another product without permission, so it is not a safely reusable permissive deck. 

### EHX Deluxe Memory Man

- Complete schematic scan (revision not specified on scan; do not label it definitively as an EH7850 without cross-check): https://civade.com/AudioPro/Electro-Harmonix/Electro-Harmonix-Deluxe-Memory-Man-Delay-Schematic.pdf. Two MN3005, 4047 clock, NE570 compander, 4558 op amps, feedback, blend, modulation and outputs; -15 V supply. 
- Identified 5-knob EH7850, MN3005, compander, chorus/vibrato and overload LED, schematic linked: https://davidmorrin.com/notes/effects/delay/eh-7850. Later EC2002 big-box reissues differ and can have 2x MN3005 or 4x MN3008: https://sites.google.com/site/davidmorrinoldsite/home/trouble/troubleeffects/electro-harmonix-memory-man/ec-2002. 
- Simulator and LTspice export button: https://bench.swellamps.com/delay-bbd-1979/. It models two MN3005, compander, clock and modulation but omits overload LED, footswitch and direct output, combines BBD outputs, assumes operating parameters and omits clock feedthrough. Site prohibits copying/redistributing or use to build another product without written permission; not a freely reusable deck. 

### MXR Carbon Copy M169

- Reverse engineer's analysis and full traced schematic: https://kanengomibako.github.io/pages/00324.html; source KiCad project cited by repair community at pinned revision https://github.com/kanengomibako/ReverseEngineering/tree/4a8cced880defbc2bc080789e18d48964db4a086/MXR_Dunlop_WayHuge/CarbonCopy. The author explicitly says unofficial, potentially inaccurate and commercial use prohibited. Board revision is not established by the page; do not claim universal coverage of M169 revisions. The author says CD4047 clock and modulation LFO, with DM-2-like pre/de-emphasis and filters. Verify the actual BBD identifiers and compander from the drawing before transcription. GitHub URL failed to open in this browsing environment, so downloadable project files/completeness are unverified. 
- No verified reusable complete SPICE deck found. A conflicting web listing claims 2x MN3205, while repair reports mention 2x V3208 and BL3208; treat chip count/type as revision-specific and defer to original trace, not marketing summaries.

- The March 2009 Rev D3 drawing places C49 in Q13's bypass mute branch. In engaged mode that branch is open. C45 provides the 33 nF feedback from the Q14 emitter to the reconstruction filter. The original model incorrectly connected the 330 nF C49 to the emitter feedback. D3 retains a fixed dry input through R48 while Mix controls the wet input through R38. [An earlier Dunlop M169 instruction sheet](https://manualzz.com/doc/6566825/mxr-m169-carbon-copy-analog-delay-user-manual) likewise describes dry-only to equal wet and dry. The [currently hosted M169 instructions](https://www.jimdunlop.com/content/manuals/M169.pdf) describe a full-wet endpoint. The factory circuit follows the traced D3 mix.
- At 48 kHz, with Mod off, Delay at noon, Mix fully clockwise and Regen off, a 120 ms, 1 kHz burst at −5 dBV produced a 344 mV peak fundamental in the isolated first echo, measured over 60 ms after the dry burst had ended. The previous model produced 1.58 mV. Measured THD was 1.64%, above the manual's 1% specification. The trace's external compander capacitors and filter values are retained. The SA571 equivalent's wet gain was recalibrated to keep a sustained repeat at maximum Regen after correcting the filter. A 1.55 kHz burst measured 201 mV on the first repeat and 121 mV on the eighth at maximum Regen, compared with 33 mV on the second repeat at the factory Regen position. Two guitar DI takes at 1 V per full scale rendered with zero rejected steps at factory, Mod on and maximum delay, Mix and Regen settings.

### BBD models

- No verified permissively licensed MN3005/MN3007 SPICE subcircuit found in this search. A forum asking for an MN3007 LTspice model does not establish one exists: https://forum.allaboutcircuits.com/threads/bbd-mn3007-ltspice-model.189930/. 
- Swell's decks are obtainable via download buttons, but site terms prohibit reuse for another product without written permission. Its simplified BBD model is not a complete two-output transistor-level device and omits clock feedthrough. 
- Related digital DSP BBD project, not a SPICE macromodel: https://github.com/jpcima/bbd-delay-experimental. License and suitability need independent verification.

## Compression

### MXR Dyna Comp (CA3080)
- Direct full circuit: https://schematicheaven.net/effects/mxr_dynacomp_compressor.pdf — 3080 OTA, five transistors, 500k sensitivity and 50k volume. The drawing does not establish a particular year or board revision; describe as vintage-style CA3080 circuit, not a verified 1976 unit. No complete mechanical bypass is shown.
- Independent analysis: https://electrosmash.mas-effects.com/mxr-dyna-comp-analysis.html; alternate project schematic https://www.madbeanpedals.com/projects/_folders/StandardSeries/pdf/Ambulator.pdf is explicitly based on Dyna Comp, not proof of identical manufacture.
- Model OTA control current, BJT envelope rectifier and timing feedback, with component choices and supply/bias as shown. OTA device behavior and saturation need parameterization.

### Dan Armstrong Orange Squeezer
- With the factory bias, the second harmonic on a 440 Hz tone was 0.13% at 15 mV peak and 34.1% at 75 mV peak. The modeled JFET compression and bias strongly affect this result. A hardware comparison is pending.
- Original-reference full schematic: https://generalguitargadgets.com/pdf/ggg_osq_sc_original.pdf; reference page https://generalguitargadgets.com/effects-projects/compression/dan-armstrong-orange-sqeezer/ explicitly labels it original. Drawing version 2012January12 is a later redraw, not an original manufacturing date or authenticated specific unit revision. Includes battery, bypass, JFETs, 4558 and trim.
- Alternate description/analysis https://device.report/m/ee5a1dec05853573a79eacd40383a4acd1bce2704ee0fd48d7dc128083ff6331. Model nonlinear JFET shunt resistance and bias trim, rectifier and envelope memory, op amp supply limits; identify transistor model rather than claim universal device behavior.

### Boss CS-3
- Original manufacturer service notes Aug 1986: https://stompboxelectronics.com/wp-content/uploads/2023/01/BOSS-CS3_ServiceNotes.pdf; mirror https://www.synfo.nl/servicemanuals/Boss/BOSS_CS-3_SERVICE_NOTES.pdf. Complete circuit with power, signal path, switching and control. This is the dated 1986 service revision; later pedals may differ.
- Model VCA gain cell and its control/rectifier, timing/attack/sustain and tone, bias, and input/output buffering. Footswitch and check circuit can be omitted only if bypass is out of scope. Verify IC pin numbers and values on actual PDF, not OCR.

### Diamond CPR-1
- No manufacturer full schematic verified. Most usable full component-valued diagram located is Aion FX Quartz documentation https://aionfx.com/app/files/docs/quartz_documentation.pdf (version 1.1.0, 2026-09-25, schematic p.10, BOM pp.3–5), a disclosed adaptation of CPR-1/BCP-1, not an authenticated original circuit. Guitar BOM differs from bass, and an added three-position frequency switch differs from original; choose guitar values and original 900 Hz position, document remaining deviations. Project attribution https://aionfx.com/project/quartz-optical-compressor/. Alternate https://madbeanpedals.com/projects/_folders/Compressor/pdf/RoughCut24.pdf is also an adaptation.
- Third-party reference drawing linked at https://effectslayouts.blogspot.com/2017/06/diamond-compressor.html was unavailable at its image host at time of research. Circuit includes a VTL5C3 optocoupler, audio op amp and tilt EQ, envelope detector and feedback. Model light-dependent resistance plus attack/release hysteresis empirically; photoconductor is not an instantaneous resistor. Diamond-original exact component-level verification remains open.

### Origin Effects Cali76 FET Compressor
- No component-level original schematic or verifiable complete trace found. Official v2.2 owner's manual https://origineffects.com/wp-content/uploads/2024/11/Origin-Effects-Cali76-FET-Compressor-Owners-Manual-V2-2.pdf supplies routing, controls and specs only. It identifies internally generated 24V and separate dry blend, in/out, ratio, attack and release. Those are constraints for a behavioral approximation, insufficient to reconstruct the circuit.
- This specific FET Compressor should not be silently substituted by older large-format Cali76, compact deluxe/bass, stacked, an Asian clone, or Urei 1176. Origin explicitly describes condensed actual circuitry in its older model: https://origineffects.com/?nonce=0df9f4b2b9&tekguild_pdf_download=7520. Original FET-component schematic unresolved; seek manufacturer diagram or independently verified unit trace.

## EQ and noise reduction

Source quality and exact product revision matter. Full schematic does not mean validated circuit model; verify image legibility and component connections before transcription. These are web research leads, not completed netlists.

### Boss GE-7
- Original combined GE-7 / GE-7B service notes: https://civade.com/AudioPro/Boss/GE-7_SERVICE_NOTES.pdf. GE-7 main board ET5215-510A, sub ET5215-520A; GE-7B has distinct board and values. Keep GE-7B bass circuit separate. Confirm GE-7 schematic image against BOM.
- More legible complete redraw, Jon Hilderson April 2000: https://civade.com/AudioPro/Boss/boss_ge7_eq.pdf. Includes full audio, seven filters, electronic switching and supply; note there are optional modification callouts; don't silently incorporate modifications. The specific redraw and service notes may represent different revisions; reconcile actual part choices before asserting identity.
- Implement seven interacting gyrator-type bands and level with input/output loading and active summing, not seven arbitrarily cascaded standard graphic EQ filters. Model bypass and loading if required. Official manual: https://www.boss.info/global/support/by_product/ge-7/owners_manuals/.

### MXR M108S Ten Band EQ
- Manufacturer manual https://www.jimdunlop.com/content/manuals/M108S.pdf identifies 31.25 through 16kHz ten bands, ±12dB, gain, volume, dual outputs, true bypass and 18V supply. It specifically says noise reduction, bypass and second output were upgrades.
- Complete older M-108 PCB Rev E traced by Apollo Music Service: https://schematicsonline.com/wp-content/uploads/2022/11/MXR-M108-10-Band-Eq.pdf. MC33174 band sections, MC33179, 12 20k sliders, 18V. This is M-108, NOT verified M108S. An M108S-specific schematic remains unverified; forum separately asks for one. Treat Rev E as a clearly labelled approximation/research baseline only, not the M108S circuit.
- For faithful M108S seek direct board trace including revised noise reduction, second output and switching; manual is insufficient to reconstruct component topology. Characterize gain vs volume independently and use coupled filters in measured processing path.

### Boss PQ-4 Parametric Equalizer
- Complete schematic drawing: https://civade.com/AudioPro/Boss/Boss_PQ-4_Parametric_EQ_Schematic.pdf. Marked Boss PQ-4, IC1–5 M5218AL, Q1 2SK184-GR, no PCB revision or provenance date clear on drawing. Includes full signal path, multiple EQ networks, switching and power. Do not claim official service notes without provenance.
- Original Boss instructions https://cdn.roland.com/assets/media/pdf/PQ-4_OM.pdf : four bands: low, swept middle 100 Hz–1.6 kHz, swept high 500 Hz–8 kHz, presence; ±18 dB; separate level is before EQ. There is no user Q control. Pay attention to dual-ganged frequency pots and interactions; derive center law and bandwidth from circuit, do not assume generic independently controlled parametric bands.

### Boss NS-2 Noise Suppressor
- On a 440 Hz, 75 mV peak input, the second harmonic was 2.31% with the current gate detector and 0.041% with the gain control held open. This isolates the ripple to the detector/control path. Compare the modeled open gate against hardware before adjusting its timing.
- Full one-page schematic https://schematicsonline.com/wp-content/uploads/2022/11/Boss-NS-2-Noise-Supressor.pdf. The drawing identifies board ET 7510452000 and IC1 M5207L01. It shows all four jacks and the detector, threshold and decay networks.
- Boss describes the envelope detector and VCA at https://www.boss.info/global/products/ns-2/. The manual at https://www.cavallimusica.com/media/productattach/b/o/boss_ns2_eng.pdf explains the send/return routing, threshold, decay and mute/reduction modes. The detector reads the input separately from the attenuated return.

### ISP Decimator II G String
- Manufacturer confirms separate guitar tracking and processing channel, link for two channels: https://www.isptechnologies.com/guitar/decimator_ii_g_string. Preserve separate detector input and controlled return, adjustable threshold, dynamics and link; do not assume conventional same-signal gate.
- Closest full downloadable project: Noise Terminator 1.1, November 8 2021, page 5 https://pcbguitarmania.com/wp-content/uploads/2021/05/Noise-Terminator-1.1v-Building-Docs.pdf. Builder explicitly says inspired by Decimator II, not an authenticated factory trace. Cover says THAT 4301 but BOM states THAT 2181, so inspect schematic and resolve discrepancy before implementation. Schematic shows four jacks and VCA, but 3PDT project bypass and lack of confirmed link implementation mean not a drop-in claim of original identity. Third-party index AI text invents release and mix controls; actual build document names threshold and switch only.
- Another community G-String circuit (without II in title) https://stompboxschematics.com/circuits/decimator-g-string-project-by-guitar-electronics-eu/ is an earlier/unspecified generation and its linked PDF returned 404. Don't conflate with II. No authenticated complete factory II G String schematic located; get hardware and trace or explicit manufacturer technical documentation to claim exact model.
