"""OR3 -- read the native panel's ACP trace without replaying an agent.

ACP reports turns, not individual model responses. Keep raw token field
names; context usage notifications are occupancy, not billed token counts.
Standard library only: it reads a trace, it never starts an agent.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


class EngineError(RuntimeError):
    def __init__(self, code: str, detail: str):
        self.code = code
        super().__init__(f"{code}: {detail}")


def encode(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=True, allow_nan=False)


def decode(value: str):
    def refuse_constant(token):
        raise ValueError(f"Nombre JSON non fini : {token}")
    def unique_members(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise ValueError(f"Clé JSON dupliquée : {key}")
            result[key] = item
        return result
    # Two values under one key would make a trace mean two different turns.
    return json.loads(value, parse_constant=refuse_constant,
                      object_pairs_hook=unique_members)


def counters(records):
    """Missing or invalid fields stay missing; they never become zero usage."""
    totals, reported = {}, {}
    for record in records:
        if not isinstance(record, dict):
            continue
        for key, value in counter_fields(record).items():
            totals[key] = totals.get(key, 0) + value
            reported[key] = reported.get(key, 0) + 1
    return {"known_totals": totals, "reported_by_field": reported,
            "records": len(records),
            "lower_bound_fields": sorted(key for key, count in reported.items()
                                         if count != len(records))}


def counter_fields(record, prefix="", depth=0):
    fields = {}
    if depth > 4:
        return fields
    for key, value in record.items():
        if not isinstance(key, str) or len(key) > 128:
            continue
        name = prefix + key
        if type(value) is int and value >= 0:
            fields[name] = value
        elif isinstance(value, dict):
            fields.update(counter_fields(value, name + ".", depth + 1))
    return fields


def engine_tool(update):
    # Use the same adapter evidence as AcpClient::IsEngineToolCall;
    # labels may arrive in a later update, not the initial announcement.
    def obj(value):
        return value if isinstance(value, dict) else {}
    def prefixed(value):
        return isinstance(value, str) and value.startswith(("mcp__cutmachine__", "mcp.cutmachine."))
    meta = obj(obj(update.get("_meta")).get("claudeCode"))
    raw = obj(update.get("rawInput"))
    return (obj(meta.get("mcpServer")).get("name") == "cutmachine"
            or prefixed(meta.get("toolName")) or prefixed(update.get("title"))
            or prefixed(update.get("name"))
            or (raw.get("server") == "cutmachine" and "tool" in raw))


def acp_report(path):
    prompts, tools, models = {}, {}, {}
    pending_models = {}
    active_turns = {}
    model_ids = set()
    total_bytes = 0
    with Path(path).open("rb") as source:
        while True:
            raw = source.readline(8 * 1024 * 1024 + 1)
            if not raw:
                break
            total_bytes += len(raw)
            if len(raw) > 8 * 1024 * 1024 or total_bytes > 256 * 1024 * 1024:
                raise EngineError("TraceLimit", "journal ACP trop grand")
            envelope = decode(raw.decode("utf-8"))
            if (not isinstance(envelope, dict) or type(envelope.get("ms")) is not int
                    or envelope["ms"] < 0 or envelope.get("dir") not in {"in", "out"}
                    or not isinstance(envelope.get("line"), str)):
                raise EngineError("InvalidAcpTrace", "enveloppe de journal ACP invalide")
            message = decode(envelope["line"])
            if not isinstance(message, dict):
                raise EngineError("InvalidAcpTrace", "message ACP invalide")
            stamp, direction = envelope["ms"], envelope["dir"]
            method = message.get("method")
            params = message.get("params") or {}
            request_id = message.get("id")
            if not isinstance(params, dict) or (request_id is not None and type(request_id) not in {str, int}):
                raise EngineError("InvalidAcpTrace", "paramètres ou identifiant ACP invalides")
            if direction == "out" and method == "session/prompt":
                if type(request_id) not in {str, int} or request_id in prompts:
                    raise EngineError("InvalidAcpTrace", "identifiant de tour ACP absent ou répété")
                prompts[request_id] = {"started_ms": stamp, "ended_ms": None,
                                      "outcome": "unfinished", "usage": None,
                                      "model": models.get(params.get("sessionId"))}
                active_turns[params.get("sessionId")] = request_id
            elif direction == "out" and method == "session/new":
                pending_models[request_id] = ("new", None)
            elif direction == "out" and method == "session/set_model":
                pending_models[request_id] = (params.get("sessionId"), params.get("modelId"))
            elif direction == "in" and method is None:
                result = message.get("result") or {}
                if not isinstance(result, dict):
                    raise EngineError("InvalidAcpTrace", "résultat ACP invalide")
                if request_id in pending_models:
                    session, model = pending_models.pop(request_id)
                    if "error" not in message:
                        if session == "new":
                            session = result.get("sessionId")
                            model = (result.get("models") or {}).get("currentModelId")
                        models[session] = model
                        if isinstance(model, str):
                            model_ids.add(model)
                if request_id not in prompts and isinstance(result.get("stopReason"), str):
                    # A trace starting after the request still carries billed
                    # usage. Its missing start must remain an unknown duration.
                    prompts[request_id] = {"started_ms": None, "ended_ms": None,
                                          "outcome": "unfinished", "usage": None, "model": None}
                if request_id in prompts:
                    turn = prompts[request_id]
                    if (turn["ended_ms"] is not None or
                            (turn["started_ms"] is not None and stamp < turn["started_ms"])):
                        raise EngineError("InvalidAcpTrace", "durée ou réponse de tour ACP invalide")
                    turn["ended_ms"] = stamp
                    turn["outcome"] = ("ok" if "error" not in message and
                                       result.get("stopReason") == "end_turn" else "error")
                    turn["usage"] = result.get("usage")
            elif direction == "in" and method == "session/update":
                update = params.get("update") or {}
                if not isinstance(update, dict):
                    raise EngineError("InvalidAcpTrace", "notification ACP invalide")
                kind = update.get("sessionUpdate")
                if kind not in {"tool_call", "tool_call_update"}:
                    continue
                call_id = update.get("toolCallId")
                session = params.get("sessionId")
                if not isinstance(call_id, str) or not call_id:
                    raise EngineError("InvalidAcpTrace", "outil ACP sans identifiant")
                # AcpChatSession resets its tool map on every prompt. IDs
                # reused by an adapter on a new turn are distinct attempts.
                key = (session, active_turns.get(session), call_id)
                if key not in tools:
                    tools[key] = {"started_ms": stamp if kind == "tool_call" else None,
                                  "ended_ms": None, "outcome": "unfinished",
                                  "engine": False}
                tool = tools[key]
                tool["engine"] = tool["engine"] or engine_tool(update)
                if update.get("status") in {"completed", "failed"} and tool["ended_ms"] is None:
                    tool["ended_ms"] = stamp
                    tool["outcome"] = "ok" if update["status"] == "completed" else "error"
    turns = list(prompts.values())
    usage = []
    for model in sorted({turn["model"] for turn in turns}, key=lambda value: value or ""):
        records = [turn["usage"] for turn in turns if turn["model"] == model]
        counted = counters(records)
        complete = (not counted["lower_bound_fields"] and
                    all(counted["reported_by_field"].get(field, 0) == len(records)
                        for field in ("inputTokens", "outputTokens")))
        usage.append({"model_id": model, **counted, "totals_are_lower_bounds": not complete})

    def counts(items):
        durations = [(item["ended_ms"] - item["started_ms"]) / 1000 for item in items
                     if item["ended_ms"] is not None and item["started_ms"] is not None
                     and item["ended_ms"] >= item["started_ms"]]
        return {"attempts": len(items), "ok": sum(item["outcome"] == "ok" for item in items),
                "errors": sum(item["outcome"] == "error" for item in items),
                "unfinished": sum(item["outcome"] == "unfinished" for item in items),
                "known_seconds": sum(durations), "duration_is_lower_bound": len(durations) != len(items)}

    return {"format": "cutmachine.acp-metrics.v1", "model_ids_reported": sorted(model_ids),
            "coverage": "trace_only", "measurements_available": bool(turns or tools),
            "unpaired_turn_responses": sum(turn["started_ms"] is None for turn in turns),
            "turns": counts(turns), "tools": counts(list(tools.values())),
            "engine_tools": counts([tool for tool in tools.values() if tool["engine"]]),
            "usage": usage, "api_cost": None, "acceptance": "not_recorded",
            "scope": "trace ACP; tours et outils, durées inclusives pouvant se chevaucher",
            "model_calls": None}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Mesurer un journal ACP du panneau Agent")
    parser.add_argument("--trace", required=True)
    args = parser.parse_args(argv)
    try:
        print(encode({"ok": True, **acp_report(args.trace)}))
        return 0
    except (EngineError, OSError, ValueError) as error:
        print(encode({"ok": False, "error": getattr(error, "code", type(error).__name__),
                      "detail": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
