import argparse
from pathlib import Path

from carla_backend import (
    BINARY_NATIVE,
    ENGINE_OPTION_OSC_ENABLED,
    ENGINE_OPTION_PATH_BINARIES,
    ENGINE_OPTION_PROCESS_MODE,
    ENGINE_OPTION_TRANSPORT_MODE,
    ENGINE_PROCESS_MODE_CONTINUOUS_RACK,
    ENGINE_TRANSPORT_MODE_INTERNAL,
    PLUGIN_OPTIONS_NULL,
    PLUGIN_VST3,
    CarlaHostDLL,
)


def check_parameters(host):
    pot_indexes = {}
    for index in range(host.get_parameter_count(0)):
        name = host.get_parameter_info(0, index)["name"]
        if not name.startswith("Pot "):
            continue
        default = host.get_default_parameter_value(0, index)
        current = host.get_current_parameter_value(0, index)
        if default != 0.5 or current != default:
            raise RuntimeError(
                f"{name} expected 0.5, got default {default:g} and current {current:g}"
            )
        pot_indexes[name] = index

    if "Pot 1" not in pot_indexes:
        raise RuntimeError("Carla did not expose LiveWDF's potentiometer parameters")

    first_pot = pot_indexes["Pot 1"]
    for position in (0.0, 1.0):
        host.set_parameter_value(0, first_pot, position)
        if host.get_current_parameter_value(0, first_pot) != position:
            raise RuntimeError(f"Carla could not set Pot 1 to {position:g}")

    print(f"Carla exposed {len(pot_indexes)} pots at 0.5 and accepted both endpoints.")


def main():
    parser = argparse.ArgumentParser(description="Check LiveWDF VST3 knob defaults in Carla.")
    parser.add_argument("carla_library", type=Path)
    parser.add_argument("vst3_bundle", type=Path)
    args = parser.parse_args()
    library = args.carla_library.resolve(strict=True)
    bundle = args.vst3_bundle.resolve(strict=True)

    host = CarlaHostDLL(str(library), False)
    host.set_engine_option(ENGINE_OPTION_PATH_BINARIES, 0, str(library.parent))
    host.set_engine_option(ENGINE_OPTION_PROCESS_MODE, ENGINE_PROCESS_MODE_CONTINUOUS_RACK, "")
    host.set_engine_option(ENGINE_OPTION_TRANSPORT_MODE, ENGINE_TRANSPORT_MODE_INTERNAL, "")
    host.set_engine_option(ENGINE_OPTION_OSC_ENABLED, 0, "")
    if not host.engine_init("Dummy", "LiveWDF smoke test"):
        raise RuntimeError(host.get_last_error())
    try:
        if not host.add_plugin(
            BINARY_NATIVE, PLUGIN_VST3, str(bundle), None, None, 0, None, PLUGIN_OPTIONS_NULL
        ):
            raise RuntimeError(host.get_last_error())
        check_parameters(host)
    finally:
        host.engine_close()


if __name__ == "__main__":
    main()
