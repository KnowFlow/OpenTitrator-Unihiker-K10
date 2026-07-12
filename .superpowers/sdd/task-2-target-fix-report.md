# Task 2 target-duration safety fix

Commit: `ec9f045e763b705bdf8e900d2b513a5e7e9134c2` (`Guard fill duration conversion range`)

## RED

Added a host regression that starts a normal 10 g fill at 100 ms, then supplies
a later tick with a finite negative target (`-4.167 g`) and a delivered value
below that target. The target produces a negative calculated fill duration.

Before the production change, the fresh MSVC command was:

```text
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests\run_engine_test.cpp ph_titrator\run_engine.cpp /Fe:build\run_engine_test.exe && build\run_engine_test.exe'
```

It compiled and then failed with:

```text
FAIL: negative later target leaves only the progress guard before its deadline
FAIL: negative later target does not create a fill-duration timeout
```

## GREEN

`maximumFillDurationMs` now rejects non-finite, negative, and `uint32_t`
out-of-range calculated durations before conversion. A rejected duration leaves
the existing 12-second sample-progress timeout as the applicable guard.

Fresh MSVC verification:

```text
cmd.exe /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests\run_engine_test.cpp ph_titrator\run_engine.cpp /Fe:build\run_engine_test.exe && build\run_engine_test.exe && cl /nologo /std:c++17 /EHsc /W4 /WX tests\ph_titrator_control_test.cpp /Fe:build\ph_titrator_control_test.exe && build\ph_titrator_control_test.exe'
```

Output:

```text
All RunEngine shell tests passed
All ph titrator control tests passed
```

The generated test executables and object files from these commands were removed.
