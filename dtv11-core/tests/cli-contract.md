<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# CLI 外部契約の検証

下記の CTest 項目は、生成された fixture コマンドに対する外部契約表である。結果の列は、PR147 L1 契約が要求する観測可能な条件を示している。

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

この Core の初期段階では、これらメタデータの要求のみを実装している。デーモン、チューナー、カード、LNB、その他のハードウェア操作が利用可能であるとは主張しない。単一の CTest 契約は、CMake の `execute_process` を使用して各行の厳密な終了ステータスと個別の stdout/stderr 値を比較し、`WILL_FAIL` は使用しない。
