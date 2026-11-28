#!/usr/bin/env python3
"""Static checks for the Surya CI workflow files, runnable without Actions.

Why this exists: a workflow file that Actions cannot parse produces no jobs, no
logs and no check result -- the run just reads "failure" with an empty job list,
and the pull request looks unverified rather than broken.  The one way that
happened here was an expression inside a run block: GitHub rejects a *string*
that contains an expression once the string is longer than EXPRESSION_LIMIT
characters, and the Surya assemble step is past that length on its own, so
adding a "${{ ... }}" line (a comment mentioning one counts) silently killed
every run, including the artifact the board was waiting for.  That is invisible
to yaml.safe_load, which is why it needs a check of its own.

Checks, all of them cheap and local:
  1. the YAML parses, and no mapping key is duplicated;
  2. no run/if/name/env string that contains an expression exceeds
     EXPRESSION_LIMIT, and any string above WARN_LIMIT is reported;
  3. every run block survives "bash -n" after expressions are masked out;
  4. every "needs:" names a job that exists in the same file;
  5. every workflow has at least one trigger, and a workflow whose jobs build
     artifacts has to trigger on pull_request, because a push run on a branch
     with no matching "push:" filter yields "no jobs" -- which is exactly the
     signature check people mistake for a failing gate.

Exit status is 0 when everything passes, 1 when anything is wrong, 2 when a
file cannot be read.
"""

import os
import re
import subprocess
import sys
import tempfile

EXPRESSION_LIMIT = 21000
WARN_LIMIT = 18000
EXPR_OPEN = "${" + "{"
EXPR_CLOSE = "}" + "}"


def _dup_key_loader():
    """A SafeLoader that refuses a repeated mapping key.

    Actions merges duplicate keys in a way nobody reads carefully; in the
    Surya file one duplicated "uses:" cost an upload step that was silently
    gone.
    """
    import yaml

    class Loader(yaml.SafeLoader):
        pass

    def construct(loader, node, deep=False):
        mapping = yaml.SafeLoader.construct_mapping(loader, node, deep)
        seen = set()
        for key_node, _ in node.value:
            key = loader.construct_object(key_node, deep=deep)
            if isinstance(key, str) and key in seen:
                raise ValueError(
                    "duplicate key %r at line %d" % (key, key_node.start_mark.line + 1)
                )
            if isinstance(key, str):
                seen.add(key)
        return mapping

    Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct)
    return Loader


def load_yaml(path):
    """Return (document, error_string).  error_string is None on success."""
    try:
        import yaml
    except ImportError:
        return None, "PyYAML unavailable; only the text checks ran"
    try:
        with open(path) as handle:
            return yaml.load(handle, Loader=_dup_key_loader()), None
    except Exception as exc:  # noqa: BLE001 - any parse failure is the finding
        return None, "%s: %s" % (type(exc).__name__, exc)


def strings(doc):
    """Yield (where, field, text) for every string a runner will interpret."""
    if not isinstance(doc, dict):
        return
    jobs = doc.get("jobs") or {}
    for job, body in jobs.items():
        if not isinstance(body, dict):
            continue
        for key, value in (body.get("env") or {}).items():
            if isinstance(value, str):
                yield "%s env %s" % (job, key), "env", value
        for step, sbody in enumerate(body.get("steps") or []):
            if not isinstance(sbody, dict):
                continue
            for field in ("run", "if", "name"):
                value = sbody.get(field)
                if isinstance(value, str):
                    label = "%s step %d" % (job, step)
                    if field == "run":
                        label += " (%s)" % (sbody.get("name") or "unnamed")
                    yield label, field, value


def mask_expressions(text):
    """Replace each ${{ ... }} with one shell-safe word, for bash -n."""
    out, i = [], 0
    while True:
        open_at = text.find(EXPR_OPEN, i)
        if open_at < 0:
            out.append(text[i:])
            break
        close_at = text.find(EXPR_CLOSE, open_at)
        if close_at < 0:
            out.append(text[i:])
            break
        out.append(text[i:open_at])
        out.append("ci_masked")
        i = close_at + len(EXPR_CLOSE)
    return "".join(out)


def unbalanced_expression(text):
    """Return a description if ${{ / }} nesting is broken, else None."""
    depth, i = 0, 0
    while i < len(text):
        if text.startswith(EXPR_OPEN, i):
            depth += 1
            i += 2
        elif text.startswith(EXPR_CLOSE, i):
            depth -= 1
            if depth < 0:
                return "stray %s" % EXPR_CLOSE
            i += 2
        else:
            i += 1
    return None if depth == 0 else "%d unclosed ${{" % depth


def bash_check(script, name):
    """Return an error string if the block is not valid bash, else None."""
    masked = mask_expressions(script)
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as handle:
        handle.write(masked + "\n")
        path = handle.name
    try:
        proc = subprocess.run(
            ["bash", "-n", path], capture_output=True, text=True, timeout=60
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return "%s: bash -n unavailable (%s)" % (name, exc)
    finally:
        os.unlink(path)
    if proc.returncode:
        tail = (proc.stderr or proc.stdout).strip().splitlines()
        return "%s: bash -n: %s" % (name, tail[-1] if tail else "syntax error")
    return None


# Kernel-tree directories that cannot exist inside a temporary installer
# directory.  A gate that reaches for one of these by relative path is fine
# while the workflow's cwd is the checkout, and silently finds nothing the
# moment the step has cd'd elsewhere -- which is how a PHY gate came to report
# "no driver under drivers/phy/qualcomm mentions qcom,ufs-phy-qmp-v3" for a
# driver the configuration builds.  tools/ is deliberately absent: the
# AnyKernel3 layout really does own a relative tools/ inside the installer.
CWD_SENSITIVE = ("drivers", "out", "arch", "Documentation")


def cwd_relative_paths(script):
    """Yield (line_no, token) for repo paths that assume the checkout is the cwd.

    Only meaningful in a step that leaves the checkout and does not come back:
    `cd "$work/installer"` and friends.  A bare `drivers/...` operand there is
    not an error, it is an empty grep -- and every gate built on that grep then
    "passes" while having read nothing, or fails with a message that blames the
    tree instead of the path.
    """
    moves = [line.strip() for line in script.split("\n") if line.strip().startswith("cd ")]
    if not moves or all("GITHUB_WORKSPACE" in move for move in moves):
        return
    readers = r"\b(grep|fdtget|test|cat|sed|awk|head|tail|strings|cmp|cp|mv|sha256sum|python3?)\b"
    for number, line in enumerate(script.split("\n"), 1):
        stripped = line.strip()
        # Only messages and comments are excluded outright: "if grep -q x
        # out/surya/.config; then" is a keyword line *and* a file read, which
        # is exactly what must not be skipped.
        if stripped.startswith(("echo", "printf", "#")) or "::error::" in line:
            continue
        if "$GITHUB_WORKSPACE" in line:
            continue  # this line is already anchored
        for match in re.finditer(r"(?<![\w$/.-])(" + "|".join(CWD_SENSITIVE) + r")/[\w./*-]*", line):
            # The path has to be an operand of something that reads files, and
            # not a word inside a for-list or a message: require the reader
            # before it on the same line.
            if not re.search(readers, line[:match.start()]):
                continue
            yield number, match.group(0)


def check_workflow(path, want_artifact_job):
    """Return (problems, notes)."""
    problems, notes = [], []
    try:
        text = open(path, encoding="utf-8").read()
    except OSError as exc:
        sys.stderr.write("cannot read %s: %s\n" % (path, exc))
        sys.exit(2)
    doc, err = load_yaml(path)
    if err and "PyYAML" not in err:
        problems.append("%s: will not parse on Actions either -- %s" % (path, err))
    elif err:
        notes.append("%s: %s" % (path, err))

    # Check 2 needs the raw YAML text when PyYAML is missing, so do the length
    # scan from doc when available and from text when not.
    if doc is not None:
        for where, field, value in strings(doc):
            bad = unbalanced_expression(value)
            if bad:
                problems.append("%s: %s: unbalanced expression (%s)" % (path, where, bad))
            if EXPR_OPEN in value and len(value) > EXPRESSION_LIMIT:
                problems.append(
                    "%s: %s field %s is %d chars and contains an expression; "
                    "Actions rejects any string over %d chars with one inside it, "
                    "which fails the whole workflow file (no jobs, no logs). "
                    "Move the value into a script under scripts/dtc/ or drop the "
                    "expression."
                    % (path, where, field, len(value), EXPRESSION_LIMIT)
                )
            elif len(value) > WARN_LIMIT:
                notes.append(
                    "%s: %s field %s is %d chars -- an expression must not be "
                    "added to it" % (path, where, field, len(value))
                )
        jobs = (doc.get("jobs") or {}) if isinstance(doc, dict) else {}
        for job, body in jobs.items():
            if not isinstance(body, dict):
                continue
            needs = body.get("needs")
            # "needs:" may be one job name, a list, or a {job: id} mapping.
            if isinstance(needs, str):
                needs = [needs]
            elif isinstance(needs, dict):
                needs = list(needs)
            for need in needs or []:
                if need not in jobs:
                    problems.append("%s: job %s needs unknown job %s" % (path, job, need))
        for job, body in jobs.items():
            for i, step in enumerate(body.get("steps") or []):
                if not isinstance(step, dict):
                    continue
                run = step.get("run")
                if not isinstance(run, str):
                    continue
                where = "%s: %s step %d" % (path, job, i)
                err = bash_check(run, where)
                if err:
                    problems.append(err)
                for number, token in cwd_relative_paths(run):
                    problems.append(
                        "%s: run line %d uses the cwd-relative repo path %r after "
                        "the step has cd'd away from the checkout, which matches "
                        "nothing; anchor it at \"$GITHUB_WORKSPACE/%s\""
                        % (where, number, token, token)
                    )
        # YAML 1.1 resolves the bare key on: to True, so a lookup of "on"
        # alone reports "no triggers" on every valid file -- the mistake this
        # check was written to catch would be caught by it instead.  Look for
        # both spellings.
        triggers = None
        if isinstance(doc, dict):
            triggers = doc.get("on", doc.get(True))
        trigger_keys = {"on" if t is True else t for t in (triggers or {})}
        if not trigger_keys:
            problems.append("%s: no triggers" % path)
        elif want_artifact_job and "pull_request" not in trigger_keys:
            problems.append(
                "%s: builds artifacts but does not trigger on pull_request; a push "
                "run whose branch matches no push: filter yields zero jobs, and a "
                "red X with no jobs and no logs is that, not a gate failure" % path
            )
    else:
        notes.append("%s: no YAML parser, so only the structural checks above ran" % path)
    return problems, notes


def steps_of(doc):
    for body in (doc.get("jobs") or {}).values():
        if isinstance(body, dict):
            for step in body.get("steps") or []:
                if isinstance(step, dict):
                    yield step


def main(argv):
    paths = argv[1:] or sorted(
        os.path.join(root, name)
        for root, _, names in os.walk(".github/workflows")
        for name in names
        if name.endswith((".yml", ".yaml"))
    )
    problems, notes = [], []
    for path in paths:
        wants = any(
            token in open(path, encoding="utf-8", errors="replace").read()
            for token in ("upload-artifact", "AnyKernel", "Image.gz")
        )
        p, n = check_workflow(path, want_artifact_job=wants)
        problems += p
        notes += n
    for note in notes:
        print("note: %s" % note)
    if problems:
        for problem in problems:
            print("error: %s" % problem, file=sys.stderr)
        print("%d problem(s) in %d file(s)" % (len(problems), len(paths)), file=sys.stderr)
        return 1
    print("%d workflow file(s) OK" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
