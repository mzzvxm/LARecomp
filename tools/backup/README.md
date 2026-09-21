# Repository backup

`backup_repo.ps1` packs the whole history — every commit, branch and tag — into
a single `.bundle` file. `git clone` accepts a bundle directly, so one file is
enough to bring the project back on any host, with authorship and history
intact, and no server involved.

This exists because the repository living in exactly one place is the single
point of failure. A disabled account, a disk that dies or a host that pulls the
repo takes issues, pull requests and every clone with it. A bundle on your own
disk does not care.

## Making one

```powershell
.\tools\backup\backup_repo.ps1
```

Bundles land in `larecomp-backups/` next to the repository, never inside the
working tree. Each run writes two files:

| File | Contents |
|---|---|
| `larecomp-<date>-<sha>.bundle` | the history itself, clonable |
| `larecomp-<date>-<sha>.txt` | what is inside, readable without git |

Options:

```powershell
.\tools\backup\backup_repo.ps1 -Dest E:\backups\larecomp   # somewhere else
.\tools\backup\backup_repo.ps1 -Keep 20                    # keep 20, 0 keeps all
.\tools\backup\backup_repo.ps1 -SkipDirty                  # committed history only
```

The last ten bundles are kept and older ones are pruned.

### Uncommitted work is included

The current working tree is committed against a throwaway index and stored in
the bundle under `refs/backup/wip/<stamp>`. Files that are new and not yet
added count too — `git stash create` would have skipped them. The working tree
and the real index are never touched, nothing is stashed, and the temporary ref
is deleted once the bundle is written; the object lives on inside the bundle.

Pass `-SkipDirty` to leave it out.

## Restoring

```powershell
git clone larecomp-20260920-9118742.bundle larecomp-restored
```

Out comes a working repository with every branch. To publish it somewhere else:

```powershell
cd larecomp-restored
git remote add origin <new repository URL>
git push origin --all
```

Getting the uncommitted work back, which a clone does not check out on its own:

```powershell
git fetch "<path to>.bundle" "refs/backup/*:refs/backup/*"
git log --oneline --all --glob=refs/backup/*     # find the wip ref
git checkout -b recovered-wip refs/backup/wip/<stamp>
```

## Reading one without restoring

```powershell
git bundle verify larecomp-20260920-9118742.bundle
git bundle list-heads larecomp-20260920-9118742.bundle
```

`verify` also states the commits the bundle expects to already exist. A bundle
from this script is self-contained, so it needs nothing.

## Worth keeping in mind

- A bundle is a snapshot. It carries what was committed when it ran, nothing
  after that. Run it before anything that rewrites history, and after landing
  work that would hurt to lose.
- One copy on the same disk as the repository is not a backup. Put one
  somewhere else — external drive, another machine, private storage.
- The bundle holds the full history, including anything ever committed. Treat
  it as you would the repository itself.
