#!/usr/bin/env python3
"""Regression tests for validate_ems_v2_routes.py."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
MODULE_PATH = TOOLS_DIR / "validate_ems_v2_routes.py"
SPEC = importlib.util.spec_from_file_location("validate_ems_v2_routes", MODULE_PATH)
VALIDATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(VALIDATOR)


EXPECTED_ACTIVE_EXTERNAL_INPUTS = {
    1030,
    1031,
    1032,
    1036,
    1037,
    1038,
    1039,
    1040,
    1041,
    1042,
    1043,
    1130,
    1131,
    1132,
    1136,
    1137,
    1138,
    1139,
    1140,
    1141,
    1142,
    1143,
    1399,
    1556,
    1557,
    1566,
    1570,
}
EXPECTED_ACTIVE_CONTROL_TARGETS = {1318, 1319, 1320, 1321, 1322, 1323}


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def virtual_point(index: int, point_code: str = "P", point_role: str = "role"):
    return {
        "index": index,
        "pointCode": point_code,
        "pointRole": point_role,
        "deviceType": "ems",
        "enabled": True,
        "write": {"enable": False},
        "tags": ["ems_virtual"],
    }


def graph_with_output(
    parameter_index: int,
    binding_index: int,
    point_code: str = "",
    semantic_role: str = "",
):
    return {
        "schemaVersion": "2.0.0",
        "graphCode": "fixture",
        "compile": {
            "virtualIndexStart": 700000,
            "virtualIndexEnd": 799999,
        },
        "nodes": [
            {
                "id": "output",
                "type": "formula",
                "enabled": True,
                "parameters": {"outputIndex": parameter_index},
                "ports": [
                    {
                        "id": "output",
                        "direction": "output",
                        "runtimePath": "/outputIndex",
                        "binding": {
                            "kind": "automatic",
                            "index": binding_index,
                            "pointCode": point_code,
                            "semanticRole": semantic_role,
                        },
                    }
                ],
            }
        ],
        "links": [],
    }


class FixtureProject:
    def __init__(self, root: Path, device_documents, graph):
        self.runtime = root / "runtime"
        device_references = []
        for position, document in enumerate(device_documents):
            name = "device_{}.json".format(position)
            write_json(self.runtime / "devices" / name, document)
            device_references.append(
                "/opt/modbus-gateway/config/runtime/devices/{}".format(name)
            )
        write_json(self.runtime / "logic" / "graph.json", graph)
        self.app_path = self.runtime / "apps" / "app.json"
        write_json(
            self.app_path,
            {
                "deviceConfigFiles": device_references,
                "computeEngine": {
                    "enabled": True,
                    "rules": [
                        {
                            "ruleCode": "fixture",
                            "enabled": True,
                            "script": {
                                "type": "graphEms",
                                "graphFile": "runtime/logic/graph.json",
                                "graphProfile": {},
                            },
                        }
                    ],
                },
            },
        )


def device_document(points):
    return {
        "deviceType": "ems",
        "meters": [
            {
                "meterCode": "EMS_CORE",
                "deviceType": "ems",
                "points": points,
            }
        ],
    }


class RouteValidatorFixtureTests(unittest.TestCase):
    def test_missing_virtual_output_is_always_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            project = FixtureProject(
                Path(directory),
                [device_document([])],
                graph_with_output(700001, 700001),
            )
            report = VALIDATOR.audit_app(project.app_path)
            self.assertEqual({700001}, report.missing_virtual_output_indexes)
            self.assertIn(
                "missing_virtual_output_route",
                {issue.code for issue in report.structural_issues},
            )
            self.assertEqual(1, report.exit_code(False))

    def test_missing_graph_output_consumed_by_another_node_is_internal_gap(self):
        with tempfile.TemporaryDirectory() as directory:
            graph = graph_with_output(700001, 700001)
            graph["nodes"].append(
                {
                    "id": "consumer",
                    "type": "formula",
                    "enabled": True,
                    "parameters": {
                        "inputs": [{"index": 700001}],
                        "outputIndex": 700002,
                    },
                    "ports": [
                        {
                            "id": "input",
                            "direction": "input",
                            "runtimePath": "/inputs/0/index",
                            "binding": {"kind": "point", "index": 700001},
                        },
                        {
                            "id": "output",
                            "direction": "output",
                            "runtimePath": "/outputIndex",
                            "binding": {"kind": "automatic", "index": 700002},
                        },
                    ],
                }
            )
            graph["links"].append(
                {
                    "id": "data",
                    "kind": "data",
                    "fromNodeId": "output",
                    "fromPortId": "output",
                    "toNodeId": "consumer",
                    "toPortId": "input",
                }
            )
            project = FixtureProject(
                Path(directory),
                [device_document([virtual_point(700002)])],
                graph,
            )
            report = VALIDATOR.audit_app(project.app_path)
            self.assertEqual({700001}, report.missing_virtual_output_indexes)
            self.assertEqual({700001}, report.missing_internal_input_indexes)

    def test_duplicate_global_index_is_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            project = FixtureProject(
                Path(directory),
                [
                    device_document([virtual_point(700001, "A")]),
                    device_document([virtual_point(700001, "B")]),
                ],
                graph_with_output(700001, 700001),
            )
            report = VALIDATOR.audit_app(project.app_path)
            self.assertIn(
                "duplicate_global_index",
                {issue.code for issue in report.structural_issues},
            )

    def test_binding_index_must_match_runtime_parameter(self):
        with tempfile.TemporaryDirectory() as directory:
            project = FixtureProject(
                Path(directory),
                [device_document([virtual_point(700001)])],
                graph_with_output(700002, 700001),
            )
            report = VALIDATOR.audit_app(project.app_path)
            self.assertIn(
                "binding_parameter_mismatch",
                {issue.code for issue in report.structural_issues},
            )

    def test_declared_binding_metadata_must_match_route(self):
        with tempfile.TemporaryDirectory() as directory:
            project = FixtureProject(
                Path(directory),
                [device_document([virtual_point(700001, "ACTUAL", "actual.role")])],
                graph_with_output(700001, 700001, "WRONG", "wrong.role"),
            )
            report = VALIDATOR.audit_app(project.app_path)
            codes = {issue.code for issue in report.structural_issues}
            self.assertIn("binding_point_code_mismatch", codes)
            self.assertIn("binding_semantic_role_mismatch", codes)

    def test_control_target_cannot_be_masked_by_virtual_route(self):
        with tempfile.TemporaryDirectory() as directory:
            point = virtual_point(1200)
            point["write"]["enable"] = True
            graph = {
                "schemaVersion": "2.0.0",
                "graphCode": "target-fixture",
                "nodes": [
                    {
                        "id": "control",
                        "type": "controlWrite",
                        "enabled": True,
                        "parameters": {"targetIndex": 1200},
                        "ports": [
                            {
                                "id": "target",
                                "direction": "target",
                                "runtimePath": "/targetIndex",
                                "binding": {"kind": "point", "index": 1200},
                            }
                        ],
                    }
                ],
                "links": [],
            }
            project = FixtureProject(
                Path(directory),
                [device_document([point])],
                graph,
            )
            report = VALIDATOR.audit_app(project.app_path)
            self.assertIn(
                "virtual_control_target_route",
                {issue.code for issue in report.structural_issues},
            )


class FactoryRouteClosureTests(unittest.TestCase):
    def test_factory_graph_has_virtual_closure_but_requires_project_points(self):
        app_path = (
            REPO_ROOT
            / "config"
            / "factory"
            / "runtime"
            / "apps"
            / "mqtt-service.json"
        )
        report = VALIDATOR.audit_app(app_path)

        self.assertEqual(set(), report.missing_virtual_output_indexes)
        self.assertEqual(set(), report.missing_internal_input_indexes)
        self.assertEqual(
            EXPECTED_ACTIVE_EXTERNAL_INPUTS,
            report.active_missing_external_input_indexes,
        )
        self.assertEqual(
            EXPECTED_ACTIVE_CONTROL_TARGETS,
            report.active_missing_control_target_indexes,
        )
        self.assertEqual([], report.structural_issues)
        self.assertEqual(0, report.exit_code(False))
        self.assertEqual(1, report.exit_code(True))

    def test_generated_graph_and_virtual_artifacts_are_synchronized(self):
        graph_paths = [
            REPO_ROOT / "config" / "examples" / "shuntong_ems_graph.json",
            REPO_ROOT / "config" / "examples" / "shuntong_ems_modular_graph.json",
            REPO_ROOT
            / "config"
            / "factory"
            / "runtime"
            / "logic"
            / "shuntong_ems_graph.json",
        ]
        virtual_paths = [
            REPO_ROOT
            / "config"
            / "examples"
            / "device_ems_modular_virtual.json",
            REPO_ROOT
            / "config"
            / "factory"
            / "runtime"
            / "devices"
            / "device_ems_virtual.json",
        ]
        self.assertEqual(1, len({path.read_bytes() for path in graph_paths}))
        self.assertEqual(1, len({path.read_bytes() for path in virtual_paths}))

        virtual_document = json.loads(virtual_paths[0].read_text(encoding="utf-8"))
        points = {
            int(point["index"]): point
            for meter in virtual_document["meters"]
            for point in meter["points"]
        }
        self.assertEqual(631, len(points))
        self.assertEqual(set(range(217, 226)), set(points).intersection(range(217, 226)))
        self.assertEqual("H_TQ_avg_SA", points[217]["pointCode"])
        self.assertEqual("kVA", points[217]["read"]["unit"])
        self.assertEqual("H_TQ_avg_P_BPH", points[225]["pointCode"])
        self.assertEqual("%", points[225]["read"]["unit"])


if __name__ == "__main__":
    unittest.main()
