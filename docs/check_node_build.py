"""Compile and link firmware/ro_node/ro_node.ino for an ATmega328P, and check it fits.

Until 2026-09-02 this sketch had never been compiled in its life. It had by then
grown a 4-20 mA pressure reader and a bit-banged 1-Wire driver, and the plan was
to flash it to three boards on a roof. That is a bad place to discover a typo.

The size check is the part that will actually catch something one day. A Nano has
32 KB of flash and 2 KB of RAM, and this sketch carries SoftwareSerial, Wire, a
median filter, an ADC path and a 1-Wire driver. Flash creeps up quietly; RAM
creeps up silently and then the board just behaves strangely, because the linker
happily fills static memory and leaves nothing for the stack.

SKIPS (exit 0) when no Arduino AVR toolchain is installed - this must not fail on
a machine that only builds the ESP32 side.

    python docs/check_node_build.py          # from the repo root
"""
import glob
import os
import subprocess
import sys
import tempfile

SKETCH = 'firmware/ro_node/ro_node.ino'

MCU = 'atmega328p'
FLASH_BYTES = 30720      # 32 K less the bootloader
RAM_BYTES = 2048

# Leave real headroom rather than "it linked". The stack lives in whatever RAM
# the linker did not take, and an AVR that runs out does not report it - it
# corrupts a variable somewhere and behaves oddly.
MAX_FLASH_PCT = 85
MAX_RAM_PCT = 70


def find_toolchain():
    """(core_dir, bin_dir) for the newest installed AVR core, or None."""
    roots = [
        os.path.expandvars(r'%LOCALAPPDATA%\Arduino15\packages\arduino'),
        os.path.expanduser('~/.arduino15/packages/arduino'),
        os.path.expanduser('~/Library/Arduino15/packages/arduino'),
    ]
    for root in roots:
        cores = sorted(glob.glob(os.path.join(root, 'hardware', 'avr', '*')))
        gccs = sorted(glob.glob(os.path.join(root, 'tools', 'avr-gcc', '*', 'bin')))
        if cores and gccs:
            return cores[-1], gccs[-1]
    return None


def main():
    found = find_toolchain()
    if not found:
        print('SKIP: no Arduino AVR toolchain installed - node sketch not compiled.')
        return 0
    core, binf = found

    def tool(name):
        p = os.path.join(binf, name)
        return p + '.exe' if os.path.exists(p + '.exe') else p

    tmp = tempfile.mkdtemp()
    inc = ['-I' + os.path.join(core, 'cores', 'arduino'),
           '-I' + os.path.join(core, 'variants', 'eightanaloginputs'),
           '-I' + os.path.join(core, 'libraries', 'SoftwareSerial', 'src'),
           '-I' + os.path.join(core, 'libraries', 'Wire', 'src')]
    common = ['-Os', '-ffunction-sections', '-fdata-sections', '-flto',
              '-mmcu=' + MCU, '-DF_CPU=16000000L', '-DARDUINO=10819',
              '-DARDUINO_AVR_NANO', '-DARDUINO_ARCH_AVR'] + inc

    # .ino is C++ with Arduino.h prepended. The IDE also generates forward
    # prototypes; this sketch defines everything before use, so it does not
    # need them - and if that ever stops being true, this check fails, which is
    # the correct outcome for a sketch that only builds under one particular IDE.
    src = os.path.join(tmp, 'ro_node.cpp')
    with open(src, 'w', encoding='utf-8') as out:
        out.write('#include <Arduino.h>\n#line 1 "ro_node.ino"\n')
        with open(SKETCH, encoding='utf-8') as f:
            out.write(f.read())

    objs = []
    sketch_obj = os.path.join(tmp, 'sketch.o')
    r = subprocess.run([tool('avr-g++'), '-c', '-std=gnu++11', '-fpermissive',
                        '-fno-exceptions', '-Wall', '-Wextra', '-Werror'] +
                       common + [src, '-o', sketch_obj],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('The node sketch does not compile for an ATmega328P:\n')
        print(r.stderr[:4000])
        return 1
    objs.append(sketch_obj)

    # The core and the two bundled libraries, without -Werror: they are not ours
    # to fix, and a warning in Arduino's own code is not this project's finding.
    def build(pattern, compiler, std, extra=()):
        for i, f in enumerate(sorted(glob.glob(pattern))):
            o = os.path.join(tmp, 'c%d_%s.o' % (len(objs), os.path.basename(f)))
            rc = subprocess.run([tool(compiler), '-c', std] + list(extra) + common +
                                [f, '-o', o], capture_output=True, text=True)
            if rc.returncode == 0:
                objs.append(o)

    ca = os.path.join(core, 'cores', 'arduino')
    build(os.path.join(ca, '*.c'), 'avr-gcc', '-std=gnu11')
    build(os.path.join(core, 'libraries', 'Wire', 'src', 'utility', '*.c'), 'avr-gcc', '-std=gnu11')
    build(os.path.join(ca, '*.cpp'), 'avr-g++', '-std=gnu++11', ('-fpermissive', '-fno-exceptions'))
    build(os.path.join(core, 'libraries', 'SoftwareSerial', 'src', '*.cpp'),
          'avr-g++', '-std=gnu++11', ('-fpermissive', '-fno-exceptions'))
    build(os.path.join(core, 'libraries', 'Wire', 'src', '*.cpp'),
          'avr-g++', '-std=gnu++11', ('-fpermissive', '-fno-exceptions'))
    for f in sorted(glob.glob(os.path.join(ca, '*.S'))):
        o = os.path.join(tmp, 'a_%s.o' % os.path.basename(f))
        rc = subprocess.run([tool('avr-gcc'), '-c', '-x', 'assembler-with-cpp',
                             '-mmcu=' + MCU, '-DF_CPU=16000000L'] + inc + [f, '-o', o],
                            capture_output=True, text=True)
        if rc.returncode == 0:
            objs.append(o)

    elf = os.path.join(tmp, 'ro_node.elf')
    r = subprocess.run([tool('avr-gcc'), '-Os', '-flto', '-fuse-linker-plugin',
                        '-Wl,--gc-sections', '-mmcu=' + MCU, '-o', elf] + objs + ['-lm'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('The node sketch compiles but does not link:\n')
        print(r.stderr[:4000])
        return 1

    r = subprocess.run([tool('avr-size'), '-A', elf], capture_output=True, text=True)
    sizes = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith('.'):
            try:
                sizes[parts[0]] = int(parts[1])
            except ValueError:
                pass
    flash = sizes.get('.text', 0) + sizes.get('.data', 0)
    ram = sizes.get('.data', 0) + sizes.get('.bss', 0) + sizes.get('.noinit', 0)
    fpct = flash * 100.0 / FLASH_BYTES
    rpct = ram * 100.0 / RAM_BYTES

    bad = []
    if fpct > MAX_FLASH_PCT:
        bad.append('flash %d B is %.0f%% of %d, over the %d%% ceiling'
                   % (flash, fpct, FLASH_BYTES, MAX_FLASH_PCT))
    if rpct > MAX_RAM_PCT:
        bad.append('static RAM %d B is %.0f%% of %d, over the %d%% ceiling - '
                   'the stack lives in what is left' % (ram, rpct, RAM_BYTES, MAX_RAM_PCT))
    if bad:
        print('The node sketch builds but no longer fits comfortably:')
        for b in bad:
            print('  ' + b)
        return 1

    print('OK: node sketch builds for %s with no warnings; '
          'flash %d B (%.0f%%), RAM %d B (%.0f%%).' % (MCU, flash, fpct, ram, rpct))
    return 0


if __name__ == '__main__':
    sys.exit(main())
