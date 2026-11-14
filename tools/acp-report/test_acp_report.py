"""OR3 -- ACP trace envelopes as the Agent panel writes them."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from acp_report import EngineError, acp_report, decode, encode


class AcpMetricsTests(unittest.TestCase):
    def trace(self, events):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "agent.acp.jsonl"
        path.write_text("".join(encode({"ms": ms, "dir": direction, "line": encode(line)}) + "\n"
                                for ms, direction, line in events))
        return path

    def test_turn_usage_tool_updates_and_context_are_counted_once(self):
        def update(ms, kind, **fields):
            return (ms, "in", {"method": "session/update", "params": {
                "sessionId": "session", "update": {"sessionUpdate": kind, **fields}}})
        path = self.trace([
            (0, "out", {"id": 1, "method": "session/new"}),
            (10, "in", {"id": 1, "result": {"sessionId": "session",
                "models": {"currentModelId": "fixture-model"}}}),
            (100, "out", {"id": 2, "method": "session/prompt", "params": {
                "sessionId": "session", "prompt": "private transcript"}}),
            update(120, "usage_update", used=9999, size=20000),
            update(150, "tool_call", toolCallId="tool-1", title="mcp__cutmachine__describe"),
            update(170, "tool_call_update", toolCallId="tool-1", status="completed"),
            update(175, "tool_call_update", toolCallId="tool-1", status="completed"),
            (200, "in", {"id": 2, "result": {"stopReason": "end_turn", "usage": {
                "inputTokens": 10, "outputTokens": 3, "cachedReadTokens": 12}}}),
            (300, "out", {"id": 3, "method": "session/prompt", "params": {"sessionId": "session"}}),
            (450, "in", {"id": 3, "result": {"stopReason": "cancelled"}}),
            (500, "out", {"id": 4, "method": "session/prompt", "params": {"sessionId": "session"}})])
        before = path.read_bytes()
        report = acp_report(path)
        self.assertEqual(report["turns"]["attempts"], 3)
        self.assertEqual(report["turns"]["errors"], 1)
        self.assertEqual(report["turns"]["unfinished"], 1)
        self.assertEqual(report["engine_tools"]["attempts"], 1)
        self.assertAlmostEqual(report["turns"]["known_seconds"], 0.25)
        self.assertEqual(report["usage"][0]["known_totals"]["inputTokens"], 10)
        self.assertTrue(report["usage"][0]["totals_are_lower_bounds"])
        self.assertNotIn("private transcript", encode(report))
        self.assertNotIn("9999", encode(report))
        self.assertEqual(path.read_bytes(), before)
        self.assertIsNone(report["model_calls"])

    def test_missing_model_and_incomplete_tool_duration_stay_unknown(self):
        path = self.trace([(2, "out", {"id": 1, "method": "session/prompt"}),
                           (3, "in", {"method": "session/update", "params": {
                               "update": {"sessionUpdate": "tool_call_update", "toolCallId": "x",
                                          "status": "failed"}}})])
        report = acp_report(path)
        self.assertIsNone(report["usage"][0]["model_id"])
        self.assertTrue(report["tools"]["duration_is_lower_bound"])
        self.assertTrue(report["usage"][0]["totals_are_lower_bounds"])

    def test_adapter_metadata_in_late_update_identifies_engine(self):
        for evidence in ({"rawInput": {"server": "cutmachine", "tool": "describe"}},
                         {"_meta": {"claudeCode": {"mcpServer": {"name": "cutmachine"}}}},
                         {"_meta": {"claudeCode": {"toolName": "mcp__cutmachine__describe"}}}):
            path = self.trace([(1, "in", {"method": "session/update", "params": {"update": {
                "sessionUpdate": "tool_call", "toolCallId": "x", "title": "Loading"}}}),
                (2, "in", {"method": "session/update", "params": {"update": {
                    "sessionUpdate": "tool_call_update", "toolCallId": "x", "status": "completed",
                    **evidence}}})])
            self.assertEqual(acp_report(path)["engine_tools"]["ok"], 1)

    def test_duplicate_response_and_backward_clock_are_refused(self):
        request = (100, "out", {"id": 1, "method": "session/prompt"})
        response = (200, "in", {"id": 1, "result": {"stopReason": "end_turn"}})
        for events in ([request, response, response], [request, (20, "in", response[2])]):
            with self.assertRaisesRegex(EngineError, "InvalidAcpTrace"):
                acp_report(self.trace(events))

    def test_response_without_request_keeps_usage_with_unknown_duration(self):
        report = acp_report(self.trace([(20, "in", {"id": 4, "result": {
            "stopReason": "end_turn", "usage": {"inputTokens": 17, "outputTokens": 6}}})]))
        self.assertEqual(report["unpaired_turn_responses"], 1)
        self.assertEqual(report["usage"][0]["known_totals"]["inputTokens"], 17)
        self.assertTrue(report["turns"]["duration_is_lower_bound"])
        self.assertEqual(report["coverage"], "trace_only")

    def test_tool_id_reused_on_a_new_turn_counts_as_another_attempt(self):
        events = []
        for turn in (1, 2):
            events.extend([(turn * 10, "out", {"id": turn, "method": "session/prompt"}),
                           (turn * 10 + 1, "in", {"method": "session/update", "params": {"update": {
                               "sessionUpdate": "tool_call", "toolCallId": "same", "title": "mcp.cutmachine.describe"}}}),
                           (turn * 10 + 2, "in", {"method": "session/update", "params": {"update": {
                               "sessionUpdate": "tool_call_update", "toolCallId": "same", "status": "completed"}}})])
        self.assertEqual(acp_report(self.trace(events))["engine_tools"]["ok"], 2)

    def test_cli_reads_without_writing_and_refuses_a_broken_trace(self):
        path = self.trace([(20, "in", {"id": 4, "result": {"stopReason": "end_turn"}})])
        before = path.read_bytes()
        script = str(Path(__file__).with_name("acp_report.py"))
        answer = subprocess.run([sys.executable, script, "--trace", str(path)],
                                capture_output=True, text=True, check=True)
        self.assertTrue(decode(answer.stdout)["ok"])
        self.assertEqual(path.read_bytes(), before)
        path.write_text('{"ms":1,"dir":"in","line":"{}","ms":2}\n')
        refused = subprocess.run([sys.executable, script, "--trace", str(path)],
                                 capture_output=True, text=True)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("dupliquée", decode(refused.stdout)["detail"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
