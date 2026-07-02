# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**`README.md` is the complete spec** — the what, why, hardware, message set, intended layout, and references. Read it before any implementation work. This file does not restate the spec; it only covers how to *operate* in the repo.

## Status & operating constraints

- **Spec only — no code yet.** There is no build/lint/test tooling. Do not invent build commands; introduce Zephyr `west` tooling only when implementation actually begins.
- Create the `proto/` / `firmware/` / `host/` / `docs/` tree (per the README's structure section) as work proceeds, not up front.
- As you resolve the README's open decisions (nanopb integration path, driver source, telemetry trigger), record the decision and its rationale in `docs/`.

## Working principles

- The deliverable is fluency in the three patterns the README calls out — TCP framing, schema versioning, nanopb on a constrained target — not a polished product. Treat those as the real work.
- When a choice trades simplicity for fidelity to the real firmware↔SW-team contract, prefer fidelity. That is the point of the exercise.
- `proto/` is the single source of truth for the wire format; firmware and host both derive from it. Change the schema there and regenerate — never hand-edit generated code.
- Sensor access goes through Zephyr's sensor API, not raw I²C command codes. `../../shared_refs/sensor/SCD4x.yaml` documents what the driver does underneath (command codes, conversion formulas, CRC-8 params) — reach for it when debugging the sensor path.
