"""Unit tests for check_changelog_fragment.py: where a pull request's changelog entry goes.

stdlib `unittest`, run by _static.yml's "Oracle unit tests" step. The rules are pure functions of
what the pull request changed, so the cases state that directly instead of building a git history.
"""

from __future__ import annotations

import unittest

import check_changelog_fragment as c

GOOD = "**A thing works.** More.\n"


def run(fragments=None, changed=(), deleted=False, allow=False, require=False):
    return c.problems_for(fragments or {}, list(changed), deleted, allow, require)


class WhereTheEntryGoes(unittest.TestCase):
    def test_a_well_formed_fragment_passes(self):
        self.assertEqual(
            run({"changelog.d/a-thing.fixed.md": GOOD}, ["changelog.d/a-thing.fixed.md"]), []
        )

    def test_editing_the_changelog_fails_and_names_the_fragment_instead(self):
        got = run(changed=["CHANGELOG.md"])
        self.assertEqual(len(got), 1)
        self.assertIn("changelog.d/<slug>.<type>.md", got[0])

    def test_the_release_may_edit_it_when_it_deletes_fragments(self):
        self.assertEqual(run(changed=["CHANGELOG.md"], deleted=True), [])

    def test_the_flag_allows_an_edit(self):
        self.assertEqual(run(changed=["CHANGELOG.md"], allow=True), [])


class FragmentShape(unittest.TestCase):
    def test_a_name_without_a_type_fails(self):
        self.assertEqual(len(run({"changelog.d/a-thing.md": GOOD})), 1)

    def test_an_unknown_type_fails(self):
        self.assertEqual(len(run({"changelog.d/a-thing.improved.md": GOOD})), 1)

    def test_every_type_is_accepted(self):
        for t in c.TYPES:
            self.assertEqual(run({f"changelog.d/x.{t}.md": GOOD}), [], t)

    def test_empty_fails(self):
        self.assertIn("empty", run({"changelog.d/x.added.md": " \n"})[0])

    def test_a_leading_dash_fails(self):
        self.assertIn("dash", run({"changelog.d/x.added.md": "- **A.** b\n"})[0])

    def test_no_bold_lead_in_fails(self):
        self.assertIn("bold lead-in", run({"changelog.d/x.added.md": "A thing.\n"})[0])

    def test_more_than_one_paragraph_is_allowed(self):
        self.assertEqual(run({"changelog.d/x.added.md": GOOD + "\nSecond paragraph.\n"}), [])


class RequiringAFragment(unittest.TestCase):
    def test_off_by_default(self):
        self.assertEqual(run(changed=["libs/ac3/src/x.cpp"]), [])

    def test_code_without_a_fragment_fails_when_required(self):
        self.assertEqual(len(run(changed=["libs/ac3/src/x.cpp"], require=True)), 1)

    def test_docs_tools_and_ci_need_none(self):
        changed = [
            "docs/a.md",
            "tools/x.py",
            ".github/workflows/a.yml",
            "planning/p.md",
            "README.md",
        ]
        self.assertEqual(run(changed=changed, require=True), [])

    def test_a_fragment_satisfies_it(self):
        self.assertEqual(
            run({"changelog.d/x.added.md": GOOD}, ["libs/ac3/src/x.cpp"], require=True), []
        )


if __name__ == "__main__":
    unittest.main()
