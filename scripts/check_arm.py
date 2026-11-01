#!/usr/bin/env python3
"""Cortex-M33 AAPCS32 and polling MMIO on QEMU MPS2 AN505, not MCXN947 emulation."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from elf_storage import verify_storage

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / 'platforms/qemu-cortex-m33'
TARGET = ['--target=thumbv8m.main-none-eabi', '--cpu=cortex-m33']
ISA = ['-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=soft']
LEVELS = ['', '-O0', '-O1', '-O2', '-O3', '-Os', '-Oz']
HARNESSES = {
    'c-abi': 'c_abi.c', 'pointers': 'pointers.c', 'global-arrays': 'global_arrays.c',
    'global-records': 'global_records.c', 'record-pointers': 'record_pointers.c',
    'volatile32': 'volatile32.c',
}
PLACEMENT = {
    'mmio': {'boot_seed': ('.data.boot', 64, True, False),
             'boot_count': ('.bss.boot', 128, True, True),
             'poll_limit': ('.rodata.uart', 32, False, False),
             'boot_table': ('.rodata.boot', 32, False, False)},
    'global-arrays': {'buffer': ('.bss.buffer', 64, True, True),
                      'squares': ('.rodata.squares', 32, False, False),
                      'words': ('.data.words', 4, True, False)},
    'global-records': {'state': ('.data.state', 64, True, False),
                       'zero': ('.bss.state', 32, True, True),
                       'config': ('.rodata.state', 8, False, False)},
}


def checked(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(f'{args}\n{result.stdout.decode()}\n{result.stderr.decode()}')
    return result.stdout


def run(out, pagosc, qemu_version):
    version = checked(['qemu-system-arm', '--version']).decode().splitlines()[0]
    if version != f'QEMU emulator version {qemu_version}':
        raise RuntimeError(f'unexpected QEMU: {version}; review before overriding version')
    runtime = Path(checked(['arm-none-eabi-gcc', *ISA, '-print-libgcc-file-name']).decode().strip())
    if not runtime.is_file() or 'thumb/v8-m.main/nofp' not in str(runtime):
        raise RuntimeError(f'missing Cortex-M33 base-PCS multilib: {runtime}')
    environment = dict(qemu=version, machine='mps2-an505', cpu='cortex-m33', isa=ISA,
                       gcc=checked(['arm-none-eabi-gcc', '--version']).decode().splitlines()[0],
                       clang=checked(['clang', '--version']).decode().splitlines()[0], runtime=str(runtime))
    print(json.dumps(environment, indent=2), flush=True)
    objects = {}
    harnesses = {}
    cases = []
    if pagosc:
        sources = {name: ROOT / f'tests/lit/codegen/{name}.pgs' for name in HARNESSES}
        sources.update(mmio=PLATFORM / 'mmio.pgs', traps=PLATFORM / 'traps.pgs',
                       crc=ROOT / 'tests/lit/codegen/optimized-crc-loop.pgs')
        for name, source in sources.items():
            for level in LEVELS:
                key = name + '-' + (level[1:] if level else 'legacy')
                obj = out / (key + '.o')
                checked([pagosc, *TARGET, *([level] if level else []),
                         '--relocation-model=static', '--code-model=small',
                         'emit-obj', source, '-o', obj])
                header = obj.read_bytes()[:40]
                assert header[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', header, 18)[0] == 40
                flags = struct.unpack_from('<I', header, 36)[0]
                assert flags & 0xff000000 == 0x05000000 and not flags & 0x400, (key, flags)
                objects[key] = obj
                if name in PLACEMENT:
                    verify_storage(obj, PLACEMENT[name],
                                   sizes={'state':24, 'zero':24, 'config':24} if name=='global-records' else None)
                if name in HARNESSES or name == 'crc':
                    harnesses[key] = ROOT / 'tests/lit/Inputs' / HARNESSES[name] if name in HARNESSES else PLATFORM / 'crc.c'
                    cases.append((key, ['WITH_PAGOS_C_ABI'], 0, 'PAGOS ARM PASS\n'))
                    if name == 'global-records':
                        for fault in ['SKIP_DATA_COPY', 'SKIP_BSS_CLEAR']:
                            cases.append((key, ['WITH_PAGOS_C_ABI', fault], 1, 'PAGOS ARM FAIL\n'))
                elif name == 'mmio':
                    for defines in [[], ['FORCE_FAILURE'], ['SKIP_DATA_COPY'], ['SKIP_BSS_CLEAR']]:
                        failure = bool(defines)
                        cases.append((key, ['WITH_PAGOS_MMIO', *defines], int(failure),
                                      'PAGOS MMIO FAIL\n' if failure else 'PAGOS MMIO PASS\n'))
                else:
                    harnesses[key] = PLATFORM / 'traps.c'
                    for trap in range(1, 8):
                        cases.append((key, ['WITH_PAGOS_C_ABI', 'EXPECT_TRAP', f'TRAP_CASE={trap}'], 2, 'PAGOS ARM TRAP\n'))
    cases += [('environment', defines, status, output) for defines, status, output in [
        ([], 0, 'PAGOS ARM PASS\n'), (['FORCE_FAILURE'], 1, 'PAGOS ARM FAIL\n'),
        (['SKIP_DATA_COPY'], 1, 'PAGOS ARM FAIL\n'), (['SKIP_BSS_CLEAR'], 1, 'PAGOS ARM FAIL\n'),
        (['EXPECT_TRAP', 'FORCE_UNDEFINED'], 3, 'PAGOS ARM FAULT\n'),
        (['FORCE_TIMEOUT'], None, 'PAGOS ARM TIMEOUT\n'),
    ]]
    results = []
    for compiler in ['clang', 'arm-none-eabi-gcc']:
        for index, (key, defines, status, output) in enumerate(cases):
            elf = out / f'{compiler}-{index}-{key}.elf'
            command = [compiler, *ISA, '-O2', '-ffreestanding', '-fno-builtin', '-fno-pic',
                       '-fno-stack-protector', '-nostdlib', '-Wl,--build-id=none',
                       f'-Wl,-T,{PLATFORM / "link.ld"}', '-DPAGOS_FREESTANDING',
                       *(f'-D{d}' for d in defines), PLATFORM / 'start.S', PLATFORM / 'smoke.c']
            if compiler == 'clang':
                command += ['--target=thumbv8m.main-none-eabi', '-fuse-ld=lld']
            if key in objects:
                command.append(objects[key])
            if key in harnesses:
                command.append(harnesses[key])
            checked([*command, runtime, '-o', elf])
            name = key.rsplit('-', 1)[0]
            if name in PLACEMENT:
                linked = {symbol: ('.bss' if spec[3] else '.data' if spec[2] else '.rodata', *spec[1:])
                          for symbol, spec in PLACEMENT[name].items()}
                verify_storage(elf, linked, sizes={'state':24,'zero':24,'config':24} if name=='global-records' else None)
            qemu = ['qemu-system-arm', '-M', 'mps2-an505', '-cpu', 'cortex-m33',
                    '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
                    '-semihosting-config', 'enable=on,target=native', '-kernel', str(elf)]
            try:
                executed = subprocess.run(qemu, capture_output=True, timeout=1 if status is None else 10)
            except subprocess.TimeoutExpired as timeout:
                if status is not None:
                    raise
                if timeout.stdout != output.encode():
                    raise RuntimeError(f'timeout before expected firmware marker: {timeout.stdout!r}') from timeout
            else:
                if status is None or executed.returncode != status or executed.stdout.decode() != output:
                    raise RuntimeError(f'{compiler}/{key}/{defines}: {executed}')
            label = f'{compiler}/{key}/' + ','.join(defines)
            results.append(dict(label=label, status=status, stdout=output, passed=True))
            print('PASS ' + label, flush=True)
    (out / 'result.json').write_text(json.dumps(dict(schema=1, environment=environment,
        pagosc=str(pagosc) if pagosc else None, levels=LEVELS, results=results),indent=2)+'\n')
    print(f'PASS {len(results)} Cortex-M33 QEMU checks', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pagosc', type=Path)
    parser.add_argument('--qemu-version', default='11.1.1')
    parser.add_argument('--out', type=Path, help='retain ELF objects and per-case results')
    args = parser.parse_args()
    pagosc = args.pagosc.resolve() if args.pagosc else None
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        run(args.out.resolve(), pagosc, args.qemu_version)
    else:
        with tempfile.TemporaryDirectory(prefix='pagos-arm-') as directory:
            run(Path(directory), pagosc, args.qemu_version)


if __name__ == '__main__':
    main()
