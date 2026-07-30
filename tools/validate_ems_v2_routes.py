#!/usr/bin/env python3
"""Validate EMS V2 graph bindings against app-referenced point routes."""

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple


RUNTIME_PREFIX = "/opt/modbus-gateway/config/runtime/"
CONFIG_PREFIX = "/opt/modbus-gateway/config/"
PROFILE_KEYS = ("profileKey", "profileKey2", "profileKey3", "profileKey4")
OPTIONAL_PROFILE_KEYS = (
    "optionalProfileKey",
    "optionalProfileKey2",
    "optionalProfileKey3",
    "optionalProfileKey4",
)
DISABLED_PROFILE_KEYS = (
    "profileDisabledKey",
    "profileDisabledKey2",
    "profileDisabledKey3",
    "profileDisabledKey4",
)
PROFILE_INT_VALUES = (
    "profileIntValue",
    "profileIntValue2",
    "profileIntValue3",
    "profileIntValue4",
)


@dataclass(frozen=True)
class Route:
    index: int
    source_file: str
    meter_code: str
    point_code: str
    point_role: str
    device_type: str
    enabled: bool
    writable: bool
    tags: Tuple[str, ...]

    @property
    def is_ems_virtual(self) -> bool:
        return self.device_type.lower() == "ems" or "ems_virtual" in {
            tag.lower() for tag in self.tags
        }

    @property
    def context(self) -> str:
        return "{}:{}:{}".format(
            self.source_file,
            self.meter_code or "<meter>",
            self.point_code or "<point>",
        )


@dataclass(frozen=True)
class Reference:
    index: int
    direction: str
    graph_file: str
    rule_code: str
    node_id: str
    port_id: str
    runtime_path: str
    point_code: str
    semantic_role: str
    node_enabled: bool
    active: bool

    @property
    def context(self) -> str:
        return "{}:{}:{}/{}".format(
            self.graph_file,
            self.rule_code,
            self.node_id,
            self.port_id,
        )


@dataclass(frozen=True)
class Issue:
    code: str
    scope: str
    message: str
    index: Optional[int] = None
    contexts: Tuple[str, ...] = ()


@dataclass
class AuditReport:
    app_path: str
    device_files: List[str] = field(default_factory=list)
    graph_files: List[str] = field(default_factory=list)
    route_count: int = 0
    unique_route_count: int = 0
    enabled_graph_count: int = 0
    active_node_count: int = 0
    enabled_node_count: int = 0
    missing_virtual_output_indexes: Set[int] = field(default_factory=set)
    missing_internal_input_indexes: Set[int] = field(default_factory=set)
    active_missing_external_input_indexes: Set[int] = field(default_factory=set)
    active_missing_control_target_indexes: Set[int] = field(default_factory=set)
    active_readonly_control_target_indexes: Set[int] = field(default_factory=set)
    dormant_missing_external_input_indexes: Set[int] = field(default_factory=set)
    dormant_missing_control_target_indexes: Set[int] = field(default_factory=set)
    issues: List[Issue] = field(default_factory=list)
    _issue_keys: Set[Tuple[Any, ...]] = field(default_factory=set, repr=False)

    def add_issue(
        self,
        code: str,
        scope: str,
        message: str,
        index: Optional[int] = None,
        contexts: Iterable[str] = (),
    ) -> None:
        normalized_contexts = tuple(sorted(set(contexts)))
        key = (code, scope, message, index, normalized_contexts)
        if key in self._issue_keys:
            return
        self._issue_keys.add(key)
        self.issues.append(Issue(code, scope, message, index, normalized_contexts))

    @property
    def structural_issues(self) -> List[Issue]:
        return [issue for issue in self.issues if issue.scope == "structural"]

    @property
    def project_issues(self) -> List[Issue]:
        return [issue for issue in self.issues if issue.scope == "project"]

    @property
    def potential_issues(self) -> List[Issue]:
        return [issue for issue in self.issues if issue.scope == "potential"]

    def exit_code(self, strict_project_routes: bool) -> int:
        if self.structural_issues:
            return 1
        if strict_project_routes and self.project_issues:
            return 1
        return 0

    def to_dict(self, strict_project_routes: bool) -> Dict[str, Any]:
        def issue_dict(issue: Issue) -> Dict[str, Any]:
            severity = "error" if (
                issue.scope == "structural"
                or (strict_project_routes and issue.scope == "project")
            ) else ("warning" if issue.scope == "project" else "info")
            return {
                "code": issue.code,
                "scope": issue.scope,
                "severity": severity,
                "index": issue.index,
                "message": issue.message,
                "contexts": list(issue.contexts),
            }

        return {
            "app": self.app_path,
            "deviceFiles": self.device_files,
            "graphFiles": self.graph_files,
            "summary": {
                "routes": self.route_count,
                "uniqueRoutes": self.unique_route_count,
                "enabledGraphs": self.enabled_graph_count,
                "enabledNodes": self.enabled_node_count,
                "activeNodes": self.active_node_count,
                "structuralErrors": len(self.structural_issues),
                "projectRouteErrors": len(self.project_issues),
                "potentialProfileDependencies": len(self.potential_issues),
                "strictProjectRoutes": strict_project_routes,
                "exitCode": self.exit_code(strict_project_routes),
            },
            "gaps": {
                "virtualOutputs": sorted(self.missing_virtual_output_indexes),
                "internalInputs": sorted(self.missing_internal_input_indexes),
                "activeExternalInputs": sorted(
                    self.active_missing_external_input_indexes
                ),
                "activeControlTargets": sorted(
                    self.active_missing_control_target_indexes
                ),
                "activeReadonlyControlTargets": sorted(
                    self.active_readonly_control_target_indexes
                ),
                "dormantExternalInputs": sorted(
                    self.dormant_missing_external_input_indexes
                ),
                "dormantControlTargets": sorted(
                    self.dormant_missing_control_target_indexes
                ),
            },
            "issues": [issue_dict(issue) for issue in self.issues],
        }


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def bool_value(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    normalized = str(value).strip().lower()
    if normalized in ("true", "yes", "on"):
        return True
    if normalized in ("false", "no", "off", ""):
        return False
    try:
        return float(normalized) != 0
    except ValueError:
        return False


def strict_positive_index(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if value > 0 else None
    if isinstance(value, float):
        return int(value) if value > 0 and value.is_integer() else None
    text = str(value).strip()
    if not text or not text.isdigit():
        return None
    parsed = int(text)
    return parsed if parsed > 0 else None


def infer_runtime_root(app_path: Path) -> Path:
    if app_path.parent.name.lower() == "apps":
        return app_path.parent.parent
    return app_path.parent


def resolve_reference(reference: str, runtime_root: Path, app_path: Path) -> Path:
    normalized = str(reference or "").replace("\\", "/").strip()
    if normalized.startswith(RUNTIME_PREFIX):
        return runtime_root.joinpath(normalized[len(RUNTIME_PREFIX):]).resolve()
    if normalized.startswith(CONFIG_PREFIX):
        return runtime_root.parent.joinpath(normalized[len(CONFIG_PREFIX):]).resolve()
    candidate = Path(normalized)
    if candidate.is_absolute():
        return candidate.resolve()
    if normalized.startswith("runtime/"):
        return runtime_root.parent.joinpath(normalized).resolve()
    if normalized.startswith(("apps/", "devices/", "logic/", "tls/")):
        return runtime_root.joinpath(normalized).resolve()
    return app_path.parent.joinpath(normalized).resolve()


def iter_meter_points(
    value: Any,
    inherited: Optional[Dict[str, str]] = None,
) -> Iterable[Tuple[Dict[str, str], Dict[str, Any]]]:
    inherited = dict(inherited or {})
    if isinstance(value, dict):
        current = dict(inherited)
        for key in ("meterCode", "deviceCode", "deviceType"):
            if value.get(key) not in (None, ""):
                current[key] = str(value[key])
        points = value.get("points")
        if isinstance(points, list):
            for point in points:
                if isinstance(point, dict):
                    yield current, point
        for key, child in value.items():
            if key != "points":
                yield from iter_meter_points(child, current)
    elif isinstance(value, list):
        for child in value:
            yield from iter_meter_points(child, inherited)


def decode_pointer_token(token: str) -> str:
    return token.replace("~1", "/").replace("~0", "~")


def resolve_json_pointer(value: Any, pointer: str) -> Tuple[bool, Any]:
    if not isinstance(pointer, str) or not pointer.startswith("/"):
        return False, None
    current = value
    for encoded in pointer[1:].split("/"):
        token = decode_pointer_token(encoded)
        if isinstance(current, dict):
            if token not in current:
                return False, None
            current = current[token]
        elif isinstance(current, list):
            if not token.isdigit():
                return False, None
            position = int(token)
            if position >= len(current):
                return False, None
            current = current[position]
        else:
            return False, None
    return True, current


def profile_int(profile: Dict[str, Any], key: str) -> int:
    try:
        return int(str(profile.get(key, 0)).strip())
    except (TypeError, ValueError):
        return 0


def node_is_active(node: Dict[str, Any], profile: Dict[str, Any]) -> bool:
    if not bool_value(node.get("enabled"), True):
        return False
    parameters = node.get("parameters")
    if not isinstance(parameters, dict):
        return False
    for field_name in PROFILE_KEYS:
        key = str(parameters.get(field_name) or "").strip()
        if key and not bool_value(profile.get(key), True):
            return False
    for field_name in OPTIONAL_PROFILE_KEYS:
        key = str(parameters.get(field_name) or "").strip()
        if key and not bool_value(profile.get(key), False):
            return False
    for field_name in DISABLED_PROFILE_KEYS:
        key = str(parameters.get(field_name) or "").strip()
        if key and bool_value(profile.get(key), False):
            return False
    int_key = str(parameters.get("profileIntKey") or "").strip()
    if int_key:
        accepted = []
        for field_name in PROFILE_INT_VALUES:
            if field_name not in parameters:
                continue
            try:
                accepted.append(int(str(parameters[field_name]).strip()))
            except (TypeError, ValueError):
                return False
        if profile_int(profile, int_key) not in accepted:
            return False
    return True


def collect_routes(
    app: Dict[str, Any],
    app_path: Path,
    runtime_root: Path,
    report: AuditReport,
) -> Dict[int, List[Route]]:
    routes: Dict[int, List[Route]] = {}
    configured_files = app.get("deviceConfigFiles") or []
    if not isinstance(configured_files, list):
        report.add_issue(
            "invalid_device_config_files",
            "structural",
            "deviceConfigFiles must be an array",
        )
        return routes

    for configured_file in configured_files:
        path = resolve_reference(str(configured_file), runtime_root, app_path)
        report.device_files.append(str(path))
        if not path.is_file():
            report.add_issue(
                "missing_device_config",
                "structural",
                "app-referenced device config does not exist: {}".format(path),
            )
            continue
        try:
            document = read_json(path)
        except (OSError, ValueError) as exc:
            report.add_issue(
                "invalid_device_config",
                "structural",
                "cannot parse device config {}: {}".format(path, exc),
            )
            continue
        for metadata, point in iter_meter_points(document):
            index = strict_positive_index(point.get("index"))
            if index is None:
                report.add_issue(
                    "invalid_route_index",
                    "structural",
                    "point route has a non-positive or non-integer index",
                    contexts=(str(path),),
                )
                continue
            tags_value = point.get("tags") or []
            tags = tuple(str(tag) for tag in tags_value) if isinstance(tags_value, list) else ()
            write = point.get("write") if isinstance(point.get("write"), dict) else {}
            route = Route(
                index=index,
                source_file=str(path),
                meter_code=str(metadata.get("meterCode") or ""),
                point_code=str(point.get("pointCode") or ""),
                point_role=str(point.get("pointRole") or ""),
                device_type=str(
                    point.get("deviceType") or metadata.get("deviceType") or ""
                ),
                enabled=bool_value(point.get("enabled"), True),
                writable=bool_value(write.get("enable"), False),
                tags=tags,
            )
            routes.setdefault(index, []).append(route)
            report.route_count += 1

    report.unique_route_count = len(routes)
    for index, candidates in sorted(routes.items()):
        if len(candidates) > 1:
            report.add_issue(
                "duplicate_global_index",
                "structural",
                "global point index is defined by multiple app-referenced routes",
                index=index,
                contexts=(route.context for route in candidates),
            )
    return routes


def validate_binding(
    binding: Any,
    parameters: Dict[str, Any],
    runtime_path: str,
    direction: str,
    context: str,
    report: AuditReport,
) -> Tuple[Optional[int], str, str]:
    if not isinstance(binding, dict):
        report.add_issue(
            "missing_binding",
            "structural",
            "V2 port binding must be an object",
            contexts=(context,),
        )
        return None, "", ""
    kind = str(binding.get("kind") or "").strip().lower()
    if kind not in ("point", "automatic", "constant"):
        report.add_issue(
            "invalid_binding_kind",
            "structural",
            "unsupported V2 binding kind: {}".format(kind or "<empty>"),
            contexts=(context,),
        )
        return None, "", ""
    found, parameter_value = resolve_json_pointer(parameters, runtime_path)
    if not found:
        report.add_issue(
            "invalid_runtime_path",
            "structural",
            "runtimePath does not resolve inside node.parameters: {}".format(runtime_path),
            contexts=(context,),
        )
        return None, "", ""

    point_code = str(binding.get("pointCode") or "")
    semantic_role = str(binding.get("semanticRole") or "")
    if kind == "constant":
        if direction != "input":
            report.add_issue(
                "constant_output_binding",
                "structural",
                "output/target ports cannot use constant bindings",
                contexts=(context,),
            )
        if binding.get("constant") is None or binding.get("index") is not None:
            report.add_issue(
                "invalid_constant_binding",
                "structural",
                "constant binding must contain only a non-null scalar constant",
                contexts=(context,),
            )
        elif parameter_value != binding.get("constant"):
            report.add_issue(
                "binding_parameter_mismatch",
                "structural",
                "binding.constant differs from node.parameters at runtimePath",
                contexts=(context,),
            )
        return None, point_code, semantic_role

    index = strict_positive_index(binding.get("index"))
    if index is None:
        report.add_issue(
            "unmaterialized_binding",
            "structural",
            "point/automatic binding requires a positive materialized index",
            contexts=(context,),
        )
        return None, point_code, semantic_role
    parameter_index = strict_positive_index(parameter_value)
    if parameter_index != index:
        report.add_issue(
            "binding_parameter_mismatch",
            "structural",
            "binding.index differs from node.parameters at runtimePath",
            index=index,
            contexts=(context,),
        )
    if binding.get("constant") is not None:
        report.add_issue(
            "mixed_binding_value",
            "structural",
            "point/automatic binding cannot also contain a constant",
            index=index,
            contexts=(context,),
        )
    return index, point_code, semantic_role


def collect_graph_references(
    graph: Dict[str, Any],
    graph_path: Path,
    rule_code: str,
    profile: Dict[str, Any],
    report: AuditReport,
) -> List[Reference]:
    schema_version = str(graph.get("schemaVersion") or "")
    if not schema_version.startswith("2."):
        report.add_issue(
            "non_v2_graph",
            "structural",
            "enabled graph must use schemaVersion 2.x: {}".format(schema_version or "<empty>"),
            contexts=(str(graph_path),),
        )
        return []
    nodes = graph.get("nodes")
    if not isinstance(nodes, list):
        report.add_issue(
            "invalid_graph_nodes",
            "structural",
            "V2 graph nodes must be an array",
            contexts=(str(graph_path),),
        )
        return []

    references: List[Reference] = []
    port_lookup: Dict[Tuple[str, str], Tuple[str, Optional[int], str]] = {}
    seen_node_ids: Set[str] = set()
    for node in nodes:
        if not isinstance(node, dict):
            report.add_issue(
                "invalid_graph_node",
                "structural",
                "V2 graph contains a non-object node",
                contexts=(str(graph_path),),
            )
            continue
        node_id = str(node.get("id") or "")
        if not node_id or node_id in seen_node_ids:
            report.add_issue(
                "duplicate_or_empty_node_id",
                "structural",
                "V2 graph node id is empty or duplicated: {}".format(node_id or "<empty>"),
                contexts=(str(graph_path),),
            )
        seen_node_ids.add(node_id)
        node_enabled = bool_value(node.get("enabled"), True)
        active = node_is_active(node, profile)
        if node_enabled:
            report.enabled_node_count += 1
        if active:
            report.active_node_count += 1
        parameters = node.get("parameters")
        if not isinstance(parameters, dict):
            report.add_issue(
                "invalid_node_parameters",
                "structural",
                "V2 node.parameters must be an object",
                contexts=("{}:{}".format(graph_path, node_id),),
            )
            continue
        ports = node.get("ports")
        if not isinstance(ports, list):
            report.add_issue(
                "invalid_node_ports",
                "structural",
                "V2 node.ports must be an array",
                contexts=("{}:{}".format(graph_path, node_id),),
            )
            continue
        seen_port_ids: Set[str] = set()
        seen_runtime_paths: Set[str] = set()
        for port in ports:
            if not isinstance(port, dict):
                report.add_issue(
                    "invalid_graph_port",
                    "structural",
                    "V2 graph contains a non-object port",
                    contexts=("{}:{}".format(graph_path, node_id),),
                )
                continue
            port_id = str(port.get("id") or "")
            context = "{}:{}:{}/{}".format(graph_path, rule_code, node_id, port_id)
            if not port_id or port_id in seen_port_ids:
                report.add_issue(
                    "duplicate_or_empty_port_id",
                    "structural",
                    "node port id is empty or duplicated: {}".format(port_id or "<empty>"),
                    contexts=(context,),
                )
            seen_port_ids.add(port_id)
            direction = str(port.get("direction") or "").strip().lower()
            if direction not in ("input", "output", "target"):
                report.add_issue(
                    "invalid_port_direction",
                    "structural",
                    "unsupported port direction: {}".format(direction or "<empty>"),
                    contexts=(context,),
                )
                continue
            runtime_path = str(port.get("runtimePath") or "")
            if runtime_path in seen_runtime_paths:
                report.add_issue(
                    "duplicate_runtime_path",
                    "structural",
                    "node contains duplicate port runtimePath values",
                    contexts=(context,),
                )
            seen_runtime_paths.add(runtime_path)
            index, point_code, semantic_role = validate_binding(
                port.get("binding"),
                parameters,
                runtime_path,
                direction,
                context,
                report,
            )
            port_lookup[(node_id, port_id)] = (direction, index, context)
            if index is not None:
                references.append(
                    Reference(
                        index=index,
                        direction=direction,
                        graph_file=str(graph_path),
                        rule_code=rule_code,
                        node_id=node_id,
                        port_id=port_id,
                        runtime_path=runtime_path,
                        point_code=point_code,
                        semantic_role=semantic_role,
                        node_enabled=node_enabled,
                        active=active,
                    )
                )

    links = graph.get("links") or []
    if not isinstance(links, list):
        report.add_issue(
            "invalid_graph_links",
            "structural",
            "V2 graph links must be an array",
            contexts=(str(graph_path),),
        )
        return references
    for link in links:
        if not isinstance(link, dict) or str(link.get("kind") or "").lower() != "data":
            continue
        source_key = (str(link.get("fromNodeId") or ""), str(link.get("fromPortId") or ""))
        target_key = (str(link.get("toNodeId") or ""), str(link.get("toPortId") or ""))
        source = port_lookup.get(source_key)
        target = port_lookup.get(target_key)
        link_context = "{}:link:{}".format(graph_path, link.get("id") or "<empty>")
        if source is None or target is None:
            report.add_issue(
                "unknown_link_port",
                "structural",
                "data link references an unknown port",
                contexts=(link_context,),
            )
            continue
        if source[0] != "output" or target[0] != "input":
            report.add_issue(
                "invalid_link_direction",
                "structural",
                "data link must connect output to input",
                contexts=(link_context, source[2], target[2]),
            )
        if source[1] is None or target[1] is None or source[1] != target[1]:
            report.add_issue(
                "link_binding_mismatch",
                "structural",
                "data link source and target binding indexes differ",
                contexts=(link_context, source[2], target[2]),
            )
    return references


def collect_enabled_graphs(
    app: Dict[str, Any],
    app_path: Path,
    runtime_root: Path,
    report: AuditReport,
) -> List[Reference]:
    compute = app.get("computeEngine")
    if not isinstance(compute, dict) or not bool_value(compute.get("enabled"), False):
        return []
    rules = compute.get("rules") or []
    if not isinstance(rules, list):
        report.add_issue(
            "invalid_compute_rules",
            "structural",
            "computeEngine.rules must be an array",
        )
        return []

    references: List[Reference] = []
    for position, rule in enumerate(rules):
        if not isinstance(rule, dict) or not bool_value(rule.get("enabled"), True):
            continue
        script = rule.get("script")
        if not isinstance(script, dict) or str(script.get("type") or "").lower() != "graphems":
            continue
        rule_code = str(rule.get("ruleCode") or "rule_{}".format(position))
        graph_reference = str(script.get("graphFile") or "")
        if not graph_reference:
            report.add_issue(
                "missing_graph_reference",
                "structural",
                "enabled graphEms rule has no graphFile",
                contexts=(rule_code,),
            )
            continue
        graph_path = resolve_reference(graph_reference, runtime_root, app_path)
        report.graph_files.append(str(graph_path))
        report.enabled_graph_count += 1
        if not graph_path.is_file():
            report.add_issue(
                "missing_graph_file",
                "structural",
                "enabled V2 graph file does not exist: {}".format(graph_path),
                contexts=(rule_code,),
            )
            continue
        try:
            graph = read_json(graph_path)
        except (OSError, ValueError) as exc:
            report.add_issue(
                "invalid_graph_file",
                "structural",
                "cannot parse graph {}: {}".format(graph_path, exc),
                contexts=(rule_code,),
            )
            continue
        if not isinstance(graph, dict):
            report.add_issue(
                "invalid_graph_root",
                "structural",
                "V2 graph root must be an object",
                contexts=(str(graph_path),),
            )
            continue
        profile = script.get("graphProfile")
        if not isinstance(profile, dict):
            profile = {}
        references.extend(
            collect_graph_references(
                graph,
                graph_path,
                rule_code,
                profile,
                report,
            )
        )
    report.graph_files = sorted(set(report.graph_files))
    return references


def unique_route(routes: Dict[int, List[Route]], index: int) -> Optional[Route]:
    candidates = routes.get(index) or []
    if len(candidates) != 1 or not candidates[0].enabled:
        return None
    return candidates[0]


def reference_contexts(references: Iterable[Reference]) -> Tuple[str, ...]:
    return tuple(reference.context for reference in references)


def validate_route_closure(
    routes: Dict[int, List[Route]],
    references: Sequence[Reference],
    report: AuditReport,
) -> None:
    enabled_references = [reference for reference in references if reference.node_enabled]
    output_references: Dict[int, List[Reference]] = {}
    input_references: Dict[int, List[Reference]] = {}
    target_references: Dict[int, List[Reference]] = {}
    for reference in enabled_references:
        destination = {
            "input": input_references,
            "output": output_references,
            "target": target_references,
        }[reference.direction]
        destination.setdefault(reference.index, []).append(reference)

    output_indexes = set(output_references)
    for index, candidates in sorted(output_references.items()):
        route = unique_route(routes, index)
        internal_consumers = input_references.get(index) or []
        if route is None:
            report.missing_virtual_output_indexes.add(index)
            if internal_consumers:
                report.missing_internal_input_indexes.add(index)
            report.add_issue(
                "missing_virtual_output_route",
                "structural",
                "graph output has no unique enabled EMS virtual route{}".format(
                    " and is consumed internally" if internal_consumers else ""
                ),
                index=index,
                contexts=reference_contexts(candidates + internal_consumers),
            )
        elif not route.is_ems_virtual:
            report.add_issue(
                "non_virtual_output_route",
                "structural",
                "graph output resolves to a physical/non-EMS route",
                index=index,
                contexts=(route.context,) + reference_contexts(candidates),
            )

    for reference in references:
        route_candidates = routes.get(reference.index) or []
        if len(route_candidates) != 1:
            continue
        route = route_candidates[0]
        if reference.point_code and reference.point_code != route.point_code:
            report.add_issue(
                "binding_point_code_mismatch",
                "structural",
                "binding.pointCode differs from the routed pointCode",
                index=reference.index,
                contexts=(reference.context, route.context),
            )
        if reference.semantic_role and reference.semantic_role != route.point_role:
            report.add_issue(
                "binding_semantic_role_mismatch",
                "structural",
                "binding.semanticRole differs from the routed pointRole",
                index=reference.index,
                contexts=(reference.context, route.context),
            )

    external_inputs = {
        index: candidates
        for index, candidates in input_references.items()
        if index not in output_indexes
    }
    for index, candidates in sorted(external_inputs.items()):
        active = [candidate for candidate in candidates if candidate.active]
        dormant = [candidate for candidate in candidates if not candidate.active]
        route = unique_route(routes, index)
        if route is not None:
            continue
        if active:
            report.active_missing_external_input_indexes.add(index)
            report.add_issue(
                "missing_external_input_route",
                "project",
                "active graph external input has no unique enabled project route",
                index=index,
                contexts=reference_contexts(active),
            )
        elif dormant:
            report.dormant_missing_external_input_indexes.add(index)
            report.add_issue(
                "dormant_external_input_dependency",
                "potential",
                "profile-disabled graph input will require a project route when enabled",
                index=index,
                contexts=reference_contexts(dormant),
            )

    for index, candidates in sorted(target_references.items()):
        active = [candidate for candidate in candidates if candidate.active]
        dormant = [candidate for candidate in candidates if not candidate.active]
        route = unique_route(routes, index)
        if route is not None and route.is_ems_virtual:
            report.add_issue(
                "virtual_control_target_route",
                "structural",
                "control target resolves to an EMS virtual route instead of a physical writable point",
                index=index,
                contexts=(route.context,) + reference_contexts(candidates),
            )
            continue
        if active:
            if route is None:
                report.active_missing_control_target_indexes.add(index)
                report.add_issue(
                    "missing_control_target_route",
                    "project",
                    "active control target has no unique enabled project route",
                    index=index,
                    contexts=reference_contexts(active),
                )
            elif not route.writable:
                report.active_readonly_control_target_indexes.add(index)
                report.add_issue(
                    "readonly_control_target_route",
                    "project",
                    "active control target route is not write-enabled",
                    index=index,
                    contexts=(route.context,) + reference_contexts(active),
                )
        elif dormant and route is None:
            report.dormant_missing_control_target_indexes.add(index)
            report.add_issue(
                "dormant_control_target_dependency",
                "potential",
                "profile-disabled control target will require a writable project route when enabled",
                index=index,
                contexts=reference_contexts(dormant),
            )


def audit_app(app_path: Path, runtime_root: Optional[Path] = None) -> AuditReport:
    app_path = app_path.resolve()
    report = AuditReport(app_path=str(app_path))
    if not app_path.is_file():
        report.add_issue(
            "missing_app_config",
            "structural",
            "app config does not exist: {}".format(app_path),
        )
        return report
    try:
        app = read_json(app_path)
    except (OSError, ValueError) as exc:
        report.add_issue(
            "invalid_app_config",
            "structural",
            "cannot parse app config {}: {}".format(app_path, exc),
        )
        return report
    if not isinstance(app, dict):
        report.add_issue(
            "invalid_app_root",
            "structural",
            "app config root must be an object",
        )
        return report
    effective_runtime_root = (
        runtime_root.resolve() if runtime_root is not None else infer_runtime_root(app_path)
    )
    routes = collect_routes(app, app_path, effective_runtime_root, report)
    references = collect_enabled_graphs(
        app,
        app_path,
        effective_runtime_root,
        report,
    )
    validate_route_closure(routes, references, report)
    report.device_files = sorted(set(report.device_files))
    return report


def format_indexes(values: Iterable[int]) -> str:
    items = sorted(values)
    return ",".join(str(item) for item in items) if items else "-"


def print_text_report(report: AuditReport, strict_project_routes: bool) -> None:
    print("EMS V2 route audit")
    print("app: {}".format(report.app_path))
    print(
        "routes: {} entries / {} unique; enabled graphs: {}; nodes: {} enabled / {} active".format(
            report.route_count,
            report.unique_route_count,
            report.enabled_graph_count,
            report.enabled_node_count,
            report.active_node_count,
        )
    )
    print("virtual output gaps: {}".format(format_indexes(report.missing_virtual_output_indexes)))
    print("internal input gaps: {}".format(format_indexes(report.missing_internal_input_indexes)))
    print(
        "active external input gaps: {}".format(
            format_indexes(report.active_missing_external_input_indexes)
        )
    )
    print(
        "active control target gaps: {}".format(
            format_indexes(report.active_missing_control_target_indexes)
        )
    )
    print(
        "active readonly targets: {}".format(
            format_indexes(report.active_readonly_control_target_indexes)
        )
    )
    print(
        "dormant profile dependencies: inputs={} targets={}".format(
            format_indexes(report.dormant_missing_external_input_indexes),
            format_indexes(report.dormant_missing_control_target_indexes),
        )
    )
    for issue in report.issues:
        severity = "ERROR" if (
            issue.scope == "structural"
            or (strict_project_routes and issue.scope == "project")
        ) else ("WARN" if issue.scope == "project" else "INFO")
        suffix = " index={}".format(issue.index) if issue.index is not None else ""
        print("[{}] {}{}: {}".format(severity, issue.code, suffix, issue.message))
        for context in issue.contexts:
            print("  - {}".format(context))
    print(
        "result: {} (structural={}, project={}, strictProjectRoutes={})".format(
            "PASS" if report.exit_code(strict_project_routes) == 0 else "FAIL",
            len(report.structural_issues),
            len(report.project_issues),
            str(strict_project_routes).lower(),
        )
    )


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate EMS V2 graph routes from an edge app config."
    )
    parser.add_argument(
        "--app",
        default="config/factory/runtime/apps/mqtt-service.json",
        help="app config to audit",
    )
    parser.add_argument(
        "--runtime-root",
        help="local directory corresponding to /opt/modbus-gateway/config/runtime",
    )
    parser.add_argument(
        "--strict-project-routes",
        action="store_true",
        help="fail when active external inputs or control targets are unresolved",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a machine-readable JSON report",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    runtime_root = Path(args.runtime_root) if args.runtime_root else None
    report = audit_app(Path(args.app), runtime_root)
    if args.json:
        json.dump(
            report.to_dict(args.strict_project_routes),
            sys.stdout,
            ensure_ascii=False,
            indent=2,
        )
        sys.stdout.write("\n")
    else:
        print_text_report(report, args.strict_project_routes)
    return report.exit_code(args.strict_project_routes)


if __name__ == "__main__":
    sys.exit(main())
