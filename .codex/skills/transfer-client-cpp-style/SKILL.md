---
name: transfer-client-cpp-style
description: Apply this repository's C++ coding and naming rules. Use when writing, editing, refactoring, or reviewing any C++ source/header code in this repository.
---

# Transfer Client C++ Style

Follow these rules whenever touching C++ code in this repository.

## Naming Rules

- Use `PascalCase` for namespaces, classes, structs, unions, enums, typedefs, and type aliases.
- Use `PascalCase` for global functions, free functions, scope-local helper functions, and member functions.
- Use `g_` + `lowerCamelCase` for global variables, namespace-scope variables, file-scope variables, and static variables.
- For private/protected class member variables, choose one style and keep it consistent in the class: `lowerCamelCase`, `m_lowerCamelCase`, or `lowerCamelCase_`.
- Use `lowerCamelCase` for local variables, function parameters, macro parameters, and struct/union data members.
- Use one consistent style for enum constants and `const`/`constexpr` variables in the touched area: either `ALL_CAPS_WITH_UNDERSCORES` or variable-style constants such as `kDefaultTimeout`.
- Use `ALL_CAPS_WITH_UNDERSCORES` for macros and goto labels.

## Workflow

- When editing existing code, update nearby touched symbols only when it is safe and relevant; do not perform broad style-only renames unless asked.
- Keep the style consistent inside each touched class, function, and file.
- If a requested change requires choosing between multiple allowed styles in the document, match the style already used in the touched area.

## Before Finishing

- Search touched C++ files for naming drift from these rules.
- Compile the example when practical:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra asu_io_example.cpp config_loader.cpp rdma_connection_manager.cpp io_splitter.cpp -o asu_io_example.exe
```
