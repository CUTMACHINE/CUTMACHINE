#!/usr/bin/env python3
"""ACP-2026-09 -- a minimal ACP agent for tests/acp_agent_tests.cc.

It does what a coding agent does with the panel, and nothing more: starts
the MCP server that `session/new` hands it, asks the client before each
tool call, calls the tool through that server, and reports the call as ACP
`tool_call` updates. What to do comes from the prompt's last text block, a
JSON object:

  {"call": {"tool": "...", "arguments": {...}}}   one engine call
  {"wait_for_cancel": true}                        run until session/cancel

The preamble, when the client sends one, is counted and said back.
"""

import json
import os
import subprocess
import sys


def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def read():
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    return json.loads(line)


def update(session_id, payload):
    send({"jsonrpc": "2.0", "method": "session/update",
          "params": {"sessionId": session_id, "update": payload}})


class McpClient:
    def __init__(self, server):
        environment = dict(os.environ)
        for variable in server.get("env", []):
            environment[variable["name"]] = variable["value"]
        self.process = subprocess.Popen(
            [server["command"]] + server["args"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env=environment, text=True, encoding="utf-8")
        self.next_id = 1
        self.request("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "fake-acp-agent", "version": "1"}})
        self.notify("notifications/initialized")

    def notify(self, method):
        self.process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.process.stdin.flush()

    def request(self, method, params):
        request_id = self.next_id
        self.next_id += 1
        self.process.stdin.write(json.dumps(
            {"jsonrpc": "2.0", "id": request_id, "method": method,
             "params": params}) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("the MCP server closed its output")
            message = json.loads(line)
            if message.get("id") == request_id:
                return message


def main():
    session_id = "fake-1"
    mcp = None
    next_request = 1000
    while True:
        message = read()
        method = message.get("method")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": message["id"], "result": {
                "protocolVersion": 1,
                "agentCapabilities": {"loadSession": False},
                "agentInfo": {"name": "fake-acp-agent",
                              "title": "Agent factice", "version": "1"},
                "authMethods": []}})
        elif method == "session/new":
            servers = message["params"]["mcpServers"]
            mcp = McpClient(servers[0])
            send({"jsonrpc": "2.0", "id": message["id"],
                  "result": {"sessionId": session_id}})
        elif method == "session/prompt":
            blocks = message["params"]["prompt"]
            order = json.loads(blocks[-1]["text"])
            update(session_id, {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text",
                            "text": "blocs: %d" % len(blocks)}})
            if order.get("wait_for_cancel"):
                while True:
                    incoming = read()
                    if incoming.get("method") == "session/cancel":
                        break
                send({"jsonrpc": "2.0", "id": message["id"],
                      "result": {"stopReason": "cancelled"}})
                continue
            call = order["call"]
            title = "mcp__cutmachine__" + call["tool"]
            update(session_id, {
                "sessionUpdate": "tool_call", "toolCallId": "call-1",
                "title": title, "kind": "edit", "status": "pending",
                "rawInput": call["arguments"]})
            next_request += 1
            send({"jsonrpc": "2.0", "id": next_request,
                  "method": "session/request_permission", "params": {
                      "sessionId": session_id,
                      "toolCall": {"toolCallId": "call-1", "title": title},
                      "options": [
                          {"optionId": "yes", "name": "Oui",
                           "kind": "allow_once"},
                          {"optionId": "no", "name": "Non",
                           "kind": "reject_once"}]}})
            answer = read()
            outcome = answer.get("result", {}).get("outcome", {})
            if outcome.get("optionId") != "yes":
                update(session_id, {
                    "sessionUpdate": "tool_call_update",
                    "toolCallId": "call-1", "status": "failed",
                    "content": [{"type": "content", "content": {
                        "type": "text", "text": "refusé"}}]})
            else:
                result = mcp.request("tools/call", {
                    "name": call["tool"], "arguments": call["arguments"]})
                body = result.get("result", {})
                text = "".join(item.get("text", "")
                               for item in body.get("content", []))
                update(session_id, {
                    "sessionUpdate": "tool_call_update",
                    "toolCallId": "call-1",
                    "status": "failed" if body.get("isError") else
                              "completed",
                    "content": [{"type": "content", "content": {
                        "type": "text", "text": text}}]})
            send({"jsonrpc": "2.0", "id": message["id"],
                  "result": {"stopReason": "end_turn"}})


if __name__ == "__main__":
    main()
