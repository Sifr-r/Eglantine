# CPMUtil Tooling

Run [`tools/cpmutil.sh`](../cpmutil.sh) from the repository root in a POSIX shell with
CMake 3.31 or newer on `PATH`. The package definitions and pins live in
[`cpmfile.json`](../../cpmfile.json). Start with commands that print information.

```sh
tools/cpmutil.sh --help
tools/cpmutil.sh ls
tools/cpmutil.sh package which fmt
tools/cpmutil.sh package url fmt
tools/cpmutil.sh package dir fmt
```

`which` exits 1 if any requested key is missing. `url` resolves the download source.
`dir` prints the calculated cache location even if the package has yet to be fetched.

| Operation | Effect |
| --- | --- |
| `package fetch` or `package reset` | Download into the cache or replace a local cached copy. |
| `format` or package pin maintenance | Can rewrite `cpmfile.json` or in-tree patches. Review the diff. |
| `update` | Download and extract the upstream CPMUtil tool bundle into the repository. |

Package commands expose their options through `--help`. For the CMake entry points and
script ownership, see the [CMake script guide](./cmake/README.md).
