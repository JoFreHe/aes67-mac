#!/usr/bin/env python3
"""Regression: downloaded sources must not quarantine the packaged launchd job."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('checker', root / 'scripts/check-package-quarantine.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
with tempfile.TemporaryDirectory(prefix='aes67-quarantine-') as temp:
    temp = Path(temp)
    source = temp / 'source'
    source.mkdir()
    plist = source / 'clock.plist'
    plist.write_bytes((root / 'Installer/org.communityaudio.aes67.clock.plist').read_bytes())
    subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', '0083;00000000;AES67Regression;', str(plist)], check=True)
    subprocess.run(['/usr/bin/xattr', '-w', 'com.apple.quarantine', '0083;00000000;AES67Regression;', str(source)], check=True)
    assert set(checker.check(source)) == {str(source), str(plist)}
    result = subprocess.run(['python3', str(root / 'scripts/check-package-quarantine.py'), str(source)], capture_output=True)
    assert result.returncode == 1 and b'Quarantined package input' in result.stderr
    subprocess.run(['/bin/cp', '-RX', str(source), str(temp / 'payload')], check=True)
    # macOS may preserve quarantine even with cp -X; staging cleanup is explicit.
    subprocess.run(['python3', str(root / 'scripts/check-package-quarantine.py'), '--clean-staging', str(temp / 'payload')], check=True)
    assert checker.check(temp / 'payload') == []
    assert (temp / 'payload/clock.plist').read_bytes() == plist.read_bytes()
    assert subprocess.check_output(['/usr/bin/xattr', '-p', 'com.apple.quarantine', str(plist)])  # Original is never changed.
    subprocess.run(['/usr/bin/plutil', '-lint', str(temp / 'payload/clock.plist')], check=True)
    missing = subprocess.run(['python3', str(root / 'scripts/check-package-quarantine.py'), str(temp / 'missing')], capture_output=True)
    assert missing.returncode == 1
print('PASS: quarantined source rejected; clean copy preserves plist bytes and source attributes; missing input rejected')
