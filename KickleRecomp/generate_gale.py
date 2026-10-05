"""Generate private build data from locally supplied original and patched ROMs."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('original', type=Path)
parser.add_argument('patched', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
base, gale = args.original.read_bytes(), args.patched.read_bytes()
if base[:4] != b'NES\x1a' or gale[:16] != base[:16] or len(base) != len(gale):
    parser.error('Expected matching iNES headers and sizes for original and Gale Festival ROMs.')
if base[6] & 4:
    parser.error('Trainer ROMs are not supported.')
prg_size = base[4] * 16384
if prg_size != 131072 or len(base) < 16 + prg_size:
    parser.error('Expected the USA ROM with 128 KiB PRG data.')
if base[16+prg_size:] != gale[16+prg_size:]:
    parser.error('This build supports PRG-only Gale Festival changes.')
rows = ['/* Generated locally; do not commit this ROM-derived file. */',
        'static const struct { unsigned offset; unsigned char original, gale; } gale_changes[] = {']
rows.extend(f'    {{0x{i:x}, 0x{a:02x}, 0x{b:02x}}},'
            for i, (a, b) in enumerate(zip(base[16:16+prg_size], gale[16:16+prg_size])) if a != b)
if len(rows) == 2:
    parser.error('No Gale Festival changes found; supply the patched ROM.')
rows.append('};')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('\n'.join(rows) + '\n', encoding='utf-8')
