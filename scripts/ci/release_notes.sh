#!/bin/bash
# Draft release notes: commits since the previous version tag, split into features
# and fixes, plus the tickets they reference. The draft is meant to be curated.
#
# usage: release_notes.sh TAG   (prints Markdown on stdout)
set -euo pipefail
TAG="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

# Previous tag: Jetstream builds compare with the previous Jetstream build (else the nearest
# v* tag); everything else with the nearest earlier v* tag, else the nearest legacy vk-* tag.
PREV=""
case "$TAG" in
  jetstream-*) PREV=$(git describe --tags --abbrev=0 --match 'jetstream-*' "${TAG}^" 2>/dev/null || true) ;;
esac
[ -n "$PREV" ] || PREV=$(git describe --tags --abbrev=0 --match 'v[0-9]*' "${TAG}^" 2>/dev/null \
       || git describe --tags --abbrev=0 --match 'vk-*' "${TAG}^" 2>/dev/null || true)
RANGE="${PREV:+$PREV..}$TAG"

python3 - "$RANGE" "$PREV" <<'PY'
import re, subprocess, sys
rng, prev = sys.argv[1], sys.argv[2]
log = subprocess.run(["git", "log", "--no-merges", "--format=%H%x1f%s%x1f%b%x1e", rng],
                     capture_output=True, text=True, check=True).stdout
fixes, features, other, tickets = [], [], [], {}
for rec in log.split("\x1e"):
    rec = rec.strip()
    if not rec:
        continue
    h, subj, body = (rec.split("\x1f") + ["", ""])[:3]
    # Housekeeping commits say nothing to a tester.
    if re.match(r"(Regenerate (world )?SPIR-V|Merge )", subj):
        continue
    # Some subjects are whole paragraphs: keep the first sentence / 200 characters.
    if len(subj) > 200:
        cut = re.search(r"^(.{40,200}?[.;:])\s", subj)
        subj = cut.group(1) if cut else subj[:200].rsplit(" ", 1)[0] + "…"
    for kind, num in re.findall(r"\b(Fixes|Refs)\s+#(\d+)", body + " " + subj):
        tickets.setdefault(int(num), set()).add(kind)
    line = f"- {subj} ({h[:10]})"
    low = subj.lower()
    if re.search(r"\b(fix|fixes|crash|bug|regression)\b", low) or "Fixes #" in body:
        fixes.append(line)
    elif re.search(r"\b(add|new|support|feature|allow|port|restore)\b", low):
        features.append(line)
    else:
        other.append(line)
out = []
if prev:
    out.append(f"Changes since {prev}.\n")
for title, items in (("New features", features), ("Fixes", fixes), ("Other changes", other)):
    if items:
        out.append(f"## {title}")
        out.extend(items)
        out.append("")
if tickets:
    out.append("## Tickets")
    for n in sorted(tickets):
        out.append(f"- #{n} ({', '.join(sorted(tickets[n]))})")
    out.append("")
print("\n".join(out).rstrip())
PY
