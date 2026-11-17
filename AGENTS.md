# Commit identity

Every commit in this repository uses the public project identity:

```sh
GIT_AUTHOR_NAME=A2Sumie \
GIT_AUTHOR_EMAIL=89308692+A2Sumie@users.noreply.github.com \
GIT_COMMITTER_NAME=A2Sumie \
GIT_COMMITTER_EMAIL=89308692+A2Sumie@users.noreply.github.com \
git commit ...
```

Check both author and committer before pushing. The local pre-commit hook
enforces this identity. Preserve attribution to other contributors.
