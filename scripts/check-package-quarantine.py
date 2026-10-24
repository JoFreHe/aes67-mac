#!/usr/bin/env python3
"""Reject quarantined package payload/scripts or explicitly clean quarantine on disposable package staging."""
import subprocess
import sys
from pathlib import Path


def check(root):
    root = Path(root)
    if not root.exists():
        raise OSError(f"Missing package input: {root}")
    paths = [root]
    if root.is_dir():
        paths.extend(root.rglob('*'))
    bad = []
    for path in paths:
        result = subprocess.run(['/usr/bin/xattr', '-s', str(path)], capture_output=True, text=True)
        if result.returncode != 0:
            raise OSError(result.stderr.strip() or f'Cannot inspect {path}')
        if 'com.apple.quarantine' in result.stdout.splitlines():
            bad.append(str(path))
    return bad


if __name__ == '__main__':
    try:
        arguments = sys.argv[1:]
        clean = arguments[:1] == ['--clean-staging']
        if clean:
            arguments = arguments[1:]
        if not arguments:
            raise ValueError('Usage: check-package-quarantine.py PATH [PATH ...]')
        failures = [p for root in arguments for p in check(root)]
        if clean:
            # Caller supplies only its disposable build staging, never installed paths.
            for path in failures:
                subprocess.run(['/usr/bin/xattr', '-s', '-d', 'com.apple.quarantine', path], check=True)
            failures = [p for root in arguments for p in check(root)]
        if failures:
            raise ValueError('Quarantined package input; rebuild and clean temporary package staging:\n' + '\n'.join(failures))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
    print('Package quarantine check passed')
