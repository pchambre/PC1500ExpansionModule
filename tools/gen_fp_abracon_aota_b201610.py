"""Footprint for the Abracon AOTA-B201610 molded power inductor (2.0 x 1.6 x
1.0 mm, "0806"), the RP2350 core regulator's inductor: Raspberry Pi's
hardware design guide specifies AOTA-B201610S3R3-101-T (3.3 uH, polarity
dot, all one way round on the reel) -- its orientation matters, so pad 1
is marked.

Numbers from Abracon's AOTA-B201610S datasheet (Initial Release 7/23/2025),
page 3, "Recommended Land Pattern (Typ.)": two pads 0.80 x 1.60 mm, 0.70 mm
apart; body 2.00 x 1.60 (+-0.20) mm, 1.00 max high, terminals 0.60 mm. That
datasheet covers the series' 0.24-4.7 uH parts; the 3.3 uH -101-T part has
its own sheet (not on hand), same 2016 body.

Usage: python gen_fp_abracon_aota_b201610.py OUT.kicad_mod"""
import sys
import uuid

PAD_W, PAD_H, GAP = 0.80, 1.60, 0.70
BODY_W, BODY_H = 2.00, 1.60
PAD_X = GAP / 2 + PAD_W / 2  # 0.75: pad centres
CRT = 0.25                   # courtyard margin
SILK = 0.12


def u():
    return str(uuid.uuid4())


def line(x1, y1, x2, y2, layer, width):
    return (f'\t(fp_line\n\t\t(start {x1:g} {y1:g})\n\t\t(end {x2:g} {y2:g})\n'
            f'\t\t(stroke\n\t\t\t(width {width:g})\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{u()}")\n\t)\n')


def rect(x1, y1, x2, y2, layer, width):
    return (f'\t(fp_rect\n\t\t(start {x1:g} {y1:g})\n\t\t(end {x2:g} {y2:g})\n'
            f'\t\t(stroke\n\t\t\t(width {width:g})\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(fill no)\n\t\t(layer "{layer}")\n\t\t(uuid "{u()}")\n\t)\n')


def circle(cx, cy, r, layer):
    return (f'\t(fp_circle\n\t\t(center {cx:g} {cy:g})\n\t\t(end {cx + r:g} {cy:g})\n'
            f'\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(fill yes)\n\t\t(layer "{layer}")\n\t\t(uuid "{u()}")\n\t)\n')


def prop(name, value, at, layer, hide=False):
    return (f'\t(property "{name}" "{value}"\n\t\t(at {at})\n\t\t(layer "{layer}")\n'
            + ('\t\t(hide yes)\n' if hide else '') +
            f'\t\t(uuid "{u()}")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n'
            f'\t\t\t)\n\t\t)\n\t)\n')


def pad(num, x):
    return (f'\t(pad "{num}" smd rect\n\t\t(at {x:g} 0)\n\t\t(size {PAD_W:g} {PAD_H:g})\n'
            f'\t\t(layers "F.Cu" "F.Paste" "F.Mask")\n\t\t(uuid "{u()}")\n\t)\n')


name = 'L_Abracon_AOTA-B201610'
cx = PAD_X + PAD_W / 2 + CRT          # 1.40
cy = max(PAD_H, BODY_H) / 2 + CRT     # 1.05
ds = 'https://abracon.com/datasheets/AOTA-B201610S.pdf'
out = (
    f'(footprint "{name}"\n\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")\n'
    f'\t(layer "F.Cu")\n'
    f'\t(descr "Abracon AOTA-B201610 molded power inductor, 2.0x1.6x1.0mm (0806); the RP2350 regulator\'s '
    f'AOTA-B201610S3R3-101-T. Land pattern from the AOTA-B201610S datasheet (Initial Release 7/23/2025, p.3): '
    f'pads 0.80x1.60mm, 0.70mm apart. Pad 1 marked (dot): orientation matters (RP2350 hardware design guide). {ds}")\n'
    f'\t(tags "inductor Abracon AOTA 2016 0806 RP2350")\n'
    + prop('Reference', 'REF**', f'0 {-cy - 0.8:g} 0', 'F.SilkS')
    + prop('Value', name, f'0 {cy + 0.8:g} 0', 'F.Fab')
    + prop('Datasheet', ds, '0 0 0', 'F.Fab', hide=True)
    + prop('Description', 'Abracon AOTA-B201610 2.0x1.6mm molded power inductor', '0 0 0', 'F.Fab', hide=True)
    + '\t(attr smd)\n'
    + rect(-BODY_W / 2, -BODY_H / 2, BODY_W / 2, BODY_H / 2, 'F.Fab', 0.1)
    # silk only between the pads, clear of the copper (mask expansion ~0.05)
    + line(-0.2, -BODY_H / 2 - SILK, 0.2, -BODY_H / 2 - SILK, 'F.SilkS', SILK)
    + line(-0.2, BODY_H / 2 + SILK, 0.2, BODY_H / 2 + SILK, 'F.SilkS', SILK)
    + circle(-PAD_X - PAD_W / 2 - 0.3, -PAD_H / 2, 0.1, 'F.SilkS')   # pad 1 dot
    + circle(-BODY_W / 2 + 0.25, -BODY_H / 2 + 0.25, 0.1, 'F.Fab')
    + rect(-cx, -cy, cx, cy, 'F.CrtYd', 0.05)
    + pad(1, -PAD_X) + pad(2, PAD_X)
    + '\t(embedded_fonts no)\n)\n'
)
open(sys.argv[1], 'w', newline='\n').write(out)
print('wrote', sys.argv[1])
