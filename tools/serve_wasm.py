#!/usr/bin/env python3
"""HTTP-сервер для WASM-сборки CrossRender с поддержкой предсжатых файлов.

Отдаёт .gz/.br-сайдкары (см. CMake-флаги CR_WASM_GZIP / CR_WASM_BROTLI) с
заголовком Content-Encoding, если браузер их принимает; иначе - обычные файлы.

Использование: python3 tools/serve_wasm.py [каталог] [порт]
По умолчанию: каталог ./build/wasm/bin, порт 8080.
"""
import mimetypes
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "wasm", "bin")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

# mimetypes не знает про wasm и внутренние форматы сборки.
mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("application/octet-stream", ".data")
mimetypes.add_type("text/html", ".html")


class PrecompressedHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _safe_path(self, url_path: str) -> str:
        name = url_path.split("?", 1)[0].split("#", 1)[0].lstrip("/")
        path = os.path.abspath(os.path.join(ROOT, name))
        # Не выпускаем запросы за пределы корня раздачи.
        if not path.startswith(os.path.abspath(ROOT) + os.sep):
            return ""
        return path

    def _accepts(self, token: str) -> bool:
        return token in self.headers.get("Accept-Encoding", "")

    def do_GET(self) -> None:
        path = self._safe_path(self.path)
        if not path:
            self.send_error(403)
            return

        # Выбираем лучший предсжатый сайдкар, который понимает клиент.
        encoding = None
        if self._accepts("br") and os.path.exists(path + ".br"):
            encoding, serve_path = "br", path + ".br"
        elif self._accepts("gzip") and os.path.exists(path + ".gz"):
            encoding, serve_path = "gzip", path + ".gz"
        elif os.path.exists(path) and os.path.isfile(path):
            serve_path = path
        else:
            self.send_error(404)
            return

        ctype = mimetypes.guess_type(path)[0] or "application/octet-stream"
        try:
            with open(serve_path, "rb") as f:
                payload = f.read()
        except OSError:
            self.send_error(404)
            return

        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        if encoding:
            self.send_header("Content-Encoding", encoding)
            # Вариант по другу кодировке меняется независимо от содержимого.
            self.send_header("Vary", "Accept-Encoding")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(payload)

    do_HEAD = do_GET

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("[serve_wasm] %s\n" % (fmt % args))


def main() -> int:
    root = os.path.abspath(ROOT)
    if not os.path.isdir(root):
        print(f"serve_wasm.py: directory not found: {root}", file=sys.stderr)
        return 2
    print(f"Serving {root} at http://localhost:{PORT}/")
    print(f"Open http://localhost:{PORT}/crossrender_example.html")
    try:
        ThreadingHTTPServer(("127.0.0.1", PORT), PrecompressedHandler).serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
