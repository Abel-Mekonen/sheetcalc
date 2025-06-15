# Security Policy

## Supported versions

The latest released minor version receives fixes. sheetcalc follows semantic
versioning; security fixes ship as patch versions.

| Version | Supported |
| ------- | --------- |
| 0.9.x   | yes       |
| < 0.9   | no        |

## Scope

sheetcalc parses untrusted *CellScript Binary* (`.scb`) workbooks supplied by
the host application. The library has no network or process access of its own;
the host controls file I/O and decides which workbooks to open.

The relevant security surface is therefore the memory safety of the library
itself: the binary parser, formula evaluator, and CSV/JSON export routines
should never read or write out of bounds, use freed memory, or leak, regardless
of the input workbook.

## Reporting a vulnerability

Please report suspected memory-safety issues privately by email to
abelmekonen559@gmail.com rather than opening a public issue. Include a minimal
`.scb` file (or hex dump) that reproduces the problem and, if possible, the
AddressSanitizer report. We aim to acknowledge reports within a few days.
