#!/usr/bin/env python3
"""Offline regression tests. No serial port, board or real broker is accessed."""
from pathlib import Path
import subprocess
import sys
FW=Path(__file__).resolve().parents[1]
(FW/'build').mkdir(exist_ok=True)
import json
# Test credentials and assets are isolated from real build secrets.
(FW/'build/setup_secret.h').write_text('#define SETUP_PASSWORD "offline-test-password"\n')
html=(FW/'web/en.html').read_text().replace('{{version}}', __import__('re').search(r'#define FW_VERSION "([0-9.]+)"', (FW/'include/core.h').read_text())[1]).encode()
(FW/'build/web_asset.h').write_text('static const char web_page[] = {'+','.join(map(str,html))+',0};\n')
messages=json.loads((FW/'locales/en.json').read_text())
(FW/'build/messages.h').write_text(''.join('#define MSG_'+k+' '+json.dumps(v)+'\n' for k,v in messages.items()))
subprocess.run([sys.executable,str(FW/'tests/test_preflight.py')],check=True)
subprocess.run([sys.executable,str(FW/'tests/test_provision.py')],check=True)
subprocess.run([sys.executable,str(FW/'tests/test_update_uart.py')],check=True)
subprocess.run([sys.executable,str(FW/'tests/test_core.py')],check=True)
subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Wno-unused-parameter','-I',str(FW/'include'),'-include',str(FW/'tests/mock_platform.h'),*[str(FW/'src'/f) for f in ['core.c','sha256.c','ota.c','mqtt.c']],str(FW/'tests/test_runtime.c'),'-o',str(FW/'build/test_runtime')],check=True)
subprocess.run([str(FW/'build/test_runtime')],check=True)

subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Wno-unused-parameter','-I',str(FW/'include'),'-I',str(FW/'build'),'-include',str(FW/'tests/mock_platform.h'),*[str(FW/'src'/f) for f in ['core.c','sha256.c','http_parse.c','http.c']],str(FW/'tests/test_http.c'),'-o',str(FW/'build/test_http')],check=True)
subprocess.run([str(FW/'build/test_http')],check=True)

subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-I',str(FW/'include'),'-I',str(FW/'vendor/ti/system'),'-include',str(FW/'tests/mock_codec.h'),str(FW/'src/codec_bus.c'),str(FW/'tests/test_codec.c'),'-o',str(FW/'build/test_codec')],check=True)
subprocess.run([str(FW/'build/test_codec')],check=True)

subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-I',str(FW/'include'),'-I',str(FW/'vendor/ti/system'),'-include',str(FW/'tests/mock_led.h'),str(FW/'src/led.c'),str(FW/'tests/test_led.c'),'-o',str(FW/'build/test_led')],check=True)
subprocess.run([str(FW/'build/test_led')],check=True)

subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-I',str(FW/'include'),'-I',str(FW/'vendor/ti/system'),'-include',str(FW/'tests/mock_audio.h'),*[str(FW/'src'/f) for f in ['audio.c','core.c','sha256.c']],str(FW/'tests/test_audio.c'),'-o',str(FW/'build/test_audio')],check=True)
subprocess.run([str(FW/'build/test_audio'),str(FW/'assets/chime.wav')],check=True)

subprocess.run(['clang','-std=gnu11','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Wno-unused-parameter','-I',str(FW/'include'),'-include',str(FW/'tests/mock_platform.h'),*[str(FW/'src'/f) for f in ['core.c','sha256.c','sound.c']],str(FW/'tests/test_sound.c'),'-o',str(FW/'build/test_sound')],check=True)
subprocess.run([str(FW/'build/test_sound')],check=True)
