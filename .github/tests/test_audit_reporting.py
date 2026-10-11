"""Exercise the exact embedded report-normalization code used by both workflows."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MESSAGE = "The audit service is temporarily unavailable. Please retry later."
UNAVAILABLE = "<!-- rayforce-audit-unavailable -->"


class AuditReportingTests(unittest.TestCase):
    def normalize(self, workflow, report, status=None):
        text = (ROOT / "workflows" / workflow).read_text()
        start = text.index("<<'RAYFORCE_REPORT_PY'\n") + len("<<'RAYFORCE_REPORT_PY'\n")
        end = text.index("          RAYFORCE_REPORT_PY", start)
        code = "\n".join(line[10:] for line in text[start:end].splitlines())
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audit-output/rayforce-audit.md"
            path.parent.mkdir()
            if report is not None:
                path.write_text(report)
            env = dict(os.environ)
            env.pop("AUDIT_COMMAND_STATUS", None)
            if status is not None:
                env["AUDIT_COMMAND_STATUS"] = str(status)
            subprocess.run(["python3", "-c", code], cwd=tmp, env=env, check=True)
            return path.read_text()

    def test_backend_failures_are_neutral_in_both_workflows(self):
        for workflow in ("rayforce-audit.yml", "rayforce-audit-comment.yml"):
            for report in (None, "", "You've hit your limit · resets 5pm", "Failed to authenticate: OAuth session expired", "Error: 429 quota exhausted", "partial findings without a verdict"):
                with self.subTest(workflow=workflow, report=report):
                    actual = self.normalize(workflow, report)
                    self.assertIn(MESSAGE, actual)
                    self.assertIn(UNAVAILABLE, actual)
                    self.assertTrue(actual.endswith("RAYFORCE_AUDIT_VERDICT: FAIL\n"))
                    for private in ("quota", "OAuth", "429", "hit your limit"):
                        self.assertNotIn(private, actual)

    def test_real_audit_reports_are_preserved(self):
        for workflow in ("rayforce-audit.yml", "rayforce-audit-comment.yml"):
            for verdict in ("PASS", "FAIL"):
                report = f"## Findings\nQuota limit validation in src/query.c is incorrect.\n\nRAYFORCE_AUDIT_VERDICT: {verdict}\n"
                self.assertEqual(self.normalize(workflow, report, 0), report)

    def test_command_failure_cannot_pass_or_leak_partial_output(self):
        for status in (1, 124, 137):
            for verdict in ("PASS", "FAIL"):
                with self.subTest(status=status, verdict=verdict):
                    actual = self.normalize("rayforce-audit.yml", f"private diagnostics\nRAYFORCE_AUDIT_VERDICT: {verdict}\n", status)
                    self.assertIn(MESSAGE, actual)
                    self.assertNotIn("private diagnostics", actual)

    def test_neutral_report_survives_commenter(self):
        report = self.normalize("rayforce-audit.yml", "quota error", 1)
        self.assertEqual(self.normalize("rayforce-audit-comment.yml", report), report)

    def step_script(self, workflow, step):
        text = (ROOT / "workflows" / workflow).read_text()
        section = text.split(f"      - name: {step}\n", 1)[1]
        section = section.split("      - name: ", 1)[0]
        return "\n".join(line[10:] for line in section.split("        run: |\n", 1)[1].splitlines())

    def test_actual_audit_shell_fails_closed(self):
        script = self.step_script("rayforce-audit.yml", "Run Claude rayforce-audit skill")
        cases = [
            ("You've hit your limit", 1, False),
            ("Failed to authenticate: OAuth session expired", 0, False),
            ("partial private output", 124, False),
            ("No blockers.\nRAYFORCE_AUDIT_VERDICT: PASS", 0, True),
            ("src/query.c: real bug\nRAYFORCE_AUDIT_VERDICT: FAIL", 0, False),
        ]
        for output, status, passes in cases:
            with self.subTest(output=output), tempfile.TemporaryDirectory() as tmp:
                stub = Path(tmp) / "claude"
                stub.write_text('#!/bin/sh\nprintf "%s\\n" "$STUB_OUTPUT"\nexit "$STUB_STATUS"\n')
                stub.chmod(0o700)
                env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                           STUB_OUTPUT=output, STUB_STATUS=str(status), PR_NUMBER="1",
                           BASE_REF="origin/dev", HEAD_SHA="test", RAYFORCE_AUDIT_PLUGIN_DIR=tmp)
                result = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, passes, result.stderr)
                report = (Path(tmp) / "audit-output/rayforce-audit.md").read_text()
                if "RAYFORCE_AUDIT_VERDICT:" not in output:
                    self.assertIn(MESSAGE, report)
                    self.assertNotIn(output, report)
                else:
                    self.assertIn(output, report)

    def test_actual_comment_shell_sanitizes_legacy_artifact(self):
        script = self.step_script("rayforce-audit-comment.yml", "Publish Rayforce audit PR comment")
        import json
        for report in ("You've hit your limit", "src/query.c: real bug\nRAYFORCE_AUDIT_VERDICT: FAIL\n"):
            with self.subTest(report=report), tempfile.TemporaryDirectory() as tmp:
                event = Path(tmp) / "event.json"
                event.write_text(json.dumps({"workflow_run": {"pull_requests": [{"number": 1}]}}))
                stub = Path(tmp) / "gh"
                stub.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
args = sys.argv[1:]
if args[:2] == ["run", "download"]:
    Path("audit-output/rayforce-audit.md").write_text(os.environ["STUB_REPORT"])
elif "POST" in args or "PATCH" in args:
    Path("posted.txt").write_text(next(arg[5:] for arg in args if arg.startswith("body=")))
''')
                stub.chmod(0o700)
                env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                           STUB_REPORT=report, GITHUB_EVENT_PATH=str(event), REPO="test/test",
                           RUN_ID="1", RUN_URL="https://example.invalid/run/1",
                           RUN_CONCLUSION="failure", HEAD_SHA="test")
                result = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                body = (Path(tmp) / "posted.txt").read_text()
                if "VERDICT" not in report:
                    self.assertIn(MESSAGE, body)
                    self.assertIn("## Rayforce audit service unavailable", body)
                    self.assertNotIn(report, body)
                    self.assertNotIn("Fix the blocking findings", body)
                else:
                    self.assertIn(report.rstrip(), body)
                    self.assertIn("Fix the blocking findings", body)

    def test_both_comment_paths_have_neutral_heading(self):
        for workflow in ("rayforce-audit.yml", "rayforce-audit-comment.yml"):
            text = (ROOT / "workflows" / workflow).read_text()
            self.assertIn('echo "## Rayforce audit service unavailable"', text)
            self.assertIn('if grep -qF "<!-- rayforce-audit-unavailable -->" "$report"; then', text)


if __name__ == "__main__":
    unittest.main()
