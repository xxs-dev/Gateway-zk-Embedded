"""Offline catalog checks against examples and model distinctions in V1.7."""

import struct
import unittest

import build_hgm6110can_point_catalog as catalog


class CatalogTests(unittest.TestCase):
    def test_coverage_and_disabled_defaults(self):
        catalog.validate()
        self.assertEqual(len(catalog.POINTS), 336)

    def test_document_battery_example(self):
        payload = bytes.fromhex("01 12 00 00")
        battery, dplus = struct.unpack(">HH", payload)
        self.assertAlmostEqual(battery * catalog.lookup(3, 24)["scale"], 27.4)
        self.assertEqual(dplus * catalog.lookup(3, 25)["scale"], 0)

    def test_document_coil_example_lsb_first(self):
        payload = bytes.fromhex("07 01 00 00 01")
        active = {i for i in range(40) if payload[i // 8] & (1 << (i % 8))}
        self.assertEqual(active, {0, 1, 2, 8, 32})
        self.assertEqual(catalog.lookup(1, 8)["name"], "紧急停机")
        self.assertEqual(catalog.lookup(1, 32)["name"], "输入口警告报警")

    def test_signed_and_multiword_types(self):
        self.assertEqual(catalog.lookup(3, 17)["dataType"], "uint16")
        self.assertEqual(catalog.lookup(3, 26)["dataType"], "int16")
        self.assertEqual(catalog.lookup(3, 68)["length"], 2)
        self.assertEqual(catalog.lookup(3, 86)["length"], 2)
        power = catalog.lookup(3, 2506)
        self.assertEqual((power["dataType"], power["length"], power["byteOrder"]), ("int32", 2, "ABCD"))
        self.assertAlmostEqual(struct.unpack(">i", bytes.fromhex("ff ff ff 85"))[0] * power["scale"], -12.3)

    def test_can_not_n_and_6110_not_6120(self):
        self.assertEqual(catalog.lookup(1, 62)["name"], "燃油1输出控制")
        self.assertEqual(catalog.lookup(1, 63)["name"], "漏液警告")
        self.assertEqual(catalog.lookup(1, 79)["name"], "ECU警告")
        self.assertIn("SPN低16位", catalog.lookup(3, 98)["name"])
        self.assertEqual(catalog.lookup(5, 5)["name"], "发电分闸")
        self.assertEqual(catalog.lookup(5, 6)["name"], "发电合闸")
        self.assertEqual(catalog.lookup(5, 3)["feedbackReference"]["address"], 41)
        self.assertNotIn("feedbackReference", catalog.lookup(5, 0))

    def test_ambiguities_remain_visible(self):
        for address in [6, 13, 32, 55, 161, 175, 177, 179, 202, 218, 219]:
            self.assertTrue(catalog.lookup(3, address)["verificationRequired"])
        for address in [32, 161]:
            self.assertIsNone(catalog.lookup(3, address)["scale"])
            self.assertTrue(catalog.lookup(3, address)["rawOnly"])
        self.assertEqual(catalog.lookup(3, 160)["unit"], "Km/L")
        self.assertEqual(catalog.lookup(3, 24)["invalidValueRule"], "raw32766")

    def test_chinese_state_labels_and_conditional_enums(self):
        self.assertEqual(catalog.lookup(3, 34)["valueMap"]["9"], "正常运行")
        self.assertEqual(catalog.lookup(3, 36)["valueMap"]["2"], "无延时")
        self.assertEqual(catalog.lookup(3, 181)["valueMap"]["7"], "存在报警，无图标显示")
        profiles = catalog.lookup(3, 177)["enumProfiles"]
        self.assertEqual(next(p for p in profiles if p["ecuTypes"] == [52])["valueMap"]["2"], "图标常亮")
        self.assertEqual(next(p for p in profiles if p["ecuTypes"] == [58, 61])["valueMap"]["2"], "图标慢闪烁")


if __name__ == "__main__":
    unittest.main()
