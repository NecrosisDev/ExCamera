import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
try:
    SPEC = importlib.util.spec_from_file_location("kb", ROOT / "tools" / "kb.py")
    kb = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(kb)
except (ImportError, SystemExit):
    kb = None


@unittest.skipIf(kb is None, "jsonschema not installed")
class KnowledgeBaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.knowledge = ROOT / "knowledge"
        cls.counts = kb.validate_knowledge(cls.knowledge)

    def test_repository_records_validate(self):
        self.assertGreaterEqual(self.counts["builds"], 1)
        self.assertGreaterEqual(self.counts["functions"], 10)

    def test_generated_files_are_current(self):
        kb.check_generated(self.knowledge, self.knowledge / "generated")

    def test_imported_functions_cannot_be_runtime_contracts(self):
        builds, existing = kb.load_knowledge(self.knowledge)
        build = builds[0]
        by_contract = {function["contract_id"]: function for function in existing}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fresh = root / "knowledge"
            dossiers = root / "dossiers"
            dossiers.mkdir()
            kb.write_json(root / "overview.json", {
                "evidence_id": "ev_" + "0" * 64,
                "subject": {"digest": {"sha256": build["executable"]["sha256"]}},
                "provider": {"id": "test", "name": "Synthetic import fixture", "version": "1"},
                "operation": "binary_overview",
                "analysis_profile": {"digest": "test-overview"},
                "normalized_result": {"image_base": build["executable"]["image_base"], "segments": []},
            })
            for targets_path in sorted((self.knowledge / "research").glob("*.json")):
                targets = kb.read_json(targets_path)
                for target in targets["functions"]:
                    original = by_contract[target["contract_id"]]
                    evidence = original["evidence"][0]
                    symbol = original["symbol"]
                    kb.write_json(dossiers / f"{target['import_name']}.json", {
                        "evidence_id": evidence["evidence_id"],
                        "subject": {"digest": {"sha256": build["executable"]["sha256"]}},
                        "provider": evidence["provider"],
                        "operation": "analyze_function",
                        "analysis_profile": {"digest": evidence["analysis_profile_digest"]},
                        "normalized_result": {
                            "procedure": {
                                "address": symbol["va"],
                                "name": symbol["provider_name"],
                                "signature": original["abi"]["provider_signature"],
                                "body": symbol["body"],
                                "classification": symbol["classification"],
                            },
                            "callers": original["relationships"]["callers"],
                            "callees": original["relationships"]["callees"],
                            "limitations": original["relationships"]["limitations"],
                        },
                    })
                kb.import_rea(root / "overview.json", dossiers, targets_path, fresh, overwrite=True)
            _, functions = kb.load_knowledge(fresh)
            self.assertEqual(len(functions), len(existing))
            for function in functions:
                with self.subTest(contract=function["contract_id"]):
                    self.assertEqual(function["states"], ["observed_static"])
                    self.assertEqual(function["abi"]["status"], "unknown")
                    self.assertIsNone(function["abi"]["calling_convention"])
                    self.assertIsNone(function["abi"]["parameters"])
                    self.assertEqual(function["thread"]["status"], "unknown")
                    self.assertEqual(function["memory"]["status"], "unknown")
                    self.assertFalse(function["generation"]["runtime_eligible"])

    def test_generation_is_deterministic(self):
        first = kb.generated_documents(self.knowledge)
        second = kb.generated_documents(self.knowledge)
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"index.md", "contracts.json"})

    def test_camera_capability_links_contracts_and_acceptance(self):
        capabilities = kb.load_capabilities(self.knowledge)
        camera = next(item for item in capabilities if item["capability_id"] == "excamera.camera.core")
        self.assertEqual(set(camera["required_contracts"]), {
            "exanima.camera.head_render_sample",
            "exanima.camera.nav_sampler",
            "exanima.camera.rebuild_projection",
        })
        self.assertEqual(camera["runtime"]["state"], "validated_practice")
        self.assertTrue(camera["runtime"]["eligible"])
        self.assertEqual(camera["runtime"]["module_faults"], 0)
        self.assertFalse(camera["runtime"]["restoration_failed"])
        for result in ("first_person", "third_person", "overhead", "paused_transitions", "clean_stop"):
            self.assertTrue(camera["acceptance"][result])

    def test_input_capability_links_mouse_contracts(self):
        capabilities = kb.load_capabilities(self.knowledge)
        input_core = next(item for item in capabilities if item["capability_id"] == "excamera.input.core")
        self.assertEqual(set(input_core["required_contracts"]), {
            "exanima.camera.copy_snapshots",
            "exanima.camera.mouse_input",
            "exanima.camera.nav_sampler",
        })
        self.assertEqual(input_core["adapter"]["hook_contract"], "exanima.camera.mouse_input")
        self.assertEqual(input_core["runtime"]["hook_count"], 2)
        self.assertEqual(input_core["acceptance"]["document"], "docs/acceptance/input-adapter.md")
        self.assertTrue(input_core["acceptance"]["paused_transitions"])

    def test_settings_capability_links_persistence_contracts(self):
        capabilities = kb.load_capabilities(self.knowledge)
        settings = next(item for item in capabilities if item["capability_id"] == "excamera.settings.core")
        self.assertEqual(settings["runtime"]["hook_count"], 2)
        self.assertEqual(settings["runtime"]["state"], "validated_practice")
        self.assertEqual(settings["acceptance"]["document"], "docs/acceptance/settings-adapter.md")
        self.assertIn("exanima.camera.rebuild_projection", settings["required_contracts"])
        self.assertIn("exanima.camera.mouse_input", settings["required_contracts"])
        self.assertTrue(settings["acceptance"]["clean_stop"])

    def test_lifecycle_capability_links_present_contract(self):
        capabilities = kb.load_capabilities(self.knowledge)
        lifecycle = next(item for item in capabilities if item["capability_id"] == "excamera.lifecycle.core")
        self.assertIn("exanima.render.present", lifecycle["required_contracts"])
        self.assertEqual(lifecycle["adapter"]["hook_contract"], "exanima.render.present")
        self.assertEqual(lifecycle["runtime"]["hook_count"], 3)
        self.assertEqual(lifecycle["acceptance"]["document"], "docs/acceptance/lifecycle-adapter.md")
        self.assertTrue(lifecycle["acceptance"]["clean_stop"])

    def test_failed_restoration_invalidates_capability(self):
        with tempfile.TemporaryDirectory() as temporary:
            knowledge = self.copy_knowledge(Path(temporary))
            path = knowledge / "capabilities" / "camera-core.json"
            value = kb.read_json(path)
            value["runtime"]["restoration_failed"] = True
            kb.write_json(path, value)
            with self.assertRaisesRegex(kb.KnowledgeError, "eligible capability"):
                kb.validate_knowledge(knowledge)

    def test_public_records_have_no_local_research_paths(self):
        documents = kb.generated_documents(self.knowledge)
        combined = documents["index.md"] + documents["contracts.json"]
        self.assertNotIn("E:\\ExCamera", combined)
        self.assertNotIn(".tools", combined)
        self.assertNotIn("local_path", combined)

    def copy_knowledge(self, destination: Path) -> Path:
        target = destination / "knowledge"
        shutil.copytree(self.knowledge, target)
        return target

    def test_rva_va_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            knowledge = self.copy_knowledge(Path(temporary))
            path = next((knowledge / "builds").glob("*/functions/*.json"))
            value = kb.read_json(path)
            value["symbol"]["va"] = kb.hex_address(kb.address(value["symbol"]["va"]) + 0x1000)
            kb.write_json(path, value)
            with self.assertRaisesRegex(kb.KnowledgeError, "VA - image base"):
                kb.validate_knowledge(knowledge)

    def test_runtime_eligibility_requires_review_and_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            knowledge = self.copy_knowledge(Path(temporary))
            path = next((knowledge / "builds").glob("*/functions/*.json"))
            value = kb.read_json(path)
            value["generation"]["runtime_eligible"] = True
            kb.write_json(path, value)
            with self.assertRaisesRegex(kb.KnowledgeError, "runtime_eligible"):
                kb.validate_knowledge(knowledge)

    def test_minimal_rea_import_preserves_unknown_semantics(self):
        digest = "a" * 64
        provider = {"id": "ghidra", "name": "Ghidra", "version": "12.1.4"}
        overview = {
            "evidence_id": "ev_" + "1" * 64,
            "subject": {"digest": {"sha256": digest}},
            "provider": {"id": "rea-workflow", "name": "REA composed investigation workflow", "version": "1"},
            "operation": "binary_overview",
            "analysis_profile": {"digest": "overview-profile"},
            "normalized_result": {
                "image_base": "0x100000000",
                "segments": [],
                "procedure_count": 1,
                "string_count": 0,
            },
        }
        dossier = {
            "evidence_id": "ev_" + "2" * 64,
            "subject": {"digest": {"sha256": digest}},
            "provider": provider,
            "operation": "analyze_function",
            "analysis_profile": {"digest": "function-profile"},
            "normalized_result": {
                "procedure": {
                    "address": "0x100001000",
                    "name": "FUN_100001000",
                    "signature": "undefined FUN_100001000()",
                    "body": None,
                    "classification": None,
                },
                "callers": [],
                "callees": [],
                "limitations": [],
            },
        }
        targets = {
            "game": {"product": "Exanima", "version": "test", "channel": "test", "steam_build": None},
            "build_id": "exanima-test",
            "executable_size": 4096,
            "toolchain": [{"name": "REA", "version": "test"}],
            "functions": [{
                "import_name": "sample",
                "contract_id": "exanima.test.sample",
                "address": "0x100001000",
                "role_hypothesis": "Synthetic test role.",
                "questions": ["Is this still unknown?"],
            }],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "dossiers").mkdir()
            kb.write_json(root / "overview.json", overview)
            kb.write_json(root / "dossiers" / "sample.json", dossier)
            kb.write_json(root / "targets.json", targets)
            knowledge = root / "knowledge"
            builds, functions = kb.import_rea(
                root / "overview.json", root / "dossiers", root / "targets.json", knowledge
            )
            self.assertEqual((builds, functions), (1, 1))
            shutil.copytree(self.knowledge / "schema", knowledge / "schema")
            counts = kb.validate_knowledge(knowledge)
            self.assertEqual(counts["functions"], 1)
            record = kb.read_json(knowledge / "builds" / digest / "functions" / "sample.json")
            self.assertEqual(record["symbol"]["rva"], "0x1000")
            self.assertEqual(record["states"], ["observed_static"])
            self.assertEqual(record["abi"]["status"], "unknown")
            self.assertFalse(record["generation"]["runtime_eligible"])
            self.assertEqual(record["evidence"][0]["analysis_profile_digest"], "function-profile")


if __name__ == "__main__":
    unittest.main()
