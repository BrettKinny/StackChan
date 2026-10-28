"""AI-assisted by GPT-6: exercise actual display branch and state hooks.

Hardware, scheduling, modifier allocation and LED painting are fake boundaries.
State transitions, event payloads, listening flag and 5 Hz lifecycle are real
source. --source-ref 3140f03 reproduces first speech staying incorrectly Idle.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_REF = None
if '--source-ref' in sys.argv:
    index = sys.argv.index('--source-ref')
    SOURCE_REF = sys.argv[index + 1]
    del sys.argv[index:index + 2]


def read_source(path):
    return (subprocess.check_output(['git', 'show', f'{SOURCE_REF}:firmware/{path}'],
                                   cwd=ROOT, text=True) if SOURCE_REF else (ROOT/path).read_text())


def block(source, start):
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[brace:end]


class VoiceSpeakingStateTest(unittest.TestCase):
    def test_actual_speaking_branch_and_mode_lifecycle(self):
        state = read_source('main/stackchan/modes/state_manager.cpp')
        display = read_source('main/hal/board/stackchan_display.cc')
        methods = []
        for name in ('stateName', 'setState', 'setListening', 'onVoiceListening',
                     'onVoiceSpeaking', 'onVoiceStandby', 'onFaceLost', 'emitStateChanged', '_update'):
            marker = f'StateManager::{name}('
            if marker not in state:
                continue  # Legacy RED has no speaking hook, nor caller.
            start = state.rfind('\n', 0, state.index(marker)) + 1
            methods.append(state[start:state.index('{', start)] + block(state, start))
        speaking = block(display, display.index('} else if (strcmp(status, Lang::Strings::SPEAKING)'))
        fixture = (ROOT/'tests/voice_speaking_platform_stub.h').read_text()
        implementation = '\nnamespace stackchan {\n' + '\n'.join(methods) + '\n}\n'
        implementation += 'void Display::Speaking() { auto& stackchan=::GetStackChan();\n' + speaking + '\n}\n'
        with tempfile.TemporaryDirectory(prefix='dotty-speaking-test-') as directory:
            temp = Path(directory)
            (temp/'test.cc').write_text(fixture + implementation
                                      + (ROOT/'tests/voice_speaking_state_test.cpp').read_text())
            subprocess.run(['g++', '-std=c++17', str(temp/'test.cc'), '-o', str(temp/'test')], check=True)
            subprocess.run([str(temp/'test')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
