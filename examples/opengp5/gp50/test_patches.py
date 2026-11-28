from pathlib import Path
import tempfile
import wave

from test_catalog import RATE, render


def patch_views(start, suffix):
    sequence = (
        (0.00, "shot home"),
        (0.05, "para turn 1"), (0.10, "shot drive"),
        (0.15, "para turn 1"), (0.20, "shot tone"),
        (0.25, "para turn 1"), (0.30, "shot volume"),
        (0.35, "para turn 1"), (0.40, "shot model"),
        (0.45, "global click"), (0.50, "shot cab"),
        (0.60, "para turn 1"), (0.65, "shot effect"),
        (0.70, "drum click"), (0.75, "shot delay"),
        (0.85, "para turn 1"), (0.90, "shot reverb"),
    )
    return [f"{start + at:.2f} {cmd}" + (f"-{suffix}" if cmd.startswith("shot") else "") for at, cmd in sequence]


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
            "0 set Model 3", "0 set POT_0 230", "0 set POT_1 740", "0 set POT_2 610",
            "0 set Input 82", "0 set Level 134",
            "0 set Cab 1", "0 set Delay 35", "0 set Reverb 15",
            "0.10 save click",
            *patch_views(1.25, "saved"),
            "2.20 fs2 click", "2.25 shot next-patch",
            "2.35 set Model 3", "2.36 set POT_0 900", "2.36 set POT_1 900", "2.36 set POT_2 900",
            "2.37 set Input 12", "2.37 set Level 29",
            "2.45 foot",
            *patch_views(2.60, "recalled"),
            "3.55 exit click", "3.60 shot exit",
            "3.70 para turn 1", "3.75 shot turn-opens-edit",
            "3.80 para turn 1", "3.85 shot scroll-to-tone",
            "3.90 para click", "4.05 shot adjust-tone",
            "4.10 para turn 2", "4.15 shot para-adjusted",
            "4.20 set POT_1 740", "4.25 set POT_1 760", "4.30 shot tone-760",
            "4.35 para click", "4.50 para turn -1", "4.55 shot scroll-back-to-drive",
            "4.60 left turn 2", "4.70 middle turn 2", "4.80 right turn 2",
            "4.85 shot three-knobs",
            "4.90 set Input 82", "4.95 set Level 134", "5.00 set POT_0 230",
            "5.05 set Input 86", "5.10 set Level 138", "5.15 set POT_0 250", "5.20 shot direct-three",
            "5.25 exit click", "5.30 shot edited-home",
            "5.40 fs2 click", "5.50 set Model 7", "5.51 set POT_0 915", "5.52 set Level 49",
            "5.53 set Effect Bypass", "5.55 para turn 1", "5.60 para click", "5.75 save click",
            "6.90 shot editor-saved", "6.95 para click", "7.10 para click", "7.25 shot browser",
            "7.30 exit click", "7.35 shot second-saved",
        ], 7.4, store=store)

        assert frame("next-patch") != frame("home-saved"), "B did not load a different patch"
        views = ("home", "drive", "tone", "volume", "model", "cab", "effect", "delay", "reverb")
        for view in views:
            assert frame(f"{view}-saved") == frame(f"{view}-recalled"), f"patch lost {view} settings"
        assert frame("exit") == frame("home-saved"), "EXIT did not restore the home screen"
        assert frame("turn-opens-edit") == frame("drive-saved"), "turning PARA did not open the edit list"
        assert frame("scroll-to-tone") == frame("tone-saved"), "PARA browsing changed a parameter"
        assert frame("adjust-tone") != frame("scroll-to-tone"), "PARA click did not enter adjust mode"
        assert frame("para-adjusted") == frame("tone-760"), "PARA did not adjust the selected Tone"
        assert frame("scroll-back-to-drive") != frame("para-adjusted"), "the second press did not resume browsing"
        assert frame("three-knobs") == frame("direct-three"), "quick knobs lost Input, Level, or Drive in edit mode"
        assert frame("edited-home") != frame("home-saved"), "EXIT discarded the unsaved edit"
        assert frame("editor-saved") == frame("browser"), "SAVE left the selected row in adjust mode"

        render(root, "restart", [
            "0.10 shot second-restored",
            "0.20 foot",
            *patch_views(0.35, "restarted"),
        ], 1.35, store=store)
        assert frame("second-restored", "restart") == frame("second-saved"), "restart lost the last saved patch"
        for view in views:
            assert frame(f"{view}-restarted", "restart") == frame(f"{view}-saved"), f"restart lost {view} settings"


if __name__ == "__main__":
    main()
