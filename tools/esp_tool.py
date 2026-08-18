#!/usr/bin/env python3
"""Штатное сетевое обновление ZiFi ESP32-S3 Zero.

Инструмент использует только стандартную библиотеку Python и не трогает
COM-порты. Первая factory-установка выполняется через USB-C, а последующие
обновления — после запуска update.sna и появления TCP-порта 8267.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import ipaddress
import socket
import sys
from pathlib import Path


OTA_PORT = 8267
MAX_LINE = 512
CONNECT_TIMEOUT = 2.0
TRANSFER_TIMEOUT = 35.0
IMAGE_HEADER_SIZE = 36
ESP_IMAGE_MAGIC = 0xE9
ESP32_S3_CHIP_ID = 9
APP_DESCRIPTOR_MAGIC = 0xABCD5432


class UpdateError(RuntimeError):
    """Понятная пользователю ошибка протокола или проверки образа."""


def receive_line(sock: socket.socket, *, limit: int = MAX_LINE) -> str:
    """Прочитать одну ASCII-строку, не позволяя серверу заполнить память."""

    data = bytearray()
    while len(data) < limit:
        chunk = sock.recv(1)
        if not chunk:
            raise UpdateError("ESP закрыл соединение до ответа")
        if chunk == b"\n":
            try:
                return data.rstrip(b"\r").decode("ascii")
            except UnicodeDecodeError as error:
                raise UpdateError("ESP прислал не-ASCII ответ") from error
        data.extend(chunk)
    raise UpdateError("ответ ESP длиннее допустимого")


def receive_exact(sock: socket.socket, size: int) -> bytes:
    """Принять ровно size байт либо сообщить о преждевременном закрытии."""

    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(min(16 * 1024, size - len(data)))
        if not chunk:
            raise UpdateError(
                f"ESP закрыл соединение на {len(data)}/{size} байт"
            )
        data.extend(chunk)
    return bytes(data)


def parse_banner(line: str) -> tuple[str, int]:
    """Вернуть (версия сборки, максимальный размер) из ZIFI-OTA/1."""

    parts = line.split()
    if len(parts) != 3 or parts[0] != "ZIFI-OTA/1":
        raise UpdateError(f"неожиданный баннер updater: {line!r}")
    try:
        maximum = int(parts[2], 10)
    except ValueError as error:
        raise UpdateError(f"неверный размер в баннере: {line!r}") from error
    if maximum <= 0:
        raise UpdateError("ESP сообщил нулевое место для OTA")
    return parts[1], maximum


def validate_application_image(path: Path) -> None:
    """Отбросить factory/чужой chip до любого сетевого соединения."""

    with path.open("rb") as stream:
        header = stream.read(IMAGE_HEADER_SIZE)
    if len(header) < IMAGE_HEADER_SIZE:
        raise UpdateError("firmware.bin слишком короткий для ESP32-S3")
    chip_id = int.from_bytes(header[12:14], "little")
    descriptor = int.from_bytes(header[32:36], "little")
    if header[0] != ESP_IMAGE_MAGIC or chip_id != ESP32_S3_CHIP_ID:
        raise UpdateError("это не application-образ ESP32-S3")
    if descriptor != APP_DESCRIPTOR_MAGIC:
        raise UpdateError(
            "нужен firmware.bin; объединённый firmware.factory.bin для OTA запрещён"
        )


def sha256_file(path: Path) -> tuple[int, str]:
    """Посчитать размер и SHA-256 до начала записи ESP."""

    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(128 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def progress(sent: int, total: int) -> None:
    """Обычная однострочная шкала; -u сохраняет её живой в любой консоли."""

    width = 32
    ratio = sent / total if total else 1.0
    filled = min(width, int(ratio * width))
    bar = "#" * filled + "." * (width - filled)
    percent = min(100, int(ratio * 100))
    print(
        f"\r[{bar}] {percent:3d}%  {sent}/{total} bytes",
        end="",
        flush=True,
    )


def probe(
    address: str, port: int, timeout: float = CONNECT_TIMEOUT
) -> tuple[str, int] | None:
    """Убедиться, что порт принадлежит ZiFi updater, а не чужой службе."""

    try:
        with socket.create_connection((address, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            return parse_banner(receive_line(sock))
    except (OSError, UpdateError):
        return None


def local_ipv4() -> ipaddress.IPv4Address:
    """Определить адрес интерфейса без отправки пакета в интернет."""

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        # UDP connect только выбирает локальный маршрут; пакет не отправляется.
        sock.connect(("8.8.8.8", 9))
        return ipaddress.IPv4Address(sock.getsockname()[0])


def discover(port: int) -> str:
    """Найти единственный ZIFI-OTA/1 в локальной /24 сети."""

    try:
        own = local_ipv4()
    except OSError as error:
        raise UpdateError(
            "не удалось определить локальный IPv4; укажите IP явно"
        ) from error
    network = ipaddress.ip_network(f"{own}/24", strict=False)
    addresses = [str(host) for host in network.hosts() if host != own]
    found: list[str] = []
    print(f"Ищу ZiFi updater в {network} ...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as executor:
        futures = {
            executor.submit(probe, address, port, 0.6): address
            for address in addresses
        }
        for future in concurrent.futures.as_completed(futures):
            if future.result() is not None:
                found.append(futures[future])
    if not found:
        raise UpdateError(
            "updater не найден; запустите update.sna и оставьте экран с IP активным"
        )
    if len(found) != 1:
        raise UpdateError("найдено несколько updater: " + ", ".join(sorted(found)))
    return found[0]


def update(address: str, port: int, image: Path) -> None:
    """Передать образ и принять независимое подтверждение SHA от ESP."""

    if not image.is_file():
        raise UpdateError(f"не найден firmware.bin: {image}")
    validate_application_image(image)
    size, digest = sha256_file(image)

    print(f"Образ: {image}")
    print(f"Размер: {size} bytes")
    print(f"SHA-256: {digest}")
    print(f"Подключение к {address}:{port} ...", flush=True)

    with socket.create_connection((address, port), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(TRANSFER_TIMEOUT)
        build, maximum = parse_banner(receive_line(sock))
        print(f"ESP: {build}; доступно для нового образа {maximum} bytes")
        if size > maximum:
            raise UpdateError(
                f"образ {size} bytes не помещается в OTA-область {maximum} bytes"
            )

        sock.sendall(f"BEGIN {size} {digest}\n".encode("ascii"))
        ready = receive_line(sock).split()
        if len(ready) != 2 or ready[0] != "READY":
            raise UpdateError("ESP отказал перед записью: " + " ".join(ready))
        try:
            chunk_size = int(ready[1], 10)
        except ValueError as error:
            raise UpdateError("ESP сообщил неверный размер блока") from error
        if not 256 <= chunk_size <= 16 * 1024:
            raise UpdateError(f"небезопасный размер блока от ESP: {chunk_size}")

        sent = 0
        progress(0, size)
        with image.open("rb") as stream:
            while chunk := stream.read(chunk_size):
                sock.sendall(chunk)
                sent += len(chunk)
                progress(sent, size)
        print()

        final = receive_line(sock)
        parts = final.split()
        if len(parts) != 2 or parts[0] != "OK":
            raise UpdateError(f"ESP не подтвердил обновление: {final}")
        if parts[1].lower() != digest:
            raise UpdateError(
                "SHA-256 в подтверждении ESP не совпал: "
                f"ожидался {digest}, получен {parts[1]}"
            )
        print("Обновление принято и проверено; ESP32-S3 перезагружается.")


def download_log(address: str, port: int, output: Path) -> None:
    """Скачать текстовый снимок кольцевого flash-журнала и проверить SHA."""

    print(f"Подключение к {address}:{port} ...", flush=True)
    with socket.create_connection((address, port), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(TRANSFER_TIMEOUT)
        build, _ = parse_banner(receive_line(sock))
        print(f"ESP: {build}")
        sock.sendall(b"LOG\n")
        header = receive_line(sock).split()
        if len(header) != 3 or header[0] != "LOG":
            raise UpdateError("ESP отказала в чтении журнала: " + " ".join(header))
        try:
            size = int(header[1], 10)
        except ValueError as error:
            raise UpdateError("ESP сообщила неверный размер журнала") from error
        digest = header[2].lower()
        if size < 0 or size > 256 * 1024 or len(digest) != 64:
            raise UpdateError("небезопасный заголовок журнала от ESP")
        data = receive_exact(sock, size)
        if receive_line(sock) != "OK":
            raise UpdateError("ESP не завершила передачу журнала")

    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        raise UpdateError(
            f"SHA-256 журнала не совпал: ожидался {digest}, получен {actual}"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(f"Журнал: {output}")
    print(f"Размер: {size} bytes")
    print(f"SHA-256: {actual}")


def build_parser() -> argparse.ArgumentParser:
    project = Path(__file__).resolve().parent.parent
    default_image = project / "firmware" / "firmware.bin"
    parser = argparse.ArgumentParser(
        description="Обновить ZiFi ESP32-S3 Zero по Wi-Fi с проверкой SHA-256"
    )
    parser.add_argument(
        "--net",
        required=True,
        metavar="IP|auto",
        help="IPv4 с экрана update.sna либо auto для поиска в локальной /24",
    )
    parser.add_argument(
        "--port", type=int, default=OTA_PORT, help="порт updater (8267)"
    )
    parser.add_argument(
        "--bin",
        type=Path,
        default=default_image,
        dest="image",
        help=f"путь к firmware.bin (по умолчанию {default_image})",
    )
    parser.add_argument(
        "--log",
        type=Path,
        metavar="FILE",
        help="вместо обновления скачать кольцевой flash-журнал в FILE",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        if not 1 <= arguments.port <= 65535:
            raise UpdateError("порт должен быть в диапазоне 1..65535")
        address = (
            discover(arguments.port)
            if arguments.net.lower() == "auto"
            else arguments.net
        )
        # DNS-имя не принимается: update.sna показывает точный IPv4 самой ESP.
        address = str(ipaddress.IPv4Address(address))
        if arguments.log is not None:
            download_log(address, arguments.port, arguments.log.resolve())
        else:
            update(address, arguments.port, arguments.image.resolve())
        return 0
    except (OSError, UpdateError, ValueError) as error:
        print(f"ОШИБКА: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
