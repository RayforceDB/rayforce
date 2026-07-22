---
id: CF-XXXX
title: # short title of the systemic defect (operator's working language)
severity: # highest severity among members
dimension: # dimension key from AUDIT.md
unit: [] # YAML list of every unit the class spans (from the census); canonical field per §2
status: reported
class: null # a CF is itself the class — it is not absorbed by another class
members: [] # F-XXXX ids absorbed by this class (they get status superseded-by-class)
attempts: 0
pass: P-XX # the pass whose finding triggered the census escalation (— if surfaced purely in remediation)
created-by: RP-XXXX # the remediation session whose census escalated
updated: YYYY-MM-DD
---

## Pattern
<!-- Remediator. Exact definition of the recurring defect + the reproducible
     search procedure used (grep/AST query/graph query/targeted reading list).
     The Verifier will re-run exactly this procedure. -->

## Census
<!-- Remediator. ALL instances with locators per XCHECK.md §6 — including
     ones no audit pass reported. This list is the fix's scope. -->

## Validation
<!-- Remediator, step 1 for an accepted CF (§8): re-run the Pattern search
     procedure to confirm the class is real and the census still holds
     (validated / disputed / obsolete + the grounds). This is the accepted-CF
     "validate" step and MUST precede remediation. -->

## Root cause
<!-- Remediator. Usually: a norm that is missing, ambiguous, or was
     systematically violated. Name the norm document. -->

## Global strategy
<!-- Remediator. The chosen rung and why:
     (a) fix all census instances;
     (b) (a) + fix the norm so the class cannot recur;
     (c) (b) + automated guard (grep check, lint rule, style script).
     Norm/guard changes are part of the fix.
     NORM RATIFICATION GATE (§8): if a (b)/(c) fix changes a norm, relies on a
     norm another normative source contradicts, or must pick a side in any norm
     conflict — STOP after writing this strategy, route the conflict to the norm
     owner through triage, and do NOT execute until it is ratified. Documenting a
     conflict and proceeding anyway is a protocol violation for class fixes: a
     class fix in the wrong direction multiplies one error across the corpus. -->

## Remediation
<!-- Remediator. What was changed, instance by instance + norm/guard edits. -->

## Verification
<!-- Verifier. Re-run the Pattern search procedure. Expected: zero instances,
     or an explicit documented-exceptions list. "Fixed 12 of 15" = reopened. -->
