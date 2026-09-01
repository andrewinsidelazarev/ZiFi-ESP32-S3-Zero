import subprocess
import os
import socket
import struct
import time
import shutil
import uuid
from pathlib import Path

def probe_negotiate_security_mode(host, port):
    dialects = (0x0202, 0x0210, 0x0300, 0x0302)
    header = (b'\xfeSMB' +
              struct.pack('<HHIHHIIQIIQ', 64, 0, 0, 0, 1, 0, 0,
                          0, 0xfeff, 0, 0) + bytes(16))
    body = (struct.pack('<HHHHI', 36, len(dialects),
                        0x0001, 0, 0) +
            uuid.uuid4().bytes + struct.pack('<IHH', 0, 0, 0) +
            b''.join(struct.pack('<H', dialect) for dialect in dialects))
    packet = header + body
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.sendall(b'\x00' + len(packet).to_bytes(3, 'big') + packet)
        netbios = connection.recv(4)
        if len(netbios) != 4:
            raise RuntimeError('Short SMB NEGOTIATE NetBIOS header')
        expected = int.from_bytes(netbios[1:4], 'big')
        response = bytearray()
        while len(response) < expected:
            part = connection.recv(expected - len(response))
            if not part:
                raise RuntimeError('Short SMB NEGOTIATE response')
            response.extend(part)
    if len(response) < 88 or response[:4] != b'\xfeSMB':
        raise RuntimeError('Invalid SMB NEGOTIATE response')
    status = struct.unpack_from('<I', response, 8)[0]
    security_mode = struct.unpack_from('<H', response, 66)[0]
    dialect = struct.unpack_from('<H', response, 68)[0]
    server_guid = bytes(response[72:88])
    if status != 0 or security_mode != 0x0001:
        raise RuntimeError(
            f'SMB signing is not optional: '
            f'status=0x{status:08x} security_mode=0x{security_mode:04x} '
            f'dialect=0x{dialect:04x}')
    if ((server_guid[7] & 0xf0) != 0x40 or
            (server_guid[8] & 0xc0) != 0x80):
        raise RuntimeError(
            'SMB ServerGuid is not a wire-order UUID v4: ' +
            server_guid.hex())
    print('NEGOTIATED SIGNING: enabled, not required (PASS); '
          f'ServerGuid={uuid.UUID(bytes_le=server_guid)}')
    return server_guid

def main():
    root = Path(__file__).resolve().parent.parent.parent
    os.chdir(root)
    print("Working directory:", root)

    vswhere = r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    res = subprocess.run([vswhere, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], capture_output=True, text=True)
    vs_path = res.stdout.strip()
    print("Found VS:", vs_path)
    vcvars = os.path.join(vs_path, r'VC\Auxiliary\Build\vcvars64.bat')

    out = Path('.test-build/host_smb')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'probe4').mkdir(parents=True, exist_ok=True)

    includes = r'/Itests\stubs_host /Iinclude /Ilib\libsmb2\include /Ilib\libsmb2\include\smb2 /Itools\host_smb'
    sources = r'tools\host_smb\main.cpp tools\host_smb\z80_sim.cpp tools\host_smb\crash_report.cpp src\smb_server.cpp src\vfs_bridge.cpp src\vfs_client.cpp src\directory_cache.cpp src\fat_allocation_cache.cpp src\spsc_ring.cpp src\protocol.cpp src\uart_transport.cpp src\ws_discovery.cpp src\ws_discovery_xml.cpp src\diagnostic_log.cpp'

    libsmb2_files = [str(p) for p in Path('lib/libsmb2/lib').glob('*.c') if not any(k in p.name for k in ['krb5', 'aes_apple', 'dreamcast'])]
    libsmb2 = ' '.join(libsmb2_files)

    cmd_host = f'cl.exe /nologo /std:c++17 /EHsc /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS /DZIFI_DIAGNOSTIC_LOG=1 /DZIFI_HOST_BUILD /D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h {includes} {sources} {libsmb2} /Fo.test-build\\host_smb\\ /Fd.test-build\\host_smb\\host_smb.pdb /Fe.test-build\\host_smb\\host_smb.exe /link ws2_32.lib'

    cmd_reproduce = f'cl.exe /nologo /TC /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS /D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h /Itests\\stubs_host /Ilib\\libsmb2\\include /Ilib\\libsmb2\\include\\smb2 tools\\host_smb\\smb_reproduce_test.c {libsmb2} /Fo.test-build\\host_smb\\probe4\\ /Fd.test-build\\host_smb\\smb_reproduce_test.pdb /Fe.test-build\\host_smb\\smb_reproduce_test.exe /link ws2_32.lib'

    bat_content = f'''@echo off
call "{vcvars}" >nul
rem Keep debug records in OBJ files; concurrent compilers must not share a PDB.
set "CL_MPCount=1"
set "_CL_=/Z7 /MP1 /FS %_CL_%"
echo Compiling host_smb.exe...
{cmd_host}
if errorlevel 1 exit /b 1
echo Compiling smb_reproduce_test.exe...
{cmd_reproduce}
if errorlevel 1 exit /b 1
echo Compilation SUCCESS!
'''
    build_script = out / 'build_host_test.bat'
    build_script.write_text(bat_content, encoding='utf-8')
    print("Building MSVC binaries...")
    r = subprocess.run(['cmd.exe', '/c', str(build_script)], capture_output=True, text=True)
    print("Build STDOUT:", r.stdout[-1500:])
    if r.returncode != 0:
        print("Build STDERR:", r.stderr)
        raise RuntimeError("Build failed")

    # Now let's set up test share directory
    test_share = Path('.test-build/test_share')
    if test_share.exists():
        shutil.rmtree(test_share)
    test_share.mkdir(parents=True, exist_ok=True)
    (test_share / 'sample.txt').write_text("Hello World Sample Text", encoding='utf-8')

    port = 14445
    print(f"Starting host_smb.exe on port {port} pointing to {test_share}...")
    # Не оставляем stdout сервера в непрочитанном PIPE: подробный SMB-журнал
    # заполняет системный буфер и блокирует сервер посреди длинной регрессии.
    server_log_path = out / 'host_smb.log'
    test_proc = None
    first_server_guid = None
    with server_log_path.open('w', encoding='utf-8') as server_log:
        server_proc = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(test_share), str(port)],
            stdout=server_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
            first_server_guid = probe_negotiate_security_mode(
                '127.0.0.1', port)
            print("Running smb_reproduce_test.exe...")
            test_proc = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{port}', 'SD'],
                capture_output=True, text=True)
            print("TEST OUTPUT:\n" + test_proc.stdout)
        finally:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()
                server_proc.wait(timeout=2)

    if test_proc is None or test_proc.returncode != 0:
        server_tail = server_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("SERVER LOG TAIL:\n" + server_tail)
        raise RuntimeError("SMB regression failed")

    # [MS-SMB2] 3.3.3 требует новый ServerGuid после новой инициализации
    # сервера. Два соединения одного процесса уже сравнивает C-регрессия;
    # отдельный короткий процесс доказывает смену GUID между экземплярами.
    identity_port = 14451
    identity_log_path = out / 'host_smb_identity_restart.log'
    second_server_guid = None
    with identity_log_path.open('w', encoding='utf-8') as identity_log:
        identity_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(test_share),
             str(identity_port)],
            stdout=identity_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
            second_server_guid = probe_negotiate_security_mode(
                '127.0.0.1', identity_port)
        finally:
            identity_server.terminate()
            try:
                identity_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                identity_server.kill()
                identity_server.wait(timeout=2)
    if (first_server_guid is None or second_server_guid is None or
            first_server_guid == second_server_guid):
        raise RuntimeError(
            'SMB ServerGuid was reused across server initialization')
    print('SERVER INSTANCE IDENTITY: new UUID v4 after restart (PASS)')

    # Сжатый тайм-аут воспроизводит дефект реального медленного FILEX без
    # двухминутного ожидания: третий WRITE и ожидающие READ из TEST 8 должны
    # пережить обычный PDU timeout, пока ими владеет очередь приложения.
    timeout_port = 14446
    timeout_env = os.environ.copy()
    timeout_env['ZIFI_HOST_PDU_TIMEOUT_SECONDS'] = '1'
    timeout_log_path = out / 'host_smb_timeout.log'
    timeout_test = None
    with timeout_log_path.open('w', encoding='utf-8') as timeout_log:
        timeout_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(test_share),
             str(timeout_port), '30000'],
            stdout=timeout_log, stderr=subprocess.STDOUT, text=True,
            env=timeout_env)
        try:
            time.sleep(1.5)
            print('Running deferred-request timeout regression...')
            timeout_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{timeout_port}', 'SD', 'test8'],
                capture_output=True, text=True, timeout=120)
            print("TIMEOUT TEST OUTPUT:\n" + timeout_test.stdout)
        finally:
            timeout_server.terminate()
            try:
                timeout_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                timeout_server.kill()
                timeout_server.wait(timeout=2)

    if timeout_test is None or timeout_test.returncode != 0:
        timeout_tail = timeout_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("TIMEOUT SERVER LOG TAIL:\n" + timeout_tail)
        raise RuntimeError("Deferred SMB request expired in libsmb2 waitqueue")

    # Реальный SD->SD Copy создаёт каталог назначения с другого SMB-сеанса,
    # пока исходный FINDNEXT ещё выполняется на единственном FILEX-мосте.
    # Отдельная задержка делает окно гонки детерминированным: старая реализация
    # немедленно отвечала ACCESS_DENIED, исправленная ждёт старший запрос FIFO.
    mkdir_port = 14447
    mkdir_share = Path('.test-build/test_share_mkdir')
    if mkdir_share.exists():
        shutil.rmtree(mkdir_share)
    mkdir_share.mkdir(parents=True)
    mkdir_log_path = out / 'host_smb_mkdir_fifo.log'
    mkdir_test = None
    with mkdir_log_path.open('w', encoding='utf-8') as mkdir_log:
        mkdir_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(mkdir_share),
             str(mkdir_port), '0', 'ZX-Evo', '250'],
            stdout=mkdir_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
            print('Running MKDIR/FINDNEXT FIFO regression...')
            mkdir_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{mkdir_port}', 'SD', 'test32'],
                capture_output=True, text=True, timeout=60)
            print("MKDIR FIFO TEST OUTPUT:\n" + mkdir_test.stdout)
        finally:
            mkdir_server.terminate()
            try:
                mkdir_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                mkdir_server.kill()
                mkdir_server.wait(timeout=2)

    if mkdir_test is None or mkdir_test.returncode != 0:
        mkdir_tail = mkdir_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("MKDIR FIFO SERVER LOG TAIL:\n" + mkdir_tail)
        raise RuntimeError("Deferred MKDIR failed behind active FINDNEXT")

    # Незавершённый WRITE второго Open и CLOSE первого Open одновременно
    # находятся на проводе. CLOSE обязан занять своё место в общей FIFO и
    # дождаться физической записи без IO_DEVICE_ERROR.
    close_port = 14448
    close_share = Path('.test-build/test_share_close_fifo')
    if close_share.exists():
        shutil.rmtree(close_share)
    close_share.mkdir(parents=True)
    close_log_path = out / 'host_smb_close_fifo.log'
    close_test = None
    write_cancel_test = None
    logical_close_test = None
    close_env = os.environ.copy()
    close_env['ZIFI_HOST_INTERIM_PENDING_MS'] = '100'
    logical_close_env = os.environ.copy()
    logical_close_env['ZIFI_EXPECT_LOGICAL_CLOSE_FIRST'] = '1'
    with close_log_path.open('w', encoding='utf-8') as close_log:
        close_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(close_share),
             str(close_port), '6000'],
            stdout=close_log, stderr=subprocess.STDOUT, text=True,
            env=close_env)
        try:
            time.sleep(1.5)
            print('Running CLOSE/WRITE FIFO regression...')
            close_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port}', 'SD', 'test33'],
                capture_output=True, text=True, timeout=90)
            print("CLOSE FIFO TEST OUTPUT:\n" + close_test.stdout)
            print('Running 64 KiB WRITE CANCEL regression...')
            write_cancel_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port}', 'SD', 'test38'],
                capture_output=True, text=True, timeout=90)
            print("WRITE CANCEL TEST OUTPUT:\n" + write_cancel_test.stdout)
            print('Running metadata CLOSE isolation regression...')
            logical_close_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port}', 'SD', 'test41'],
                capture_output=True, text=True, timeout=90,
                env=logical_close_env)
            print("METADATA CLOSE TEST OUTPUT:\n" +
                  logical_close_test.stdout)
        finally:
            close_server.terminate()
            try:
                close_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                close_server.kill()
                close_server.wait(timeout=2)

    if close_test is None or close_test.returncode != 0:
        close_tail = close_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("CLOSE FIFO SERVER LOG TAIL:\n" + close_tail)
        raise RuntimeError("Deferred CLOSE failed behind physical WRITE")
    if write_cancel_test is None or write_cancel_test.returncode != 0:
        close_tail = close_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("WRITE CANCEL SERVER LOG TAIL:\n" + close_tail)
        raise RuntimeError("64 KiB WRITE CANCEL regression failed")
    if logical_close_test is None or logical_close_test.returncode != 0:
        close_tail = close_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("METADATA CLOSE SERVER LOG TAIL:\n" + close_tail)
        raise RuntimeError("Metadata CLOSE waited behind unrelated WRITE")

    # На железе один 64-КиБ FILEX READ уже приближался к 60 секундам. Здесь
    # медленный UART и 100-мс порог детерминированно проверяют отдельный
    # STATUS_PENDING, единый AsyncId и восстановление credits после финала.
    pending_port = 14449
    pending_share = Path('.test-build/test_share_pending')
    if pending_share.exists():
        shutil.rmtree(pending_share)
    pending_share.mkdir(parents=True)
    pending_env = os.environ.copy()
    pending_env['ZIFI_HOST_INTERIM_PENDING_MS'] = '100'
    pending_log_path = out / 'host_smb_interim_pending.log'
    pending_test = None
    with pending_log_path.open('w', encoding='utf-8') as pending_log:
        pending_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(pending_share),
             str(pending_port), '8192'],
            stdout=pending_log, stderr=subprocess.STDOUT, text=True,
            env=pending_env)
        try:
            time.sleep(1.5)
            print('Running delayed READ interim STATUS_PENDING regression...')
            pending_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{pending_port}', 'SD', 'test39'],
                capture_output=True, text=True, timeout=60)
            print("INTERIM PENDING TEST OUTPUT:\n" + pending_test.stdout)
        finally:
            pending_server.terminate()
            try:
                pending_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pending_server.kill()
                pending_server.wait(timeout=2)

    if pending_test is None or pending_test.returncode != 0:
        pending_tail = pending_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("INTERIM PENDING SERVER LOG TAIL:\n" + pending_tail)
        raise RuntimeError("Delayed SMB READ did not preserve async contract")

    # CopyFile сообщает конечный EOF до первого WRITE, затем на границе 4 МиБ
    # делает metadata-only Open и FLUSH при уже поставленных внепорядковых WRITE.
    # SET_EOF должен физически завершиться через PENDING, а FLUSH — занять место
    # FIFO-барьера и ответить только после всех старших WRITE.
    set_eof_port = 14450
    set_eof_share = Path('.test-build/test_share_set_eof')
    if set_eof_share.exists():
        shutil.rmtree(set_eof_share)
    set_eof_share.mkdir(parents=True)
    set_eof_log_path = out / 'host_smb_set_eof.log'
    set_eof_test = None
    set_eof_env = os.environ.copy()
    set_eof_env['ZIFI_HOST_INTERIM_PENDING_MS'] = '100'
    with set_eof_log_path.open('w', encoding='utf-8') as set_eof_log:
        set_eof_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(set_eof_share),
             str(set_eof_port), '0', 'ZX-Evo', '0', '600', '350',
             str(4 * 1024 * 1024)],
            stdout=set_eof_log, stderr=subprocess.STDOUT, text=True,
            env=set_eof_env)
        try:
            time.sleep(1.5)
            print('Running physical EOF and 4 MiB FLUSH FIFO regression...')
            set_eof_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{set_eof_port}', 'SD', 'test40'],
                capture_output=True, text=True, timeout=60)
            print("4 MIB FLUSH TEST OUTPUT:\n" + set_eof_test.stdout)
        finally:
            set_eof_server.terminate()
            try:
                set_eof_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                set_eof_server.kill()
                set_eof_server.wait(timeout=2)

    if set_eof_test is None or set_eof_test.returncode != 0:
        set_eof_tail = set_eof_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("4 MIB FLUSH SERVER LOG TAIL:\n" + set_eof_tail)
        raise RuntimeError(
            "Physical SET_EOF or 4 MiB FLUSH FIFO barrier failed")
    set_eof_lines = set_eof_log_path.read_text(
        encoding='utf-8', errors='replace').splitlines()
    first_write = next(
        (i for i, line in enumerate(set_eof_lines)
         if line.startswith('[COPYING] WRITE ')), -1)
    done_write = next(
        (i for i, line in enumerate(set_eof_lines[first_write + 1:],
                                    first_write + 1)
         if line.startswith('[COPYING] DONE ')), -1)
    if first_write < 0 or done_write < 0:
        raise RuntimeError("4 MiB regression emitted no WRITE/DONE progress")
    transfer_lines = set_eof_lines[first_write:done_write + 1]
    if '[COPYING] ' in transfer_lines:
        raise RuntimeError("Metadata Open reset Copying at the 4 MiB boundary")
    if any(line.startswith('[LAST] OPEN ') for line in transfer_lines):
        raise RuntimeError("Metadata Open replaced active WRITE indication")
    print("4 MiB metadata Open: progress and active WRITE preserved")

    # Windows перед CLOSE присылает BASIC_INFORMATION. SMB с первого WRITE
    # использует FILEX mode=3, поэтому данные и SET_METADATA остаются в одном
    # физическом контексте без отложенного последовательного APPEND. Тесты
    # используют точные размеры clock.wmf из 0.6.79 и plasmatw.wmf из 0.6.80.
    metadata_test = None
    sequential_close_test = None
    progress_test = None
    maxwrite_test = None
    metadata_log_path = out / 'host_smb_metadata_close.log'
    with metadata_log_path.open('w', encoding='utf-8') as metadata_log:
        metadata_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(close_share),
             str(close_port + 1)],
            stdout=metadata_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
            print('Running random WRITE/metadata CLOSE regression...')
            metadata_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port + 1}', 'SD', 'test34'],
                capture_output=True, text=True, timeout=90)
            print("METADATA CLOSE TEST OUTPUT:\n" + metadata_test.stdout)
            print('Running deferred sequential CLOSE rejection regression...')
            sequential_close_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port + 1}', 'SD', 'test35'],
                capture_output=True, text=True, timeout=90)
            print("SEQUENTIAL CLOSE TEST OUTPUT:\n" +
                  sequential_close_test.stdout)
            print('Running negotiated 64 KiB WRITE regression...')
            maxwrite_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port + 1}', 'SD', 'test37'],
                capture_output=True, text=True, timeout=90)
            print("64 KIB WRITE TEST OUTPUT:\n" + maxwrite_test.stdout)
            print('Running out-of-order progress regression...')
            progress_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port + 1}', 'SD', 'test36'],
                capture_output=True, text=True, timeout=90)
            print("OUT-OF-ORDER PROGRESS TEST OUTPUT:\n" +
                  progress_test.stdout)
        finally:
            metadata_server.terminate()
            try:
                metadata_server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                metadata_server.kill()
                metadata_server.wait(timeout=2)

    if metadata_test is None or metadata_test.returncode != 0:
        metadata_tail = metadata_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("METADATA CLOSE SERVER LOG TAIL:\n" + metadata_tail)
        raise RuntimeError(
            "Random WRITE metadata CLOSE did not preserve the file")
    if (sequential_close_test is None or
            sequential_close_test.returncode != 0):
        metadata_tail = metadata_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("SEQUENTIAL CLOSE SERVER LOG TAIL:\n" + metadata_tail)
        raise RuntimeError(
            "New SMB file still depended on deferred sequential CLOSE")
    if progress_test is None or progress_test.returncode != 0:
        metadata_tail = metadata_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("OUT-OF-ORDER PROGRESS SERVER LOG TAIL:\n" + metadata_tail)
        raise RuntimeError("Out-of-order WRITE progress regression failed")
    if maxwrite_test is None or maxwrite_test.returncode != 0:
        metadata_tail = metadata_log_path.read_text(
            encoding='utf-8', errors='replace')[-4000:]
        print("64 KIB WRITE SERVER LOG TAIL:\n" + metadata_tail)
        raise RuntimeError("Negotiated 64 KiB WRITE regression failed")

    server_text = server_log_path.read_text(encoding='utf-8', errors='replace')

    def has_partial_progress(operation):
        prefix = f'[COPYING] {operation} '
        for line in server_text.splitlines():
            if not line.startswith(prefix):
                continue
            values = line[len(prefix):].split('/', 1)
            if len(values) == 2 and values[0] != values[1]:
                return True
        return False

    missing_progress = [
        operation for operation in ('READ', 'WRITE')
        if not has_partial_progress(operation)
    ]
    if missing_progress:
        raise RuntimeError(
            'No partial Copying progress for: ' + ', '.join(missing_progress))
    print("Copying progress: partial READ and WRITE events found")

    # TEST 36 идёт последним и закрывает полностью покрытый файл после
    # внепорядковых и повторных WRITE. Старый счётчик показывал конец
    # последнего блока (256 КиБ), наивная сумма показала бы 448 КиБ.
    metadata_text = metadata_log_path.read_text(
        encoding='utf-8', errors='replace')
    maxwrite_operations = metadata_text.count(
        '[LAST] WRITE /write_64k.bin')
    if maxwrite_operations != 1:
        raise RuntimeError(
            '64 KiB pwrite was split into '
            f'{maxwrite_operations} server WRITE operations')
    print('Negotiated 64 KiB WRITE: one server request verified')
    done_lines = [
        line for line in metadata_text.splitlines()
        if line.startswith('[COPYING] DONE ')
    ]
    expected_done = '[COPYING] DONE 320.0K/320.0K'
    if not done_lines or done_lines[-1] != expected_done:
        actual = done_lines[-1] if done_lines else '<missing>'
        raise RuntimeError(
            f'Out-of-order progress mismatch: {actual}; '
            f'expected {expected_done}')

    progress_file = close_share / 'progress_out_of_order.bin'
    progress_data = progress_file.read_bytes()
    if len(progress_data) != 5 * 65536 or any(
            value != (((offset * 37) ^ (offset >> 8) ^
                       (offset >> 16)) & 0xff)
            for offset, value in enumerate(progress_data)):
        raise RuntimeError('Out-of-order WRITE data verification failed')
    print('Out-of-order progress: unique coverage 320.0K/320.0K verified')
    print("Test finished!")

if __name__ == '__main__':
    main()
