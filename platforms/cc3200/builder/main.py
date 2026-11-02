"""PlatformIO/SCons build graph for the CC3200's two SRAM images."""
from pathlib import Path
import hashlib
import json
import os
import re
import secrets
import struct
import zlib
from SCons.Script import Default, DefaultEnvironment

env = DefaultEnvironment()
root = Path(env.subst("$PROJECT_DIR"))
out = Path(env.subst("$BUILD_DIR"))
out.mkdir(parents=True, exist_ok=True)
toolchain = env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi")
env.PrependENVPath("PATH", str(Path(toolchain) / "bin"))
env.Replace(CC="arm-none-eabi-gcc", AS="arm-none-eabi-gcc", LINK="arm-none-eabi-gcc",
            AR="arm-none-eabi-ar", RANLIB="arm-none-eabi-ranlib",
            OBJCOPY="arm-none-eabi-objcopy", SIZE="arm-none-eabi-size")
vendor = root / "vendor/ti"
sl = vendor / "libraries/WiFi/utility"
language = env.GetProjectOption("custom_language", "en")
if not re.fullmatch(r"[a-z][a-z0-9-]*", language):
    raise ValueError("Invalid custom_language")
html_file = root / "web" / (language + ".html")
messages_file = root / "locales" / (language + ".json")
if not html_file.is_file() or not messages_file.is_file():
    raise ValueError("Language needs web/<language>.html and locales/<language>.json")
messages = json.loads(messages_file.read_text())
expected = json.loads((root / "locales/en.json").read_text())
if messages.keys() != expected.keys() or not all(isinstance(v, str) for v in messages.values()):
    raise ValueError("Translation keys must match locales/en.json")
core = (root / "include/core.h").read_text()
version = re.search(r'#define FW_VERSION "([0-9.]+)"', core)[1]
build = int(re.search(r'#define FW_BUILD ([0-9]+)u', core)[1])
html = html_file.read_text().replace("{{version}}", version).encode()

def write_changed(path, data):
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)

# Secret is deliberately environment-local and never printed in the build log.
secret = out / "setup-password.txt"
if not secret.exists():
    fd = os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(secrets.token_urlsafe(12) + "\n")
os.chmod(secret, 0o600)
write_changed(out / "setup_secret.h", ('#define SETUP_PASSWORD ' + json.dumps(secret.read_text().strip()) + '\n').encode())
write_changed(out / "web_asset.h", ('static const char web_page[] = {' + ','.join(map(str, html)) + ',0};\n').encode())
write_changed(out / "messages.h", ''.join('#define MSG_' + k + ' ' + json.dumps(v, ensure_ascii=False) + '\n' for k, v in messages.items()).encode())
env.Append(CCFLAGS=["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-Os", "-g3",
                    "-ffunction-sections", "-fdata-sections", "-fno-common", "-std=gnu11",
                    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-misleading-indentation"],
           ASFLAGS=["-mcpu=cortex-m4", "-mthumb", "-x", "assembler-with-cpp"],
           CPPDEFINES=["TARGET_IS_CC3200", "SL_FULL"],
           CPPPATH=[str(root / "include"), str(out), str(vendor / "system"), str(sl)],
           LINKFLAGS=["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-nostartfiles",
                      "--specs=nano.specs", "-Wl,--gc-sections"],
           LIBS=["c", "m", "gcc"])

sdk = []
sl_env = env.Clone()
sl_env.Append(CCFLAGS=["-Wno-incompatible-pointer-types", "-Wno-pointer-sign"])
for name in "device driver flowcont fs netapp netcfg nonos socket wlan cc_pal".split():
    sdk += sl_env.Object(str(out / ("sl_" + name + ".o")), str(sl / (name + ".c")))
for name in "cpu interrupt prcm utils udma i2s i2c uart gpio pin systick wdt flash spi".split():
    sdk += env.Object(str(out / ("dl_" + name + ".o")), str(vendor / "system/driverlib" / (name + ".c")))

def objects(names):
    return [env.Object(str(out / (name + ".o")), str(root / "src" / (name + ".c")))[0] for name in names.split()]

shared = objects("startup platform led sha256 core http_parse")
def program(name, sources, script):
    linkenv = env.Clone()
    linkenv.Append(LINKFLAGS=["-T" + str(root / "linker" / script), "-Wl,-Map=" + str(out / (name + ".map"))])
    elf = linkenv.Program(str(out / (name + ".elf")), sdk + shared + sources)
    env.Depends(elf, str(root / "linker" / script))
    return env.Command(str(out / (name + ".bin")), elf, "$OBJCOPY -O binary $SOURCE $TARGET")

boot = program("boot-body", objects("boot"), "boot.ld")
app = program("chime-app", objects("app audio codec_bus http mqtt ota sound"), "app.ld")

def boot_length(source, target, env):
    data = Path(str(source[0])).read_bytes()
    size = (len(data) + 3) & ~3
    if not 0 < size < 0x4000:
        raise ValueError("Boot relocation would overwrite the running entry stub")
    Path(str(target[0])).write_text("#define BOOT_BYTES " + str(size) + "\n")

length = env.Command(str(out / "boot_length.h"), boot, boot_length)
stubenv = env.Clone()
stubenv.Append(ASFLAGS=["-include", str(out / "boot_length.h")])
stub = stubenv.Object(str(out / "boot-entry.o"), str(root / "src/boot_entry.S"))
env.Depends(stub, length)
entryenv = env.Clone()
entryenv.Replace(LINKFLAGS=["-mcpu=cortex-m4", "-mthumb", "-nostdlib", "-Wl,-Ttext=0x20004000,-e,_entry"], LIBS=[])
entry = entryenv.Program(str(out / "boot-entry.elf"), stub)
entrybin = env.Command(str(out / "boot-entry.bin"), entry, "$OBJCOPY -O binary $SOURCE $TARGET")

def artifacts(source, target, env):
    body = Path(str(source[0])).read_bytes()
    entry = Path(str(source[1])).read_bytes()
    app = Path(str(source[2])).read_bytes()
    sp, pc = struct.unpack_from("<II", entry)
    if len(entry) > 256 or sp != 0x20010000 or not pc & 1 or not 0x20004020 <= pc < 0x20004100:
        raise ValueError("Invalid ROM entry vectors")
    sp, pc = struct.unpack_from("<II", app)
    if not (1024 <= len(app) <= 0x20000 and 0x20010000 + len(app) < sp <= 0x20040000 and sp % 8 == 0 and pc & 1 and 0x20010000 <= pc < 0x20010000 + len(app)):
        raise ValueError("Invalid application vectors or size")
    (out / "chime-bootloader.bin").write_bytes(entry.ljust(256, b"\0") + body + b"\0" * (-len(body) % 4))
    package = "chime-ng-" + version + ".chime"
    header = struct.pack("<8s6I32s", b"CHIMNG01", 0x43483332, 1, 0x20010000, len(app), build, 0, hashlib.sha256(app).digest())
    (out / package).write_bytes(header + app)
    record = struct.pack("<I4BI", 0x43484231, 0, 255, 0, 0, 1)
    (out / "boot-initial.bin").write_bytes(record + struct.pack("<I", zlib.crc32(record)))
    (out / "chime.wav").write_bytes((root / "assets/chime.wav").read_bytes())
    names = ["chime-bootloader.bin", package, "boot-initial.bin", "chime.wav"]
    manifest = {name: {"size": (out / name).stat().st_size, "sha256": hashlib.sha256((out / name).read_bytes()).hexdigest()} for name in names}
    Path(str(target[0])).write_text(json.dumps(manifest, indent=2) + "\n")

final = env.Command([str(out / "manifest.json"), str(out / "chime-bootloader.bin"), str(out / ("chime-ng-" + version + ".chime")), str(out / "boot-initial.bin"), str(out / "chime.wav")], boot + entrybin + app + [str(root / "assets/chime.wav")], artifacts)
env.Depends(final, str(root / "include/core.h"))
env.Alias("upload", [], lambda **kwargs: (_ for _ in ()).throw(RuntimeError("Use tools/provision.py after making private backups; see docs/flashing.md")))
Default(final)
