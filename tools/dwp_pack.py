#!/usr/bin/env python3
"""
dwp_pack.py — 把 Dweep 的單關自訂關卡 (.DWP) 打包成官方關卡組格式。

用法：
    python dwp_pack.py 自訂關卡資料夾 -o MyPack.DWP --title "My Custom Levels"
    python dwp_pack.py a.DWP b.DWP c.DWP -o MyPack.DWP
    python dwp_pack.py 資料夾 --list            # 只列出關卡，不打包
    python dwp_pack.py 資料夾 -o Pack.DWP --split 30   # 每 30 關一個檔

輸入可以混用：單關自訂檔、官方關卡組 (Dweep.DWP、Bonus.DWP…) 都能讀，會依序合併。

格式說明（逆向分析得出）：
  官方關卡組： "Dweep\\0" + 標題(40) + 關卡數(int32 LE) = 50 bytes 檔頭，接著每關 488 bytes
              （關卡數以檔案大小為準：Dweep.DWP 檔頭寫 30，實際有 31 關）
  單關自訂檔： "Dweep\\0" + 版權字串(40)            = 46 bytes 檔頭，接著 1 關 488 bytes，最後多 4 bytes
  每關 488 bytes： 名稱(40) + 提示(200) + 地圖 16x10(160) + 起始道具欄(88)
"""
import argparse
import glob
import os
import re
import struct
import sys

MAGIC = b"Dweep\0"
REC = 488            # 每關大小
OFF_HDR = 50         # 官方關卡組檔頭
CUS_HDR = 46         # 單關自訂檔檔頭
CUS_SIZE = CUS_HDR + REC + 4   # 538
MAX_TILE = 32


class Level:
    def __init__(self, raw, src):
        assert len(raw) == REC
        self.raw = bytearray(raw)
        self.src = src

    @property
    def name(self):
        return self.raw[:40].split(b"\0")[0].decode("cp1252", "replace")

    @property
    def hint(self):
        return self.raw[40:240].split(b"\0")[0].decode("cp1252", "replace")

    @property
    def grid(self):
        return bytes(self.raw[240:400])

    def problems(self):
        g = self.grid
        out = []
        if g.count(5) != 1:
            out.append(f"Dweep 起點數量 = {g.count(5)}")
        if g.count(4) != 1:
            out.append(f"終點數量 = {g.count(4)}")
        bad = sorted({b for b in g if b > MAX_TILE})
        if bad:
            out.append(f"未知的格子代碼 {bad}")
        return out

    def cleaned(self):
        """把字串 \\0 後的殘留垃圾清成 0，道具欄在結尾符號(種類 9)之後也清成 0。地圖不動。"""
        r = bytearray(self.raw)
        for start, size in ((0, 40), (40, 200)):
            field = r[start:start + size]
            n = field.find(b"\0")
            if n >= 0:
                r[start + n:start + size] = bytes(size - n)
        tail = r[400:488]
        for slot in range(10):
            p = 8 + slot * 8
            if p + 8 > 88:
                break
            typ = struct.unpack_from("<i", tail, p)[0]
            if typ == 9:
                tail[p + 8:] = bytes(88 - p - 8)
                break
        r[400:488] = tail
        return bytes(r)


def read_levels(path):
    data = open(path, "rb").read()
    if not data.startswith(MAGIC):
        raise ValueError("不是 Dweep 關卡檔（開頭不是 'Dweep'）")
    count = struct.unpack_from("<I", data, 46)[0] if len(data) >= 50 else None
    # 單關自訂檔：46 bytes 檔頭 + 488 + 4。
    # 538 bytes 也剛好等於「只有 1 關的官方關卡組」，用檔頭的關卡數區分：
    # 單關檔在位移 46 是關卡名稱的文字，不會是 01 00 00 00。
    if len(data) == CUS_SIZE and count != 1:
        return [Level(data[CUS_HDR:CUS_HDR + REC], path)]
    # 官方關卡組：50 bytes 檔頭 + n*488。
    # 關卡數以檔案大小為準，檔頭的數量不一定可靠（Dweep.DWP 檔頭寫 30，實際有 31 關）。
    if len(data) >= OFF_HDR + REC and (len(data) - OFF_HDR) % REC == 0:
        n = (len(data) - OFF_HDR) // REC
        if count != n:
            print(f"  ! {os.path.basename(path)}: 檔頭記載 {count} 關，依檔案大小實際有 {n} 關，全部讀取", file=sys.stderr)
        return [Level(data[OFF_HDR + i * REC: OFF_HDR + (i + 1) * REC], f"{path}#{i + 1}") for i in range(n)]
    # 寬鬆模式：46 bytes 檔頭後面接著 488 的倍數（多出來的尾巴忽略）
    if len(data) >= CUS_HDR + REC:
        n = (len(data) - CUS_HDR) // REC
        print(f"  ! {os.path.basename(path)}: 大小 {len(data)} 不是已知格式，嘗試以 46 bytes 檔頭讀取 {n} 關", file=sys.stderr)
        return [Level(data[CUS_HDR + i * REC: CUS_HDR + (i + 1) * REC], f"{path}#{i + 1}") for i in range(n)]
    raise ValueError(f"檔案大小 {len(data)} bytes，無法辨識")


def natural_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def collect(inputs, recursive):
    files = []
    for p in inputs:
        if os.path.isdir(p):
            pat = os.path.join(p, "**", "*") if recursive else os.path.join(p, "*")
            files += [f for f in glob.glob(pat, recursive=recursive) if f.lower().endswith(".dwp") and os.path.isfile(f)]
        else:
            files += glob.glob(p) or [p]
    # 資料夾內依檔名自然排序；命令列上直接列的檔案保持原順序
    seen, out = set(), []
    for f in files:
        a = os.path.abspath(f)
        if a not in seen:
            seen.add(a); out.append(f)
    return out


def build_pack(levels, title, clean):
    t = title.encode("cp1252", "replace")[:39]
    hdr = MAGIC + t.ljust(40, b"\0") + struct.pack("<I", len(levels))
    body = b"".join(lv.cleaned() if clean else bytes(lv.raw) for lv in levels)
    return hdr + body


def main():
    # 輸出導到檔案或管線時會用系統編碼（例如 cp950），編不出來的字元（✓ ✗）改印成 ? 而不是讓程式中斷
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="把 Dweep 單關自訂關卡打包成官方關卡組 .DWP")
    ap.add_argument("inputs", nargs="+", help="自訂關卡檔或資料夾（可混合官方關卡組）")
    ap.add_argument("-o", "--output", default="CustomPack.DWP", help="輸出檔名（預設 CustomPack.DWP）")
    ap.add_argument("-t", "--title", default="Custom Levels", help="關卡組名稱，最多 39 個英數字元")
    ap.add_argument("-r", "--recursive", action="store_true", help="連子資料夾一起找")
    ap.add_argument("--sort", choices=["file", "name", "none"], default="file",
                    help="排序：file=依檔名(預設) name=依關卡名稱 none=照輸入順序")
    ap.add_argument("--split", type=int, default=0, help="每 N 關輸出一個檔（例如 30）")
    ap.add_argument("--dedupe", action="store_true", help="略過地圖完全相同的重複關卡")
    ap.add_argument("--raw", action="store_true", help="原封不動複製每關資料（預設會清掉字串後的殘留垃圾）")
    ap.add_argument("--skip-bad", action="store_true", help="略過起點/終點數量不對的關卡")
    ap.add_argument("--list", action="store_true", help="只列出關卡資訊，不輸出檔案")
    a = ap.parse_args()

    files = collect(a.inputs, a.recursive)
    if a.sort == "file":
        files.sort(key=lambda f: natural_key(os.path.basename(f)))
    levels = []
    for f in files:
        try:
            lv = read_levels(f)
        except Exception as e:
            print(f"  ✗ 略過 {f}：{e}", file=sys.stderr)
            continue
        levels += lv
    if a.sort == "name":
        levels.sort(key=lambda lv: natural_key(lv.name))

    kept, grids = [], set()
    for lv in levels:
        probs = lv.problems()
        if probs:
            print(f"  ! {lv.name or '(無名稱)'}  [{lv.src}]：{'；'.join(probs)}", file=sys.stderr)
            if a.skip_bad:
                continue
        if a.dedupe:
            if lv.grid in grids:
                print(f"  - 重複，略過：{lv.name}  [{lv.src}]", file=sys.stderr)
                continue
            grids.add(lv.grid)
        kept.append(lv)

    if a.list or not kept:
        for i, lv in enumerate(kept, 1):
            print(f"{i:3d}. {lv.name:<40} {lv.hint[:50]}")
        if not kept:
            print("沒有可打包的關卡。", file=sys.stderr)
            sys.exit(1)
        return

    chunks = [kept] if a.split <= 0 else [kept[i:i + a.split] for i in range(0, len(kept), a.split)]
    base, ext = os.path.splitext(a.output)
    for k, chunk in enumerate(chunks, 1):
        out = a.output if len(chunks) == 1 else f"{base}_{k}{ext or '.DWP'}"
        title = a.title if len(chunks) == 1 else f"{a.title[:34]} {k}"
        data = build_pack(chunk, title, clean=not a.raw)
        # 寫完立刻讀回來驗證
        with open(out, "wb") as fh:
            fh.write(data)
        back = read_levels(out)
        assert len(back) == len(chunk) and all(b.grid == c.grid for b, c in zip(back, chunk)), "驗證失敗"
        print(f"✓ {out}：{len(chunk)} 關，{len(data)} bytes（標題：{title}）")


if __name__ == "__main__":
    main()
