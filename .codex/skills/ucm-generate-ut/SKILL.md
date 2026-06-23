---
name: ucm-generate-ut
description: Generate repository-specific C++ gtest unit tests for the unified-cache-management codebase. Use when Codex is asked to add, improve, or auto-generate C++ UTs/gtest tests for UCM functions, classes, headers, protocol structs, CMake modules, bug fixes, or uncovered C++ logic in this repository.
---

# UCM Generate UT

## Workflow

1. Inspect the target C++ symbol and nearby tests before writing code.
   - Use `rg` to find existing tests for the same symbol, header, source file, namespace, and module.
   - Read the implementation, headers, constructor/config structs, status/error paths, constants, and direct dependencies.
   - Read `references/cpp-gtest-patterns.md` for this repository's placement, style, and verification rules.

2. Select the lightest viable gtest layer.
   - Prefer deterministic unit tests for pure logic, containers, parsers, routing, config validation, protocol packing, state transitions, and status handling.
   - Mock or fake only external boundaries: filesystem, device/runtime APIs, networking, threads with timing sensitivity, randomness, environment variables, and heavyweight store/transport backends.
   - Avoid E2E, benchmark, real CUDA/NPU/Ascend, Docker, SSH, or remote edits unless the user explicitly asks.

3. Derive cases from function behavior and branches.
   - Cover nominal flow, empty inputs, boundary sizes, invalid config, duplicate inputs, repeated calls, ordering, idempotency, error/status returns, cleanup, and ownership/lifetime expectations.
   - For protocol or packed structs, assert exact size, status, command fields, payload length, and relevant bytes.
   - For data structures, assert invariants after mutation, eviction, clear/reuse, duplicate handling, and capacity boundaries.

4. Place tests in the current CMake discovery path.
   - Add `*_test.cc` under the module's existing `test/case` tree when the module uses recursive CMake globbing.
   - Add `*_test.cpp` beside existing tests when that module already uses `.cpp`.
   - Update CMake only when the existing module does not auto-discover the new file.

5. Keep tests maintainable.
   - Use fixed inputs and small helper builders in anonymous namespaces.
   - Prefer public behavior over private implementation details.
   - Do not introduce sleeps, performance thresholds, large allocations, or hardware-only assumptions for coverage work.

6. Verify narrowly.
   - Build/run the smallest relevant gtest target or `ctest -R` filter.
   - If local dependencies are unavailable, run static checks that are possible and report the exact blocker.
   - Use the remote Ascend environment only for explicit environment-specific verification, read-only by default.

## Output Expectations

- Create or update C++ gtest files directly in this repository when asked to generate UTs.
- Include small fakes/mocks in the test file unless shared helpers are already established nearby.
- Report which branches or behaviors the new tests cover, which command was run, and any environment limitation.
- If coverage tooling is available, compare before/after for the touched module; if not, explain the expected coverage gain from covered branches.
