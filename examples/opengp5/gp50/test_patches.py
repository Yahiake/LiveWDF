from pathlib import Path
import tempfile
import wave

from test_catalog import RATE, render


def main():
    with tempfile.TemporaryDirectory(prefix="livewdf-gp50-patches-") as directory:
        root = Path(directory)
        store = root / "patches.store"
        with wave.open(str(root / "input.wav"), "wb") as stream:
            stream.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(bytes(14 * RATE))

        def frame(name, run="patches"):
            return next((root / run).glob(f"{name}.p*")).read_bytes()

        render(root, "patches", [
            "0 set Model 3",
            "0 set POT_0 230", "0 set POT_1 740", "0 set POT_2 610",
            "0 set Input 82", "0 set Level 134",
            "0 set Cab 1", "0 set Delay 35", "0 set Reverb 15",
            "0.10 save click", "1.25 shot home-saved",
            "1.30 para click", "1.45 shot circuit1-saved",
            "1.50 para turn 1", "1.55 shot circuit2-saved",
            "1.60 para turn 1", "1.65 shot model-saved",
            "1.70 global click", "1.75 shot cab-saved",
            "1.85 drum click", "1.90 shot ambience-saved",
            "2.00 fs2 click", "2.05 shot next-patch",
            "2.15 set Model 3", "2.16 set POT_0 900", "2.16 set POT_1 900", "2.16 set POT_2 900",
            "2.17 set Input 12", "2.17 set Level 29",
            "2.25 foot", "2.30 shot home-recalled",
            "2.40 para click", "2.55 shot circuit1-recalled",
            "2.60 para turn 1", "2.65 shot circuit2-recalled",
            "2.70 para turn 1", "2.75 shot model-recalled",
            "2.80 global click", "2.85 shot cab-recalled",
            "2.95 drum click", "3.00 shot ambience-recalled",
            "3.10 left turn 2", "3.15 shot delay-changed",
            "3.20 middle turn 2", "3.25 shot edit-knob-level",
            "3.30 set Level 136", "3.35 set Level 138", "3.40 shot edit-direct-level",
            "3.45 exit click", "3.50 shot exit-knob-level",
            "3.55 set Level 136", "3.60 set Level 138", "3.65 shot home-direct-level",
            "3.70 drum click", "3.75 shot delay-after-exit",
            "3.85 exit click", "3.90 left turn -100", "3.95 shot left-drive",
            "4.00 set POT_0 10", "4.05 set POT_0 0", "4.10 shot drive-zero",
            "4.20 right turn 1", "4.25 shot right-tone",
            "4.30 set POT_1 740", "4.35 set POT_1 750", "4.40 shot tone-750",
            "4.50 middle turn 2", "4.55 shot home-knob-level",
            "4.60 set Level 140", "4.65 set Level 142", "4.70 shot level-142",
            "4.80 para turn 1", "4.85 shot para-next",
            "4.90 set Model 7", "4.91 set POT_0 915", "4.92 set Level 49",
            "5.00 save click", "6.20 shot second-saved",
        ], 6.25, store=store)

        assert frame("next-patch") != frame("home-saved"), "B did not load a different patch"
        assert frame("para-next") == frame("next-patch"), "PARA did not browse patches from home"
        pages = ("home", "circuit1", "circuit2", "model", "cab", "ambience")
        for page in pages:
            assert frame(f"{page}-saved") == frame(f"{page}-recalled"), f"patch lost {page} settings"
        assert frame("ambience-saved") != frame("delay-changed"), "left knob did not adjust Delay"
        assert frame("edit-knob-level") == frame("edit-direct-level"), "middle knob did not adjust Level in edit"
        assert frame("exit-knob-level") == frame("home-direct-level"), "EXIT discarded the unsaved Level"
        assert frame("edit-knob-level") == frame("delay-after-exit"), "EXIT discarded the unsaved Delay"
        assert frame("left-drive") == frame("drive-zero"), "left knob did not turn Drive down to zero"
        assert frame("right-tone") == frame("tone-750"), "right knob did not adjust the TS808 Tone"
        assert frame("home-knob-level") == frame("level-142"), "middle knob did not adjust Level on home"

        render(root, "restart", [
            "0.10 shot second-restored",
            "0.20 foot", "0.25 shot home-restarted",
            "0.35 para click", "0.50 shot circuit1-restarted",
            "0.55 para turn 1", "0.60 shot circuit2-restarted",
            "0.65 para turn 1", "0.70 shot model-restarted",
            "0.75 global click", "0.80 shot cab-restarted",
            "0.90 drum click", "0.95 shot ambience-restarted",
        ], 1.1, store=store)
        assert frame("second-restored", "restart") == frame("second-saved"), "restart lost the last saved patch"
        for page in pages:
            assert frame(f"{page}-restarted", "restart") == frame(f"{page}-saved"), f"restart lost {page} settings"


if __name__ == "__main__":
    main()
