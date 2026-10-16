# Builds the README screenshots in docs/screenshots from the --render-test output (synthetic data, no personal info).
#   GlassStack.exe --render-test tests\readme
#   python tools\readme_shots.py tests\readme docs\screenshots
import sys, os
from PIL import Image, ImageDraw, ImageFilter

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
S = 1.5  # the renders are at 150 % scale

def L(n): return Image.open(os.path.join(src, n + '.png')).convert('RGBA')

def rounded(im, r):
    m = Image.new('L', im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, im.size[0] - 1, im.size[1] - 1], r, fill=255)
    im = im.copy(); im.putalpha(m); return im

def shadowed(im, r, dark, pad=48):
    """window on a transparent canvas with a soft drop shadow and a hairline border"""
    W, H = im.size
    can = Image.new('RGBA', (W + 2 * pad, H + 2 * pad), (0, 0, 0, 0))
    sh = Image.new('L', can.size, 0)
    ImageDraw.Draw(sh).rounded_rectangle([pad, pad + 10, pad + W, pad + H + 10], r, fill=110 if dark else 70)
    sh = sh.filter(ImageFilter.GaussianBlur(22))
    can.paste(Image.new('RGBA', can.size, (0, 0, 0, 255)), (0, 0), sh)
    win = rounded(im, r)
    can.alpha_composite(win, (pad, pad))
    ImageDraw.Draw(can).rounded_rectangle([pad, pad, pad + W - 1, pad + H - 1], r,
                                          outline=(255, 255, 255, 40) if dark else (0, 0, 0, 38), width=1)
    return can

def wallpaper(w, h, dark):
    """soft two-blob gradient, like the overview backdrop"""
    base = (22, 22, 34) if dark else (238, 238, 246)
    im = Image.new('RGB', (w, h), base)
    blob = Image.new('RGB', (w, h), base)
    d = ImageDraw.Draw(blob)
    c1 = (70, 48, 120) if dark else (214, 200, 250)
    c2 = (24, 80, 84) if dark else (196, 236, 226)
    d.ellipse([w * 0.05, -h * 0.6, w * 0.6, h * 0.9], fill=c1)
    d.ellipse([w * 0.5, -h * 0.3, w * 1.1, h * 1.2], fill=c2)
    return Image.blend(im, blob.filter(ImageFilter.GaussianBlur(min(w, h) * 0.25)), 0.85)

def save(im, name, maxw=None):
    if maxw and im.size[0] > maxw:
        im = im.resize((maxw, round(im.size[1] * maxw / im.size[0])), Image.LANCZOS)
    im.save(os.path.join(dst, name), optimize=True)
    print(name, im.size)

# 3D overview: full screen renders, scaled for the page
for t, n in (('light', 'overview_light_0'), ('dark', 'overview_dark_1')):
    save(L(n).convert('RGB'), f'overview-{t}.png', 1600)

# section (detail) windows
for t, n in (('light', 'section_gpu_light'), ('dark', 'section_cpu_dark'), ('light-memory', 'section_mem_light')):
    save(shadowed(L(n), round(8 * S), 'dark' in t), f'details-{t}.png', 1400)

# strip popup: bottom of the screen, strip over the taskbar, readout above the hovered slot
def strip_scene(theme):
    dark = theme == 'dark'
    strip = L(f'strip_{theme}_hover').convert('RGB')
    pop = L(f'popup_{theme}')
    W, Hs = 1280, strip.size[1]
    tb = round(48 * S)
    gap = round(8 * S)
    top = 64
    H = top + pop.size[1] + gap + Hs + tb
    scene = wallpaper(W, H, dark).convert('RGBA')
    # hovered slot 1 of 8: centre on the screen is (1.5/8) * 2560 = 480
    scene.paste(strip.crop((0, 0, W, Hs)), (0, H - tb - Hs))
    ImageDraw.Draw(scene).rectangle([0, H - tb, W, H], fill=(28, 28, 28) if dark else (238, 238, 238))
    cx = round(1.5 / 8 * 2560)
    px, py = cx - pop.size[0] // 2, H - tb - Hs - gap - pop.size[1]
    pad = 48
    win = shadowed(pop, round(8 * S), dark, pad)
    scene.alpha_composite(win, (px - pad, py - pad))
    # the cursor, resting on the slot
    cur = [(0, 0), (0, 25), (6, 19), (10, 28), (14, 26), (10, 17), (18, 17)]
    ox, oy = cx + 30, H - tb - Hs - 4
    pts = [(ox + x * 1.1, oy + y * 1.1) for x, y in cur]
    d = ImageDraw.Draw(scene)
    d.polygon(pts, fill=(255, 255, 255) if not dark else (255, 255, 255), outline=(0, 0, 0))
    return scene.convert('RGB')

for t in ('light', 'dark'):
    save(strip_scene(t), f'popup-{t}.png')

# the strip on its own (light): left part of the screen at 150 %
strip = L('strip_light').convert('RGB')
W, Hs, tb = 1600, strip.size[1], round(48 * S)
scene = wallpaper(W, 90 + Hs + tb, False)
scene.paste(strip.crop((0, 0, W, Hs)), (0, 90))
ImageDraw.Draw(scene).rectangle([0, 90 + Hs, W, 90 + Hs + tb], fill=(238, 238, 238))
save(scene, 'strip-light.png')
