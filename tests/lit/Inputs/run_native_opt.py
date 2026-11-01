"""Native optimization: C ABI results, RV32 loop shape, and Stage-cache isolation."""
import ctypes
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile
import zlib


def run(args, **kwargs):
    result = subprocess.run(list(map(str, args)), timeout=60,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)
    assert result.returncode == 0, (args, result.stderr.decode())
    return result


def main():
    pagosc, clang, lld, nm, objdump, source = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='pagos-native-opt-') as temporary:
        root = Path(temporary)
        rng = random.Random(20261101)
        cases = [b'', b'123456789', bytes(1024), bytes([255])*1024, bytes(range(256))]
        cases += [bytes([n]) for n in range(256)]
        cases += [bytes(rng.randrange(256) for _ in range(n)) for n in (16, 64, 255, 1024)]
        for level in ['-O0', '-O1', '-O2', '-O3', '-Os', '-Oz']:
            obj = root / (level + '.o')
            run([pagosc, level, 'emit-obj', source, '-o', obj])
            library = root / (level + '.so')
            run([clang, '-shared', obj, '-o', library])
            loaded = ctypes.CDLL(str(library))
            loaded.generic_crc.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32, ctypes.c_uint32]
            loaded.generic_crc.restype = ctypes.c_uint32
            loaded.frozen_crc.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
            loaded.frozen_crc.restype = ctypes.c_uint32
            for data in cases:
                buf = (ctypes.c_uint8 * max(1, len(data)))(*data)
                assert loaded.frozen_crc(buf, len(data)) == zlib.crc32(data), level
                for seed in [0xffffffff, 0x12345678]:
                    assert loaded.generic_crc(buf, len(data), seed) == zlib.crc32(data, seed ^ 0xffffffff), level
            assert loaded.frozen_crc(None, 0) == 0

        target = ['--target=riscv32-unknown-elf', '--cpu=generic-rv32', '--features=+m,+a,+c']
        policy = ['--relocation-model=static', '--code-model=medium']
        raw = run([pagosc, *target, 'emit-llvm', source]).stdout
        optimized = run([pagosc, *target, '-O2', *policy, 'emit-llvm', source]).stdout
        assert b'call i32 @pagos.fold' in raw
        assert b'call i32 @pagos.fold' not in optimized
        assert b'@llvm.trap' in optimized, 'optimization erased pointer checks'
        assert b'constant [256 x i32]' in optimized
        assert b'alloca' not in optimized
        obj, elf = root / 'rv32.o', root / 'rv32.elf'
        run([pagosc, *target, '-O2', *policy, 'emit-obj', source, '-o', obj])
        run([lld, '-m', 'elf32lriscv', '--no-relax', '-e', 'frozen_crc', obj, '-o', elf])
        sizes = run([nm, '-S', '--defined-only', elf]).stdout.decode()
        code = run([objdump, '-d', '--no-show-raw-insn', elf]).stdout.decode()
        instructions = []
        for line in code.splitlines():
            match = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
            if match: instructions.append((int(match[1], 16), match[2], match[3]))
        for function in ['generic_crc', 'frozen_crc']:
            symbol = re.search(r'^([0-9a-f]+)\s+([0-9a-f]+)\s+[Tt]\s+' + function + r'$', sizes, re.M)
            assert symbol, sizes
            begin, size = (int(v, 16) for v in symbol.groups())
            body = [i for i in instructions if begin <= i[0] < begin + size]
            loops = []
            for address, mnemonic, operands in body:
                if mnemonic.startswith('b') or mnemonic in ('j', 'c.j'):
                    destination = re.search(r'0x([0-9a-f]+)', operands)
                    if destination and int(destination[1], 16) < address:
                        loops.append([i for i in body if int(destination[1], 16) <= i[0] <= address])
            assert len(loops) == 1, (function, code)
            loop = loops[0]
            assert not any(i[1] in ('jal', 'jalr', 'call') for i in loop), loop
            # A second induction update was S4's long-buffer regression. Ignore
            # exact register names, labels and instruction offsets across LLVMs.
            updates = [i for i in loop if i[1] in ('addi', 'c.addi') and
                       re.search(r',\s*(-?0x1|-?1)\s*$', i[2])]
            assert len(updates) == 1, (function, loop)

        cache = root / 'cache'
        cached = [pagosc, *target, '--cache-stats', f'--cache-dir={cache}']
        run([*cached, '--stage-cache=read-write', 'emit-llvm', source])
        before = {str(p.relative_to(cache)): (p.stat().st_size, p.stat().st_mtime_ns)
                  for p in cache.rglob('*') if p.is_file()}
        hot = run([*cached, '-O2', *policy, '--stage-cache=read-only', 'emit-llvm', source])
        assert hot.stdout == optimized
        hits = re.search(rb'(?<![-a-z])hits=(\d+)', hot.stderr)
        assert hits and int(hits[1]) > 0, hot.stderr
        assert before == {str(p.relative_to(cache)): (p.stat().st_size, p.stat().st_mtime_ns)
                          for p in cache.rglob('*') if p.is_file()}
        diagnostics = []
        for flags in [[], ['-O2', *policy]]:
            failed = subprocess.run([*cached, *flags, '--max-fuel=1', '--stage-cache=read-only',
                                     'emit-llvm', source], capture_output=True, timeout=60)
            assert failed.returncode != 0 and b'error[' in failed.stderr
            diagnostics.append(b''.join(line for line in failed.stderr.splitlines(keepends=True)
                                        if not line.startswith(b'stage-cache:')))
        assert diagnostics[0] == diagnostics[1]


if __name__ == '__main__': main()
