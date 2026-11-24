# CPMUtil CMake entry points

[`tools/cpmutil.sh`](../../cpmutil.sh) is the normal entry point. CMake 3.31 or newer
can also run the scripts directly from the repository root.

```sh
cmake -P tools/cpm/cmake/ls.cmake
```

- [`ls.cmake`](./ls.cmake) prints package keys from the root
  [`cpmfile.json`](../../../cpmfile.json).
- [`ScriptUtils.cmake`](./ScriptUtils.cmake) supplies shared script helpers. The build
  integration lives in [`CMakeModules/CPMUtil.cmake`](../../../CMakeModules/CPMUtil.cmake).
- [`package/`](./package/) implements package inspection and maintenance.
  [`format.cmake`](./format.cmake) rewrites package definitions, and
  [`update.cmake`](./update.cmake) extracts the upstream tool bundle into the current
  checkout.
