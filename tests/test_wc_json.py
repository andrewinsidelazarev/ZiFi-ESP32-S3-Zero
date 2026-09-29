"""Разбор ответов GitHub API и SHA-1 обновлятора WC (src/github_tree.cpp,
src/git_sha1.cpp) — отдельно от стенда, на неудобных входах.

Эталон списка файлов нельзя угадывать: любой JSON не по форме, обрезанный
список, опасный путь или неверный SHA обязаны давать отказ, а не частичный
список. SHA-1 сверяется с hashlib на длинах вокруг границ блока 64 байта.
Без Visual Studio Build Tools тесты пропускаются.
"""
import hashlib
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'wc_json'
SHA = 'a' * 40


class Host:
    def __init__(self, exe: pathlib.Path) -> None:
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True,
                                        encoding='utf-8')
        self.folder = pathlib.Path(tempfile.mkdtemp(prefix='wc_json_'))
        self.counter = 0

    def ask(self, command: str, data: bytes) -> str:
        self.counter += 1
        path = self.folder / f'{self.counter}.bin'
        path.write_bytes(data)
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(f'{command} {path}\n')
        self.process.stdin.flush()
        return self.process.stdout.readline().strip()

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        self.process.wait(timeout=10)


def tree(entries: list, truncated: bool = False) -> bytes:
    return json.dumps({'sha': SHA, 'url': 'x', 'tree': entries,
                       'truncated': truncated}).encode('utf-8')


def blob(path: str, size: int = 10, sha: str = SHA) -> dict:
    return {'path': path, 'mode': '100644', 'type': 'blob', 'sha': sha,
            'size': size, 'url': 'x'}


class WcJsonTest(unittest.TestCase):
    host: Host

    @classmethod
    def setUpClass(cls) -> None:
        exe = build_exe(BUILD, 'wc_json_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'github_tree.cpp', 'github_tree.obj'),
                         (ROOT / 'src' / 'git_sha1.cpp', 'git_sha1.obj'),
                         (ROOT / 'tests' / 'wc_json_host.cpp', 'wc_json_host.obj')])
        if exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')
        cls.host = Host(exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.host.close()

    def test_sha1_matches_hashlib_around_block_edges(self) -> None:
        for size in (0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 1000, 70000):
            data = bytes((index * 7 + size) & 0xFF for index in range(size))
            with self.subTest(size=size):
                self.assertEqual(self.host.ask('sha1', data), hashlib.sha1(data).hexdigest())
                self.assertEqual(self.host.ask('blob', data),
                                 hashlib.sha1(b'blob %d\0' % size + data).hexdigest())

    def test_ref_gives_commit(self) -> None:
        answer = json.dumps({'ref': 'refs/heads/main', 'node_id': 'x', 'url': 'x',
                             'object': {'sha': 'B' * 40, 'type': 'commit', 'url': 'x'}})
        self.assertEqual(self.host.ask('ref', answer.encode()), 'ok ' + 'B' * 40)

    def test_ref_rejects_tag_and_garbage(self) -> None:
        tag = json.dumps({'object': {'sha': SHA, 'type': 'tag'}})
        self.assertEqual(self.host.ask('ref', tag.encode()), 'fail')
        self.assertEqual(self.host.ask('ref', b'{"object":{"sha":"' + b'a' * 39 +
                                       b'","type":"commit"}}'), 'fail')
        self.assertEqual(self.host.ask('ref', b'{"message":"Not Found"}'), 'fail')
        self.assertEqual(self.host.ask('ref', b'<html>rate limit</html>'), 'fail')

    def test_tree_takes_blobs_and_directories(self) -> None:
        answer = tree([{'path': 'WC', 'mode': '040000', 'type': 'tree', 'sha': 'c' * 40,
                        'url': 'x'},
                       blob('WC/FILEX.WMF', 7237, 'd' * 40),
                       blob('WC/MENU/Hrust v1.3.spg', 9762, 'e' * 40),
                       {'path': 'sub', 'mode': '160000', 'type': 'commit', 'sha': 'f' * 40}])
        self.assertEqual(self.host.ask('tree', answer),
                         'ok 3 0 d 0 ' + 'c' * 40 + ' WC f 7237 ' + 'd' * 40 +
                         ' WC/FILEX.WMF f 9762 ' + 'e' * 40 + ' WC/MENU/Hrust v1.3.spg')

    def test_tree_decodes_escapes(self) -> None:
        raw = (b'{"sha":"' + SHA.encode() + b'","tree":[{"path":"WC/\\u0418\\u043c\\u044f.txt",'
               b'"type":"blob","sha":"' + SHA.encode() + b'","size":5}],"truncated":false}')
        self.assertEqual(self.host.ask('tree', raw),
                         'ok 1 0 f 5 ' + SHA + ' WC/Имя.txt')

    def test_tree_skips_valid_numbers(self) -> None:
        answer = (b'{"sha":"' + SHA.encode() + b'","n":[0,-1,2.5,1e9,-3.25E-7,6e+2],'
                  b'"tree":[],"truncated":false}')
        self.assertEqual(self.host.ask('tree', answer), 'ok 0 0')

    def test_tree_reports_truncation(self) -> None:
        self.assertEqual(self.host.ask('tree', tree([blob('a')], truncated=True)),
                         'ok 1 1 f 10 ' + SHA + ' a')

    def test_tree_rejects_unsafe_or_broken_input(self) -> None:
        cases = {
            'parent path': tree([blob('../boot.$C')]),
            'absolute path': tree([blob('/boot.$C')]),
            'empty part': tree([blob('WC//A.WMF')]),
            'backslash': tree([blob('WC\\A.WMF')]),
            'short sha': tree([blob('a', sha='a' * 39)]),
            'blob without size': json.dumps({'tree': [{'path': 'a', 'type': 'blob',
                                                       'sha': SHA}],
                                             'truncated': False}).encode(),
            'no truncated flag': json.dumps({'tree': [blob('a')]}).encode(),
            'cut answer': tree([blob('a'), blob('b')])[:-20],
            'trailing garbage': tree([blob('a')]) + b'x',
            'too many entries': tree([blob(f'f{index}') for index in range(65)]),
            'error message': b'{"message":"API rate limit exceeded"}',
            'bad number': (b'{"tree":[],"truncated":false,"x":1e+}'),
            'bad fraction': (b'{"tree":[],"truncated":false,"x":1.}'),
            'leading zero size': (b'{"tree":[{"path":"a","type":"blob","sha":"'
                                  + SHA.encode() + b'","size":012}],"truncated":false}'),
            'NUL in path': (b'{"tree":[{"path":"a\\u0000b","type":"blob","sha":"'
                            + SHA.encode() + b'","size":1}],"truncated":false}'),
        }
        for name, data in cases.items():
            with self.subTest(name):
                self.assertEqual(self.host.ask('tree', data), 'fail')


if __name__ == '__main__':
    unittest.main()
