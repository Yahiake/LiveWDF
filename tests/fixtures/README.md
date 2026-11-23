# Dry guitar test inputs

| Pedal family | Audio input | Complementary checks |
|---|---|---|
| Distortion and noise reduction | [PF300 palm mutes](guitar-techs-di/pf300-palm-mutes.flac) and [major chords](guitar-techs-di/pf300-major-chords.flac) | Repeat at multiple input levels. For a gate with a separate detector, feed clean DI to the detector and a reamped or simulated high-gain signal to the return. Use the recorded spaces to check closure and decay. |
| Overdrive and EQ | [PF300 major chords](guitar-techs-di/pf300-major-chords.flac) and [Strat single-note DI](guitarjam-di/strat-neck-middle-di.wav) | Sweep each EQ band or frequency control at a low input level, then check headroom at a higher level. |
| Compressors and envelope filters | [Sire T7 performance](guitar-techs-di/sire-t7-performance-11.flac) and [Strat single-note DI](guitarjam-di/strat-neck-middle-di.wav) | Use stepped burst amplitudes for attack, release and gain; preserve dynamics in the musical signal. |
| Chorus, flanger, phaser, tremolo, and delay | [PF300 major chords](guitar-techs-di/pf300-major-chords.flac) and [Sire T7 performance](guitar-techs-di/sire-t7-performance-11.flac) | Check note separation, modulation depth and tail timing. Use a tone or impulse for timing and notch measurements. |
| Fuzz and octave fuzz | [PF300 high bends](guitar-techs-di/pf300-high-bends.flac) and [major chords](guitar-techs-di/pf300-major-chords.flac) | Use single notes for octave tracking and chords for intermodulation. |

Set the simulator sample rate to match the file or resample the audio.
Record input scaling in volts and use the same scale for comparisons.
The recordings cover played notes, chords and rests. Measured circuit
accuracy still needs calibrated input, documented controls and a circuit
reference.
