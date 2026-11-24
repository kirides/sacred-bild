# Runs git with SACRED_NO_BUMP=1 so the post-merge hook does not bump versions. For agents (commit/merge); John's own git keeps bumping.
# Usage: pwsh tools/agent-git.ps1 merge --no-ff my-branch   |   pwsh tools/agent-git.ps1 commit -m "..."
$env:SACRED_NO_BUMP = '1'
& git @args
exit $LASTEXITCODE
