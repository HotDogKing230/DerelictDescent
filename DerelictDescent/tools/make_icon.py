"""Render a 40x40 1-bit PocketMage app icon from an image.

Usage:
    python3 make_icon.py input.png output.bin
"""

from pathlib import Path

from PIL import Image

ICON_SIZE = 40
CONFIRMATION = 200


def convert_icon(input_path: Path, output_path: Path) -> None:
    img = Image.open(input_path)
    img = img.convert("L").resize((ICON_SIZE, ICON_SIZE), Image.NEAREST)

    # ink when the source pixel is darker than the midpoint
    ink = lambda v: 0 if v < 128 else 255  # noqa: E731
    img = img.point(ink, "1")

    pixels = img.load()
    data = bytearray()
    for y in range(ICON_SIZE):
        for x0 in range(0, ICON_SIZE, 8):
            byte = 0
            for bit in range(8):
                byte <<= 1
                if x0 + bit < ICON_SIZE and pixels[x0 + bit, y] == 0:
                    byte |= 1
            data.append(byte)

    if len(data) != CONFIRMATION:
        raise SystemExit(f"bug: expected {CONFIRMATION} bytes, got {len(data)}")

    output_path.write_bytes(data)
    print(f"wrote {output_path} ({CONFIRMATION} bytes)")


def main() -> None:
    import sys

    if len(sys.argv) != 3:
        raise SystemExit("usage: python3 make_icon.py input.png output.bin")
    convert_icon(Path(sys.argv[1]), Path(sys.argv[2]))


if __name__ == "__main__":
    main()