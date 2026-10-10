"""Unit tests for how a Dependabot pull request gets into the merge queue.

stdlib only, like the other suites in tools/ci: these read the workflow files as text. They
pin the seams a change to one file could break without any other file noticing: the gate
calls _dependabot-enqueue.yml for a Dependabot pull request after `CI Status` passed (and is
not part of what `CI Status` waits for), the reusable workflow only asks for the queue and
keeps the rules that hold a major back, and the workflow that only set the auto-merge bit
(which never queued anything) is gone.
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
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text(workflow), re.S | re.M
    )
    if not match:
        raise AssertionError(f"no job {name!r} in {workflow}")
    return match.group(1)


class TheGateCallsTheEnqueue(unittest.TestCase):
    def test_it_runs_after_ci_status_and_only_when_that_passed(self):
        enqueue = job("pr-gate.yml", "enqueue-dependabot")
        self.assertIn("needs: [ci-status]", enqueue)
        self.assertIn("needs.ci-status.result == 'success'", enqueue)
        self.assertIn("uses: ./.github/workflows/_dependabot-enqueue.yml", enqueue)

    def test_only_for_a_dependabot_pull_request_from_this_repository(self):
        enqueue = job("pr-gate.yml", "enqueue-dependabot")
        self.assertIn("github.event_name == 'pull_request'", enqueue)
        self.assertIn("github.event.pull_request.user.login == 'dependabot[bot]'", enqueue)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", enqueue)

    def test_it_is_handed_the_pull_request_and_the_commit_the_gate_ran_on(self):
        enqueue = job("pr-gate.yml", "enqueue-dependabot")
        self.assertIn("pr_number: ${{ github.event.pull_request.number }}", enqueue)
        self.assertIn("head_sha: ${{ github.event.pull_request.head.sha }}", enqueue)

    def test_the_write_token_is_given_to_this_job_alone(self):
        gate = text("pr-gate.yml")
        # The workflow's own default stays read-only; the one write grant is this job's.
        self.assertRegex(gate, r"(?m)^permissions:\n  contents: read\n")
        enqueue = job("pr-gate.yml", "enqueue-dependabot")
        self.assertRegex(enqueue, r"permissions:\n\s+contents: write\n\s+pull-requests: write")
        self.assertEqual(len(re.findall(r"contents: write", gate)), 1)

    def test_ci_status_does_not_wait_for_it(self):
        # It is a leaf: if the gate's own result waited for it, the check that lets a
        # pull request merge would be waiting for the job that is waiting for it.
        status = job("pr-gate.yml", "ci-status")
        self.assertNotIn("enqueue-dependabot", status)


class TheReusableWorkflow(unittest.TestCase):
    def test_it_is_a_workflow_call_that_takes_the_pull_request_and_its_head(self):
        enqueue = text("_dependabot-enqueue.yml")
        self.assertRegex(enqueue, r"(?m)^on:\n  workflow_call:")
        self.assertRegex(enqueue, r"pr_number:\n\s+description:.*\n\s+type: number")
        self.assertRegex(enqueue, r"head_sha:\n\s+description:.*\n\s+type: string")

    def test_it_asks_for_the_queue_pinned_to_the_tested_commit(self):
        enqueue = job("_dependabot-enqueue.yml", "enqueue")
        self.assertIn("enqueuePullRequest", enqueue)
        self.assertIn("expectedHeadOid", enqueue)
        # An entry is a merge: it needs the write grant, and has a backstop on time.
        self.assertRegex(enqueue, r"contents: write")
        self.assertIn("timeout-minutes:", enqueue)

    def test_a_major_or_unlabelled_update_is_left_for_a_person(self):
        enqueue = job("_dependabot-enqueue.yml", "enqueue")
        self.assertIn("update-type:", enqueue)
        self.assertIn("semver-major", enqueue)
        # No entries at all is "unknown", which is not routine.
        self.assertRegex(enqueue, r'if \[ -z "\$kinds" \]')

    def test_it_only_joins_a_mergeable_open_pull_request_that_has_not_moved(self):
        enqueue = job("_dependabot-enqueue.yml", "enqueue")
        for word in ("CLEAN", "UNSTABLE", "isInMergeQueue", "isDraft", "headRefOid"):
            with self.subTest(word=word):
                self.assertIn(word, enqueue)

    def test_nothing_from_the_pull_request_is_checked_out_or_run(self):
        enqueue = text("_dependabot-enqueue.yml")
        self.assertNotIn("actions/checkout", enqueue)
        self.assertNotIn("${{ github.event", job("_dependabot-enqueue.yml", "enqueue"))


class TheOldWorkflowIsGone(unittest.TestCase):
    def test_the_auto_merge_bit_workflow_no_longer_exists(self):
        # It set the bit and nothing more, and under the queue the bit queued nothing.
        self.assertFalse((WORKFLOWS / "dependabot-auto-merge.yml").exists())


if __name__ == "__main__":
    unittest.main()
