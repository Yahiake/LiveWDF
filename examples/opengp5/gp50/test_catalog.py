import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


RATE = 44100


def render(root, name, events, duration, input_name="input"):
    script = root / f"{name}.txt"
    script.write_text("\n".join(events) + "\n", encoding="utf-8")
    output = root / f"{name}.wav"
    subprocess.run([sys.executable, *sys.argv[1:3], "--gp50", "--host-dsp", sys.argv[3],
                    "--script", str(script), "--in", str(root / f"{input_name}.wav"),
                    "--seconds", str(duration), "--out", str(output),
                    "--shots", str(root / "frames")], check=True)
    with wave.open(str(output), "rb") as stream:
        return stream.readframes(stream.getnframes())


def main():
    with tempfile.TemporaryDirectory(prefix="livewdf-gp50-catalog-") as directory:
        root = Path(directory)
        segment = RATE // 4
        drive = [round(3700 * math.sin(2 * math.pi * 204 * i / RATE)
                       + 2700 * math.sin(2 * math.pi * 932 * i / RATE)
                       + 1700 * math.sin(2 * math.pi * 2416 * i / RATE)) for i in range(segment)]
        with wave.open(str(root / "input.wav"), "wb") as stream:
            stream.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(b"".join(struct.pack("<h", sample) for sample in drive) * 17)

        sequence = [f"{model / 4} set Model {model}" for model in range(16)]
        wet = render(root, "catalog", sequence, 4.25)
        dry = render(root, "bypass", ["0 foot"], 0.25)
        start_frame, end_frame = segment // 2, segment - 64
        start, end = start_frame * 6, end_frame * 6
        dry_fragment = dry[start:end]
        expected = b"".join((sample << 8).to_bytes(3, "little", signed=True) * 2
                            for sample in drive[start_frame:end_frame])
        assert dry_fragment == expected, "FS1 bypass changed the input"
        responses = set()
        for model in range(16):
            offset = model * segment * 6
            fragment = wet[offset + start:offset + end]
            assert any(fragment) and fragment != dry_fragment, f"model {model} failed to process audio"
            responses.add(fragment)
        assert len(responses) == 16, "two model choices rendered the same response"

        expression = render(root, "expression", ["0 expression 32", "0.10 set Model 15",
                            "0.20 expression 255", "0.30 expression 0", "0.40 expression 128"], 0.5)
        manual = render(root, "manual", ["0.10 set Model 15", "0.10 set Value 125",
                        "0.20 set Value 1000", "0.30 set Value 0", "0.40 set Value 502"], 0.5)
        still = render(root, "still", ["0.10 set Model 15", "0.10 set Value 125"], 0.5)
        assert expression == manual, "expression motion differs from changing the original wah potentiometer"
        assert expression != still, "the Cry Baby did not respond to expression movement"

        with wave.open(str(root / "silence.wav"), "wb") as stream:
            stream.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(bytes(4 * RATE))
        bias = render(root, "bias", ["0 set Model 7", "0.10 set Control 1", "0.20 set Value 1000"],
                      2.0, "silence")
        peak = max(abs(int.from_bytes(bias[i:i + 3], "little", signed=True))
                   for i in range(0, len(bias), 3)) / 2 ** 23
        assert peak < 0.5, "adjusting Orange Squeezer Bias caused a loud transient on silent input"


if __name__ == "__main__":
    main()
