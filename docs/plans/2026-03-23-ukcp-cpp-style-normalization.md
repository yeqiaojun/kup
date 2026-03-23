# UKCP C++ Style Normalization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Normalize `cpp/ukcp_server` code style so short declarations, definitions, and calls stay on one line whenever readability allows.

**Architecture:** Use a local `.clang-format` to do the bulk C++/header formatting consistently, then make a small manual pass for files outside clang-format coverage such as `CMakeLists.txt`. Keep behavior unchanged and use the existing test suite as the regression check.

**Tech Stack:** C++23, clang-format, CMake, existing ukcp_server tests.

---

### Task 1: Define local formatting policy

**Files:**
- Create: `cpp/ukcp_server/.clang-format`
- Modify: `docs/plans/2026-03-23-ukcp-cpp-style-normalization.md`

**Step 1: Write the formatting policy**

Use a local style file with short-function and short-control-statement options enabled:

```yaml
BasedOnStyle: LLVM
ColumnLimit: 100
AllowShortFunctionsOnASingleLine: All
AllowShortIfStatementsOnASingleLine: AllIfsAndElse
AllowShortLoopsOnASingleLine: true
AllowShortBlocksOnASingleLine: Always
BinPackArguments: true
BinPackParameters: true
```

**Step 2: Verify the style file exists**

Run: `Test-Path cpp/ukcp_server/.clang-format`
Expected: `True`

### Task 2: Reformat the ukcp_server tree

**Files:**
- Modify: `cpp/ukcp_server/src/*.cpp`
- Modify: `cpp/ukcp_server/src/*.hpp`
- Modify: `cpp/ukcp_server/include/ukcp/*.hpp`
- Modify: `cpp/ukcp_server/tests/*.cpp`
- Modify: `cpp/ukcp_server/tests/*.hpp`
- Modify: `cpp/ukcp_server/examples/echo/main.cpp`
- Modify: `cpp/ukcp_server/bench/perf_client/main.cpp`
- Modify: `cpp/ukcp_server/bench/perf_server/main.cpp`
- Modify: `cpp/ukcp_server/bench/windows_perf/main.cpp`
- Modify: `cpp/ukcp_server/CMakeLists.txt`

**Step 1: Run clang-format across C++ sources**

Run:

```powershell
& '<clang-format-path>' -i <file list>
```

Expected: files rewritten in place with no command errors.

**Step 2: Manually normalize non-clang-format files**

Adjust `CMakeLists.txt` and any remaining awkward multi-line layout by hand so short constructs stay compact.

**Step 3: Inspect diff for behavior neutrality**

Run: `git diff -- cpp/ukcp_server`
Expected: style-only diff, no logic changes.

### Task 3: Verify both platform builds still pass

**Files:**
- Use: `cpp/ukcp_server/build-split-win`
- Use: `cpp/ukcp_server/build-split-wsl`

**Step 1: Rebuild and run Windows tests**

Run:

```powershell
& '<cmake-path>' --build cpp/ukcp_server/build-split-win --config Debug --target ukcp_server_tests
& 'D:\tkcp\ukcp\cpp\ukcp_server\build-split-win\Debug\ukcp_server_tests.exe'
```

Expected: all tests pass.

**Step 2: Rebuild and run WSL tests**

Run:

```powershell
wsl bash -lc "cmake --build /mnt/d/tkcp/ukcp/cpp/ukcp_server/build-split-wsl --target ukcp_server_tests"
wsl bash -lc "/mnt/d/tkcp/ukcp/cpp/ukcp_server/build-split-wsl/ukcp_server_tests"
```

Expected: all tests pass.
