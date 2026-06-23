# unified-cache-management C++ gtest Patterns

## Repository Shape

- Mixed C++/CMake and Python repository, but this skill is for C++ gtest only.
- Top-level CMake option: `BUILD_UNIT_TESTS=ON` enables gtest discovery.
- Main C++ source tree: `ucm/`.
- Common C++ test locations:
  - `ucm/shared/test/case/.../*_test.cc`
  - `ucm/store/test/case/.../*_test.cc`
  - `ucm/transport/kv/common/test/*_test.cpp`
  - `ucm/transport/kv/asu/.../test/.../*_test.cpp`

## CMake Discovery

- `ucm/shared/test/CMakeLists.txt` recursively glob-discoveries `./case/*.cc` into `ucmshared.test`.
- `ucm/store/test/CMakeLists.txt` recursively glob-discoveries `./case/*.cc` into `ucmstore.test`.
- `ucm/transport/kv/common/test/router_test.cpp` is an example for transport common tests; inspect the owning `CMakeLists.txt` before adding a file there.
- When `BUILD_UCM_ASU` is off, store tests under `case/asu` are filtered out.
- Use the existing module target name when building/running tests, such as `ucmshared.test` or `ucmstore.test`.

## File And Style Conventions

- Use GoogleTest/GoogleMock.
- Include the MIT license header when creating a new C++ test file in an area where neighboring tests use it.
- Keep helpers in an anonymous namespace.
- Use `TEST_F` when shared setup/state is useful; otherwise use `TEST`.
- Match neighboring include order and extension style (`.cc` vs `.cpp`).
- Prefer small deterministic helpers over random values. If nearby tests already use `UC::Test::Detail::Random` or `TypesHelper`, reuse them when it improves consistency.
- Use `ASSERT_*` before dereferencing or continuing after setup, and `EXPECT_*` for independent checks.
- For status objects, follow local style:
  - `ASSERT_TRUE(status.ok()) << status.message`
  - or `ASSERT_EQ(expr, UC::Status::OK())` when neighboring tests do that.

## High-Value Test Case Patterns

### Containers And Algorithms

Cover:

- empty state
- zero/one capacity
- full capacity
- duplicate values
- replacement/eviction
- pop/order invariants
- clear/reuse
- lvalue/rvalue overload behavior

### Protocol Packing And Binary Layout

Cover:

- successful pack status
- final buffer size
- header fields
- command id
- payload length
- representative payload offsets
- invalid request sizes or unsupported values if the implementation validates them

Build expected buffers explicitly and compare bytes with mismatch context.

### Config Parsers And Validators

Cover:

- default config
- valid explicit config
- missing fields
- invalid enum/string values
- numeric boundaries
- malformed input
- idempotent repeated setup

### Stores, Queues, Managers

Cover:

- setup success/failure
- insert/find/exist behavior
- duplicate insert
- FIFO/LRU/eviction policy
- capacity boundary
- reset/cleanup
- task completion or status transition

Avoid real device or filesystem dependencies where a temporary directory or fake backend can cover the branch.

### Routing

Cover:

- distribution sanity with stable hashes
- invalid config rejection
- key grouping affinity
- top-k/touched-node limits
- migration ratio when adding/removing nodes

Use stable deterministic hash helpers in the test file.

## Placement Guide

- Shared infra/template/trans tests: `ucm/shared/test/case/<area>/`.
- Store tests: `ucm/store/test/case/<backend-or-area>/`.
- Transport common tests: `ucm/transport/kv/common/test/`.
- ASU transport/client tests: place beside existing ASU component tests and inspect the owning CMake file before relying on discovery.

## Verification Commands

Start with the smallest command that exercises the new test.

```bash
cmake -S . -B build -DBUILD_UNIT_TESTS=ON -DBUILD_UCM_STORE=ON -DBUILD_UCM_SPARSE=OFF -DRUNTIME_ENVIRONMENT=simu
cmake --build build --target ucmshared.test -j
ctest --test-dir build --output-on-failure -R TargetTestName
```

For store tests:

```bash
cmake --build build --target ucmstore.test -j
ctest --test-dir build --output-on-failure -R UCFakeMetaManagerTest
```

If build directories or dependencies differ, inspect existing build scripts and CMake targets before guessing.

## Remote Ascend Boundary

- Remote details are documented in repository `AGENTS.md`.
- Remote inspection is allowed for diagnosis; do not edit files in the remote container unless the user explicitly requests remote changes.
- Do not change Docker, SSH, host, or server configuration.

## Final Checklist

1. Every target branch has at least one deterministic assertion.
2. The new file is discovered by CMake or CMake was updated minimally.
3. Hardware/runtime dependencies are mocked, faked, or avoided.
4. The test fails for plausible implementation mistakes, not only for crashes.
5. A targeted build or `ctest -R` was attempted, or the blocker is reported precisely.
