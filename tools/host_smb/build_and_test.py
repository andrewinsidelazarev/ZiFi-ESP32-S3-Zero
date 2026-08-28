import subprocess
import os
import time
import shutil
from pathlib import Path

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
    with server_log_path.open('w', encoding='utf-8') as server_log:
        server_proc = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(test_share), str(port)],
            stdout=server_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
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

    # WRITE второго Open уже подтверждён клиенту, но при медленном физическом
    # канале остаётся в очереди FILEX. CLOSE первого Open обязан занять своё
    # место в общей FIFO и дождаться этой записи без IO_DEVICE_ERROR.
    close_port = 14448
    close_share = Path('.test-build/test_share_close_fifo')
    if close_share.exists():
        shutil.rmtree(close_share)
    close_share.mkdir(parents=True)
    close_log_path = out / 'host_smb_close_fifo.log'
    close_test = None
    with close_log_path.open('w', encoding='utf-8') as close_log:
        close_server = subprocess.Popen(
            ['.test-build/host_smb/host_smb.exe', str(close_share),
             str(close_port), '6000'],
            stdout=close_log, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(1.5)
            print('Running CLOSE/WRITE FIFO regression...')
            close_test = subprocess.run(
                ['.test-build/host_smb/smb_reproduce_test.exe',
                 f'127.0.0.1:{close_port}', 'SD', 'test33'],
                capture_output=True, text=True, timeout=90)
            print("CLOSE FIFO TEST OUTPUT:\n" + close_test.stdout)
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

    # Windows перед CLOSE присылает BASIC_INFORMATION. SMB с первого WRITE
    # использует FILEX mode=3, поэтому данные и SET_METADATA остаются в одном
    # физическом контексте без отложенного последовательного APPEND. Тесты
    # используют точные размеры clock.wmf из 0.6.79 и plasmatw.wmf из 0.6.80.
    metadata_test = None
    sequential_close_test = None
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
    print("Test finished!")

if __name__ == '__main__':
    main()
