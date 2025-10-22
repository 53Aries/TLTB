# Generates 160x128 PNG images of the TLTB UI screens for the manual.
# This approximates the Adafruit GFX default font; layout, sizes, and colors match your code.
# Output: docs/screens/*.png

from PIL import Image, ImageDraw, ImageFont
import argparse
from pathlib import Path

W, H = 160, 128

# ST77XX color palette (approximate sRGB)
COLORS = {
    'BLACK':   (0, 0, 0),
    'WHITE':   (255, 255, 255),
    'CYAN':    (0, 255, 255),
    'YELLOW':  (255, 255, 0),
    'GREEN':   (0, 255, 0),
    'BLUE':    (0, 0, 255),
    'RED':     (255, 0, 0),
    'DARKGREY':(66, 66, 66),
}

# Try to use a monospaced font; fall back to PIL default
# This matches the very first implementation we used.
def load_fonts():
    try:
        mono = ImageFont.truetype("DejaVuSansMono.ttf", 8)
        mono2 = ImageFont.truetype("DejaVuSansMono.ttf", 16)
        return mono, mono2
    except Exception:
        try:
            cons = ImageFont.truetype("consola.ttf", 9)
            cons2 = ImageFont.truetype("consola.ttf", 18)
            return cons, cons2
        except Exception:
            f1 = ImageFont.load_default()
            f2 = ImageFont.load_default()
            return f1, f2

FONT1, FONT2 = load_fonts()

out_dir = Path("docs/screens")
out_dir.mkdir(parents=True, exist_ok=True)


def draw_text(img: Image.Image, draw: ImageDraw.ImageDraw, x: int, y: int, text: str, color: str = 'WHITE', size: int = 1, bg: str | None = None):
    if not text:
        return
    f = FONT2 if size == 2 else FONT1
    if bg is not None:
        tw = int(draw.textlength(text, font=f))
        th = getattr(f, 'size', 8) + 2
        draw.rectangle([x, y-2, x + tw + 2, y + th], fill=COLORS[bg])
    draw.text((x, y), text, font=f, fill=COLORS[color])


def measure_text(text: str, size: int = 1):
    """Return (width, height) using the chosen FONT1/FONT2 (no sprite)."""
    if not text:
        return (0, 0)
    tmp = Image.new('L', (1, 1))
    td = ImageDraw.Draw(tmp)
    f = FONT2 if size == 2 else FONT1
    w = int(td.textlength(text, font=f))
    h = getattr(f, 'size', 8) + 2
    return (max(1, w), max(1, h))


def save(img: Image.Image, name: str, scale: int = 1):
    p = out_dir / f"{name}.png"
    if scale and scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(p)
    return p


# ---------- Specific screens ----------

def screen_home(mode: str = 'HD', load_a: float = 0.00, active: str = 'RF',
                sys12_enabled: bool = True, lvp_bypass: bool = False, lvp_latched: bool = False,
                ocp_latched: bool = False,
                fault: str | None = None, focus_mode: bool = False):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    # Layout mirrors DisplayUI::showStatus constants
    yMode, hMode = 6, 18
    yLoad, hLoad = yMode + hMode + 2, 18
    yActive, hActive = yLoad + hLoad + 2, 18
    # InputV removed; place 12V directly below Active
    y12, h12 = yActive + hActive + 2, 12
    yLvp, hLvp = y12 + h12 + 2, 12
    yOcp, hOcp = yLvp + hLvp + 2, 12
    yHint, hHint = 114, 12

    # MODE line (size=2) with optional focus highlight
    bg = 'GREEN' if focus_mode else 'BLACK'
    draw_text(img, d, 4, yMode, f"MODE: {mode}", color='WHITE', size=2, bg=bg)

    # Load
    draw_text(img, d, 4, yLoad, "Load:", size=2)
    load_val = f"{load_a:4.2f} A"
    lv_w, _ = measure_text(load_val, size=2)
    draw_text(img, d, max(4, W - 4 - lv_w), yLoad, load_val, size=2)

    # Active
    draw_text(img, d, 4, yActive, "Active:", size=2)
    # try to keep within width (160)
    label = active
    av_w, _ = measure_text(label, size=2)
    draw_text(img, d, max(4, W - 4 - av_w), yActive, label, size=2)

    # 12V sys
    draw_text(img, d, 4, y12, f"12V sys: {'ENABLED' if sys12_enabled else 'DISABLED'}", size=1)

    # LVP status
    if lvp_bypass:
        draw_text(img, d, 4, yLvp, "LVP : BYPASS", color='YELLOW', size=1)
    else:
        draw_text(img, d, 4, yLvp, f"LVP : {'ACTIVE' if lvp_latched else 'ok'}", color='WHITE', size=1)

    # OCP status
    draw_text(img, d, 4, yOcp, f"OCP : {'ACTIVE' if ocp_latched else 'ok'}", size=1)

    # Footer
    draw_text(img, d, 4, yHint, "OK=Menu  BACK=Home", color='YELLOW', size=1)

    # Fault ticker stub (if provided, show along bottom edge)
    if fault:
        d.rectangle([0, H-10, W, H], fill=COLORS['RED'])
        draw_text(img, d, 2, H-10, fault, color='WHITE', size=1)

    return img


def screen_menu(selected: int = 0):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    items = [
        "Set LVP Cutoff",
        "LVP Bypass",
        "Set OCP Limit",
        "12V System",
        "Learn RF Button",
        "Clear RF Remotes",
        "Wi-Fi Connect",
        "Wi-Fi Forget",
        "OTA Update",
        "System Info",
    ]

    rows, y0, rowH = 8, 8, 12
    top = min(max(selected - rows//2, 0), max(0, len(items) - rows))

    for i in range(top, min(top + rows, len(items))):
        y = y0 + (i - top) * rowH
        sel = (i == selected)
        if sel:
            d.rectangle([0, y-2, W, y-2 + rowH], fill=COLORS['BLUE'])
            draw_text(img, d, 6, y, items[i], color='WHITE', size=1)
        else:
            draw_text(img, d, 6, y, items[i], color='WHITE', size=1)

    return img


def screen_system_info(fw: str = "v1.0.2", wifi: str = "OK 192.168.1.23", lvp_bypass: bool = False,
                       faults: list[str] | None = None):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    draw_text(img, d, 4, 6, "System Info & Faults", color='CYAN', size=1)
    y = 22
    def line(k, v):
        nonlocal y
        draw_text(img, d, 4, y, f"{k}: {v}", size=1)
        y += 12

    line("Firmware", fw or "unknown")
    line("Wi-Fi", wifi)
    line("LVP Bypass", "ON" if lvp_bypass else "OFF")

    if faults:
        line("Faults", "")
        for f in faults:
            draw_text(img, d, 10, y, f"- {f}", size=1)
            y += 12

    return img


def screen_simple_title_body(title: str, lines: list[str]):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    draw_text(img, d, 6, 10, title, size=1)
    y = 28
    for ln in lines:
        draw_text(img, d, 6, y, ln, size=1)
        y += 12
    return img


# ---------- Batch generation ----------

def generate_all(scale: int = 1):
    # Home (HD)
    save(screen_home(mode='HD', load_a=0.00, active='RF', sys12_enabled=True, lvp_bypass=False, lvp_latched=False, ocp_latched=False), 'home_hd_idle', scale)
    save(screen_home(mode='HD', load_a=3.42, active='LEFT', sys12_enabled=True, lvp_bypass=False, lvp_latched=False, ocp_latched=False), 'home_hd_left', scale)
    save(screen_home(mode='HD', load_a=1.02, active='BRAKE', sys12_enabled=True, lvp_bypass=False, lvp_latched=True, ocp_latched=False), 'home_hd_brake_lvp', scale)

    # Home (RV) with MODE focus
    save(screen_home(mode='RV', load_a=0.55, active='RF', sys12_enabled=True, lvp_bypass=False, lvp_latched=False, ocp_latched=False, focus_mode=True), 'home_rv_idle_focus', scale)
    save(screen_home(mode='RV', load_a=2.10, active='BRAKE', sys12_enabled=True, lvp_bypass=False, lvp_latched=False, ocp_latched=True), 'home_rv_brake', scale)

    # Menu
    save(screen_menu(selected=0), 'menu_top', scale)
    save(screen_menu(selected=4), 'menu_mid', scale)

    # System Info
    save(screen_system_info(fw='v1.0.2', wifi='OK 192.168.1.23', lvp_bypass=False,
                            faults=['INA226 load missing', 'Wi-Fi disconnected']), 'system_info', scale)

    # Adjusters and simple flows (Brightness removed)
    save(screen_simple_title_body('Set LVP Cutoff (V)', ['15.50', '', 'OK=Save  BACK=Cancel']), 'lvp_cutoff', scale)
    save(screen_simple_title_body('Set OCP (A)', ['20.0', '', 'OK=Save  BACK=Cancel']), 'ocp_limit', scale)

    save(screen_simple_title_body('LVP Bypass', ['State: ON', '', 'OK=Toggle  BACK=Exit']), 'lvp_bypass_on', scale)
    save(screen_simple_title_body('LVP Bypass', ['State: OFF', '', 'OK=Toggle  BACK=Exit']), 'lvp_bypass_off', scale)

    save(screen_simple_title_body('12V System', ['State: ENABLED', '', 'OK=Toggle  BACK=Exit']), 'sys12_enabled', scale)
    save(screen_simple_title_body('12V System', ['State: DISABLED', '', 'OK=Toggle  BACK=Exit']), 'sys12_disabled', scale)

    save(screen_simple_title_body('Learn RF for:', ['LEFT', '', 'OK=Start  BACK=Exit']), 'rf_learn', scale)

    save(screen_simple_title_body('Clear RF Remotes', ['Erase all learned', 'remotes from memory?', '', 'OK=Erase  BACK=Cancel']), 'rf_clear_confirm', scale)

    save(screen_simple_title_body('Wi-Fi Connect', ['Scanning...']), 'wifi_scanning', scale)
    save(screen_simple_title_body('Wi-Fi Connect', ['Connecting to MySSID']), 'wifi_connecting', scale)

    save(screen_simple_title_body('Wi-Fi Forget...', ['Disconnect + erase creds', '', 'Done.']), 'wifi_forget', scale)

    save(screen_simple_title_body('OTA Update', ['Ready', '', 'OK to start']), 'ota_idle', scale)
    save(screen_simple_title_body('OTA Update', ['Downloading...', '153600/921600']), 'ota_downloading', scale)

    save(screen_simple_title_body('Scanning relays...', ['', '', '', 'BACK=Exit']), 'scan_begin', scale)
    save(screen_simple_title_body('Scan Result', ['Relay 1: OK', 'Relay 2: OK', 'Relay 3: OK']), 'scan_result', scale)
    save(screen_simple_title_body('Scan Done', ['All relays tested.']), 'scan_done', scale)

    # OCP modal (approximate; OK-only)
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    d.rectangle([8, 20, W-8, 92], outline=COLORS['YELLOW'], width=1)
    draw_text(img, d, 16, 28, 'Overcurrent', color='YELLOW', size=1)
    draw_text(img, d, 16, 44, 'OCP latched. Press OK', size=1)
    draw_text(img, d, 16, 56, 'to acknowledge.', size=1)
    draw_text(img, d, 16, 76, 'OK=Clear latch', color='WHITE', size=1)
    save(img, 'ocp_modal', scale)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate TLTB UI screenshots (PNG).')
    parser.add_argument('--scale', type=int, default=1, help='Integer scale factor for output images (e.g., 2 or 3).')
    args = parser.parse_args()
    p = generate_all(scale=max(1, int(args.scale)))
    print(f"Screens generated in {out_dir.resolve()} (scale={max(1, int(args.scale))}x)")
