# Changelog

All notable changes to sheetcalc are documented here. The project follows
[Semantic Versioning](https://semver.org/).

## [0.9.2] — 2025-06-15

### Fixed
- Formula parser now handles deeply nested parentheses without mis-tokenizing
  closing parens inside function arguments.

### Added
- `VLOOKUP` and `HLOOKUP` built-in functions with approximate/exact match modes.

## [0.9.1] — 2025-04-02

### Fixed
- CSV export now quotes fields containing commas, quotes, and newlines per RFC 4180.

### Changed
- Eval loop reordered dependency resolution for fewer redundant cell visits on
  large sheets.

## [0.9.0] — 2025-02-20

Initial public release.

- `.scb` parser with multi-sheet workbooks, named ranges, and string/style tables.
- Formula evaluation with 30+ built-in functions.
- CSV and JSON export, workbook validation, and the `sheetcalc` CLI.
- Static library (`libsheetcalc.a`) and public header `sheetcalc.h`.

## [0.8.0] — 2024-12-10

Beta release focused on evaluation and export.

- Dependency-ordered formula evaluation across sheets.
- Basic CSV export and in-memory value normalization.
- Reference graph and circular-reference detection during validation.

## [0.7.0] — 2024-10-05

Alpha — core types and binary parser.

- CellScript Binary (`.scb`) header and section layout.
- Sheet/cell storage, formula text storage, and checksum verification.

[0.9.2]: https://github.com/sheetcalc/sheetcalc/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/sheetcalc/sheetcalc/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/sheetcalc/sheetcalc/releases/tag/v0.9.0
