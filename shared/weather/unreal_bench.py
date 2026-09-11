"""Подготовить отдельный каталог Unreal для заставки погоды (TS-Conf или VDAC2).

Ничего общего не трогает: из Desktop\\Unreal берутся только копии настроек,
ПЗУ, CMOS и базового образа wc.img, из стенда ZiFi FAT32-драйвера — сборка
Unreal.exe с режимом ZiFi=UART и эмулятором FT812 (VDAC2). В копии образа:

  /boot.$C и /WC/<заставка>.WMF — текущие сборки;
  /WC/wc.ini — заставка первой в [PLUGINS], ScreenSaver=1 (минута простоя);
  /zifi/zifi.ini — тестовый файл: вымышленная сеть, city: Rome, country: IT.

Вымышленный zifi.ini уходит только в модель ESP (esp_model.py рядом), не в
настоящую плату: в Unreal.ini включается ZiFi=UART, а не COM-порт. С --vdac2
в Unreal.ini включается TS_VDAC2=1: кадр FT812 пишется в ft812_dump.bmp.

Запуск: python unreal_bench.py --run-dir <каталог> --wmf <файл.WMF> [--vdac2]
(обычно — через tools/prepare_unreal.py заставки).
"""
import argparse
import ctypes
import importlib.util
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]                # ZiFi ESP32-S3 Zero
WC_ROOT = PROJECT.parent
DESKTOP_UNREAL = Path.home() / 'Desktop' / 'Unreal'
BENCH_UNREAL = WC_ROOT / 'FAT32 Driver' / 'build' / 'unreal-zifi'
WC_EXE = WC_ROOT / 'WildCommander Improved' / 'exe'
UPDATE = WC_ROOT / 'Chkdsk' / 'Debug' / 'update_wc_image.py'
TEST_INI = (b'; test zifi.ini for the Unreal model of ESP (never sent to a real board)\r\n'
            b'SSID: UART-TEST\r\npassword: fixture\r\ntime: +2\r\n'
            b'city: Rome\r\ncountry: IT\r\n')


def short(path):
    """Короткий путь 8.3: Unreal читает ini в ANSI, кириллица профиля ему чужая."""
    buffer = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer)):
        raise RuntimeError(f'нет короткого имени для {path}')
    return buffer.value


def load_update():
    spec = importlib.util.spec_from_file_location('wc_update', UPDATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def host_exe():
    """Собранный код разбора прошивки; при отсутствии — собрать host-тестом."""
    sys.path.insert(0, str(PROJECT / 'tests'))
    import test_weather_parse
    exe = test_weather_parse.BUILD / 'weather_parse_host.exe'
    if not exe.exists():
        exe = test_weather_parse.build_host()
        if exe is None:
            raise RuntimeError('нужны Visual Studio Build Tools для weather_parse_host.exe')
    return exe


def configured_wc_ini(payload, name):
    """Заставка name первой в [PLUGINS] (другие заставки погоды убираются: WC
    запускает первую), ScreenSaver=1; разделители строк — как были."""
    newline = b'\r\n' if b'\r\n' in payload else (b'\r' if b'\r' in payload else b'\n')
    lines = payload.split(newline)
    result = []
    for line in lines:
        stripped = line.strip()
        upper = stripped.split(b';', 1)[0].strip().upper()
        if upper.startswith(b'SCREENSAVER='):
            line = b'ScreenSaver=1; weather saver test: 1 minute'
        if upper in (b'WEATHER.WMF', b'WEATHER2.WMF'):
            continue
        result.append(line)
        if stripped.upper() == b'[PLUGINS]':
            result.append(name.encode('ascii'))
    return newline.join(result)


def prepare_image(image_path, wmf):
    update = load_update()
    image = update.Fat32Image(image_path)
    try:
        root = image.root_cluster
        wc = image.find_entry(root, 'WC')
        if not wc:
            raise RuntimeError('в образе нет каталога WC')
        update.write_file_any(image, root, 'boot.$C', (WC_EXE / 'boot.$C').read_bytes())
        update.write_file_any(image, wc['cluster'], wmf.name.upper(), wmf.read_bytes())
        ini = image.read_file(wc['cluster'], 'wc.ini')
        update.write_file_any(image, wc['cluster'], 'wc.ini',
                              configured_wc_ini(ini, wmf.name.upper()))
        zifi = update.ensure_dir(image, root, 'zifi')
        update.write_file_any(image, zifi, 'zifi.ini', TEST_INI)
    finally:
        image.save()
    check = update.Fat32Image(image_path)
    wc = check.find_entry(check.root_cluster, 'WC')
    assert check.read_file(wc['cluster'], wmf.name.upper()) == wmf.read_bytes()
    zifi = check.find_entry(check.root_cluster, 'zifi')
    assert check.read_file(zifi['cluster'], 'zifi.ini') == TEST_INI
    return check.read_file(wc['cluster'], 'wc.ini')


def configured_unreal_ini(data, run_dir, vdac2=False):
    """SDCARD — копия образа, ZiFi=UART с моделью ESP на обычном Python;
    vdac2 — включить плату VDAC2 (эмулятор FT812)."""
    python = short(Path(sys.executable))
    if not re.search(rb'(?m)^SDCARD=', data) or not re.search(rb'(?m)^ZiFi=', data):
        raise RuntimeError('в Unreal.ini нет строк SDCARD= или ZiFi=')
    data = re.sub(rb'(?m)^SDCARD=[^\r\n]*', b'SDCARD=weather.img', data)
    data = re.sub(rb'(?m)^ZiFiUART[^\r\n]*\r?\n', b'', data)
    zifi = (b'ZiFi=UART\r\nZiFiUARTExe=' + python.encode('ascii') +
            b'\r\nZiFiUARTDir=' + short(run_dir).encode('ascii') +
            b'\r\nZiFiUARTScript=esp_model.py\r\nZiFiUARTHeap=')
    # функция вместо строки замены: в путях Windows обратные косые черты
    data = re.sub(rb'(?m)^ZiFi=[^\r\n]*', lambda match: zifi, data)
    if not re.search(rb'(?m)^TS_VDAC2=', data):
        raise RuntimeError('в Unreal.ini нет строки TS_VDAC2=')
    data = re.sub(rb'(?m)^TS_VDAC2=[^\r\n]*', b'TS_VDAC2=1' if vdac2 else b'TS_VDAC2=0', data)
    return data


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--wmf', type=Path, required=True, help='собранная заставка .WMF')
    parser.add_argument('--vdac2', action='store_true', help='включить плату VDAC2 (FT812)')
    args = parser.parse_args(argv)
    wmf = args.wmf.resolve()
    run_dir = args.run_dir.resolve()
    run_dir.mkdir(parents=True, exist_ok=True)

    for name in ('Unreal.exe', 'bass.dll', 'bt8xxemu.dll', 'ft8xxemu.dll', 'libmpsse.dll'):
        shutil.copy2(BENCH_UNREAL / name, run_dir / name)
    for name in ('CMOS', 'NVRAM'):
        shutil.copy2(DESKTOP_UNREAL / name, run_dir / name)
    shutil.copytree(DESKTOP_UNREAL / 'rom', run_dir / 'rom', dirs_exist_ok=True)
    shutil.copy2(DESKTOP_UNREAL / 'wc.img', run_dir / 'weather.img')
    shutil.copy2(HERE / 'esp_model.py', run_dir / 'esp_model.py')
    (run_dir / 'esp_model.json').write_text(
        json.dumps({'weather_parse_host': str(host_exe())}, ensure_ascii=False),
        encoding='utf-8')
    (run_dir / 'esp_model.log').unlink(missing_ok=True)
    ini = configured_unreal_ini((DESKTOP_UNREAL / 'Unreal.ini').read_bytes(), run_dir,
                                args.vdac2)
    (run_dir / 'Unreal.ini').write_bytes(ini)
    wc_ini = prepare_image(run_dir / 'weather.img', wmf)
    plugins = wc_ini.replace(b'\r', b'\n').split(b'[PLUGINS]')[1].split(b'\n')
    print('PREPARED', run_dir)
    print('PLUGINS', [line.decode('cp866') for line in plugins[:4] if line.strip()])


if __name__ == '__main__':
    main()
