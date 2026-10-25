import shutil
import subprocess
from pathlib import Path


def test_actual_light_sleep_and_network_barrier(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware/device/main/power_key.cpp").read_text()
    marker = "bool light_sleep_ui() {"
    (tmp_path / "light_sleep_under_test.hpp").write_text(
        marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"
    )
    network = (root / "firmware/device/main/network.cpp").read_text()
    (tmp_path / "network_awake_under_test.hpp").write_text(
        "networking = true;"
        + network.split("networking = true;", 1)[1].split("    deadline =", 1)[0]
    )
    main = (root / "firmware/device/main/main.cpp").read_text()
    marker = "    const bool usb = pm1_vbus_voltage() > 4000;"
    (tmp_path / "idle_loop_under_test.hpp").write_text(
        marker + main.split(marker, 1)[1].split("    if (usb &&", 1)[0]
    )
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler
    binary = tmp_path / "ui-power"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-I",
            str(root / "firmware/device/main"),
            "-I",
            str(tmp_path),
            str(root / "tests/ui_power.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=15)
