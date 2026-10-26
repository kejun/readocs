import shutil
import subprocess
from pathlib import Path


def test_actual_setup_page_error_results_and_bounded_polling(tmp_path):
    root = Path(__file__).resolve().parents[1]
    page = (root / "firmware/device/main/setup_page.hpp").read_text()
    script = page.split("<script>", 1)[1].split("</script>", 1)[0]
    script = script.split("load().catch", 1)[0]
    (tmp_path / "setup-page.js").write_text(script)
    node = shutil.which("node")
    assert node
    subprocess.run([node, "--check", str(tmp_path / "setup-page.js")], check=True)
    subprocess.run(
        [node, str(root / "tests/setup_page.js"), str(tmp_path / "setup-page.js")],
        check=True,
        timeout=15,
    )
