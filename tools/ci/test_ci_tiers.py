"""Unit tests for how ci.yml's two tiers are wired through the workflows.

stdlib only, like the other suites in tools/ci: the static job that runs them installs
nothing beyond its linters, so these read the workflow files as text. They pin the
placement of jobs in the run after a merge and in the nightly run (docs/ci-agentic.md,
"The tiers"), so moving a job between them is a change that shows in a diff of this
file too.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

WORKFLOWS = Path(__file__).resolve().parents[2] / ".github" / "workflows"


def text(name: str) -> str:
    return (WORKFLOWS / name).read_text(encoding="utf-8")


def job(workflow: str, name: str) -> str:
    """The source of one top-level job: from `  name:` to the next job or the end."""
    body = text(workflow)
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", body, re.S | re.M
    )
    if not match:
        raise AssertionError(f"no job {name!r} in {workflow}")
    return match.group(1)


def condition(workflow: str, name: str) -> str:
    """The job-level `if:` of a job, joined onto one line; empty when it has none."""
    lines = job(workflow, name).splitlines()
    for i, line in enumerate(lines):
        if line.startswith("    if:"):
            out = [line.removeprefix("    if:").strip()]
            for more in lines[i + 1 :]:
                if more.startswith("      ") and not more.startswith("    #"):
                    out.append(more.strip())
                else:
                    break
            return " ".join(out).lstrip(">-").strip()
    return ""


class Triggers(unittest.TestCase):
    def test_ci_runs_after_a_merge_and_nightly_and_on_request(self):
        ci = text("ci.yml")
        self.assertRegex(ci, r"(?m)^  push:\n    branches: \[ main \]")
        # 13:17 UTC starts the run at about 19:47 UTC once GitHub's delay is added (see ci.yml).
        self.assertRegex(ci, r"(?m)^  schedule:\n    - cron: '17 13 \* \* \*'")
        self.assertIn("workflow_dispatch:", ci)

    def test_the_label_workflow_reacts_only_to_ci_deep_on_a_pull_request_from_this_repository(self):
        label = text("deep-on-label.yml")
        self.assertRegex(label, r"types: \[ labeled \]")
        self.assertIn("github.event.label.name == 'ci:deep'", label)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", label)
        self.assertNotIn("pull_request_target", label)
        self.assertIn("-f tier=all", label)


class TierIsPassedDown(unittest.TestCase):
    def test_the_changes_job_decides_the_tier(self):
        changes = job("ci.yml", "changes")
        self.assertIn("tier: ${{ steps.detect.outputs.tier }}", changes)
        self.assertIn("tier=t2", changes)
        self.assertIn('echo "tier=$tier" >> "$GITHUB_OUTPUT"', changes)

    def test_the_builds_and_the_core_checks_get_it(self):
        ci = text("ci.yml")
        self.assertEqual(ci.count("tier: ${{ needs.changes.outputs.tier }}"), 2)
        self.assertIn("_build.yml", job("ci.yml", "build-and-test"))
        self.assertIn("tier: ${{ needs.changes.outputs.tier }}", job("ci.yml", "build-and-test"))
        self.assertIn("tier: ${{ needs.changes.outputs.tier }}", job("ci.yml", "core"))

    def test_the_reusable_workflows_default_to_everything(self):
        for workflow in ("_build.yml", "_ci-core.yml"):
            with self.subTest(workflow=workflow):
                self.assertRegex(
                    text(workflow), r"(?m)^      tier:\n(?:        .*\n)*?        default: all\n"
                )


class WhatRunsAfterAMerge(unittest.TestCase):
    NIGHTLY_ONLY_CORE = ("coverage", "coverage-qt", "abi-gate", "ffmpeg-validate")
    STAYS_CORE = ("adm-validate", "hearth-validate")

    def test_the_slow_core_jobs_wait_for_the_nightly_run(self):
        for name in self.NIGHTLY_ONLY_CORE:
            with self.subTest(job=name):
                self.assertIn("inputs.tier != 't2'", condition("_ci-core.yml", name))

    def test_the_other_core_jobs_stay(self):
        for name in self.STAYS_CORE:
            with self.subTest(job=name):
                self.assertNotIn("tier", condition("_ci-core.yml", name))

    def test_the_trend_publishers_that_read_ffmpeg_validate_run_where_it_runs(self):
        """On a run that records the trend, after the job whose scores they publish: the
        nightly run and a dispatch on main, since the run after a merge leaves that job out."""
        for name in ("persist-external-comparison-trend", "persist-object-quality-trend"):
            with self.subTest(job=name):
                cond = condition("_ci-core.yml", name)
                self.assertIn("inputs.record_trend", cond)
                self.assertIn("needs: ffmpeg-validate", job("_ci-core.yml", name))

    def test_the_comparison_publisher_also_runs_after_a_failed_ffmpeg_validate(self):
        """A gate that fails after the scoring steps must not discard what they measured.

        2026-10-11: the nightly scored every AC-4 encode leg and decode stream, uploaded them,
        and failed a coverage check three steps later. The publisher needed the whole job to
        pass, was skipped, and the first night anything wrote the AC-4 encode series left no
        point on it.
        """
        name = "persist-external-comparison-trend"
        cond = condition("_ci-core.yml", name)
        self.assertIn("!cancelled()", cond)
        self.assertIn("needs.ffmpeg-validate.result == 'success'", cond)
        self.assertIn("needs.ffmpeg-validate.result == 'failure'", cond)
        # Skipped (tier t2) and cancelled stay out: the condition names the two results it
        # takes rather than "anything but skipped".
        self.assertNotIn("'cancelled'", cond)
        self.assertNotIn("'skipped'", cond)
        body = job("_ci-core.yml", name)
        # The two downloads a failed run may lack are tolerated only on a run that failed: on
        # one that passed, a missing artifact is a real fault and fails the job as it did.
        self.assertEqual(
            body.count("continue-on-error: ${{ needs.ffmpeg-validate.result != 'success' }}"), 2
        )
        # ...and what needs them runs only when they arrived, or on a run that passed.
        self.assertIn(
            "needs.ffmpeg-validate.result == 'success' || hashFiles('results/trend.json') != ''",
            body,
        )
        self.assertIn("hashFiles('results/spectrograms/*.png') != ''", body)

    def test_the_object_publisher_still_needs_a_passing_ffmpeg_validate(self):
        """Its scores come from the job's last step: a failed run has none to publish."""
        cond = condition("_ci-core.yml", "persist-object-quality-trend")
        self.assertNotIn("needs.ffmpeg-validate.result", cond)

    def test_the_ac4_scores_are_uploaded_even_when_the_scorer_fails_a_floor(self):
        """Both scorers write --json-out before they exit non-zero, so a breach is a point."""
        ffmpeg = job("_ci-core.yml", "ffmpeg-validate")
        for scorer, upload in (
            ("ac4-decode-score", "Upload AC-4 decode quality results"),
            ("ac4-encode-score", "Upload AC-4 encode quality results"),
        ):
            with self.subTest(scorer=scorer):
                self.assertRegex(ffmpeg, rf"(?m)^        id: {scorer}\n        run: \|")
                following = r"(?=^      - |^      #|\Z)"
                step = re.search(
                    rf"(?ms)^      - name: {re.escape(upload)}\n(.*?){following}", ffmpeg
                )
                self.assertIsNotNone(step, upload)
                self.assertIn(
                    f"if: ${{{{ !cancelled() && steps.{scorer}.outcome != 'skipped' }}}}",
                    step.group(1),
                )

    def test_the_appimage_is_nightly_only(self):
        self.assertIn("inputs.tier != 't2'", condition("_build.yml", "linux-appimage"))

    PUBLISHERS = (
        ("ci.yml", "persist-performance-trend"),
        ("ci.yml", "performance-trend-arm64"),
        ("_build.yml", "quality-trend"),
        ("_ci-core.yml", "persist-external-comparison-trend"),
        ("_ci-core.yml", "persist-object-quality-trend"),
    )

    def test_one_rule_says_which_runs_record_the_trend(self):
        """After a merge, nightly, and a dispatch on main that names no legs.

        Decided once, by the `changes` job, and read by every publisher. Each used to ask the
        event for itself and they did not agree: the performance series was written by the run
        after a merge alone, the comparison series by the nightly alone, and a dispatch wrote
        nothing, so no one run could record every series.
        """
        changes = job("ci.yml", "changes")
        self.assertIn("record_trend: ${{ steps.detect.outputs.record_trend }}", changes)
        self.assertIn('if [ "$REF" = "refs/heads/main" ] && [ -z "${LEGS:-}" ]; then', changes)
        self.assertIn("push|schedule|workflow_dispatch) record_trend=true ;;", changes)
        passed = "${{ needs.changes.outputs.record_trend == 'true' }}"
        self.assertIn(f"persist_quality_trend: {passed}", job("ci.yml", "build-and-test"))
        self.assertIn(f"record_trend: {passed}", job("ci.yml", "core"))
        for name in ("persist-performance-trend", "performance-trend-arm64"):
            with self.subTest(job=name):
                self.assertIn(
                    "needs.changes.outputs.record_trend == 'true'", condition("ci.yml", name)
                )
        for workflow, name in self.PUBLISHERS:
            with self.subTest(job=name):
                self.assertNotIn("github.event_name", condition(workflow, name))

    def test_the_performance_publishers_do_not_wait_for_the_build_matrix(self):
        """They build the benchmarks and measure them by themselves.

        From 2026-10-12 02:12Z none of the 19 runs after a merge that finished in the next day
        (136 merges) recorded a speed or memory figure: the publisher needed all of
        build-and-test green, and it was red on a browser test, the wheels, an ESP32 image and
        a macOS test while the Linux GCC leg passed in every one of those runs.
        """
        for name in ("persist-performance-trend", "performance-trend-arm64"):
            with self.subTest(job=name):
                needs = re.search(r"(?m)^    needs: \[(.*)\]$", job("ci.yml", name))
                self.assertIsNotNone(needs)
                self.assertNotIn("build-and-test", needs.group(1))
                cond = condition("ci.yml", name)
                self.assertNotIn("build-and-test", cond)
                # A batch of merges with no code in it measures nothing, as before.
                self.assertIn("needs.changes.outputs.code == 'true'", cond)
        # The arm64 leg's artifact is optional (continue-on-error on its download), so its
        # failure must not cost the x86 series its record.
        cond = condition("ci.yml", "persist-performance-trend")
        self.assertIn("needs.performance-trend-arm64.result != 'cancelled'", cond)
        self.assertNotIn("needs.performance-trend-arm64.result == 'success'", cond)

    def test_the_quality_trend_records_the_legs_that_passed_when_another_failed(self):
        """One platform that ran is enough, and a leg's own gates decide whether it is recorded.

        From 2026-10-12 15:20Z a Hearth test on the macOS leg kept the Linux and Windows
        figures of every later merge off the trend: the job needed every platform's job-call
        to pass.
        """
        trend = condition("_build.yml", "quality-trend")
        self.assertIn("!cancelled()", trend)
        self.assertIn("inputs.persist_quality_trend", trend)
        for platform in ("windows", "linux", "macos"):
            self.assertIn(
                f"""contains(fromJSON('["success","failure"]'), needs.build-{platform}.result)""",
                trend,
            )
        self.assertEqual(trend.count("||"), 2)
        self.assertNotIn('"skipped"', trend)
        # With no artifact at all (every leg failed) there is nothing to append or push.
        body = job("_build.yml", "quality-trend")
        self.assertEqual(body.count("if: hashFiles('results/**/*.json') != ''"), 2)
        # What keeps a broken leg's figures out of the history, which the per-channel floors
        # are derived from: a leg uploads its results under the default `if`, after its gates.
        for workflow in ("_ci-linux.yml", "_ci-macos.yml", "_ci-windows.yml"):
            with self.subTest(workflow=workflow):
                self.assertIn(
                    "        if: matrix.gold_reference && inputs.persist_quality_trend\n",
                    text(workflow),
                )

    def test_the_lane_range_is_what_merged_since_verified_and_satellites_are_direct(self):
        changes = job("ci.yml", "changes")
        self.assertIn("git/ref/heads/verified", changes)
        self.assertIn("compare/$verified...$SHA", changes)
        self.assertIn("--satellites-direct", changes)
        self.assertIn('[ "$tier" = t2 ]', changes)

    def test_a_failed_lookup_is_told_by_its_exit_status_not_by_what_it_printed(self):
        # gh prints a failed request's JSON body on stdout, so a lookup that only
        # checked for output took the 404 body of a missing `verified` ref for a
        # commit id and logged it as one.
        changes = job("ci.yml", "changes")
        self.assertIn('if verified="$(gh api "repos/$REPO/git/ref/heads/verified"', changes)
        self.assertIn('[ "${#verified}" -eq 40 ]', changes)
        self.assertIn('if files="$(gh api --paginate "repos/$REPO/compare/$verified', changes)
        self.assertNotIn("|| true)", changes)

    def test_the_nightly_run_and_a_dispatch_force_every_lane(self):
        changes = job("ci.yml", "changes")
        self.assertIn('[ "$EVENT_NAME" = "schedule" ]', changes)
        self.assertIn('[ "$EVENT_NAME" = "workflow_dispatch" ]', changes)
        # The run after a merge no longer forces them.
        forcing = re.search(r"if \[ \"\$EVENT_NAME\" = \"merge_group\" \].*?then", changes, re.S)
        self.assertIsNotNone(forcing)
        self.assertNotIn("refs/heads/main", forcing.group(0))


if __name__ == "__main__":
    unittest.main()
