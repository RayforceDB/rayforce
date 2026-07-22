---
id: RP-XXXX
findings: [] # accepted F-XXXX and/or CF-XXXX ids in this batch (≤ remediation_batch_size);
             # an accepted CF is a batch of its own (§8)
status: open # open | done
updated: YYYY-MM-DD
---

## Batch validation results
<!-- Remediator, step 1 per finding: validated / disputed / obsolete.
     Disputed and obsolete findings leave the batch here.
     For an accepted CF (§8): validate = re-run the recorded census procedure
     (record it in the CF file's Validation section); the steps below map as
     census = already done (the CF's census IS the scope), plan = the CF's
     Global strategy. -->

## Census results
<!-- Remediator, step 2 per validated finding: pattern formulated, corpus
     searched (procedure recorded). Two branches (§8):
     • < class_threshold instances → fix the instance found AND record every
       sibling in the FINDING FILE (not only here);
     • ≥ class_threshold instances → create CF-XXXX, member findings →
       superseded-by-class, note it here; the batch continues without them. -->

## Fix plan
<!-- Remediator, step 3. Per remaining finding: exact edit
     intended, order, risks (what could break nearby).
     NORM RATIFICATION GATE (§8) for any class fix: if a (b)/(c) strategy
     changes a norm, relies on a norm another source contradicts, or must pick a
     side in a norm conflict — STOP after writing the plan, route the conflict to
     the norm owner through triage, and do NOT enter the Execution log until it
     is ratified. -->

## Execution log
<!-- Remediator, steps 4–5. What was actually changed (files/sections/
     commits) + self-check result per finding before setting status fixed. -->
