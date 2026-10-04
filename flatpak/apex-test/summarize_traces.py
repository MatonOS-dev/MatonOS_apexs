#!/usr/bin/env python3
"""Write deduplicated pathname and network summaries for apex-test traces."""
from __future__ import annotations

import ast
import re
from pathlib import Path

SCRATCH = Path("/mnt/data/aosp/out/pc-logs/apex-test")
OUT = SCRATCH / "paths"
OUT.mkdir(exist_ok=True)

PATH_CALLS = re.compile(
    r"^(?:\d+\s+)?(?:open|openat|openat2|creat|stat|lstat|fstatat|newfstatat|statx|"
    r"access|faccessat|faccessat2|readlink|readlinkat|chdir|mkdir|mkdirat|"
    r"rmdir|unlink|unlinkat|rename|renameat|renameat2|link|linkat|"
    r"symlink|symlinkat|statfs|statvfs)\("
)
QUOTED = re.compile(r'"(?:\\.|[^"\\])*"')
NETWORK = re.compile(r"^(?:\d+\s+)?(?:connect|bind|listen|accept|accept4|getpeername)\(")
RECV = re.compile(r"(?:recvfrom|recvmsg|recvmmsg)\(.*\)\s+=\s+(\d+)(?:\s|$)")


def quoted_strings(line: str) -> list[str]:
    values = []
    for match in QUOTED.finditer(line):
        try:
            value = ast.literal_eval(match.group(0))
        except (SyntaxError, ValueError):
            value = match.group(0)[1:-1]
        if isinstance(value, str):
            values.append(value)
    return values


index = []
for trace in sorted((SCRATCH / "traces").glob("*.trace")):
    paths: set[str] = set()
    endpoints: set[str] = set()
    received = 0
    network_calls = 0
    path_calls = 0
    with trace.open(errors="replace") as stream:
        for line in stream:
            if PATH_CALLS.match(line):
                path_calls += 1
                paths.update(quoted_strings(line))
            if NETWORK.match(line):
                network_calls += 1
                strings = quoted_strings(line)
                for value in strings:
                    if value.startswith("/") or value.startswith("unix:"):
                        endpoints.add(value)
                if "AF_INET" in line or "AF_INET6" in line:
                    endpoint = re.search(r"(?:sin_addr|inet_addr)=(?:inet_addr\()?([^,}]+)|"
                                         r"sin6_addr=inet_pton\([^,]+,\s*([^,]+)", line)
                    port = re.search(r"sin_port=htons\((\d+)\)", line)
                    if endpoint:
                        address = re.sub(r"[^0-9A-Fa-f:.]", "", next(x for x in endpoint.groups() if x))
                        endpoints.add(f"network-peer {address.strip()}:{port.group(1) if port else '?'}")
            match = RECV.search(line)
            if match:
                received += int(match.group(1))

    path_file = OUT / f"{trace.stem}.paths.txt"
    path_file.write_text("\n".join(sorted(paths)) + ("\n" if paths else ""))
    network_file = OUT / f"{trace.stem}.network.txt"
    network_file.write_text(
        f"network syscalls: {network_calls}\n"
        f"successful recvfrom/recvmsg bytes: {received}\n"
        f"unique endpoints/paths: {len(endpoints)}\n" +
        "\n".join(sorted(endpoints)) + ("\n" if endpoints else "")
    )
    index.append(
        f"{trace.stem}\tstatus={((SCRATCH / 'logs' / f'{trace.stem}.status').read_text().strip() if (SCRATCH / 'logs' / f'{trace.stem}.status').exists() else 'unknown')}"
        f"\tpath-syscalls={path_calls}\tunique-path-args={len(paths)}"
        f"\tnetwork-syscalls={network_calls}\trecv-bytes={received}"
        f"\tpaths={path_file}\tnetwork={network_file}"
    )

(OUT / "index.tsv").write_text("\n".join(index) + "\n")
