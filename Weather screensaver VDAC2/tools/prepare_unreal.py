"""Каталог Unreal для заставки WEATHER2.WMF (VDAC2): общий стенд
../../shared/weather/unreal_bench.py с этой заставкой первой в [PLUGINS] и
включённой платой VDAC2 (TS_VDAC2=1). Кадр FT812 Unreal пишет в
ft812_dump.bmp в каталоге запуска.

Запуск: python tools/prepare_unreal.py --run-dir <каталог>
"""
import sys
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN.parent / 'shared' / 'weather'))
import unreal_bench  # noqa: E402

if __name__ == '__main__':
    unreal_bench.main(sys.argv[1:] + ['--wmf', str(PLUGIN / 'build' / 'WEATHER2.WMF'),
                                      '--vdac2'])
