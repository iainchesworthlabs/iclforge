"""Unit tests for how a Dependabot pull request gets into the merge queue.

stdlib only, like the other suites in tools/ci: these read the workflow file as text. They pin
the one thing that went wrong: the bit that makes GitHub queue a pull request was being set
with the Actions GITHUB_TOKEN, which the merge queue never acts on (and whose queue entries
get no merge_group run), so green Dependabot pull requests sat unmerged for days. The Actions
token also stays out as a FALLBACK: with the secret missing the first version still set the bit
that way, which left the pull request stuck even after the secret was added.
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

    def test_without_the_secret_it_fails_before_anything_is_enabled(self):
        # A missing or expired secret is a red check on the Dependabot PR, not a green
        # run that left the PR unmerged, and nothing may be enabled on the way: the
        # `gh pr merge` below must be unreachable once the secret is found empty.
        enable = step(self.ENABLE)
        self.assertIn('if [ -z "$MERGE_QUEUE_TOKEN" ]', enable)
        self.assertIn("::error title=MERGE_QUEUE_TOKEN is not set", enable)
        guard = enable.index('if [ -z "$MERGE_QUEUE_TOKEN" ]')
        bail = enable.index("exit 1", guard)
        merge = enable.index("gh pr merge")
        self.assertLess(guard, bail)
        self.assertLess(bail, merge)

    def test_the_actions_token_is_not_wired_in_at_all(self):
        # A bit set with the Actions token is inert under the merge queue, and
        # `gh pr merge --auto` leaves a bit that is already set alone, so using it as
        # a fallback left PRs stuck even after the person's token was in place.
        enable = step(self.ENABLE)
        for needle in ("secrets.GITHUB_TOKEN", "github.token", "ACTIONS_TOKEN"):
            self.assertNotIn(needle, enable)
        self.assertNotRegex(text(), r"(?m)^\s+GH_TOKEN: \$\{\{ secrets\.GITHUB_TOKEN \}\}")
        self.assertNotRegex(text(), r"export GH_TOKEN=\"\$\{?ACTIONS_TOKEN")

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

    def test_the_header_says_a_missing_secret_fails_the_job(self):
        header = text().split("\nname:", 1)[0]
        self.assertIn("FAILS", header)
        self.assertNotIn("falls back to GITHUB_TOKEN", header)


if __name__ == "__main__":
    unittest.main()
