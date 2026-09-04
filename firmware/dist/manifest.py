#!/usr/bin/env python3
"""`firmware/dist/manifest.json` 的读写与校验。

**它解决的问题**：`dist/` 里的二进制是构建产物，git 不知道它和源码的关系。
源码改了忘记重编，仓库里就躺着一份「看起来是最新的」固件 —— 而这种不同步
只会在烧板子之后暴露，那时症状是「代码明明改了但行为没变」，最难查的一类。

所以清单里记下**每个固件源文件的 sha256**（列表来自 `git ls-files`）。
`server/tests/test_firmware_contract.py` 重算一遍并比对：
不一致就是「改了源码没重编」，pytest 当场红。

`write` 由 `firmware/build.sh` 调用 —— 不要手写这个 json。
`check` 可以单独跑：`python3 firmware/dist/manifest.py check`。
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / "firmware" / "dist"
MANIFEST = DIST / "manifest.json"
ARTIFACTS = ("zephyr.uf2", "zephyr.hex")

# 构建输入 = 固件目录下所有受版本控制的文件。用 git ls-files 而不是 rglob：
# 后者会把 /tmp 软链、编辑器临时文件、未跟踪的实验文件也算进来，
# 那会让清单在别人的机器上无缘无故不一致。
SOURCE_DIR = "firmware/nrf52840"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def source_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", SOURCE_DIR],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout
    return sorted(ln for ln in out.splitlines() if ln.strip())


def source_hashes() -> dict[str, str]:
    return {rel: sha256(ROOT / rel) for rel in source_files()}


def parse_build_log(log: Path) -> dict[str, object]:
    """从 west build 的输出里抠 FLASH/RAM 用量和警告数。"""
    if not log.exists():
        return {}
    text = log.read_text(errors="replace")
    out: dict[str, object] = {}
    for region in ("FLASH", "RAM"):
        m = re.search(rf"^\s*{region}:\s+(\d+) B\s+\S+ \S+\s+([\d.]+)%", text, re.M)
        if m:
            out[region.lower()] = {"bytes": int(m.group(1)), "percent": float(m.group(2))}
    out["warnings"] = len(re.findall(r"\bwarning:", text, re.I))
    return out


def uf2_header(path: Path) -> dict[str, object]:
    """UF2 第一块的 family / 起始地址 / 块数 —— 拖进 U 盘能不能被认就看这三个。"""
    import struct
    b = path.read_bytes()[:512]
    magic0, magic1, _flags, addr, _size, _blkno, numblk, fam = struct.unpack("<8I", b[:32])
    assert magic0 == 0x0A324655 and magic1 == 0x9E5D5157, "不是 UF2 文件"
    return {"family": f"0x{fam:08X}", "start": f"0x{addr:X}", "blocks": numblk}


def build() -> dict[str, object]:
    return {
        "board": os.environ.get("BOARD", "promicro_nrf52840/nrf52840/uf2"),
        "warnings_as_errors": True,
        "artifacts": {
            name: {"size": (DIST / name).stat().st_size, "sha256": sha256(DIST / name)}
            for name in ARTIFACTS
        },
        "uf2": uf2_header(DIST / "zephyr.uf2"),
        "memory": parse_build_log(Path(os.environ.get("BUILD_LOG", "/nonexistent"))),
        "sources": source_hashes(),
    }


def cmd_write() -> int:
    data = build()
    MANIFEST.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    art = data["artifacts"]
    mem = data["memory"]
    print(f"  manifest: {len(data['sources'])} 个源文件")
    for name in ARTIFACTS:
        print(f"  {name}: {art[name]['size']} B  {art[name]['sha256']}")
    if mem:
        print(f"  FLASH {mem['flash']['percent']}%  RAM {mem['ram']['percent']}%  "
              f"warnings={mem['warnings']}")
    return 0


def cmd_check() -> int:
    if not MANIFEST.exists():
        print(f"没有 {MANIFEST} —— 跑 firmware/build.sh", file=sys.stderr)
        return 1
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    bad = []

    for name, want in data["artifacts"].items():
        p = DIST / name
        if not p.exists():
            bad.append(f"{name} 不存在")
        elif sha256(p) != want["sha256"]:
            bad.append(f"{name} 与清单不一致")

    now = source_hashes()
    recorded = data["sources"]
    for rel in sorted(set(recorded) | set(now)):
        if rel not in recorded:
            bad.append(f"新源文件未进清单：{rel}")
        elif rel not in now:
            bad.append(f"清单里的源文件已删除：{rel}")
        elif recorded[rel] != now[rel]:
            bad.append(f"源文件改了但没重编：{rel}")

    if bad:
        print("dist/ 与源码不同步：", file=sys.stderr)
        for b in bad:
            print(f"  - {b}", file=sys.stderr)
        print("跑 firmware/build.sh 重新生成", file=sys.stderr)
        return 1
    print(f"dist/ 与源码一致（{len(now)} 个源文件，{len(data['artifacts'])} 个产物）")
    return 0


if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "check"
    if action not in ("write", "check"):
        print(f"用法：{sys.argv[0]} [write|check]", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(cmd_write() if action == "write" else cmd_check())
