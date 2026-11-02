"""Real App/host/capture keyboard regression, using private runtime profiles.

The strict fixture deliberately places capture below the host and foreground
fixture, reproducing Windows' unmodified-key integrity boundary without UAC.
No clipboard writes: every real capture is cancelled with Escape.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--capture-dir', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--strict', action='store_true')
    parser.add_argument('--english', action='store_true')
    parser.add_argument('--expect-local-failure', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
    user.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
    if not user.RegisterHotKey(None, 0x7118, 0x4000, 0x70):
        raise SystemExit('F1 is already registered; no keyboard input was sent.')
    user.UnregisterHotKey(None, 0x7118)
    env = os.environ.copy()
    env['QT_FORCE_STDERR_LOGGING'] = '1'
    source = Path(__file__).resolve().parents[1] / 'src/capture-plugin/package'
    with tempfile.TemporaryDirectory(prefix='WinShun-hotkey-package-') as temporary:
        fixture = Path(temporary) / 'winshun.capture'
        shutil.copytree(source, fixture)
        for name in ('WinShunCapture.exe', 'WinShunCaptureApp.dll'):
            shutil.copy2(args.capture_dir / name, fixture / name)
        manifest_path = fixture / 'plugin.json'
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if args.strict:
            manifest['runtime']['sandbox'] = 'strict'
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
        command = [str(args.exe.resolve()), '--probe-runtime', str(args.out.resolve()),
                   '--runtime-plugins', temporary, '--capture-hotkey-probe',
                   '--probe-hotkey', 'F1', '--renderer', 'd3d11', '--settle-ms', '100']
        if args.english:
            command.append('--english')
        with (args.out / 'process.log').open('w', encoding='utf-8') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    report_path = args.out / ('capture-hotkey-failed.json' if args.expect_local_failure else 'runtime.json')
    if not report_path.exists():
        failure = args.out / 'capture-hotkey-failed.json'
        raise SystemExit(f'Probe failed (exit {result.returncode}); see {failure if failure.exists() else args.out}')
    report = json.loads(report_path.read_text(encoding='utf-8'))
    if args.strict:
        assert report['capture_integrity_rid'] < report['host_integrity_rid'], report
    if args.expect_local_failure:
        assert result.returncode != 0, result.returncode
        assert report['capture_hotkey_checks'].get('before_settings_overlay') is False, report
        print('Baseline reproduced: F1 registered, but no capture across the integrity boundary.')
    else:
        assert result.returncode == 0, f'Probe failed: {result.returncode}; see {args.out}'
        assert report['capture_hotkeys_passed'], report
        assert report['warnings'] == 0, report['warnings']
        print(f"Passed {len(report['capture_hotkey_checks'])} checks; "
              f"host/capture integrity {report['host_integrity_rid']}/{report['capture_integrity_rid']}.")


if __name__ == '__main__':
    main()
