#!/usr/bin/env python3
"""Упаковывает каталог ассетов игры в assets.dat — zip по содержимому (нативные платформы, см. CR_ASSET_ZIP).

Использование: python3 tools/make_asset_zip.py <каталог-ассетов> <выход.zip>

Записи внутри архива получают префикс "assets/", чтобы совпадать с путями,
по которым движок запрашивает ресурсы (File.cpp понимает оба варианта ключей).
"""
import os
import sys
import zipfile

# Служебные файлы, которые не должны попадать в архив.
SKIP_NAMES = {".DS_Store", "Thumbs.db", "desktop.ini"}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_asset_zip.py <asset-dir> <output.zip>", file=sys.stderr)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        print(f"make_asset_zip.py: {src} is not a directory", file=sys.stderr)
        return 2

    files = []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for name in sorted(names):
            if name in SKIP_NAMES:
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            files.append((full, "assets/" + rel))

    # ZIP_DEFLATED, уровень 9: ассеты читаются движком через собственный
    # DEFLATE-декодер (engine/src/core/Zip.cpp), поэтому метод сжатия - deflate.
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for full, arcname in files:
            z.write(full, arcname)

    raw = sum(os.path.getsize(f) for f, _ in files)
    packed = os.path.getsize(out)
    print(f"{os.path.basename(out)}: {len(files)} files, {raw} -> {packed} bytes "
          f"({100 - packed * 100 // max(raw, 1)}% smaller)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
