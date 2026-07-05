#!/usr/bin/env python3
"""extract_fonts.py — Parse lv_font_conv generated C font files and produce
a self-describing binary blob for the fonts flash partition.

Usage:
    python extract_fonts.py --digital main/font_digital.c --station main/font_station.c -o fonts.bin

The blob is mmap'd at runtime by fonts_loader.c.
LVGL version: 9.4.0 (pass --lvgl-version 9, the default).
"""

import argparse
import re
import struct
import sys
from typing import Any


# ═══════════════════════════════════════════════════════════════
# C array parser — handles the regular output of lv_font_conv
# ═══════════════════════════════════════════════════════════════


def parse_hex_array(text: str, array_name: str) -> bytearray | None:
    """Parse `static ... const uint8_t glyph_bitmap[] = { ... };`"""
    # Match the array body: { ... };
    pattern = rf'{re.escape(array_name)}\[\]\s*=\s*\{{'
    m = re.search(pattern, text)
    if not m:
        return None
    # start points to the opening brace
    brace_pos = m.end() - 1  # text[brace_pos] == '{'
    brace_count = 1
    i = brace_pos + 1
    content_start = i
    while i < len(text):
        if text[i] == '{':
            brace_count += 1
        elif text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                body = text[content_start:i]
                break
        i += 1
    else:
        raise ValueError(f"Unmatched brace in {array_name}")

    # Extract hex bytes: patterns like 0xNN, 0xN,
    result = bytearray()
    for m in re.finditer(r'0x([0-9a-fA-F]+)', body):
        val = int(m.group(1), 16)
        result.append(val)
    return result


def parse_glyph_dsc(text: str, lvgl_version: int = 9) -> list[dict] | None:
    """Parse `static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = { ... };`

    For LVGL 9 with LV_FONT_FMT_TXT_LARGE=y, each entry is:
    {.bitmap_index = N, .adv_w = N, .box_w = N, .box_h = N, .ofs_x = N, .ofs_y = N}
    """
    array_name = 'glyph_dsc'
    pattern = rf'{re.escape(array_name)}\[\]\s*=\s*\{{'
    m = re.search(pattern, text)
    if not m:
        return None

    brace_pos = m.end() - 1  # text[brace_pos] == '{'
    brace_count = 1
    i = brace_pos + 1
    content_start = i
    while i < len(text):
        if text[i] == '{':
            brace_count += 1
        elif text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                body = text[content_start:i]
                break
        i += 1
    else:
        raise ValueError("Unmatched brace in glyph_dsc")

    # Parse each entry: {.field = val, ...}
    entries = []
    for m_entry in re.finditer(r'\{([^}]+)\}', body, re.DOTALL):
        entry_str = m_entry.group(1)
        entry = {}
        for m_field in re.finditer(r'\.(\w+)\s*=\s*(-?\d+)', entry_str):
            entry[m_field.group(1)] = int(m_field.group(2))
        if not entry:
            continue  # skip comment-only entries like { /* id=0 reserved */}
        entries.append(entry)

    return entries


def parse_aux_arrays(text: str) -> dict[str, list[int]]:
    """Parse auxiliary arrays like unicode_list_N and glyph_id_ofs_list_N."""
    arrays: dict[str, list[int]] = {}

    # Match uint16_t arrays: static const uint16_t name[] = { ... };
    for m in re.finditer(
        r'static\s+const\s+uint16_t\s+(\w+)\[\]\s*=\s*\{([^}]+)\}',
        text,
    ):
        name = m.group(1)
        body = m.group(2)
        vals = [int(x, 16) if x.strip().startswith('0x') else int(x)
                for x in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
        arrays[name] = vals

    # Match uint8_t arrays: static const uint8_t name[] = { ... };
    for m in re.finditer(
        r'static\s+const\s+uint8_t\s+(\w+)\[\]\s*=\s*\{([^}]+)\}',
        text,
    ):
        name = m.group(1)
        body = m.group(2)
        vals = [int(x) for x in re.findall(r'\d+', body)]
        arrays[name] = vals

    return arrays


def parse_cmaps(text: str) -> list[dict]:
    """Parse `static const lv_font_fmt_txt_cmap_t cmaps[] = { ... };`"""
    array_name = 'cmaps'
    pattern = rf'{re.escape(array_name)}\[\]\s*=\s*\{{'
    m = re.search(pattern, text)
    if not m:
        return []

    brace_pos = m.end() - 1  # text[brace_pos] == '{'
    brace_count = 1
    i = brace_pos + 1
    content_start = i
    while i < len(text):
        if text[i] == '{':
            brace_count += 1
        elif text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                body = text[content_start:i]
                break
        i += 1
    else:
        raise ValueError("Unmatched brace in cmaps")

    entries = []
    for m_entry in re.finditer(r'\{([^}]+)\}', body):
        entry_str = m_entry.group(1)
        entry = {}
        for m_field in re.finditer(
            r'\.(\w+)\s*=\s*([^,}]+?)(?=\s*[,}])',
            entry_str,
        ):
            name = m_field.group(1)
            val = m_field.group(2).strip()
            if val == 'NULL':
                entry[name] = None
            elif val.startswith('0x'):
                entry[name] = int(val, 16)
            elif val.startswith('LV_FONT_FMT_TXT_CMAP_'):
                entry[name] = val
            else:
                try:
                    entry[name] = int(val)
                except ValueError:
                    entry[name] = val  # preserve name references
        entries.append(entry)

    return entries


def parse_struct(text: str, var_name: str) -> dict:
    """Parse a C struct initializer: `static const X name = { ... };`"""
    pattern = rf'{re.escape(var_name)}\s*=\s*\{{'
    m = re.search(pattern, text)
    if not m:
        return {}

    brace_pos = m.end() - 1  # text[brace_pos] == '{'
    brace_count = 1
    i = brace_pos + 1
    content_start = i
    while i < len(text):
        if text[i] == '{':
            brace_count += 1
        elif text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                body = text[content_start:i]
                break
        i += 1
    else:
        raise ValueError(f"Unmatched brace in {var_name}")

    result = {}
    for m_field in re.finditer(
        r'\.([\w]+)\s*=\s*([^,}]+?)(?=\s*[,}])',
        body,
    ):
        name = m_field.group(1)
        val = m_field.group(2).strip()
        if val == 'NULL':
            result[name] = None
        elif val.startswith('0x'):
            result[name] = int(val, 16)
        elif val.startswith('LV_FONT_SUBPX_'):
            result[name] = val
        elif val.startswith('"'):
            result[name] = val.strip('"')
        elif re.match(r'^-?\d+$', val):
            result[name] = int(val)
        else:
            # Variable reference like &font_dsc, &cache, glyph_bitmap, cmaps, etc.
            result[name] = val.lstrip('&')
    return result


def get_lvgl_define(text: str, name: str) -> int | None:
    """Get a #define'd value (e.g. LV_FONT_SUBPX_NONE = 0)."""
    m = re.search(rf'#define\s+{re.escape(name)}\s+(\d+)', text)
    if m:
        return int(m.group(1))
    return None


def djb2_hash(s: str) -> int:
    h = 5381
    for c in s:
        h = ((h * 33) + ord(c)) & 0xFFFFFFFF
    return h


# ═══════════════════════════════════════════════════════════════
# Blob builder
# ═══════════════════════════════════════════════════════════════

BLOB_MAGIC = 0x544E4684  # "FNT\x01" in little-endian
BLOB_VERSION = 1
CMAP_ENTRY_SIZE = 24  # lv_font_fmt_txt_cmap_t in 32-bit alignment
GLYPH_DSC_ENTRY_SIZE_LARGE = 16  # with LV_FONT_FMT_TXT_LARGE=y
FONT_ENTRY_SIZE = 60  # padded to 4-byte alignment
HEADER_SIZE = 16


def make_font_entry(
    name_hash: int,
    font_data: bytearray,
    bitmap_offset: int,
    bitmap_size: int,
    dsc_offset: int,
    dsc_count: int,
    dsc_entry_size: int,
    cmap_offset: int,
    cmap_count: int,
    cmap_entry_size: int,
    bpp: int,
    bitmap_format: int,
    line_height: int,
    base_line: int,
    subpx: int,
    underline_pos: int,
    underline_thick: int,
) -> bytes:
    """Pack a font entry (56 bytes)."""
    data_offset = 0  # filled later
    data_size = len(font_data)
    return struct.pack(
        '<11I3H4hH',
        name_hash,
        data_offset,  # placeholder
        data_size,
        bitmap_offset,
        bitmap_size,
        dsc_offset,
        dsc_count,
        dsc_entry_size,
        cmap_offset,
        cmap_count,
        cmap_entry_size,
        bpp,
        bitmap_format,
        subpx,
        line_height,
        base_line,
        underline_pos,
        underline_thick,
        0,  # reserved/padding
    )


def build_blob(fonts: list[dict]) -> bytes:
    """Build the complete fonts.bin from parsed font data."""
    header = bytearray(HEADER_SIZE)
    struct.pack_into('<IIII', header, 0,
                     BLOB_MAGIC, BLOB_VERSION, len(fonts), HEADER_SIZE)

    # Build font entry table and data blocks
    entry_table = bytearray()
    data_blocks = bytearray()

    for i, font in enumerate(fonts):
        data_block = font['data_block']
        data_offset_in_blob = HEADER_SIZE + len(fonts) * FONT_ENTRY_SIZE + len(data_blocks)

        entry = make_font_entry(
            name_hash=font['name_hash'],
            font_data=data_block,
            bitmap_offset=font['bitmap_offset'],
            bitmap_size=font['bitmap_size'],
            dsc_offset=font['dsc_offset'],
            dsc_count=font['dsc_count'],
            dsc_entry_size=font['dsc_entry_size'],
            cmap_offset=font['cmap_offset'],
            cmap_count=font['cmap_count'],
            cmap_entry_size=font['cmap_entry_size'],
            bpp=font['bpp'],
            bitmap_format=font['bitmap_format'],
            line_height=font['line_height'],
            base_line=font['base_line'],
            subpx=font['subpx'],
            underline_pos=font['underline_pos'],
            underline_thick=font['underline_thick'],
        )
        # Patch data_offset
        entry = entry[:4] + struct.pack('<I', data_offset_in_blob) + entry[8:]
        entry_table += entry
        data_blocks += data_block

    return bytes(header + entry_table + data_blocks)


# ═══════════════════════════════════════════════════════════════
# Font parser — processes one font C file
# ═══════════════════════════════════════════════════════════════

CMAP_TYPE_TOKENS = {
    'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY': 0,
    'LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL': 1,
    'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY': 2,
    'LV_FONT_FMT_TXT_CMAP_SPARSE_FULL': 3,
}


def parse_font_file(filepath: str, lvgl_version: int, var_name: str) -> dict | None:
    """Parse one font C file and return font data for the blob."""
    with open(filepath, 'r', encoding='utf-8') as f:
        text = f.read()

    # Only take the code path for LVGL 9.x.
    # Step 1: Handle #if >= 8 / #else / #endif — keep the >= 8 branch
    text = re.sub(
        r'#if\s+LVGL_VERSION_MAJOR\s*>=\s*8\s*\n(.*?)#else\s*\n.*?#endif',
        r'\1',
        text,
        flags=re.DOTALL,
    )
    # Step 2: Remove #if ... == 8 ... #endif blocks entirely
    text = re.sub(
        r'#if\s+LVGL_VERSION_MAJOR\s*==\s*8\s*\n.*?#endif\s*\n',
        '',
        text,
        flags=re.DOTALL,
    )
    # Step 3: Remove leftover #if/#else/#endif lines
    text = re.sub(r'^\s*#\s*(if|else|elif|endif).*$', '', text, flags=re.MULTILINE)

    # Parse glyph bitmap
    glyph_bitmap = parse_hex_array(text, 'glyph_bitmap')
    if glyph_bitmap is None:
        print(f"WARNING: {filepath}: glyph_bitmap not found")
        return None

    # Parse glyph descriptors
    glyph_dsc = parse_glyph_dsc(text, lvgl_version)
    if glyph_dsc is None:
        print(f"WARNING: {filepath}: glyph_dsc not found")
        return None

    # Parse auxiliary arrays (unicode_list_*, glyph_id_ofs_list_*)
    aux_arrays = parse_aux_arrays(text)

    # Parse cmaps
    cmaps = parse_cmaps(text)

    # Parse font_dsc
    font_dsc = parse_struct(text, 'font_dsc')

    # Parse lv_font_* struct
    lv_font = parse_struct(text, var_name)

    # ── Build data block ──
    data_block = bytearray()

    # 1. glyph_bitmap
    bitmap_offset = len(data_block)
    bitmap_size = len(glyph_bitmap)
    data_block += glyph_bitmap

    # 2. glyph_dsc — serialize to binary (LV_FONT_FMT_TXT_LARGE=y format)
    dsc_offset = len(data_block)
    dsc_entry_size = GLYPH_DSC_ENTRY_SIZE_LARGE
    dsc_buf = bytearray()
    for entry in glyph_dsc:
        dsc_buf += struct.pack(
            '<IIhhhh',
            entry.get('bitmap_index', 0),
            entry.get('adv_w', 0),
            entry.get('box_w', 0),
            entry.get('box_h', 0),
            entry.get('ofs_x', 0),
            entry.get('ofs_y', 0),
        )
    dsc_count = len(glyph_dsc)
    data_block += dsc_buf

    # 3. Auxiliary arrays (unicode_list_*, glyph_id_ofs_list_*) — stored inline
    # We'll store them after cmaps and patch cmap entries to use offsets
    aux_data = bytearray()
    aux_offsets: dict[str, int] = {}
    for name, vals in aux_arrays.items():
        aux_offsets[name] = len(aux_data)
        if name.startswith('unicode_list'):
            for v in vals:
                aux_data += struct.pack('<H', v)
        elif name.startswith('glyph_id_ofs_list'):
            for v in vals:
                aux_data += struct.pack('<B', v)
        else:
            # unknown auxiliary type, skip
            pass

    # 4. cmaps — serialize with offsets replacing name references
    # Layout: [bitmap][dsc][cmaps][aux_data]
    cmap_offset = len(data_block)
    total_cmap_size = len(cmaps) * CMAP_ENTRY_SIZE
    aux_base = cmap_offset + total_cmap_size  # where aux_data will be placed

    cmap_buf = bytearray()
    for entry in cmaps:
        unicode_ref = entry.get('unicode_list')
        glyph_ref = entry.get('glyph_id_ofs_list')
        cmap_type_str = entry.get('type', 'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY')
        cmap_type = CMAP_TYPE_TOKENS.get(cmap_type_str, 0)

        unicode_off = 0xFFFFFFFF  # sentinel for NULL
        glyph_off = 0xFFFFFFFF

        if isinstance(unicode_ref, str) and unicode_ref in aux_offsets:
            unicode_off = aux_base + aux_offsets[unicode_ref]
        if isinstance(glyph_ref, str) and glyph_ref in aux_offsets:
            glyph_off = aux_base + aux_offsets[glyph_ref]

        # Pack cmap entry (24 bytes)
        cmap_buf += struct.pack(
            '<IHhIIIHH',
            entry.get('range_start', 0) & 0xFFFFFFFF,
            entry.get('range_length', 0) & 0xFFFF,
            entry.get('glyph_id_start', 0) & 0xFFFF,
            unicode_off,
            glyph_off,
            entry.get('list_length', 0),
            cmap_type & 0xFFFF,
            0,  # padding
        )

    cmap_count = len(cmaps)
    cmap_entry_size = CMAP_ENTRY_SIZE

    # Pack cmaps first, then auxiliary data
    data_block += cmap_buf
    data_block += aux_data

    # ── Collect metadata ──
    bpp_val = font_dsc.get('bpp', 1)
    bitmap_format_val = font_dsc.get('bitmap_format', 0)

    # Resolve LV_FONT_SUBPX_NONE -> 0
    subpx_str = lv_font.get('subpx', 'LV_FONT_SUBPX_NONE')
    if isinstance(subpx_str, str) and 'NONE' in subpx_str:
        subpx = 0
    elif isinstance(subpx_str, str) and 'HOR' in subpx_str:
        subpx = 1
    elif isinstance(subpx_str, str) and 'VER' in subpx_str:
        subpx = 2
    else:
        subpx = 0

    return {
        'name_hash': djb2_hash(var_name),
        'data_block': data_block,
        'bitmap_offset': bitmap_offset,
        'bitmap_size': bitmap_size,
        'dsc_offset': dsc_offset,
        'dsc_count': dsc_count,
        'dsc_entry_size': dsc_entry_size,
        'cmap_offset': cmap_offset,
        'cmap_count': cmap_count,
        'cmap_entry_size': cmap_entry_size,
        'bpp': bpp_val,
        'bitmap_format': bitmap_format_val,
        'line_height': lv_font.get('line_height', 0),
        'base_line': lv_font.get('base_line', 0),
        'subpx': subpx,
        'underline_pos': lv_font.get('underline_position', 0),
        'underline_thick': lv_font.get('underline_thickness', 0),
    }


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='Extract LVGL font binary blob from C sources')
    parser.add_argument('--digital', required=True, help='Path to font_digital.c')
    parser.add_argument('--station', required=True, help='Path to font_station.c')
    parser.add_argument('-o', '--output', required=True, help='Output fonts.bin path')
    parser.add_argument('--lvgl-version', type=int, default=9, help='LVGL major version')
    args = parser.parse_args()

    lvgl_ver = args.lvgl_version

    font_files = [
        (args.digital, 'lv_font_digital'),
        (args.station, 'lv_font_station'),
    ]

    fonts = []
    for filepath, var_name in font_files:
        print(f"Parsing {filepath} ({var_name})...")
        font_data = parse_font_file(filepath, lvgl_ver, var_name)
        if font_data is None:
            print(f"  ERROR: failed to parse {filepath}")
            sys.exit(1)
        blob_size = len(font_data['data_block'])
        print(f"  glyphs: {font_data['dsc_count']}, "
              f"bitmap: {font_data['bitmap_size']}B, "
              f"dsc: {font_data['dsc_count'] * font_data['dsc_entry_size']}B, "
              f"cmaps: {font_data['cmap_count']} entries, "
              f"total: {blob_size}B")
        fonts.append(font_data)

    blob = build_blob(fonts)
    with open(args.output, 'wb') as f:
        f.write(blob)

    print(f"\nWrote {args.output}: {len(blob)} bytes ({len(blob)/1024:.1f} KB)")
    print(f"  Header: {HEADER_SIZE}B")
    print(f"  Font entries: {len(fonts) * FONT_ENTRY_SIZE}B")
    print(f"  Raw font data: {sum(len(f['data_block']) for f in fonts)}B")
    print("  Done.")


if __name__ == '__main__':
    main()
