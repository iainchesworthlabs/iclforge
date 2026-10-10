"""Unit tests for how a Dependabot pull request gets into the merge queue.

stdlib only, like the other suites in tools/ci: these read the workflow file as text. They pin
the one thing that went wrong: the bit that makes GitHub queue a pull request was being set
with the Actions GITHUB_TOKEN, which the merge queue never acts on (and whose queue entries
get no merge_group run), so green Dependabot pull requests sat unmerged for days.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

WORKFLOW = (
    Path(__file__).resolve().parents[2] / ".github" / "workflows" / "dependabot-auto-merge.yml"
)


def text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def step(name: str) -> str:
    """The source of one step: from its `- name:` line to the next step or the end."""
    match = re.search(
        rf"^      - name: {re.escape(name)}\n(.*?)(?=^      - |\Z)", text(), re.S | re.M
    )
    if not match:
        raise AssertionError(f"no step {name!r} in {WORKFLOW.name}")
    return match.group(1)


class TheBitIsSetByAPerson(unittest.TestCase):
    ENABLE = "Enable auto-merge (non-major updates only)"

    def test_the_merge_queue_token_is_what_flips_the_bit(self):
        enable = step(self.ENABLE)
        self.assertIn("MERGE_QUEUE_TOKEN: ${{ secrets.MERGE_QUEUE_TOKEN }}", enable)
        self.assertIn('export GH_TOKEN="$MERGE_QUEUE_TOKEN"', enable)
        self.assertIn('gh pr merge --auto --squash "$PR_URL"', enable)

    def test_without_the_secret_it_warns_instead_of_failing_silently(self):
        enable = step(self.ENABLE)
        self.assertIn('if [ -n "$MERGE_QUEUE_TOKEN" ]', enable)
        self.assertIn("::warning title=MERGE_QUEUE_TOKEN is not set", enable)
        self.assertIn('export GH_TOKEN="$ACTIONS_TOKEN"', enable)

    def test_the_actions_token_is_never_the_only_one_wired_in(self):
        # GH_TOKEN as a step-level `${{ secrets.GITHUB_TOKEN }}` is the shape that
        # silently queued nothing; the Actions token may only be the fallback.
        self.assertNotRegex(text(), r"(?m)^\s+GH_TOKEN: \$\{\{ secrets\.GITHUB_TOKEN \}\}")

    def test_major_updates_are_still_left_for_a_person(self):
        enable = step(self.ENABLE)
        self.assertIn("steps.metadata.outputs.update-type != 'version-update:semver-major'", enable)

    def test_only_dependabot_pull_requests_from_this_owner_get_here(self):
        self.assertIn("github.event.pull_request.user.login == 'dependabot[bot]'", text())
        self.assertIn("github.repository_owner == 'iainchesworthlabs'", text())

    def test_the_header_says_whose_token_and_where_to_store_it(self):
        header = text().split("\nname:", 1)[0]
        self.assertIn("MERGE_QUEUE_TOKEN", header)
        self.assertIn("DEPENDABOT", header)
        self.assertIn("merge_group", header)


if __name__ == "__main__":
    unittest.main()
