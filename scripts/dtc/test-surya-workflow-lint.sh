#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Self-test for surya-workflow-lint.py, using synthetic workflow files.
#
# The reason this test exists is that the failure it detects cannot be detected
# by the thing that would report it: when Actions rejects a workflow file it
# produces no jobs and no annotations, so the linter itself has to be the one
# that is known-good.  Each case below is a shape that either actually broke a
# Surya run or would have, asserted in both directions (accepted / rejected) so
# a linter that always passes is caught too.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
lint="$here/surya-workflow-lint.py"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' 0

# A padding block of $1 lines, used to push a run: string past the length limit.
pad() {
	n=$1
	i=0
	while [ "$i" -lt "$n" ]; do
		printf '          echo "filler line %s for the block length"\n' "$i"
		i=$((i + 1))
	done
}

cat > "$tmp/good.yml" <<'YML'
name: Good
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: Nothing to see
        run: |
          echo hello
YML
if ! "$lint" "$tmp/good.yml" > "$tmp/good.out" 2>&1; then
	echo 'a plain valid workflow was rejected:' >&2
	cat "$tmp/good.out" >&2
	exit 1
fi

# Case 1: an expression inside a run block longer than the limit.  This is the
# real 2026-11-28 failure: a comment line carrying "${{ github.sha }}" inside
# a 26 KB assemble step.  yaml.safe_load accepts it; Actions refuses the file.
{
	printf 'name: Long\non:\n  pull_request:\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - name: Big\n        run: |\n'
	pad 800
	printf '          printf "%%s\\n" "${{ github.sha }}"\n'
	printf '      - name: Small\n        run: echo ok\n'
} > "$tmp/long-expr.yml"
if "$lint" "$tmp/long-expr.yml" > "$tmp/long.out" 2>&1; then
	echo 'the over-long run string with an expression was accepted' >&2
	exit 1
fi
grep -qF 'contains an expression' "$tmp/long.out"
grep -qF 'no jobs, no logs' "$tmp/long.out"

# The same padding without an expression must pass: the limit only applies to
# strings that contain one, and a linter that flagged every long step would be
# ignored within a week.
{
	printf 'name: Long but plain\non:\n  pull_request:\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - name: Big\n        run: |\n'
	pad 800
	printf '      - name: Small\n        run: echo ok\n'
} > "$tmp/long-plain.yml"
"$lint" "$tmp/long-plain.yml" > "$tmp/plain.out" 2>&1 || {
	echo 'a long run block without an expression was rejected:' >&2
	cat "$tmp/plain.out" >&2
	exit 1
}

cat > "$tmp/bad-needs.yml" <<'YML'
name: Needs
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    needs: [not-there]
    steps:
      - run: echo ok
YML
if "$lint" "$tmp/bad-needs.yml" > "$tmp/needs.out" 2>&1; then
	echo 'an unknown needs: job was accepted' >&2
	exit 1
fi
grep -qF 'needs unknown job not-there' "$tmp/needs.out"

# A single-job name under needs: is a string, not a list; iterating it
# character by character used to report "needs unknown job s" for "surya-dtb".
cat > "$tmp/needs-string.yml" <<'YML'
name: Needs string
on:
  pull_request:
jobs:
  surya-dtb:
    runs-on: ubuntu-latest
    steps:
      - run: echo ok
  other:
    runs-on: ubuntu-latest
    needs: surya-dtb
    steps:
      - run: echo ok
YML
"$lint" "$tmp/needs-string.yml" > /dev/null 2>&1 || {
	echo 'needs: with a plain job name was rejected' >&2
	exit 1
}

cat > "$tmp/dup-key.yml" <<'YML'
name: Duplicate
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: one
        run: echo ok
      - name: two
        run: echo ok
        run: echo overwritten
YML
if "$lint" "$tmp/dup-key.yml" > "$tmp/dup.out" 2>&1; then
	echo 'a duplicated mapping key was accepted' >&2
	exit 1
fi
grep -qF 'duplicate key' "$tmp/dup.out"

cat > "$tmp/bad-bash.yml" <<'YML'
name: Bad bash
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: Unclosed
        run: |
          if grep -q x file; then
            echo no fi here
YML
if "$lint" "$tmp/bad-bash.yml" > "$tmp/bash.out" 2>&1; then
	echo 'a run block with unbalanced shell syntax was accepted' >&2
	exit 1
fi
grep -qF 'bash -n' "$tmp/bash.out"

# An expression with no closing brace parses as YAML and ships; the runner
# treats it as literal text, so a guard silently stops guarding.
cat > "$tmp/unbalanced.yml" <<'YML'
name: Unbalanced
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: Ouch
        if: "allow: ${{ github.event_name == 'pull_request' }"
        run: echo ok
YML
if "$lint" "$tmp/unbalanced.yml" > "$tmp/unb.out" 2>&1; then
	echo 'an unclosed expression was accepted' >&2
	exit 1
fi
grep -qF 'unbalanced expression' "$tmp/unb.out"

# A workflow that builds the flashable artifact but triggers only on push is
# the shape that produced "failure with zero jobs" and an afternoon of guessing.
cat > "$tmp/no-pr-trigger.yml" <<'YML'
name: No pull_request
on:
  push:
    branches:
      - nowhere-matched
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: zip
          path: out/Image.gz
YML
if "$lint" "$tmp/no-pr-trigger.yml" > "$tmp/trigger.out" 2>&1; then
	echo 'an artifact workflow with no pull_request trigger was accepted' >&2
	exit 1
fi
grep -qF 'does not trigger on pull_request' "$tmp/trigger.out"

# And the same file with the trigger present must pass, or that check is noise.
sed 's/  push:/  pull_request:/' "$tmp/no-pr-trigger.yml" > "$tmp/pr-trigger.yml"
"$lint" "$tmp/pr-trigger.yml" > /dev/null 2>&1 || {
	echo 'the same file with a pull_request trigger was rejected' >&2
	exit 1
}

# A gate whose paths are relative to the checkout silently reads nothing once
# the step has cd'd into the temporary installer directory.  This is the fault
# that made a passing-on-the-tree PHY gate fail only in CI, with a message that
# blamed the tree ("no driver under drivers/phy/qualcomm mentions ...").
cat > "$tmp/cwd-relative.yml" <<'YML'
name: Cwd sensitive
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: Assemble
        run: |
          work=$(mktemp -d)
          cd "$work/installer"
          for c in qcom,ufs-phy-qmp-v3; do
            files=$(grep -l ""$c"" drivers/phy/qualcomm/*.c || true)
            test -n "$files"
            if grep -Fxq CONFIG_X=y out/surya/.config; then
              echo built
            fi
          done
YML
if "$lint" "$tmp/cwd-relative.yml" > "$tmp/cwd.out" 2>&1; then
	echo 'a cwd-relative repo path in a step that left the checkout was accepted' >&2
	exit 1
fi
grep -qF "cwd-relative repo path 'drivers/phy/qualcomm/*.c'" "$tmp/cwd.out"
grep -qF "cwd-relative repo path 'out/surya/.config'" "$tmp/cwd.out"

# The same step anchored at $GITHUB_WORKSPACE must pass -- and a bare path in a
# step that never cd's is legitimate, since the checkout is then the cwd.
sed 's|drivers/phy/qualcomm|"$GITHUB_WORKSPACE/drivers/phy/qualcomm"|; s|out/surya/.config|"$GITHUB_WORKSPACE/out/surya/.config"|' \
	"$tmp/cwd-relative.yml" > "$tmp/cwd-anchored.yml"
"$lint" "$tmp/cwd-anchored.yml" > "$tmp/cwd2.out" 2>&1 || {
	echo 'the anchored form was rejected:' >&2
	cat "$tmp/cwd2.out" >&2
	exit 1
}
cat > "$tmp/repo-cwd.yml" <<'YML'
name: No cd
on:
  pull_request:
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: From the checkout
        run: |
          grep -l x drivers/phy/qualcomm/*.c
YML
"$lint" "$tmp/repo-cwd.yml" > /dev/null 2>&1 || {
	echo 'a relative path in a step that stays in the checkout was rejected' >&2
	exit 1
}

echo 'surya-workflow-lint self-test passed'
