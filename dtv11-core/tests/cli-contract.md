<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# CLI external contract checks

The CTest entries below are an external-contract table for the generated fixture command.
The result column is the observable condition required by the PR147 L1 contract.

| Invocation | Exit | stdout | stderr |
|---|---:|---|---|
| `fixturectl --help` | 0 | Help text | empty |
| `fixturectl --help --unknown` | 0 | Help text | empty |
| `fixturectl --version --help` | 0 | Help text | empty |
| `fixturectl --help --version` | 0 | Help text | empty |
| `fixturectl --models --help` | 0 | Help text | empty |
| `fixturectl --version` | 0 | `fixture 1.2.3` | empty |
| `fixturectl --version --unknown` | 0 | `fixture 1.2.3` | empty |
| `fixturectl --models --version` | 0 | `fixture 1.2.3` | empty |
| `fixturectl --models` | 0 | Model names and VID:PID lines | empty |
| `fixturectl tune` | 2 | empty | `fixturectl: unknown argument` |
| `fixturectl --unknown` | 2 | empty | `fixturectl: unknown argument` |
| `fixturectl --models --unknown` | 2 | empty | `fixturectl: --models cannot be combined with other arguments` |
| `fixturectl --models --models` | 2 | empty | `fixturectl: --models cannot be combined with other arguments` |

This first Core increment implements only these metadata requests. It does not claim
that daemon, tuner, card, LNB, or other hardware operations are available. The single
CTest contract uses CMake `execute_process` to compare each row's exact status and
separate stdout/stderr values; it does not use `WILL_FAIL`.
