# Factory reverb impulse responses

These are stereo, wet-only impulse responses in lossless 24-bit FLAC at 48 kHz.

## Original synthetic responses

The synthetic responses are covered by the repository license.

| File | Preset | Sound | Length |
| --- | --- | --- | ---: |
| `plate-studio.flac` | Studio Plate | Bright, smooth plate with a quick build | 1.85 s |
| `plate-dark.flac` | Dark Plate | Warmer plate with a longer low-mid tail | 2.55 s |
| `spring-combo.flac` | Combo Spring | Short, centered amp spring with a pronounced drip | 1.34 s |
| `spring-tank.flac` | Long Tank Spring | Wider, darker tank with longer reflections | 2.14 s |
| `hall-small.flac` | Small Hall | Close reflections and a short diffuse decay | 1.91 s |
| `hall-concert.flac` | Concert Hall | Spacious reflections and a long, darkening tail | 3.65 s |

The plates combine dispersed early paths with a seeded, band-dependent diffuse field. The springs use damped dispersive paths and secondary modes. The halls use image-source reflections from two modeled rooms, blended into a diffuse decay. The two channels model different pickup or listener positions. A common low cut and a smooth fade to silence finish the responses. The six responses were matched by stereo impulse energy to keep the wet level consistent across presets.

## Measured responses

The OpenAIR recordings are from the Open Acoustic Impulse Response Library, AudioLab, University of York, and are licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The Maida Vale recording is licensed [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/). These licenses apply to the respective bundled adaptations. [OpenAIR project](https://www.york.ac.uk/physics-engineering-technology/research/communication-technologies/projects/open-acoustic-impulse-response-library/)

| File | Preset | Capture and credit | Length |
| --- | --- | --- | ---: |
| `room-genesis-live.flac` | Genesis 6 Live Room | Snare position in a recording studio live room, OpenAIR. [96 kHz B-format](https://webfiles.york.ac.uk/OPENAIR/IRs/genesis-6-studio-live-room-drum-set/b-format/snare_ir.wav) | 0.87 s |
| `room-maida-vale.flac` | Maida Vale Studio 5 | MV5 Acoustic Setup 2, receiver PA-7B, source PA-3A facing south, `MV5_AS2_KEMAR_R_PA-7B_S_PA-3A_S.wav`, AudioLab at the University of York and BBC R&D. [KEMAR stereo dataset](https://zenodo.org/records/10020866) | 0.64 s |
| `hall-jack-lyons.flac` | Jack Lyons Concert Hall | Stage left seating block at the University of York, OpenAIR. [96 kHz B-format](https://webfiles.york.ac.uk/OPENAIR/IRs/jack-lyons-concert-hall-university-york/b-format/rir_jack_lyons_lp4_96k.wav) | 2.60 s |
| `hall-usina-del-arte.flac` | Usina del Arte Symphony Hall | Buenos Aires, Argentina, source 1 and position 3, Leandro Rodiño, Alejandro Bidondo and Nahuel Cacavelos, Universidad Nacional de Tres de Febrero. [KEMAR stereo](https://webfiles.york.ac.uk/OPENAIR/IRs/usina-del-arte-symphony-hall/stereo/usina_main_s1_p3.wav) | 1.75 s |
| `church-st-albans.flac` | St Albans Lady Chapel | Position B at St Albans Cathedral, Marcin Gorzel, Gavin Kearney, Aglaia Foteinou, Sorrel Hoare and Simon Shelley, OpenAIR. [ORTF stereo](https://webfiles.york.ac.uk/OPENAIR/IRs/lady-chapel-st-albans-cathedral/stereo/stalbans_b_ortf.wav) | 3.35 s |
| `church-york-minster.flac` | York Minster | Centre of the nave, Damian T. Murphy, OpenAIR. [ORTF stereo](https://webfiles.york.ac.uk/OPENAIR/IRs/york-minster/stereo/minster1_000_ortf_48k.wav) | 8.00 s |
| `outdoor-trollers-gill.flac` | Troller's Gill Gorge | Limestone gorge in the Yorkshire Dales, site 2 with four-way source, OpenAIR. [96 kHz B-format](https://webfiles.york.ac.uk/OPENAIR/IRs/trollers-gill/b-format/dales_site2_4way_bformat.wav) | 0.76 s |
| `outdoor-koli-forest.flac` | Koli Forest in Winter | Koli National Park, Finland, site 1 with four-way source, Simon Shelley, Damian Murphy and Andrew Chadwick, OpenAIR. [96 kHz B-format](https://webfiles.york.ac.uk/OPENAIR/IRs/koli-national-park-winter/b-format/koli_snow_site1_4way_bformat.wav) | 1.08 s |

Gavin Kearney, Helena Daffern, Patrick Cairns, Anthony Hunt, Ben Lee, Jacob Cooper, Panos Tsagkarakis, Tomasz Rudzki and Daniel Johnston (2022), *Measuring the Acoustical Properties of the BBC Maida Vale Recording Studios for Virtual Reality*, Acoustics 4(3), 783–799. [Paper](https://doi.org/10.3390/acoustics4030047)

For LiveWDF, each measurement was aligned to the direct arrival and gated through it, retaining the timing of the subsequent reflections. B-format measurements were decoded to a coincident stereo pair. The responses were resampled where needed, high-passed at 32 Hz, faded to silence and level-matched against the synthetic presets by stereo impulse energy and guitar DI peaks.
