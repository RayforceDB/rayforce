# XCHECK — Cross-Agent Audit & Remediation Methodology

version: 0.1.2

Self-contained. If you are an agent reading this inside a project's `audit/`
directory, this file plus `AUDIT.md` plus your charter is everything you need.
Do not read the whole project "for context" — the Session Protocol (§4) tells
you exactly what to read.

Language rule: this methodology and all templates are English. Findings,
plans, and reports are written in the operator's working language — pick one
per audit and stay consistent. Evidence quotes are always verbatim
in the original language of the material.

## 1. System Overview

The methodology runs one cycle, repeatedly, over a project of any size: `audit → human triage → remediate → verify`. An Auditor finds defects and files them as evidence-backed findings. A human triages them. A Remediator fixes the accepted ones. A Verifier gives each fix a binding verdict. Findings that survive verification close; findings that don't reopen and go back to remediation.

The cycle exists because the obvious alternative — one long free-form agent session working through a large project — fails predictably. Long sessions lose the thread: they forget earlier decisions, invent particulars they never actually checked, and hallucinate both findings and fixes. This methodology substitutes three concrete mechanisms for confidence in an agent's memory: narrow chartered sessions that never run long enough to lose the thread, findings anchored to evidence that can be mechanically re-checked, and cross-agent verification where the fixer is never the judge.

Architecture, in short: a findings ledger plus a finding state machine (§5) is the skeleton — durable, file-based state that outlives any single session. Per-session narrow charters (§4) are the working discipline — what makes each individual session reliable even though it starts from a blank context every time.

Universality is a property of one boundary, and it is load-bearing: *"XCHECK.md defines mechanics and knows nothing about domains; AUDIT.md instantiates the methodology for one concrete project."* Everything in this file must hold for a project made of code and a project made of text, unchanged. Anything that depends on what kind of material is being audited belongs in AUDIT.md, not here.

## 2. Artifacts

A **unit** is one addressable piece of the material — one file, or one section of continuous text. A **dimension** is one audit angle, always backed by a norm (§6). AUDIT.md defines both concretely for a given project; this file only assumes they exist.

The methodology's state lives entirely in the `audit/` directory of the audited project:

```
audit/
  XCHECK.md                    # copy of this file — the audit is self-contained
  AUDIT.md                     # charter: dimensions, unit map, pass queue, norms, limits
  LEDGER.md                    # index: one row per finding
  templates/                   # artifact skeletons (finding, class-finding, pass, plan) — copy the matching one when creating a new artifact file
  findings/
    F-0001-<slug>.md           # one finding, one file, full biography inside
    CF-0001-<slug>.md          # one class finding
  passes/
    P-01-<dimension>.md        # one pass report: what was covered, what was not
  plans/
    RP-0001.md                 # remediation plan for one batch of findings
```

IDs: `F-NNNN`, `CF-NNNN`, `RP-NNNN` are 4 digits. `P-NN` is 2 digits. Slugs: ASCII,
lowercase, hyphenated English gist of the title (titles themselves may be in the operator's working language).

A finding file's frontmatter is the canonical record:

```yaml
id: F-0001            # or CF-0001 for a class finding
title: <short title>
severity: critical | major | minor | info
dimension: <dimension slug>
unit: <unit path>              # a YAML list for a cross-unit finding
status: reported
class: null | CF-NNNN          # which class finding, if any, absorbed this one
attempts: 0                    # incremented on each reopened cycle
pass: P-01                     # which pass discovered it
updated: <date>
```

`LEDGER.md` rows follow one fixed format: `| id | title | severity | status | next | updated |`. `next` names whichever role or the human is expected to act on the finding next: a canonical role name, `human`, or `—` for terminal states. The Verifier appends `⚠ needs-human` in the `next` column when `reopen_limit` consecutive reopenings are reached.

Ownership rules:

1. A finding file is the single source of truth for that finding. Everything about it — evidence, validation, remediation, verification — lives in that one file.
2. `LEDGER.md` is a derived index, not a second copy. One row per finding, nothing more.
3. The finding file is the source of truth for every status except pending triage decisions. Triage decisions live in LEDGER.md first: when a ledger row shows a triage status that is a legal triage transition from the file's frontmatter status (`reported` → `accepted`/`rejected`/`deferred`; member reversion to `accepted` on a rejected CF), the next agent session that touches that finding applies the decision to the file before any other work. Applying a pending triage decision is executing the human's call, not taking ownership of a triage status (§5). For all other disagreements, fix the ledger to match the file.

## 3. Roles

A role is a function, not an agent identity. The default assignment is Codex for Planner, Auditor, and Verifier, and Claude for Remediator — but any role can be played by any capable agent, and a third agent can join without changing this file. One rule is hard: **the Verifier is never the agent or session that produced the fix it verifies.** Prefer a different agent; the minimum acceptable substitute is a different, fresh session of the same agent.

The only mandatory human step in the cycle is Triage. Everything else runs agent-to-agent through files.

### Planner
- **Mission:** inventory the project and its norms; produce dimensions, a unit map, and a pass queue.
- **Default agent:** Codex, one session.
- **Reads:** XCHECK.md; the project's normative documents; a structural listing of the project. Not the full content of every unit.
- **Writes:** AUDIT.md.
- **Stop conditions:** AUDIT.md exists, with at least one dimension backed by a norm and a non-empty pass queue.
- **Forbidden:** filing findings; reading unit content end to end "to get a feel for it" — inventory only.

### Auditor
- **Mission:** run one pass — one dimension against a batch of units — and record findings; also give disputed findings one written round of objection.
- **Default agent:** Codex, one session per pass (or a split of one, §4 rule 4).
- **Reads:** XCHECK.md; AUDIT.md; the pass charter; the units in the charter's scope; the norm the charter cites; templates/pass.md and templates/finding.md, copied when starting a pass or filing a finding.
- **Writes:** `findings/`, `passes/P-NN-<dimension>.md`, new rows in LEDGER.md; the AUDIT.md pass queue — tick the completed pass's checkbox; append split remainders as new queued passes; one round of written objection in a finding file when the Remediator disputes it (§5, §9 rule 3).
- **Dedup rule:** before filing a finding, check it against ledger titles only — not finding bodies, not memory of earlier passes. A matching title gets new evidence appended to the existing finding, not a duplicate.
- **Stop conditions:** charter's unit range exhausted, or `max_findings_per_pass` (§10) reached, or context budget exhausted.
- **Forbidden:** fixing anything; exceeding the charter's unit range instead of splitting it; filing a finding that does not meet the Evidence Standard (§6).

### Triage
- **Mission:** decide which findings get worked.
- **Default agent:** human. The one mandatory gate in the cycle.
- **Reads:** LEDGER.md; finding files for any finding whose title alone doesn't settle the call.
- **Writes:** the status column in LEDGER.md only — `reported` to `accepted`, `rejected`, or `deferred`. Batch decisions are allowed, e.g. all `critical` and `major` findings to `accepted` in one action. Rejecting a CF also reverts every finding in its `members:` list to `accepted` (write the ledger rows; agents apply to files per §2).
- **Stop conditions:** every `reported` row under review has a decision.
- **Forbidden:** editing finding file body sections — frontmatter sync for triage decisions is carried out by agent sessions per §2; setting any status other than `accepted`, `rejected`, or `deferred`.
- **Agent-as-pen:** the human may run triage through an interactive agent session. The agent presents findings and records the human's stated decisions in LEDGER.md verbatim; the decisions remain the human's. An agent must never accept, reject, or defer a finding on its own judgment, and must not batch-infer decisions the human did not state ("everything else rejected" counts only if the human said it).

### Remediator
- **Mission:** fix a batch of accepted findings.
- **Default agent:** Claude, one or more sessions.
- **Reads:** XCHECK.md; AUDIT.md; the charter (a list of finding IDs); the finding files in scope; templates/plan.md and templates/class-finding.md, copied when opening a remediation plan or a class finding.
- **Writes:** `plans/RP-NNNN.md`; edits to the project material; the Validation and Remediation sections of each finding file; LEDGER.md.
- **Mandatory in-session sequence:** **validate → census → plan → fix → self-check.** No step is skipped or reordered (§7, §8 rule 1). Self-check means re-running the finding's own "how to verify" procedure before marking it fixed — catching what the Verifier would catch anyway, before it costs a reopen cycle.
- **Stop conditions:** every finding in the charter reaches `fixed`, `disputed`, `obsolete`, or `superseded-by-class`; or `remediation_batch_size` (§10) is exhausted; or context budget exhausted.
- **Forbidden:** touching a finding outside the charter; marking a finding `fixed` without a Remediation section naming what changed and where; verifying its own fix.

### Verifier
- **Mission:** give a binding verdict on a batch of fixed findings.
- **Default agent:** Codex, one or more sessions.
- **Reads:** XCHECK.md; AUDIT.md; the charter; the finding files in scope; the diffs or changes the Remediation section points to.
- **Writes:** the Verification section of each finding file; LEDGER.md (`closed` or `reopened`).
- **Hard rule:** never the agent or session that produced the fix. **Adversarial stance:** *"Your job is to prove the fix wrong, not to confirm it."*
- **Stop conditions:** every finding in the charter has a verdict.
- **Forbidden:** closing a finding without running its "how to verify" procedure; skipping the adversarial check of the surrounding change; verifying its own fix.

## 4. Session Protocol

Eight rules. They apply to every session, in every role.

1. **Charter required.** No session starts without an exact scope and stop conditions — e.g. "dimension X, units 4–6, stop after 15 findings" for an Auditor, or "findings F-0012..F-0019" for a Remediator.
2. **Fresh eyes.** State comes only from files. A session does not inherit conclusions from earlier sessions by memory. Anything a session acts on is re-verified against its source at the moment it is touched, regardless of what any prior session recorded about it.
3. **Explicit reading list.** Read XCHECK.md, AUDIT.md, your charter, the files the ledger points you to, and the artifact templates you instantiate. Nothing else. Reading the whole project "for context" is forbidden.
4. **Stop conditions are sacred.** Scope overflow: split the charter and write the remainder into the pass queue in AUDIT.md; skimming through to the end anyway is forbidden. Context running out: record what was covered and what was not, then exit cleanly.
5. **Coverage report is mandatory.** Every pass states explicitly what it did not cover. A silent gap is a protocol violation, not an acceptable shortcut.
6. **No finding quota.** Caps (§10) are upper bounds, not targets, and there is no minimum. A clean pass — zero findings — is a valid result and is recorded as one.
7. **Role launch is a one-liner.** A human starts a session with a single line: `Read audit/XCHECK.md. Role: <Role>. Charter: <charter text>.`
8. **One active writing session per project at a time.** Coordination in v1 is manual: one human relays the baton between sessions. Concurrent writers are out of scope. Launcher tooling and scripted chains must serialize the same way: never start a writing session while another is active.

## 5. Finding Lifecycle

```
reported    → (Triage)      → accepted | rejected | deferred
accepted    → (Remediator)  → validated | disputed | obsolete
validated   →                  planned → fixed
fixed       → (Verifier)    → closed | reopened
reopened    →                  back to the Remediator (planned → fixed again), attempts += 1
disputed    → (Auditor/human) → accepted | withdrawn
any status  → (Remediator)  → superseded-by-class (absorbed by a CF finding, §8)
superseded-by-class → (Triage)   → accepted (CF rejected at triage, §8 rule 4)
```

| From | Event / actor | To |
|---|---|---|
| `reported` | Triage decides | `accepted`, `rejected`, or `deferred` |
| `accepted` | Remediator confirms the evidence | `validated` |
| `accepted` | Remediator cannot confirm the evidence | `disputed` |
| `accepted` | Remediator finds the defect already gone | `obsolete` |
| `validated` | Remediator writes a plan | `planned` |
| `planned` | Remediator applies the fix | `fixed` |
| `fixed` | Verifier confirms | `closed` |
| `fixed` | Verifier rejects | `reopened` |
| `reopened` | returns to the Remediator | `planned`, then `fixed` again; the Remediator increments `attempts` |
| `disputed` | Auditor or human sides with the finding | `accepted` |
| `disputed` | Auditor or human sides against the finding | `withdrawn` |
| any | Remediator opens a class finding for its pattern | `superseded-by-class` |
| `superseded-by-class` | Triage rejects the CF | `accepted` |

Status ownership is the rule that keeps five roles from stepping on each other: *"One status, one owner: triage statuses are changed only by the human; validated/planned/fixed only by the Remediator; closed/reopened only by the Verifier; disputed resolution by the Auditor or the human. Never touch a status you do not own."*

Definitions:

- **`obsolete`** — validation shows the defect is already gone: an unrelated edit removed it before remediation touched it.
- **`disputed`** — the quote is not found in the material, or the problem is not substantively confirmed.
- **`reopen_limit` breach** — `reopen_limit` (default 2, §10) consecutive `reopened` verdicts on the same finding set the ledger flag `⚠ needs-human`; the automatic cycle stops and a human decides what happens next.
- **`withdrawn`** — a `disputed` finding resolved against the Auditor; it closes without a fix.
- **`superseded-by-class`** — set immediately on every member finding when a class finding is opened for its pattern (§8 rule 4); reverts to `accepted` if the human rejects the CF at triage.

## 6. Evidence Standard

Evidence is the anti-hallucination mechanism at the center of this methodology. Six rules govern every finding:

1. **The quote is the primary anchor.** Character-exact, findable by a literal search in the material. A line number is a hint, not an anchor — text moves as the material is edited.
2. **A norm reference is mandatory.** A finding is a discrepancy between the material and a norm: a style contract, a spec, an ADR, a sourced fact, or internal consistency — in which case the contradicting passage is itself quoted as the norm. No norm, no finding: it is an opinion, and its severity is capped at `info`.
3. **"How to verify the fix" is written by the Auditor at creation time**, while the defect is in front of them. It is never written by the Verifier after the fact.
4. **A finding without evidence meeting this standard is invalid by construction.** It must not reach Triage.
5. **Locators follow the material kind.** Code: `file:line` plus the enclosing symbol name. Text: file, section, and the quote itself.
6. **Auditor-authored sections of a finding are append-only for other roles.** Evidence, Why this is a defect, and How to verify the fix are written once, by the Auditor; other roles may add to them but not rewrite them. The only in-place edit allowed is a locator update (§9 rule 1).

## 7. Validation & Verification

Two checks apply the Evidence Standard at two different points in the cycle.

**Validation** is the mandatory first step of remediation (§3). Before planning a fix, the Remediator confirms the finding against the source: the quote exists, and the problem is real. This is close to free — the source has to be read to plan the fix anyway. Failure routes the finding to `disputed` (problem not confirmed) or `obsolete` (defect already gone).

**Verification** is the Verifier's binding verdict on a `fixed` finding, and it has two parts. First, run the procedure the Auditor wrote in "How to verify the fix" against the current material. Second, adversarially inspect the surroundings of the change for collateral damage that procedure alone would not catch. The verdict is `closed` or `reopened`. Evidence for a `reopened` verdict is held to the same Evidence Standard (§6) as an original finding — an exact quote and a norm, not an impression. When a fix installed an automated guard, verify the guard adversarially: plant the defect it claims to catch in a temporary copy and expect the guard to fail it — a guard that passes its own selftest but not a live mutation is a hole, not a defense.

## 8. Class Escalation

A finding may be one instance of a systematic defect rather than a one-off. Escalation exists to catch that and fix it globally instead of one instance at a time.

1. **Census is the mandatory second step of remediation**, after validation and before planning. The Remediator formulates the finding's pattern and searches for it across the whole project — not only in the finding's own unit. The search procedure fits the material (a literal-text or pattern search across files; a targeted read across sections where the pattern could recur) and must be recorded precisely enough for someone else to rerun it.
2. **Escalation threshold: `class_threshold` instances (default 3, §10).** Below threshold, fix the instance found and note its siblings in the finding file. At or above threshold, open a class finding.
3. **A class finding (`CF-NNNN`) contains:** an exact definition of the pattern and its search procedure; a full census of instances with locators, including any the original pass missed; the root cause — most often a norm that is missing, ambiguous, or inconsistently enforced; and a global strategy on an increasing scale:
   - (a) fix every instance;
   - (b) (a), plus fix the norm so the class cannot recur;
   - (c) (b), plus an automated guard where one is possible — a search check, a lint rule, a style script — so the class becomes structurally hard to reintroduce.
4. **A CF finding passes through the same Triage gate** as any other finding — status `reported`, the human decides. The single human gate does not multiply, but a global change never bypasses it. On creation, every member finding moves to `superseded-by-class` immediately; if the human rejects the CF at triage, members revert to `accepted` for point fixes instead. The Remediator continues the current session with the rest of the batch — the CF waits for the next triage round.
5. **Class verification is a re-census**, performed by the Verifier: rerun the recorded search procedure and expect zero instances, or explicitly documented exceptions. This is what catches "fixed 12 of 15."
6. **Norm write-back is part of the fix, not a side effect.** Strategies (b) and (c) edit the project's normative documents in the same remediation, not as optional follow-up work. If the project maintains a persistent agent-memory or lessons store, record the norm change there in the same remediation.
7. **The five-step Remediator sequence maps onto an accepted CF as follows:** validate = re-run the recorded census procedure; census = already done (the CF's census IS the scope); plan = the Global strategy section; fix as usual; self-check = re-run the census procedure expecting zero instances or documented exceptions.
8. **Norm ratification gate.** If a class fix under strategy (b) or (c) changes a norm, relies on a norm that another normative source contradicts (including machine registries and configuration files), or must pick a side in any conflict between norms — the Remediator stops after writing the plan and routes the conflict to the norm owner through triage before executing. Documenting a norm conflict and proceeding anyway is a protocol violation for class fixes: a class fix in the wrong direction multiplies one error across the whole corpus.

## 9. Failure Modes

1. **Stale finding.** The material moved since the finding was filed, and the quote is no longer found by a literal search. Re-locate by meaning. If the defect is gone, mark `obsolete`. If it moved, update the locator and proceed. Fixing "from memory of where it used to be" is forbidden.
2. **Non-converging cycle.** `reopen_limit` (§10) consecutive `reopened` verdicts on one finding set `⚠ needs-human` in the ledger (§5). The automatic cycle does not keep spinning on its own.
3. **Agent dispute.** The Auditor insists on a finding the Remediator marked `disputed`. One written round of objection in the finding file, then the human decides. Ping-pong between agents is forbidden.
4. **Ledger drift.** The finding file is truth; LEDGER.md is a derived index (§2). Whoever notices a mismatch fixes the ledger silently — except a pending triage decision, which flows the other way: apply it to the file per §2 rule 3.
5. **Charter overflow.** The remainder of an overflowing charter becomes a new queued pass in AUDIT.md (§4 rule 4). A session interrupted mid-charter must still leave a coverage report (§4 rule 5) — otherwise the pass counts as not done at all.
6. **Concurrent human edits to the material.** Not forbidden. The Evidence Standard (§6) self-protects: a quote either still matches, or the finding falls into the stale-finding path above.
7. **Git.** If the project is under git: remediation commits reference finding IDs, and verification reads diffs. If not: the finding's Remediation section lists the files or sections changed. Git is an amplifier, not a requirement.

## 10. Configuration Defaults

| Key | Default | Meaning |
|---|---|---|
| `max_findings_per_pass` | 15 | Upper bound on findings recorded by one Auditor pass (§4 rule 6). |
| `remediation_batch_size` | 8 | Upper bound on findings worked by one Remediator session (§3). |
| `class_threshold` | 3 | Minimum instance count that escalates a finding to a class finding (§8 rule 2). |
| `reopen_limit` | 2 | Consecutive `reopened` verdicts on one finding before it is flagged `⚠ needs-human` (§5, §9 rule 2). |

AUDIT.md may override any of these defaults per project.
