"""AI-assisted (GPT-6): compile actual patched wake callers and continuation.

Use --patch-ref 1b2b0cb for the legacy RED case. Source is reconstructed from
the pinned upstream tag and the committed patch, not a rewritten state model.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH_REF = None
if '--patch-ref' in sys.argv:
    index = sys.argv.index('--patch-ref')
    PATCH_REF = sys.argv[index + 1]
    del sys.argv[index:index + 2]


def method(source, name):
    start = source.index('void Application::' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class WakeChannelStateTest(unittest.TestCase):
    def test_real_callers_with_live_or_closed_channel_and_privacy(self):
        patch = (subprocess.check_output(['git', 'show',
                    f'{PATCH_REF}:firmware/patches/xiaozhi-esp32.patch'], cwd=ROOT, text=True)
                 if PATCH_REF else (ROOT/'patches/xiaozhi-esp32.patch').read_text())
        section = next(part for part in patch.split('diff --git ')
                       if part.startswith('c/main/application.cc '))
        original = subprocess.check_output(['git', 'show', 'v2.2.4:main/application.cc'],
                                           cwd=ROOT/'xiaozhi-esp32', text=True)
        with tempfile.TemporaryDirectory(prefix='dotty-wake-test-') as directory:
            temp = Path(directory)
            (temp/'main').mkdir()
            (temp/'main/application.cc').write_text(original)
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', str(temp)],
                           input='diff --git '+section, text=True, check=True)
            source = (temp/'main/application.cc').read_text()
            implementation = '\n'.join(method(source, name) for name in
                ('HandleWakeWordDetectedEvent', 'WakeWordInvoke', 'ContinueWakeWordInvoke'))
            generated = temp/'test.cc'
            generated.write_text((ROOT/'tests/wake_channel_platform_stub.h').read_text()
                                 + implementation
                                 + (ROOT/'tests/wake_channel_state_test.cpp').read_text())
            binary = temp/'wake-test'
            subprocess.run(['g++', '-std=c++17', str(generated), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
