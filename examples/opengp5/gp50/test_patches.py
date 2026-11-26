from pathlib import Path
import tempfile
import wave

from test_catalog import RATE, render


def screens(time, suffix):
    events = []
    for control in range(3):
        at = time + 0.1 * control
        events.extend((f"{at:.2f} set Control {control}", f"{at + 0.05:.2f} shot pot{control}-{suffix}"))
    return events


def main():
    with tempfile.TemporaryDirectory(prefix="livewdf-gp50-patches-") as directory:
        root = Path(directory)
        with wave.open(str(root / "input.wav"), "wb") as stream:
            stream.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(bytes(12 * RATE))
        render(root, "patches", [
            "0 set Model 3",
            "0 set POT_0 230", "0 set POT_1 740", "0 set POT_2 610",
            "0 set Input 82", "0 set Level 134",
            "0 set Cab 1", "0 set Delay 35", "0 set Reverb 15", "0 set Effect 0",
            "0.02 drum click", "0.06 para turn 2", "0.10 save click", "0.24 exit click",
            *screens(1.15, "saved"),
            "1.42 para turn 4", "1.44 shot input-saved",
            "1.45 global click", "1.60 shot output-saved",
            "1.70 fs2 click", "1.85 shot next-patch",
            "1.91 set Model 3",
            "1.92 set POT_0 900", "1.92 set POT_1 900", "1.92 set POT_2 900",
            "1.93 set Input 12", "1.93 set Level 29",
            "2.05 foot",
            *screens(2.20, "recalled"),
            "2.47 para turn 4", "2.49 shot input-recalled",
            "2.50 global click", "2.65 shot output-recalled",
            "2.75 drum click", "2.90 shot delay-before",
            "3.00 para turn 4", "3.10 shot delay-changed",
            "3.20 exit click", "3.35 drum click", "3.50 shot delay-cancelled",
            "3.60 exit click", "3.75 exit click", "3.90 shot patch-back",
            "4.00 left turn -100", "4.10 shot left-drive",
            "4.20 set POT_0 0", "4.25 shot drive-zero",
            "4.30 right turn 1", "4.40 shot right-tone",
            "4.50 set POT_1 750", "4.60 shot tone-750",
            "4.70 middle turn 2", "4.80 shot middle-level",
            "4.90 set Level 138", "5.00 shot level-138",
        ], 5.1)

        def frame(name):
            return next((root / "frames").glob(f"{name}.p*")).read_bytes()

        assert frame("next-patch") != frame("pot0-saved"), "B did not load a different patch"
        for i in range(3):
            assert frame(f"pot{i}-saved") == frame(f"pot{i}-recalled"), f"patch lost model control {i}"
        assert frame("input-saved") == frame("input-recalled"), "patch lost Input trim"
        assert frame("output-saved") == frame("output-recalled"), "patch lost cabinet, delay, reverb or bypass"
        assert frame("delay-before") != frame("delay-changed"), "DRUM did not open Delay editing"
        assert frame("delay-before") == frame("delay-cancelled"), "EXIT did not cancel the edit"
        assert frame("patch-back") == frame("pot2-recalled"), "EXIT did not return to the patch"
        assert frame("left-drive") == frame("drive-zero"), "left knob did not turn Drive down to zero"
        assert frame("right-tone") == frame("tone-750"), "right knob did not adjust the TS808 Tone"
        assert frame("middle-level") == frame("level-138"), "middle knob did not adjust Level"


if __name__ == "__main__":
    main()
