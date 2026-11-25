# Factory cabinet impulse responses

These are measurements of physical speakers, cabinets and microphones. Each WAV retains its upstream license. Cabinet and microphone names identify the recording equipment.

The source samples were converted to mono 48 kHz with a polyphase resampler where needed. We trimmed before the first sample within 50 dB of the impulse peak, leaving up to eight samples before that point. We kept up to 4,096 samples and applied a cosine fade over the final 256 samples or 10% of the response, whichever was shorter. Gain targets a −6 dB pink-weighted mean-square response over 80 Hz to 6 kHz, with a 0 dB ceiling on the highest bin of a 16,384-point FFT. Files are mono 24-bit PCM WAVs and end in silence. There is no equalization or minimum-phase conversion.

## Jester Dyne measurements

These six recordings by [jesterdyne on Freesound](https://freesound.org/people/jesterdyne/) are licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). We obtained the audio from the lossless FLAC copies listed in the [Axion IR manifest](https://axion.cab/irs/MANIFEST.json).

| Factory WAV | Original recording |
|---|---|
| `jensen-sm57.wav` | [Jensen Cab SM57 center.wav](https://freesound.org/s/116743/) |
| `jensen-e606.wav` | [Jensen Cab e606 center.wav](https://freesound.org/s/116746/) |
| `hk-celestion-sm57.wav` | [Clestion Cabinet SM57 center.wav](https://freesound.org/s/99945/) |
| `hk-celestion-c414.wav` | [Clestion Cabinet C414 center.wav](https://freesound.org/s/99941/) |
| `engl-v30-sm57.wav` | [Engl Celestion V30 SM57 center-01.wav](https://freesound.org/s/116735/) |
| `engl-v30-e606.wav` | [Engl Celestion V30 e606 center-02.wav](https://freesound.org/s/116738/) |

The following are Jester Dyne Productions' recordings of a modified Behringer BG412S. The handbook in the [original Brutal IR Pack archive](https://www.jester-dyne-productions.com/brutal-ir-pack/) dedicates them under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The source filenames refer to its `Impulses/48kHz` folder.

| Factory WAV | Original recording | Speakers and microphones |
|---|---|---|
| `bg412s-v30-sm57.wav` | `1_Cookie_Monster.wav` | Celestion Vintage 30, Shure SM57 |
| `bg412s-dv77-sm57.wav` | `2_Darth_Genocider.wav` | Eminence DV-77, Shure SM57 |
| `bg412s-rockdriver-e606.wav` | `3_Kitten_Slayer.wav` | Celestion Rockdriver Junior, Sennheiser e606 |
| `bg412s-v30-dv77-sm57.wav` | `6_Vertical_Lip_Stabber.wav` | Vintage 30 and DV-77, two Shure SM57s |

## Orange cabinet measurements

- `orange-ppc112-v30-sm57.wav` is from the [Puke Amp Asset Library](https://puke.studio/), copyright © 2026 the RATLab. It captures an Orange PPC112 with a Celestion Vintage 30 and a Shure SM57 one inch off center and one inch from the grille. [Original WAV](https://github.com/ElectricGuitarInnovationLab/Puke-Amp/blob/f6b4c9956860146c1212f39e6c7755babb74ad35/Models/IR/Orange_Celestion_V30_Shure_SM57_1_from_center_1_from_grill_LL.wav) · [Asset license and attribution](https://github.com/ElectricGuitarInnovationLab/Puke-Amp/blob/f6b4c9956860146c1212f39e6c7755babb74ad35/ASSETS_LICENSE.md) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
- `orange-ppc212v-neocreamback.wav` is `02-dark-matter.wav` by Darwin's Cat. It captures an open-back Orange PPC212V with two Celestion Neo Creambacks and a blend of Shure SM57, Sennheiser e906 and Neumann U87 Ai microphones. [Original Orange PPC212V capture and archive](https://darwinscat.com/felitronics/ir/orange-ppc212v) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

## TONE3000 measurements

These cabinet IRs are from the [TONE3000 Neural Amp Modeler WASM demo](https://github.com/tone-3000/neural-amp-modeler-wasm/blob/ea8dc285462d6cedd982691b9f090cd03dac04a2/ui/README.md), copyright © 2026 TONE3000, and ship under the [MIT license](LICENSE-TONE3000.txt). The originals are pinned to commit `ea8dc285462d6cedd982691b9f090cd03dac04a2`.

| Factory WAV | Original recording |
|---|---|
| `mesa-412-os-v30.wav` | [Mesa 412 OS](https://github.com/tone-3000/neural-amp-modeler-wasm/blob/ea8dc285462d6cedd982691b9f090cd03dac04a2/ui/public/irs/mesa.wav) |
| `celestion-g12-vintage.wav` | [Celestion G12 Vintage](https://github.com/tone-3000/neural-amp-modeler-wasm/blob/ea8dc285462d6cedd982691b9f090cd03dac04a2/ui/public/irs/celestion.wav) |
| `eminence-governor.wav` | [Eminence Governor](https://github.com/tone-3000/neural-amp-modeler-wasm/blob/ea8dc285462d6cedd982691b9f090cd03dac04a2/ui/public/irs/eminence.wav) |
| `ampeg-810.wav` | [Ampeg 8x10](https://github.com/tone-3000/neural-amp-modeler-wasm/blob/ea8dc285462d6cedd982691b9f090cd03dac04a2/ui/public/irs/ampeg.wav) |

## Suggested factory amp pairings

Use these as starting points. Exact cabinet configuration varies by amp and rig.

| Factory amp | Suggested WAV | Pairing |
|---|---|---|
| EVH 5150 III Stealth, Peavey 5150 | `bg412s-dv77-sm57.wav` | Modern high-gain 4x12 alternative |
| Marshall Valvestate 8100 | `hk-celestion-sm57.wav` | British-voiced Celestion cabinet alternative |
| Mesa Dual Rectifier | `mesa-412-os-v30.wav` | Mesa oversized Rectifier cabinet line, Vintage 30 |
| Orange Rockerverb Clean | `orange-ppc212v-neocreamback.wav` | Open-back Orange 2x12 alternative |
| Orange Rockerverb Dirty, Orange OR120 | `orange-ppc112-v30-sm57.wav` | Closed-back Orange Vintage 30 alternative |
| Fender Princeton Reverb, Deluxe Reverb, Twin Reverb, Tweed Champ | `jensen-sm57.wav` | American clean Jensen voicing |
| Fender Bassman, Tweed Deluxe | `jensen-e606.wav` | Jensen with a darker microphone perspective |
| Roland JC-120 | `jensen-sm57.wav` | Clean cabinet alternative |
| Vox AC15, Vox AC30 | `orange-ppc212v-neocreamback.wav` | Open-back Celestion alternative with Neo Creambacks |
| Marshall 1959, Marshall JCM800, Marshall Silver Jubilee, Laney GH100L | `hk-celestion-sm57.wav` | British-voiced Celestion cabinet alternative |
| Hiwatt DR103 | `hk-celestion-c414.wav` | British-voiced alternative with a condenser microphone |
| ENGL E530 | `engl-v30-sm57.wav` | ENGL Celestion Vintage 30 recording |
| Mesa Mark IIC+, Soldano SLO-100 | `mesa-412-os-v30.wav` | Modern Vintage 30 4x12 alternative |
| Ampeg B-15N Portaflex | `ampeg-810.wav` | Ampeg bass cabinet alternative, 8x10 format |
