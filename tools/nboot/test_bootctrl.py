#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
#
# Copyright 2026 Pharos Tech

import importlib.util
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("bootctrl.py")
SPEC = importlib.util.spec_from_file_location("bootctrl", MODULE_PATH)
BOOTCTRL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOOTCTRL)


class BootctrlTest(unittest.TestCase):
    def setUp(self):
        self.empty = {"size": 0, "version": 0, "sha256": bytes(32)}
        self.image = {
            "size": 3,
            "version": 1,
            "sha256": bytes.fromhex("11" * 32),
        }

    def test_record_layout_matches_nboot_disk_format(self):
        self.assertEqual(BOOTCTRL.HEADER.size, 20)
        self.assertEqual(BOOTCTRL.DOMAIN_HEADER.size, 4)
        self.assertEqual(BOOTCTRL.SLOT.size, 52)
        self.assertEqual(BOOTCTRL.CRC_OFFSET, 4092)

    def test_redundant_records_survive_one_corrupt_copy(self):
        domains = {
            "nuttx": BOOTCTRL.initial_domain(self.image, self.empty),
            "amp": BOOTCTRL.initial_domain(self.empty, self.empty,
                                           enable_a=False),
        }
        record = BOOTCTRL.encode_record(7, domains)

        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / "bootctrl.bin"
            data = bytearray(record * 2)
            data[32] ^= 0x80
            image.write_bytes(data)
            copies = BOOTCTRL.read_copies(image)

        self.assertIn("error", copies[0])
        self.assertEqual(copies[1]["generation"], 7)
        self.assertEqual(copies[1]["domains"]["nuttx"]["active_slot"], "a")

    def test_newer_valid_generation_wins(self):
        domains = {
            "nuttx": BOOTCTRL.initial_domain(self.image, self.empty),
            "amp": BOOTCTRL.initial_domain(self.empty, self.empty,
                                           enable_a=False),
        }
        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / "bootctrl.bin"
            image.write_bytes(BOOTCTRL.encode_record(4, domains) +
                              BOOTCTRL.encode_record(5, domains))
            copies = BOOTCTRL.read_copies(image)
        selected = max(copies, key=lambda copy: copy["generation"])
        self.assertEqual(selected["copy"], 1)

    def test_supplied_b_slot_is_enabled_at_lower_priority(self):
        domain = BOOTCTRL.initial_domain(self.image, self.image,
                                         enable_a=True, enable_b=True)
        self.assertEqual(domain["slots"][0]["priority"], 15)
        self.assertEqual(domain["slots"][1]["priority"], 14)
        self.assertEqual(domain["slots"][1]["successful"], 1)

    def test_empty_amp_domain_has_no_bootable_slot(self):
        domain = BOOTCTRL.initial_domain(self.empty, self.empty,
                                         enable_a=False, enable_b=False)
        self.assertEqual([slot["priority"] for slot in domain["slots"]],
                         [0, 0])
        self.assertEqual([slot["successful"] for slot in domain["slots"]],
                         [0, 0])


if __name__ == "__main__":
    unittest.main()
