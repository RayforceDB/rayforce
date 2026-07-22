---
id: F-XXXX
title: # short title of the defect (operator's working language)
severity: # critical | major | minor | info
dimension: # one dimension key from AUDIT.md
unit: # material unit; YAML list if the finding spans units
status: reported
class: null # CF-XXXX once absorbed by a class finding
attempts: 0 # the Remediator increments this when re-taking a reopened finding
pass: P-XX # the audit pass that produced this finding
updated: YYYY-MM-DD
---

## Evidence
<!-- Auditor. Operator's working language; quotes verbatim in the material's language.
     Required by XCHECK.md §6: (1) character-exact quote, findable by search;
     (2) locator: code — file:line + symbol; text — file + section;
     (3) the norm this material violates, with its own quote
         (style contract / spec / ADR / sourced fact / contradicting passage).
     No norm → severity at most info. -->

## Why this is a defect
<!-- Auditor. 1–3 sentences. -->

## How to verify the fix
<!-- Auditor, at creation time. A reproducible procedure (search command,
     section to re-read, check to run) + the expected result. -->

## Validation
<!-- Remediator, step 1. validated / disputed / obsolete + the grounds.
     Quote re-found? Problem real? -->

## Objection
<!-- Auditor — only when Validation = disputed (§9 Agent dispute). Exactly ONE
     written round: why the finding stands, evidence per §6. Append-only; then
     the human decides. No further agent ping-pong. Leave empty otherwise. -->

## Remediation
<!-- Remediator. Link to RP-XXXX, what was changed and where
     (files/sections; commit IDs if the project uses git). -->

## Verification
<!-- Verifier — never the fixer. Verdict (closed / reopened) + how the
     "How to verify" procedure was executed + what was inspected around
     the change for collateral damage. Evidence standard applies to
     reopening: exact quote of the surviving/new defect. -->
