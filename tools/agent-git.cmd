@echo off
rem Runs git with SACRED_NO_BUMP=1 (skips the post-merge version bump). Usage: tools\agent-git.cmd merge my-branch
set SACRED_NO_BUMP=1
git %*
exit /b %ERRORLEVEL%
