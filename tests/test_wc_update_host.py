"""Обновлятор Wild Commander с GitHub на стенде ПК (аналог sfc /scannow).

tools/host_wcu собирает НАСТОЯЩИЕ WcUpdater, VfsBridge, VfsClient и протокол
прошивки. Вместо Z80 — эмулятор WC поверх папки «SD», вместо GitHub — папка
с ответами API и файлами. Проверяется:
- состояние каждого файла: совпал SHA, отличается (по SHA и по длине), новый,
  только на SD, защищённый wc.ini, файл с длинным именем в новом каталоге;
- флаги строки списка: можно обновить, отметить по клавише A (только
  исполняемые .WMF, .$C, .spg);
- обновление: копия на SD совпадает с GitHub байт в байт, wc.ini и чужие
  файлы не тронуты, временных файлов не осталось;
- помехи: испорченное скачивание скачивается заново, испорченная запись на SD
  (ловится только чтением обратно) пишется заново;
- ошибка GitHub API и случай «всё совпадает».
Сборка стенда: tools/host_wcu/build.ps1 (нужны Visual Studio Build Tools).
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / '.test-build' / 'host_wcu' / 'host_wcu.exe'
REPO = 'owner/Wild-Commander-Improved'
COMMIT = '5dc0a43681f114f75f6a01b0bb0065bf46e09b36'

SAME, DIFFERENT, NEW, LOCAL_ONLY = 1, 2, 3, 4
KEPT_SAME, KEPT_DIFFERENT, READ_ERROR, UPDATED, FAILED = 5, 6, 7, 8, 9
REMOTE, LOCAL, KEPT, CAN_UPDATE, AUTO = 1, 2, 4, 8, 16


def git_sha(data: bytes) -> str:
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def payload(seed: int, size: int) -> bytes:
    return bytes((seed * 31 + index * 7) & 0xFF for index in range(size))


class Stand:
    """Две папки: «GitHub» (exe/ и ответы API) и «SD»."""

    def __init__(self) -> None:
        self.base = pathlib.Path(tempfile.mkdtemp(prefix='zifi_wcu_'))
        self.github = self.base / 'github'
        self.sd = self.base / 'sd'
        (self.github / 'exe').mkdir(parents=True)
        self.sd.mkdir()

    def remote(self, path: str, data: bytes) -> None:
        target = self.github / 'exe' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def local(self, path: str, data: bytes) -> None:
        target = self.sd / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def publish(self) -> None:
        """Ответы API — как у GitHub: ссылка на ветку и рекурсивное дерево."""
        root = self.github / 'exe'
        tree = []
        for item in sorted(root.rglob('*')):
            relative = item.relative_to(root).as_posix()
            if item.is_dir():
                tree.append({'path': relative, 'mode': '040000', 'type': 'tree',
                             'sha': hashlib.sha1(relative.encode()).hexdigest(),
                             'url': 'https://api.github.com/x'})
            else:
                data = item.read_bytes()
                tree.append({'path': relative, 'mode': '100644', 'type': 'blob',
                             'sha': git_sha(data), 'size': len(data),
                             'url': 'https://api.github.com/x'})
        (self.github / '_ref.json').write_text(json.dumps({
            'ref': 'refs/heads/main', 'node_id': 'X',
            'url': 'https://api.github.com/x',
            'object': {'sha': COMMIT, 'type': 'commit',
                       'url': 'https://api.github.com/x'}}), encoding='utf-8')
        (self.github / '_tree.json').write_text(json.dumps(
            {'sha': 'f' * 40, 'url': 'https://api.github.com/x', 'tree': tree,
             'truncated': False}, indent=2), encoding='utf-8')

    def run(self, mode: str, environment: dict | None = None) -> 'Run':
        env = dict(os.environ)
        env.pop('ZIFI_WCU_CORRUPT_DOWNLOAD', None)
        env.pop('ZIFI_SIM_CORRUPT_WRITE', None)
        env.pop('ZIFI_SIM_FAIL_RENAME', None)
        for name in ('ZIFI_SIM_NO_FILEX_MOVE', 'ZIFI_SIM_FAIL_MOVE',
                     'ZIFI_SIM_FAIL_MOVE_AFTER', 'ZIFI_SIM_REFUSE_OPENDIR',
                     'ZIFI_SIM_WRITE_TAIL', 'ZIFI_SIM_HIDE_ENTRY',
                     'ZIFI_SIM_FAIL_RENAME_AT', 'ZIFI_SIM_RENAME_CODE',
                     'ZIFI_SIM_RENAME_LOSE_AT', 'ZIFI_SIM_REFUSE_STAT',
                     'ZIFI_SIM_REFUSE_STAT_COUNT', 'ZIFI_SIM_FAIL_READ',
                     'ZIFI_SIM_FAIL_READ_COUNT', 'ZIFI_SIM_FAIL_WRITE',
                     'ZIFI_SIM_FAIL_DELETE', 'ZIFI_WCU_CORRUPT_TIMES',
                     'ZIFI_WCU_RETRY'):
            env.pop(name, None)
        env.update(environment or {})
        result = subprocess.run(
            [str(EXE), str(self.sd), str(self.github), REPO, 'main', 'exe',
             mode, 'WC/wc.ini'],
            capture_output=True, timeout=300, env=env)
        return Run(result.stdout.decode('utf-8', 'replace'), result.returncode)

    def cleanup(self) -> None:
        shutil.rmtree(self.base, ignore_errors=True)


class Run:
    def __init__(self, output: str, code: int) -> None:
        self.output = output
        self.code = code
        self.entries: dict[str, tuple[int, int, int, int]] = {}
        self.states: list[tuple[int, int, int, int, str]] = []
        self.http: list[str] = []
        for line in output.splitlines():
            if line.startswith('ENTRY '):
                parts = line.split(' ', 6)
                self.entries[parts[6]] = (int(parts[2]), int(parts[3]),
                                          int(parts[4]), int(parts[5]))
            elif line.startswith('STATE '):
                parts = line.split(' ', 5)
                self.states.append((int(parts[1]), int(parts[2]), int(parts[3]),
                                    int(parts[4]), parts[5] if len(parts) > 5 else ''))
            elif line.startswith('HTTP '):
                self.http.append(line[5:])

    def status(self, path: str) -> int:
        return self.entries[path][0]

    def flags(self, path: str) -> int:
        return self.entries[path][1]


def stand_is_stale() -> bool:
    """Стенд старше любого своего исходника — собрать заново: иначе тесты
    проверяли бы прежний код."""
    if not EXE.exists():
        return True
    built = EXE.stat().st_mtime
    sources = [ROOT / 'tools' / 'host_wcu' / 'main.cpp',
               ROOT / 'tools' / 'host_wcu' / 'build.ps1',
               ROOT / 'tools' / 'host_smb' / 'z80_sim.cpp',
               ROOT / 'tools' / 'host_smb' / 'z80_sim.hpp']
    sources += list((ROOT / 'src').glob('*.cpp'))
    sources += list((ROOT / 'include' / 'zifi').glob('*.hpp'))
    sources += [path for path in (ROOT / 'tests' / 'stubs_host').rglob('*')
                if path.is_file()]
    return any(path.stat().st_mtime > built for path in sources if path.exists())


VSWHERE = pathlib.Path(r'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')


class WcUpdateHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if stand_is_stale():
            script = ROOT / 'tools' / 'host_wcu' / 'build.ps1'
            result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy',
                                     'Bypass', '-File', str(script)],
                                    capture_output=True, cwd=ROOT)
            if not VSWHERE.exists():
                raise unittest.SkipTest('стенд host_wcu не собран (нужны Build Tools)')
            # Build Tools есть, а сборка не прошла — это ошибка в коде, а не
            # повод пропустить тесты на прежнем стенде.
            if result.returncode != 0 or not EXE.exists():
                raise RuntimeError('сборка host_wcu не удалась:\n' +
                                   result.stdout.decode('utf-8', 'replace') +
                                   result.stderr.decode('utf-8', 'replace'))

    def setUp(self) -> None:
        self.stand = Stand()
        self.addCleanup(self.stand.cleanup)

    def scenario(self) -> None:
        """Все случаи сразу, как на настоящей карте после старой версии WC."""
        s = self.stand
        s.remote('boot.$C', payload(1, 31761))
        s.local('boot.$C', payload(1, 31761))                  # совпадает
        s.remote('Help.txt', b'help ' * 300)                   # новый, текст
        s.remote('WC/A.WMF', payload(2, 20000))
        s.local('WC/A.WMF', payload(3, 20000))                 # та же длина, другой SHA
        s.remote('WC/C.WMF', payload(4, 9000))
        s.local('WC/C.WMF', payload(4, 8999))                  # другая длина
        s.remote('WC/B.WMF', payload(5, 40000))                # новый плагин
        s.remote('WC/wc.ini', b'[WC]\r\nDEFAULT=1\r\n')
        s.local('WC/wc.ini', b'[WC]\r\nMINE=1\r\n')            # настройки пользователя
        s.local('WC/ZIFIFTP.WMF', payload(6, 13568))           # чужой плагин
        s.local('readme.txt', b'user file in root')            # корень карты — не WC
        s.remote('WC/MENU/Hrust v1.3.spg', payload(7, 9762))   # нового каталога нет
        s.publish()

    def assert_same_file(self, path: str) -> None:
        self.assertEqual((self.stand.sd / path).read_bytes(),
                         (self.stand.github / 'exe' / path).read_bytes(), path)

    def assert_no_temp(self) -> None:
        self.assertEqual(list(self.stand.sd.rglob('WCUPD.TMP')), [])

    def test_check_classifies_every_file(self) -> None:
        self.scenario()
        run = self.stand.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('boot.$C'), SAME)
        self.assertEqual(run.status('WC/A.WMF'), DIFFERENT)
        self.assertEqual(run.status('WC/C.WMF'), DIFFERENT)
        self.assertEqual(run.status('WC/B.WMF'), NEW)
        self.assertEqual(run.status('Help.txt'), NEW)
        self.assertEqual(run.status('WC/MENU/Hrust v1.3.spg'), NEW)
        self.assertEqual(run.status('WC/wc.ini'), KEPT_DIFFERENT)
        self.assertEqual(run.status('WC/ZIFIFTP.WMF'), LOCAL_ONLY)
        self.assertNotIn('readme.txt', run.entries, 'корень карты — не каталог WC')
        # Клавиша A: только исполняемые; тексты и wc.ini — нет.
        for path in ('WC/A.WMF', 'WC/C.WMF', 'WC/B.WMF', 'WC/MENU/Hrust v1.3.spg'):
            self.assertEqual(run.flags(path) & (CAN_UPDATE | AUTO), CAN_UPDATE | AUTO, path)
        self.assertEqual(run.flags('Help.txt') & (CAN_UPDATE | AUTO), CAN_UPDATE)
        self.assertEqual(run.flags('WC/wc.ini') & (CAN_UPDATE | KEPT), KEPT)
        self.assertEqual(run.flags('boot.$C') & CAN_UPDATE, 0)
        self.assertEqual(run.flags('WC/ZIFIFTP.WMF') & (REMOTE | LOCAL), LOCAL)
        # A.WMF той же длины читается целиком; C.WMF другой длины — нет.
        ready = run.states[-1]
        self.assertEqual(ready[0], 4)
        self.assertEqual(ready[1], 5, 'пять файлов к обновлению')
        self.assertIn('5 to update', ready[4])
        # Проверка ничего не пишет.
        self.assertEqual((self.stand.sd / 'WC/A.WMF').read_bytes(), payload(3, 20000))
        self.assertTrue(any(state[0] == 3 and state[3] > 0 for state in run.states),
                        'полоса прогресса проверки')
        # Строки списка идут по ходу проверки, а не пачкой в конце.
        lines = run.output.splitlines()
        first_entry = next(i for i, line in enumerate(lines) if line.startswith('ENTRY '))
        last_check = max(i for i, line in enumerate(lines) if line.startswith('STATE 3 '))
        self.assertLess(first_entry, last_check, 'список растёт во время проверки')

    def test_apply_restores_and_keeps_user_files(self) -> None:
        self.scenario()
        run = self.stand.run('apply')
        self.assertEqual(run.code, 0, run.output)
        for path in ('WC/A.WMF', 'WC/C.WMF', 'WC/B.WMF', 'Help.txt',
                     'WC/MENU/Hrust v1.3.spg'):
            self.assertEqual(run.status(path), UPDATED, path)
            self.assert_same_file(path)
        self.assertEqual((self.stand.sd / 'WC/wc.ini').read_bytes(), b'[WC]\r\nMINE=1\r\n')
        self.assertEqual((self.stand.sd / 'WC/ZIFIFTP.WMF').read_bytes(), payload(6, 13568))
        self.assertEqual((self.stand.sd / 'readme.txt').read_bytes(), b'user file in root')
        self.assert_no_temp()
        self.assertIn('All files match', run.states[-1][4])
        self.assertEqual(run.states[-1][3], 100)

    def test_auto_selection_skips_texts(self) -> None:
        self.scenario()
        run = self.stand.run('auto')
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/B.WMF'), UPDATED)
        self.assertEqual(run.status('WC/MENU/Hrust v1.3.spg'), UPDATED)
        self.assertFalse((self.stand.sd / 'Help.txt').exists(), 'текст не отмечается по A')

    def test_corrupted_download_is_downloaded_again(self) -> None:
        self.scenario()
        run = self.stand.run('apply', {'ZIFI_WCU_CORRUPT_DOWNLOAD': 'B.WMF'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/B.WMF'), UPDATED)
        self.assert_same_file('WC/B.WMF')
        downloads = [line for line in run.http if line.endswith('/exe/WC/B.WMF')]
        self.assertEqual(len(downloads), 2, 'первое скачивание испорчено')

    def test_bad_sd_write_is_written_again(self) -> None:
        self.scenario()
        run = self.stand.run('apply', {'ZIFI_SIM_CORRUPT_WRITE': '1'})
        self.assertEqual(run.code, 0, run.output)
        for path in ('WC/A.WMF', 'WC/C.WMF', 'WC/B.WMF', 'Help.txt',
                     'WC/MENU/Hrust v1.3.spg'):
            self.assertEqual(run.status(path), UPDATED, path)
            self.assert_same_file(path)
        self.assert_no_temp()

    def test_interrupted_update_leftovers(self) -> None:
        """Остаток прерванной замены (WCUPD.TMP/WCUPD.OLD) может делить
        цепочку с файлом: он не показывается, не удаляется, и запись в его
        каталог запрещена до проверки карты. Другие каталоги обновляются;
        имя на SD в другом регистре (8.3 без LFN) — тот же файл."""
        s = self.stand
        s.remote('boot.$C', payload(1, 5000))
        s.local('BOOT.$C', payload(1, 5000))                   # 8.3 заглавными
        s.remote('Help.txt', b'help')
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/WCUPD.TMP', payload(2, 3000))              # недописанная копия
        s.publish()
        check = s.run('check')
        self.assertEqual(check.code, 0, check.output)
        self.assertEqual(check.status('boot.$C'), SAME)
        self.assertEqual(check.status('WC/A.WMF'), NEW)
        self.assertNotIn('WC/WCUPD.TMP', check.entries)
        self.assertIn('CHKDSK', check.states[-1][4])
        run = s.run('apply')
        self.assertEqual(run.status('WC/A.WMF'), FAILED, run.output)
        self.assertFalse((s.sd / 'WC/A.WMF').exists())
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(2, 3000),
                         'остаток не тронут')
        self.assertEqual(run.status('Help.txt'), UPDATED, 'корень — другой каталог')
        self.assertIn('CHKDSK: old WCUPD', run.states[-1][4])
        # После проверки карты и удаления остатка файл ставится.
        (s.sd / 'WC/WCUPD.TMP').unlink()
        again = s.run('apply')
        self.assertEqual(again.code, 0, again.output)
        self.assertEqual(again.status('WC/A.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assert_no_temp()

    def assert_no_leftovers(self) -> None:
        self.assert_no_temp()
        self.assertEqual(list(self.stand.sd.rglob('WCUPD.OLD')), [])

    def test_fallback_sets_old_file_aside(self) -> None:
        """WC без FILEX MOVE: прежний файл откладывается в WCUPD.OLD, копия
        получает имя, отложенный удаляется."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.remote('WC/B.WMF', payload(4, 500))                  # новый
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/A.WMF'), UPDATED)
        self.assertEqual(run.status('WC/B.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assert_same_file('WC/B.WMF')
        self.assert_no_leftovers()

    def test_fallback_rename_refusal_keeps_copies(self) -> None:
        """Любой отказ RENAME запасного пути — исход неизвестен (у WC A=0
        бывает и после легшего удаления записи): проверенная копия
        WCUPD.TMP не удаляется, запись останавливается, просьба проверить
        карту. Прежний файл не отложился (1-е переименование) или возвращён
        на имя (2-е; и при коде #FF). Следующий запуск в этот каталог не
        пишет."""
        for failing, code in (('1', '1'), ('2', '1'), ('2', '255')):
            with self.subTest(rename=failing, code=code):
                self.stand.cleanup()
                self.stand = Stand()
                self.addCleanup(self.stand.cleanup)
                s = self.stand
                s.remote('WC/A.WMF', payload(2, 7000))
                s.local('WC/A.WMF', payload(3, 7000))
                s.publish()
                run = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1',
                                      'ZIFI_SIM_FAIL_RENAME_AT': failing,
                                      'ZIFI_SIM_RENAME_CODE': code})
                self.assertEqual(run.code, 1, run.output)
                self.assertEqual(run.status('WC/A.WMF'), FAILED)
                self.assertEqual((s.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
                self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(2, 7000))
                self.assertFalse((s.sd / 'WC/WCUPD.OLD').exists())
                self.assertNotIn('DELETE path=/WC/WCUPD.TMP', run.output)
                self.assertEqual(run.states[-1][0], 255)
                self.assertIn('rename failed: CHKDSK', run.states[-1][4])
                again = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1'})
                self.assertEqual(again.status('WC/A.WMF'), FAILED, again.output)
                self.assertEqual((s.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
                self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(2, 7000))

    def test_fallback_lost_rename_keeps_verified_copy(self) -> None:
        """Сценарий Н1: RENAME прежнего файла в WCUPD.OLD потерял запись
        (ни старого, ни нового имени), а ответ — обычный отказ. Проверенная
        копия — последняя целая — остаётся на карте."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.remote('WC/B.WMF', payload(4, 500))
        s.local('WC/B.WMF', payload(5, 500))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1',
                              'ZIFI_SIM_RENAME_LOSE_AT': '1'})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.status('WC/A.WMF'), FAILED)
        self.assertFalse((s.sd / 'WC/A.WMF').exists())
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(2, 7000),
                         'проверенная копия не удалена')
        self.assertNotIn('DELETE path=/WC/WCUPD.TMP', run.output)
        self.assertEqual((s.sd / 'WC/B.WMF').read_bytes(), payload(5, 500),
                         'после неизвестного исхода больше ничего не пишется')
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('CHKDSK', run.states[-1][4])

    def test_fallback_old_copy_delete_failure_blocks_folder(self) -> None:
        """Сценарий Н3: оба RENAME прошли, а WCUPD.OLD не удалился. Файл
        обновлён, но каталог с остатком — только для чтения уже в этом
        сеансе: следующий файл в нём не пишется, итог просит CHKDSK."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.remote('WC/B.WMF', payload(4, 500))
        s.local('WC/B.WMF', payload(5, 500))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1',
                              'ZIFI_SIM_FAIL_DELETE': 'WCUPD.OLD'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/A.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assertEqual((s.sd / 'WC/WCUPD.OLD').read_bytes(), payload(3, 7000))
        self.assertEqual(run.status('WC/B.WMF'), FAILED)
        self.assertEqual((s.sd / 'WC/B.WMF').read_bytes(), payload(5, 500))
        self.assert_no_temp()
        self.assertEqual(run.states[-1][0], 4)
        self.assertIn('1 failed: CHKDSK: old WCUPD', run.states[-1][4])

    def test_protected_file_unknown_rename_is_not_deleted(self) -> None:
        """Установка отсутствующего wc.ini запасным путём: исход RENAME
        неизвестен — копия не удаляется, запись останавливается."""
        s = self.stand
        s.remote('WC/wc.ini', b'[WC]')
        s.local('WC/A.WMF', b'x')
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_NO_FILEX_MOVE': '1',
                              'ZIFI_SIM_FAIL_RENAME_AT': '1',
                              'ZIFI_SIM_RENAME_CODE': '255'})
        self.assertEqual(run.status('WC/wc.ini'), FAILED, run.output)
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), b'[WC]')
        self.assertEqual(run.states[-1][0], 255)

    def test_reserved_names_in_reference_are_refused(self) -> None:
        for name in ('WC/WCUPD.TMP', 'wcupd.old'):
            with self.subTest(name=name):
                self.stand.cleanup()
                self.stand = Stand()
                self.addCleanup(self.stand.cleanup)
                s = self.stand
                s.remote(name, b'x')
                s.publish()
                run = s.run('check')
                self.assertEqual(run.code, 1, run.output)
                self.assertIn('reserved name', run.states[-1][4])

    def test_file_and_directory_with_one_fat_name_are_refused(self) -> None:
        s = self.stand
        s.remote('WC/a/B.WMF', payload(1, 10))
        s.publish()
        tree = json.loads((s.github / '_tree.json').read_text(encoding='utf-8'))
        tree['tree'].append({'path': 'WC/A', 'mode': '100644', 'type': 'blob',
                             'sha': 'e' * 40, 'size': 1, 'url': 'x'})
        (s.github / '_tree.json').write_text(json.dumps(tree), encoding='utf-8')
        run = s.run('check')
        self.assertEqual(run.code, 1, run.output)
        self.assertIn('names differ only in case', run.states[-1][4])

    def two_files(self) -> None:
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.remote('WC/B.WMF', payload(4, 5000))
        s.local('WC/B.WMF', payload(5, 5000))
        s.publish()

    def test_move_refused_keeps_old_file(self) -> None:
        """FILEX отверг перенос до изменений (#1C): старый файл цел, копия
        убрана, остальные файлы обновляются."""
        self.two_files()
        run = self.stand.run('apply', {'ZIFI_SIM_FAIL_MOVE': str(0x1C)})
        self.assertEqual(run.status('WC/A.WMF'), FAILED, run.output)
        self.assertEqual((self.stand.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
        self.assertEqual(run.status('WC/B.WMF'), UPDATED)
        self.assert_same_file('WC/B.WMF')
        self.assert_no_temp()

    def test_move_rollback_unknown_stops_and_keeps_copy(self) -> None:
        """Откат не подтверждён (#24): копию не трогать (она может делить
        цепочку с файлом), остальные файлы не писать, просить проверить карту."""
        self.two_files()
        run = self.stand.run('apply', {'ZIFI_SIM_FAIL_MOVE': str(0x24)})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.status('WC/A.WMF'), FAILED)
        self.assertEqual((self.stand.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
        self.assertEqual((self.stand.sd / 'WC/WCUPD.TMP').read_bytes(), payload(2, 7000))
        self.assertEqual((self.stand.sd / 'WC/B.WMF').read_bytes(), payload(5, 5000),
                         'после неизвестного исхода больше ничего не пишется')
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('check the disk', run.states[-1][4])

    def test_move_done_but_reply_says_error(self) -> None:
        """Перенос лёг, а ответ — сбой носителя (#21): файл проверяется
        чтением и засчитывается, но исход замены неизвестен — остальные файлы
        в этом сеансе не пишутся, нужна проверка карты."""
        self.two_files()
        run = self.stand.run('apply', {'ZIFI_SIM_FAIL_MOVE': str(0x21),
                                       'ZIFI_SIM_FAIL_MOVE_AFTER': '1'})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.status('WC/A.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assertEqual(run.status('WC/B.WMF'), DIFFERENT)
        self.assertEqual((self.stand.sd / 'WC/B.WMF').read_bytes(), payload(5, 5000))
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('check the disk', run.states[-1][4])

    def test_move_committed_cleanup_is_success(self) -> None:
        """#25: перенос состоялся, не освободилась лишь старая цепочка."""
        self.two_files()
        run = self.stand.run('apply', {'ZIFI_SIM_FAIL_MOVE': str(0x25),
                                       'ZIFI_SIM_FAIL_MOVE_AFTER': '1'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/A.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assert_no_temp()

    def test_protected_file_is_never_replaced(self) -> None:
        """Опись по ошибке не увидела wc.ini (повреждённый каталог): он
        выглядит новым, но имеющийся файл пользователя не заменяется ни
        переносом (без REPLACE), ни запасным путём."""
        for extra in ({}, {'ZIFI_SIM_NO_FILEX_MOVE': '1'}):
            with self.subTest(fallback=bool(extra)):
                self.stand.cleanup()
                self.stand = Stand()
                self.addCleanup(self.stand.cleanup)
                s = self.stand
                s.remote('WC/wc.ini', b'[WC]' + bytes([13, 10]) + b'DEFAULT=1')
                s.local('WC/wc.ini', b'[WC]' + bytes([13, 10]) + b'MINE=1')
                s.publish()
                env = {'ZIFI_SIM_HIDE_ENTRY': 'wc.ini', **extra}
                check = s.run('check', env)
                self.assertEqual(check.status('WC/wc.ini'), NEW, check.output)
                run = s.run('apply', env)
                # Файл нашёлся при записи: только сверка, без скачивания.
                self.assertEqual(run.status('WC/wc.ini'), KEPT_DIFFERENT, run.output)
                self.assertEqual(run.flags('WC/wc.ini') & CAN_UPDATE, 0)
                self.assertEqual((s.sd / 'WC/wc.ini').read_bytes(),
                                 b'[WC]' + bytes([13, 10]) + b'MINE=1')
                self.assertFalse(any(line.endswith('/exe/WC/wc.ini')
                                     for line in run.http))
                self.assert_no_temp()

    def test_protected_file_verified_after_read_error(self) -> None:
        """Сценарий Н4: отсутствующий wc.ini поставлен, но проверка чтением
        отказала. Второй круг видит файл на SD и только сверяет его — не
        застревает в FAILED."""
        s = self.stand
        s.remote('WC/wc.ini', b'[WC]')
        s.remote('WC/A.WMF', payload(1, 100))
        s.local('WC/A.WMF', payload(1, 100))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_FAIL_READ': '/WC/wc.ini',
                              'ZIFI_SIM_FAIL_READ_COUNT': '1'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/wc.ini'), UPDATED)
        self.assert_same_file('WC/wc.ini')
        self.assertEqual(run.flags('WC/wc.ini') & CAN_UPDATE, 0)
        self.assert_no_temp()

    def test_leftover_hidden_from_listing_is_found_by_stat(self) -> None:
        """Листинг обрезан (ошибка чтения для WC — конец каталога) и не
        показал WCUPD.TMP: опись ищет служебные имена ещё и прямым STAT."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/WCUPD.TMP', payload(9, 3000))
        s.publish()
        env = {'ZIFI_SIM_HIDE_ENTRY': 'WCUPD.TMP'}
        check = s.run('check', env)
        self.assertEqual(check.code, 0, check.output)
        self.assertIn('CHKDSK', check.states[-1][4])
        run = s.run('apply', env)
        self.assertEqual(run.status('WC/A.WMF'), FAILED, run.output)
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(9, 3000))
        self.assertNotIn('DELETE path=/WC/WCUPD.TMP', run.output)

    def test_invisible_leftover_is_never_touched(self) -> None:
        """Сценарий Н2 в худшем виде: остаток не виден ни листингу, ни STAT
        (оба — ошибки чтения, которые WC отдаёт как «нет»). Обновлятор его
        не удаляет и не перезаписывает: DELETE копии перед записью нет, а
        OPEN на запись занятое имя не берёт."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.local('WC/WCUPD.TMP', payload(9, 3000))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_HIDE_ENTRY': 'WCUPD.TMP',
                              'ZIFI_SIM_REFUSE_STAT': '/WC/WCUPD.TMP'})
        self.assertEqual(run.status('WC/A.WMF'), FAILED, run.output)
        self.assertEqual((s.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(9, 3000))
        self.assertNotIn('DELETE path=/WC/WCUPD.TMP', run.output)
        self.assertIn('OPEN refused: exists /WC/WCUPD.TMP', run.output)
        self.assertIn('cannot create WCUPD.TMP', run.states[-1][4])

    def test_directory_missing_at_inventory_but_present_at_apply_stops(self) -> None:
        """Сценарий Н2 из отчёта: при описи OPENDIR и STAT каталога отказали
        (каталог «нет», остаток в нём не замечен), а к записи чтение
        восстановилось. Противоречие — признак сбоев карты: запись
        останавливается, остаток не трогается."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.local('WC/WCUPD.TMP', payload(9, 3000))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_REFUSE_OPENDIR': '/WC',
                              'ZIFI_SIM_REFUSE_STAT': '/WC',
                              'ZIFI_SIM_REFUSE_STAT_COUNT': '1'})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.status('WC/A.WMF'), FAILED)
        self.assertEqual((s.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
        self.assertEqual((s.sd / 'WC/WCUPD.TMP').read_bytes(), payload(9, 3000))
        self.assertNotIn('DELETE path=/WC/WCUPD.TMP', run.output)
        self.assertNotIn('OPEN mode=1', run.output)
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('SD read errors: CHKDSK', run.states[-1][4])

    def test_write_failures_report_the_write_error(self) -> None:
        """Сценарий Н7: все попытки записи отказали, недописанная копия
        каждый раз убрана. Причина в итоге — сбой записи, а не отказ
        уборки."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_FAIL_WRITE': 'all'})
        self.assertEqual(run.status('WC/A.WMF'), FAILED, run.output)
        self.assertEqual((s.sd / 'WC/A.WMF').read_bytes(), payload(3, 7000))
        self.assert_no_temp()
        ready = run.states[-1]
        self.assertEqual(ready[0], 4)
        self.assertIn('1 failed: block-status', ready[4], 'отказ окна записи')
        self.assertNotIn('delete', ready[4])

    def test_summary_reason_belongs_to_a_failed_file(self) -> None:
        """Сценарий Н7: A и B не обновились; повтор B удался, A — по-прежнему
        FAILED. Итог показывает причину A, а не прошлую причину B."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 700))
        s.local('WC/A.WMF', payload(3, 700))
        s.remote('WC/B.WMF', payload(4, 500))
        s.local('WC/B.WMF', payload(5, 500))
        s.publish()
        (s.github / 'exe' / 'WC' / 'A.WMF').unlink()         # raw отвечает 404
        run = s.run('retry', {'ZIFI_WCU_CORRUPT_DOWNLOAD': 'B.WMF',
                              'ZIFI_WCU_CORRUPT_TIMES': '3',
                              'ZIFI_WCU_RETRY': 'B.WMF'})
        self.assertEqual(run.code, 0, run.output)
        ready = [state for state in run.states if state[0] == 4]
        self.assertIn('2 failed: download: SHA mismatch', ready[-2][4])
        self.assertEqual(run.status('WC/B.WMF'), UPDATED)
        self.assert_same_file('WC/B.WMF')
        self.assertEqual(run.status('WC/A.WMF'), FAILED)
        self.assertIn('1 failed: download: HTTP 404', ready[-1][4])

    def test_refused_opendir_of_existing_directory_is_an_error(self) -> None:
        """Отказ OPENDIR ещё не значит «каталога нет»: STAT видит каталог —
        значит, опись недостоверна, и проверка кончается ошибкой, а не
        объявляет все его файлы новыми."""
        s = self.stand
        s.remote('WC/wc.ini', b'[WC]')
        s.local('WC/wc.ini', b'[WC]MINE')
        s.publish()
        run = s.run('check', {'ZIFI_SIM_REFUSE_OPENDIR': '/WC'})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('SD: cannot read /WC', run.states[-1][4])

    def test_extra_tail_after_write_is_written_again(self) -> None:
        """Верное начало с лишним хвостом не сходит за целый файл: длина по
        записи каталога сверяется до SHA, копия пишется заново."""
        s = self.stand
        s.remote('WC/A.WMF', payload(2, 7000))
        s.local('WC/A.WMF', payload(3, 7000))
        s.publish()
        run = s.run('apply', {'ZIFI_SIM_WRITE_TAIL': '512'})
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/A.WMF'), UPDATED)
        self.assert_same_file('WC/A.WMF')
        self.assert_no_temp()

    def test_failed_install_of_missing_protected_file_can_be_retried(self) -> None:
        """wc.ini нет на SD, скачать не удалось: строка FAILED и по-прежнему
        обновляемая, а итог — «1 failed», а не «всё совпадает»."""
        s = self.stand
        s.remote('WC/wc.ini', b'[WC]')
        s.remote('WC/A.WMF', payload(1, 100))
        s.local('WC/A.WMF', payload(1, 100))
        s.publish()
        (s.github / 'exe' / 'WC' / 'wc.ini').unlink()        # raw отвечает 404
        run = s.run('apply')
        self.assertEqual(run.status('WC/wc.ini'), FAILED, run.output)
        self.assertEqual(run.flags('WC/wc.ini') & CAN_UPDATE, CAN_UPDATE)
        self.assertIn('1 to update, 1 failed: download', run.states[-1][4])
        self.assertFalse((s.sd / 'WC/wc.ini').exists())

    def test_names_differing_only_in_case_are_refused(self) -> None:
        s = self.stand
        s.remote('WC/A.WMF', payload(1, 100))
        s.publish()
        tree = json.loads((s.github / '_tree.json').read_text(encoding='utf-8'))
        twin = dict(tree['tree'][-1])
        twin['path'] = 'WC/a.wmf'
        twin['sha'] = 'e' * 40
        tree['tree'].append(twin)
        (s.github / '_tree.json').write_text(json.dumps(tree), encoding='utf-8')
        run = s.run('check')
        self.assertEqual(run.code, 1, run.output)
        self.assertIn('names differ only in case', run.states[-1][4])

    def test_sync_sends_the_list_again(self) -> None:
        """Повтор списка: метка (этап 6, всего — число строк), все строки по
        порядку номеров, затем последнее состояние без изменений."""
        self.scenario()
        run = self.stand.run('sync')
        self.assertEqual(run.code, 0, run.output)
        lines = [line for line in run.output.splitlines() if line.startswith('ENTRY ')]
        self.assertEqual(len(lines), 2 * len(run.entries), 'каждая строка дважды')
        self.assertEqual(run.states[-1][0], 4, 'и последнее состояние — итог проверки')
        self.assertEqual(run.states[-1][4], run.states[-3][4])
        marker = run.states[-2]
        self.assertEqual(marker[:3], (6, 0, len(run.entries)))
        output = run.output.splitlines()
        start = next(i for i, line in enumerate(output) if line.startswith('STATE 6 '))
        resent = [int(line.split(' ')[1]) for line in output[start:]
                  if line.startswith('ENTRY ')]
        self.assertEqual(resent, list(range(len(run.entries))))

    def test_size_over_16mib_is_saturated(self) -> None:
        """Размер больше поля LE24 приходит как 0xFFFFFF — плагин покажет >16M."""
        s = self.stand
        s.remote('WC/A.WMF', payload(1, 100))
        s.local('WC/A.WMF', payload(1, 100))
        big = s.sd / 'WC' / 'BIG.DAT'
        with open(big, 'wb') as handle:
            handle.truncate(17 * 1024 * 1024)
        s.publish()
        run = s.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('WC/BIG.DAT'), LOCAL_ONLY)
        self.assertEqual(run.entries['WC/BIG.DAT'][2], 0xFFFFFF)

    def test_missing_protected_file_is_installed(self) -> None:
        s = self.stand
        s.remote('WC/wc.ini', b'[WC]')
        s.remote('WC/A.WMF', payload(1, 100))
        s.local('WC/A.WMF', payload(1, 100))
        s.publish()
        run = s.run('apply')
        self.assertEqual(run.status('WC/wc.ini'), UPDATED, run.output)
        self.assert_same_file('WC/wc.ini')

    def test_long_path_sends_its_tail(self) -> None:
        """Путь длиннее события (54 байта) — приходит хвост с именем файла,
        срезанный по границе символа UTF-8."""
        s = self.stand
        name = 'WC/MENU/Очень длинное имя плагина для проверки хвоста.spg'
        s.remote(name, payload(8, 100))
        s.publish()
        run = s.run('check')
        self.assertEqual(run.code, 0, run.output)
        tails = [path for path in run.entries if path.endswith('хвоста.spg')]
        self.assertEqual(len(tails), 1, run.entries)
        tail = tails[0]
        self.assertNotIn('\ufffd', tail, 'срез посреди символа UTF-8')
        self.assertLessEqual(len(tail.encode('utf-8')), 54)
        self.assertTrue(name.endswith(tail))
        self.assertEqual(run.status(tail), NEW)

    def test_list_order_commander_plugins_rest(self) -> None:
        """Порядок строк: сам WC (boot.$C), плагины по алфавиту — и чужие, что
        есть только на SD, — затем всё остальное по алфавиту пути; регистр не
        важен. Номера строк — этот же порядок."""
        s = self.stand
        s.remote('Help.txt', b'h')
        s.remote('WC/b.WMF', payload(1, 10))
        s.remote('WC/wc.ini', b'[WC]')
        s.remote('boot.$C', payload(2, 10))
        s.remote('WC/A.WMF', payload(3, 10))
        s.remote('WC/MENU/X.spg', payload(4, 10))
        s.remote('WC_History.txt', b'h')
        s.local('WC/ZIFIFTP.WMF', payload(5, 10))            # только на SD
        s.local('WC/zz.txt', b'user')                        # только на SD
        s.publish()
        run = s.run('check')
        self.assertEqual(run.code, 0, run.output)
        order = [line.split(' ', 6)[6] for line in run.output.splitlines()
                 if line.startswith('ENTRY ')]
        self.assertEqual(order, ['boot.$C',
                                 'WC/A.WMF', 'WC/b.WMF', 'WC/ZIFIFTP.WMF',
                                 'Help.txt', 'WC/MENU/X.spg', 'WC/wc.ini',
                                 'WC/zz.txt', 'WC_History.txt'])
        numbers = [int(line.split(' ')[1]) for line in run.output.splitlines()
                   if line.startswith('ENTRY ')]
        self.assertEqual(numbers, list(range(len(order))))

    def test_everything_matches(self) -> None:
        s = self.stand
        s.remote('boot.$C', payload(1, 5000))
        s.local('boot.$C', payload(1, 5000))
        s.remote('WC/A.WMF', payload(2, 70000))
        s.local('WC/A.WMF', payload(2, 70000))
        s.publish()
        run = s.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.status('boot.$C'), SAME)
        self.assertEqual(run.status('WC/A.WMF'), SAME)
        self.assertIn('All files match GitHub 5dc0a43', run.states[-1][4])

    def test_github_error_is_reported(self) -> None:
        self.scenario()
        (self.stand.github / '_tree.json').unlink()
        run = self.stand.run('check')
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.states[-1][0], 255)
        self.assertIn('HTTP 404', run.states[-1][4])


if __name__ == '__main__':
    unittest.main()
