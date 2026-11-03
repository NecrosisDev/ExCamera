#!/usr/bin/env python3
"""Import, validate, and generate Exanima native knowledge records."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Iterable

try:
    from jsonschema import Draft202012Validator
except ImportError as exc:  # pragma: no cover - depends on local setup
    raise SystemExit("Install tools/requirements-kb.txt in the project Python environment") from exc

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_KNOWLEDGE = ROOT / "knowledge"


class KnowledgeError(Exception):
    """A knowledge import, validation, or generation error."""


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise KnowledgeError(f"{path}: {exc}") from exc


def write_json(path: Path, value: Any, *, overwrite: bool = True) -> None:
    if path.exists() and not overwrite:
        raise KnowledgeError(f"refusing to overwrite {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def envelope(path: Path) -> dict[str, Any]:
    value = read_json(path)
    evidence = value.get("structuredContent", value)
    if not isinstance(evidence, dict) or "normalized_result" not in evidence:
        raise KnowledgeError(f"{path}: not a REA Evidence record")
    return evidence


def address(value: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise KnowledgeError(f"invalid native address {value!r}")
    try:
        return int(value, 16)
    except ValueError as exc:
        raise KnowledgeError(f"invalid native address {value!r}") from exc


def hex_address(value: int) -> str:
    if value < 0:
        raise KnowledgeError("native address cannot be negative")
    return hex(value)


def profile_digest(evidence: dict[str, Any]) -> str | None:
    profile = evidence.get("analysis_profile") or {}
    digest = profile.get("digest")
    if isinstance(digest, str):
        return digest
    parameters = profile.get("parameters") or {}
    digest = parameters.get("digest")
    return digest if isinstance(digest, str) else None


def evidence_reference(evidence: dict[str, Any], kind: str) -> dict[str, Any]:
    evidence_id = evidence.get("evidence_id")
    provider = evidence.get("provider")
    if not isinstance(evidence_id, str) or not isinstance(provider, dict):
        raise KnowledgeError("Evidence is missing its identity or provider")
    return {
        "kind": kind,
        "evidence_id": evidence_id,
        "operation": evidence.get("operation"),
        "provider": provider,
        "analysis_profile_digest": profile_digest(evidence),
    }


def native_references(values: Any) -> list[dict[str, Any]]:
    result = []
    for item in values or []:
        if not isinstance(item, dict):
            continue
        procedure = item.get("procedure") if isinstance(item.get("procedure"), dict) else {}
        result.append({
            "address": item.get("address", procedure.get("address")),
            "name": item.get("name", procedure.get("name")),
        })
    return sorted(result, key=lambda item: (item["address"] or "", item["name"] or ""))


def import_rea(overview_path: Path, dossiers: Path, targets_path: Path,
               knowledge: Path, overwrite: bool = False) -> tuple[int, int]:
    overview = envelope(overview_path)
    summary = overview["normalized_result"]
    targets = read_json(targets_path)
    subject = overview.get("subject") or {}
    digest = (subject.get("digest") or {}).get("sha256")
    if not isinstance(digest, str):
        raise KnowledgeError("overview has no executable SHA-256")
    image_base = summary.get("image_base")
    if not isinstance(image_base, str):
        raise KnowledgeError("overview has no image base")
    base_address = address(image_base)
    executable_size = targets.get("executable_size")
    if not isinstance(executable_size, int) or executable_size < 1:
        raise KnowledgeError(f"{targets_path}: executable_size must be a positive integer")

    build_dir = knowledge / "builds" / digest
    build_record = {
        "schema_version": 1,
        "build_id": targets["build_id"],
        "game": targets["game"],
        "executable": {
            "sha256": digest,
            "size": executable_size,
            "format": "pe32-plus",
            "architecture": "x86_64",
            "image_base": image_base,
            "sections": [
                {key: section[key] for key in ("name", "start", "end", "length")}
                for section in summary.get("segments", [])
            ],
        },
        "analysis": {
            "procedure_count": summary.get("procedure_count", 0),
            "string_count": summary.get("string_count", 0),
            "toolchain": targets["toolchain"],
        },
        "support": {
            "status": "research_only",
            "release_enabled": False,
            "reason": "Static research imported; runtime attachment and gameplay acceptance have not been established.",
        },
        "evidence": [evidence_reference(overview, "rea_ghidra_binary_overview")],
    }
    write_json(build_dir / "build.json", build_record, overwrite=overwrite)

    count = 0
    for target in targets["functions"]:
        dossier_path = dossiers / f"{target['import_name']}.json"
        evidence = envelope(dossier_path)
        function_subject = evidence.get("subject") or {}
        function_digest = (function_subject.get("digest") or {}).get("sha256")
        if function_digest != digest:
            raise KnowledgeError(f"{dossier_path}: executable hash differs from overview")
        normalized = evidence["normalized_result"]
        procedure = normalized.get("procedure") or {}
        va = procedure.get("address")
        if va != target["address"]:
            raise KnowledgeError(f"{dossier_path}: expected {target['address']}, observed {va!r}")
        rva = address(va) - base_address
        record = {
            "schema_version": 1,
            "contract_id": target["contract_id"],
            "build_sha256": digest,
            "symbol": {
                "provider_name": procedure.get("name"),
                "rva": hex_address(rva),
                "va": va,
                "body": procedure.get("body"),
                "classification": procedure.get("classification"),
            },
            "role": {"summary": target["role_hypothesis"], "state": "hypothesis"},
            "abi": {
                "status": "unknown",
                "provider_signature": procedure.get("signature"),
                "calling_convention": None,
                "parameters": None,
                "return": None,
            },
            "thread": {"status": "unknown", "details": None},
            "memory": {"status": "unknown", "reads": None, "writes": None, "lifetime": None},
            "relationships": {
                "callers": native_references(normalized.get("callers")),
                "callees": native_references(normalized.get("callees")),
                "limitations": normalized.get("limitations") or [],
            },
            "evidence": [evidence_reference(evidence, "rea_ghidra_function_dossier")],
            "states": ["observed_static"],
            "research": {"questions": target["questions"]},
            "generation": {
                "runtime_eligible": False,
                "reason": "Imported static observations only; ABI, thread, memory, and runtime behavior require independent review.",
            },
        }
        write_json(build_dir / "functions" / f"{target['import_name']}.json", record, overwrite=overwrite)
        count += 1
    return 1, count


def schema_validator(path: Path) -> Draft202012Validator:
    schema = read_json(path)
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def schema_errors(validator: Draft202012Validator, value: Any) -> list[str]:
    errors = []
    for error in sorted(validator.iter_errors(value), key=lambda item: list(item.absolute_path)):
        location = ".".join(str(part) for part in error.absolute_path) or "<root>"
        errors.append(f"{location}: {error.message}")
    return errors


def load_knowledge(knowledge: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    builds, functions = [], []
    for build_path in sorted(knowledge.glob("builds/*/build.json")):
        build = read_json(build_path)
        build["_record_path"] = build_path
        builds.append(build)
    for function_path in sorted(knowledge.glob("builds/*/functions/*.json")):
        function = read_json(function_path)
        function["_record_path"] = function_path
        functions.append(function)
    if not builds:
        raise KnowledgeError(f"{knowledge}: no build records found")
    return builds, functions


def load_capabilities(knowledge: Path) -> list[dict[str, Any]]:
    capabilities = []
    for path in sorted(knowledge.glob("capabilities/*.json")):
        capability = read_json(path)
        capability["_record_path"] = path
        capabilities.append(capability)
    return capabilities


def validate_knowledge(knowledge: Path, schema_dir: Path | None = None) -> dict[str, int]:
    schema_dir = schema_dir or knowledge / "schema"
    build_validator = schema_validator(schema_dir / "build.schema.json")
    function_validator = schema_validator(schema_dir / "function.schema.json")
    capability_validator = schema_validator(schema_dir / "capability.schema.json")
    builds, functions = load_knowledge(knowledge)
    capabilities = load_capabilities(knowledge)
    errors: list[str] = []
    build_by_hash: dict[str, dict[str, Any]] = {}
    contract_ids: set[str] = set()
    capability_ids: set[str] = set()

    for build in builds:
        path = build["_record_path"]
        for issue in schema_errors(build_validator, {k: v for k, v in build.items() if k != "_record_path"}):
            errors.append(f"{path}: {issue}")
        digest = build.get("executable", {}).get("sha256")
        if digest in build_by_hash:
            errors.append(f"{path}: duplicate executable hash {digest}")
        build_by_hash[digest] = build
        if path.parent.name != digest:
            errors.append(f"{path}: directory name does not equal executable SHA-256")
        support = build.get("support", {})
        if support.get("release_enabled") and support.get("status") not in {"release_candidate", "supported"}:
            errors.append(f"{path}: release_enabled requires release_candidate or supported status")

    for function in functions:
        path = function["_record_path"]
        plain = {k: v for k, v in function.items() if k != "_record_path"}
        for issue in schema_errors(function_validator, plain):
            errors.append(f"{path}: {issue}")
        digest = function.get("build_sha256")
        build = build_by_hash.get(digest)
        if not build:
            errors.append(f"{path}: no build record for {digest}")
            continue
        if path.parent.parent.name != digest:
            errors.append(f"{path}: record is under the wrong executable hash")
        contract_id = function.get("contract_id")
        if contract_id in contract_ids:
            errors.append(f"{path}: duplicate contract ID {contract_id}")
        contract_ids.add(contract_id)
        try:
            image_base = address(build["executable"]["image_base"])
            va = address(function["symbol"]["va"])
            rva = address(function["symbol"]["rva"])
            if va - image_base != rva:
                errors.append(f"{path}: VA - image base does not equal RVA")
        except (KeyError, KnowledgeError) as exc:
            errors.append(f"{path}: {exc}")
        generation = function.get("generation", {})
        states = set(function.get("states", []))
        if generation.get("runtime_eligible") and not {"reviewed_static", "byte_validated"} <= states:
            errors.append(f"{path}: runtime_eligible requires reviewed_static and byte_validated")
        if function.get("abi", {}).get("status") == "unknown" and generation.get("runtime_eligible"):
            errors.append(f"{path}: unknown ABI cannot be runtime_eligible")

    for capability in capabilities:
        path = capability["_record_path"]
        plain = {k: v for k, v in capability.items() if k != "_record_path"}
        for issue in schema_errors(capability_validator, plain):
            errors.append(f"{path}: {issue}")
        capability_id = capability.get("capability_id")
        if capability_id in capability_ids:
            errors.append(f"{path}: duplicate capability ID {capability_id}")
        capability_ids.add(capability_id)
        if capability.get("build_sha256") not in build_by_hash:
            errors.append(f"{path}: no build record for {capability.get('build_sha256')}")
        required = set(capability.get("required_contracts", []))
        for contract_id in sorted(required - contract_ids):
            errors.append(f"{path}: missing native contract {contract_id}")
        adapter = capability.get("adapter", {})
        if adapter.get("hook_contract") not in required:
            errors.append(f"{path}: adapter hook_contract is not listed in required_contracts")
        for contract_id in adapter.get("helper_contracts", []):
            if contract_id not in required:
                errors.append(f"{path}: adapter helper {contract_id} is not listed in required_contracts")
        runtime = capability.get("runtime", {})
        acceptance = capability.get("acceptance", {})
        if runtime.get("eligible"):
            if "validated_runtime" not in capability.get("evidence_states", []):
                errors.append(f"{path}: eligible capability requires validated_runtime evidence")
            if runtime.get("module_faults") != 0 or runtime.get("restoration_failed"):
                errors.append(f"{path}: eligible capability cannot have faults or failed restoration")
            if not acceptance.get("clean_stop"):
                errors.append(f"{path}: eligible capability requires a clean-stop acceptance")

    if errors:
        raise KnowledgeError("knowledge validation failed:\n  " + "\n  ".join(errors))
    return {"builds": len(builds), "functions": len(functions), "capabilities": len(capabilities)}


def generated_documents(knowledge: Path) -> dict[str, str]:
    builds, functions = load_knowledge(knowledge)
    capabilities = load_capabilities(knowledge)
    builds.sort(key=lambda item: item["build_id"])
    functions.sort(key=lambda item: item["contract_id"])
    capabilities.sort(key=lambda item: item["capability_id"])
    plain_builds = [{k: v for k, v in item.items() if k != "_record_path"} for item in builds]
    plain_functions = [{k: v for k, v in item.items() if k != "_record_path"} for item in functions]
    plain_capabilities = [{k: v for k, v in item.items() if k != "_record_path"} for item in capabilities]
    contracts = {
        "schema_version": 1,
        "builds": plain_builds,
        "functions": plain_functions,
        "capabilities": plain_capabilities,
    }

    lines = [
        "# Generated Exanima native knowledge index",
        "",
        "<!-- Generated by tools/kb.py; do not edit by hand. -->",
        "",
        "Static research and runtime acceptance are recorded separately; eligibility and evidence states are explicit.",
        "",
        "## Builds",
        "",
        "| Build | Executable SHA-256 | Procedures | Support | Runtime release |",
        "|---|---|---:|---|---|",
    ]
    for build in builds:
        lines.append(
            f"| {build['build_id']} | `{build['executable']['sha256']}` | "
            f"{build['analysis']['procedure_count']} | {build['support']['status']} | "
            f"{str(build['support']['release_enabled']).lower()} |"
        )
    lines.extend([
        "",
        "## Functions",
        "",
        "| Contract | RVA | Role state | Evidence states | Runtime eligible | Research summary |",
        "|---|---|---|---|---|---|",
    ])
    for function in functions:
        summary = function["role"]["summary"].replace("|", "\\|")
        lines.append(
            f"| `{function['contract_id']}` | `{function['symbol']['rva']}` | "
            f"{function['role']['state']} | {', '.join(function['states'])} | "
            f"{str(function['generation']['runtime_eligible']).lower()} | {summary} |"
        )
    lines.extend([
        "",
        "## Capabilities",
        "",
        "| Capability | State | Eligible | Hooks | Faults | Acceptance |",
        "|---|---|---|---:|---:|---|",
    ])
    for capability in capabilities:
        runtime = capability["runtime"]
        lines.append(
            f"| `{capability['capability_id']}` | {runtime['state']} | "
            f"{str(runtime['eligible']).lower()} | {runtime['hook_count']} | "
            f"{runtime['module_faults']} | {capability['acceptance']['document']} |"
        )
    lines.append("")
    return {
        "index.md": "\n".join(lines),
        "contracts.json": json.dumps(contracts, indent=2, sort_keys=True) + "\n",
    }


def generate(knowledge: Path, output: Path) -> None:
    validate_knowledge(knowledge)
    documents = generated_documents(knowledge)
    output.mkdir(parents=True, exist_ok=True)
    for name, text in documents.items():
        (output / name).write_text(text, encoding="utf-8", newline="\n")


def check_generated(knowledge: Path, generated: Path) -> None:
    validate_knowledge(knowledge)
    expected = generated_documents(knowledge)
    errors = []
    for name, text in expected.items():
        path = generated / name
        if not path.exists():
            errors.append(f"missing {path}")
        elif path.read_text(encoding="utf-8") != text:
            errors.append(f"stale {path}; run tools/kb.py generate")
    if errors:
        raise KnowledgeError("generated knowledge check failed:\n  " + "\n  ".join(errors))


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    importer = sub.add_parser("import-rea", help="Import REA Evidence as research-only records")
    importer.add_argument("--overview", type=Path, required=True)
    importer.add_argument("--dossiers", type=Path, required=True)
    importer.add_argument("--targets", type=Path, required=True)
    importer.add_argument("--knowledge", type=Path, default=DEFAULT_KNOWLEDGE)
    importer.add_argument("--overwrite", action="store_true")
    for command in ("validate", "generate", "check"):
        child = sub.add_parser(command)
        child.add_argument("--knowledge", type=Path, default=DEFAULT_KNOWLEDGE)
        if command == "generate":
            child.add_argument("--output", type=Path)
        if command == "check":
            child.add_argument("--generated", type=Path)
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        if args.command == "import-rea":
            builds, functions = import_rea(args.overview, args.dossiers, args.targets, args.knowledge, args.overwrite)
            print(f"imported {builds} build and {functions} research-only function records")
        elif args.command == "validate":
            counts = validate_knowledge(args.knowledge)
            print(f"knowledge valid: {counts['builds']} builds, {counts['functions']} functions, {counts['capabilities']} capabilities")
        elif args.command == "generate":
            output = args.output or args.knowledge / "generated"
            generate(args.knowledge, output)
            print(f"generated knowledge index and contracts in {output}")
        elif args.command == "check":
            generated = args.generated or args.knowledge / "generated"
            check_generated(args.knowledge, generated)
            print("generated knowledge is current")
        return 0
    except KnowledgeError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
