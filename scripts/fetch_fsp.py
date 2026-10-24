#!/usr/bin/env python3
"""下载 Renesas FSP 发布资源（带断点续传 + 重试 + 大小校验）。

用法:
    python scripts/fetch_fsp.py                     # 下载 FSP_Packs
    python scripts/fetch_fsp.py --only rasc         # 只下载 RASC 安装器
    python scripts/fetch_fsp.py --version 6.6.0

背景: PowerShell 的 Invoke-WebRequest 拉取 GitHub Release 大文件时会出现
      "Received an unexpected EOF or 0 bytes from the transport stream"，
      文件被静默截断。改用 urllib + HTTP Range 续传，并按 GitHub API 返回的
      精确字节数校验，避免拿到坏包。
"""
import argparse
import json
import os
import sys
import time
import urllib.request

API = "https://api.github.com/repos/renesas/fsp/releases/tags/v{version}"
DL = "https://github.com/renesas/fsp/releases/download/v{version}/{name}"

WANTED = {
    "packs": "FSP_Packs_v{version}.zip",
    "rasc": "setup_fsp_v{short}_rasc_v{stamp}.exe",
}

DOWNLOAD_DIR = r"E:\software\Renesas\_downloads"


def api_get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "fetch-fsp/1.0",
                                               "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def pick_asset(assets, version, kind):
    """按命名规则挑出目标资产（RASC 文件名里的日期戳会随版本变，做后缀匹配）。"""
    if kind == "packs":
        name = f"FSP_Packs_v{version}.zip"
        hits = [a for a in assets if a["name"] == name]
    else:
        hits = [a for a in assets
                if a["name"].startswith(f"setup_fsp_v{version.replace('.', '_')}_rasc_")
                and a["name"].endswith(".exe")]
    if not hits:
        raise SystemExit(f"发布里找不到 {kind} 资产；现有: "
                         f"{[a['name'] for a in assets]}")
    return hits[0]


def download(url, path, expected, max_attempts=40):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for attempt in range(1, max_attempts + 1):
        have = os.path.getsize(path) if os.path.exists(path) else 0
        if have == expected:
            print(f"  已完成: {have}/{expected} 字节")
            return True
        if have > expected:
            os.remove(path)
            have = 0

        headers = {"User-Agent": "fetch-fsp/1.0"}
        mode = "wb"
        if have:
            headers["Range"] = f"bytes={have}-"
            mode = "ab"
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=120) as r:
                if have and r.status != 206:
                    # 服务端不认 Range，从头重来
                    have, mode = 0, "wb"
                with open(path, mode) as f:
                    while True:
                        chunk = r.read(256 * 1024)
                        if not chunk:
                            break
                        f.write(chunk)
                        have += len(chunk)
        except Exception as e:
            print(f"  第 {attempt} 次中断 ({type(e).__name__}: {e})，已下 "
                  f"{os.path.getsize(path) if os.path.exists(path) else 0}/{expected}")
            time.sleep(min(attempt * 2, 15))

    got = os.path.getsize(path) if os.path.exists(path) else 0
    print(f"  放弃: {got}/{expected}")
    return got == expected


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="6.6.0")
    ap.add_argument("--only", choices=["packs", "rasc", "both"], default="packs")
    ap.add_argument("--dir", default=DOWNLOAD_DIR)
    args = ap.parse_args()

    rel = api_get(API.format(version=args.version))
    print(f"发布: {rel['tag_name']}  ({rel['published_at']})")

    kinds = [args.only] if args.only != "both" else ["packs", "rasc"]
    ok = True
    for kind in kinds:
        asset = pick_asset(rel["assets"], args.version, kind)
        name, size = asset["name"], asset["size"]
        path = os.path.join(args.dir, name)
        print(f"\n[{kind}] {name}\n  目标大小 {size:,} 字节 -> {path}")
        good = download(DL.format(version=args.version, name=name), path, size)
        print(f"  结果: {'OK' if good else '未完成'}")
        ok = ok and good

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
