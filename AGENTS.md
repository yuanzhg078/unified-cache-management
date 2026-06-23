# Agent Operating Notes

## Remote Ascend Test Environment

- SSH target: `root@110.138.0.3`
- Account: `root`
- Assume SSH key authentication is configured for non-interactive Codex commands.
- Container shell after login:

```bash
docker exec -it zy_ascend_test /bin/bash
```

- Code directory inside the container:

```bash
cd /home/zy/pod_code_test/unified-cache-management/
```

- Non-interactive command pattern:

```bash
ssh root@110.138.0.3 "docker exec zy_ascend_test /bin/bash -lc 'cd /home/zy/pod_code_test/unified-cache-management/ && <command>'"
```

## Safety Boundary

- Do not modify files in the remote container code directory unless the user explicitly asks for remote changes.
- Prefer read-only inspection on the remote environment by default.
- Keep code edits in the local workspace unless the user specifically requests changes on the server or inside the container.
- Do not change Docker, SSH, host, or server configuration.
- Before irreversible or high-risk operations, explain the impact and wait for explicit user confirmation. This includes deleting files, force pushing, changing configuration, destructive Git commands, and modifying server/container settings.
- Do not run destructive Git commands such as `reset --hard`, forced checkout, clean, or branch deletion unless the user explicitly requests them.
- Do not push from the remote environment unless the user explicitly requests it.

## Remote Git And Logs

- `git fetch origin` is allowed when the user asks to update remote refs.
- `git status`, `git branch`, `git log`, `git diff`, and similar read-only Git inspection commands are allowed for diagnosis.
- Use `pull`, `merge`, `rebase`, `checkout`, or branch creation only when the user clearly asks for that remote workflow.
- Report forced updates, changed remote refs, dirty worktrees, or untracked files back to the user.
- Short command output can return through stdout/stderr.
- For long test output, write logs under a temporary path only when needed and tell the user where they are.
