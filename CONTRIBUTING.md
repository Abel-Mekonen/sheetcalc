# Contributing to sheetcalc

Thanks for taking a look. Bug fixes, docs tweaks, and small features are
welcome — just keep PRs focused.

## Build & test

```sh
make
make test
```

Everything in `tests/` needs to pass. If you touch the parser or evaluator,
consider running an ASan build (`make asan`) and exercising your change against
a few `.scb` fixtures.

## Code style

C11, four-space indent, no tabs. There's a `.clang-format` in the repo root; run
`clang-format -i` on whatever you touched before opening a PR.

Match the existing naming (`cs_` for public API, `csi_` for internals). Keep
functions small where you can — the formula parser and eval loop are the
exceptions and should stay readable as single units.

## Pull requests

One logical change per PR. Mention which area you touched (parser, eval,
export, etc.) and include a test if the behaviour is user-visible.

For anything that smells like a memory-safety bug, see [SECURITY.md](SECURITY.md)
instead of filing a public issue with exploit details.
