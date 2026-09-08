# SPDX-License-Identifier: GPL-2.0+
"""Compile the actual slot-selection functions against legacy metadata."""
import pathlib
import subprocess
import sys
import tempfile


def extract(source, name):
    start = source.index("static ", source.index(name) - 20)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source_path = (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else
               pathlib.Path(__file__).resolve().parents[2] / "cmd/bootnuttx.c")
source = source_path.read_text()
functions = "\n".join(extract(source, name) for name in
                       ("k7_bootctrl_slot_bootable", "k7_bootctrl_choose"))
program = """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
struct k7_slot_disk {
    uint8_t priority, tries_remaining, successful, reserved;
    uint64_t image_size, image_version;
    uint8_t sha256[32];
};
struct k7_domain_disk {
    uint8_t active_slot, reserved[3];
    struct k7_slot_disk slots[2];
};
""" + functions + """
int main(void) {
    struct k7_domain_disk d = { .active_slot = 1,
        .slots = {{.priority = 0}, {.priority = 15}} };
    /* Already exhausted by the old three-boot policy. */
    assert(k7_bootctrl_choose(&d) == 1);
    for (int boot = 0; boot < 10; boot++)
        assert(k7_bootctrl_choose(&d) == 1);
    /* Failed image verification clears priority; fallback still works. */
    d.slots[0].priority = 14;
    d.slots[0].successful = 1;
    d.slots[1].priority = 0;
    assert(k7_bootctrl_choose(&d) == 0);
    d.slots[0].priority = 0;
    assert(k7_bootctrl_choose(&d) == -1);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="nboot-eligibility-") as directory:
    root = pathlib.Path(directory)
    (root / "test.c").write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(root / "test.c"),
                    "-o", str(root / "test")], check=True)
    subprocess.run([str(root / "test")], check=True)
print("BOOT_ELIGIBILITY_PASS")
