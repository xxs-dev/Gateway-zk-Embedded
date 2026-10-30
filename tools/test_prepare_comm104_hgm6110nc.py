import json
from pathlib import Path
import tempfile
import unittest

from prepare_comm104_hgm6110nc import build, check_baseline, INDEX_BASE, N_COILS


class GeneratorCandidateTests(unittest.TestCase):
    def setUp(self):
        self.config, self.plan = build()
        self.points = self.config["meters"][0]["points"]
        self.by_address = {(p["read"]["function"], p["address"]): p for p in self.points}

    def test_disabled_and_no_write_paths(self):
        self.assertFalse(self.config["enabled"])
        self.assertFalse(self.config["meters"][0]["enabled"])
        self.assertFalse(self.config["northboundServer"]["enabled"])
        self.assertTrue(all(p["write"] == {"enable": False} for p in self.points))
        self.assertTrue(all(p["read"]["function"] in (0, 1, 3) for p in self.points))

    def test_n_coil_differences(self):
        for address, name in N_COILS.items():
            p = self.by_address[(1, address)]
            self.assertEqual(p["name"], name)
            self.assertEqual(p["valueMap"]["1"], name)
        self.assertNotIn((1, 5), self.by_address)
        self.assertNotIn((1, 55), self.by_address)
        self.assertNotIn((1, 80), self.by_address)

    def test_no_can_only_registers(self):
        for address in [32, 33, *range(56, 71), *range(82, 92), *range(111, 2500)]:
            self.assertNotIn((3, address), self.by_address)
        self.assertIn("油位", self.by_address[(3, 98)]["name"])
        self.assertEqual(self.by_address[(3, 98)]["read"]["dataType"], "int16")

    def test_transport_and_unique_ids(self):
        t = self.config["protocol"]["transport"]
        self.assertEqual((t["serialPort"], t["baudRate"], self.config["protocol"]["slave"]),
                         ("/dev/ttySP6", 9600, 1))
        self.assertGreater(t["timeoutMs"], 200)
        self.assertLessEqual(self.config["collect"]["maxRequestRegisters"], 120)
        for key in ("index", "pointCode"):
            self.assertEqual(len({p[key] for p in self.points}), len(self.points))

    def test_frequency_not_pretending_verified_hz(self):
        for address in (6, 13):
            read = self.by_address[(3, address)]["read"]
            self.assertEqual((read["unit"], read["scale"]), ("raw", 1))
        self.assertIn("raw_32766_quality_filter_required", self.plan["requiredBeforeEnable"])

    def test_decimal_accumulators_not_binary32(self):
        for address in range(42, 50):
            self.assertEqual(self.by_address[(3, address)]["read"]["length"], 1)
        self.assertEqual(self.plan["derivedNotInstalled"][3]["expression"], "R48*10000+R49")
        for address in range(2500, 2512, 2):
            self.assertEqual(self.by_address[(3, address)]["read"]["length"], 2)

    def test_bindings_reference_existing_points(self):
        codes = {p["pointCode"] for p in self.points}
        for refs in self.plan["displayBindingsNotInstalled"].values():
            for code in refs if isinstance(refs, list) else [refs]:
                self.assertIn(code, codes)

    def test_baseline_collision_refusal(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                check_baseline(self.config, folder)
            path = Path(folder) / "device.json"
            for device in [dict(points=[dict(index=INDEX_BASE)]),
                           dict(protocol=dict(transport=dict(serialPort="/dev/ttySP6"))),
                           dict(memoryStore=dict(sharedMemoryName="gateway_point_store_ttySP6"))]:
                path.write_text(json.dumps(device), encoding="utf-8")
                with self.assertRaises(ValueError):
                    check_baseline(self.config, folder)
            path.write_text('{"points":[{"index":1}]}', encoding="utf-8")
            self.assertEqual(check_baseline(self.config, folder)["status"], "local-snapshot-only-not-live")


if __name__ == "__main__":
    unittest.main()
