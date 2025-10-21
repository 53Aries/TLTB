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

# Use PIL's built-in bitmap font for crisp 1-bit style text,
# then scale the rendered text block with NEAREST for size=2.
BASE_FONT = ImageFont.load_default()

out_dir = Path("docs/screens")
out_dir.mkdir(parents=True, exist_ok=True)


def draw_text(img: Image.Image, draw: ImageDraw.ImageDraw, x: int, y: int, text: str, color: str = 'WHITE', size: int = 1, bg: str | None = None):
    # Render text to an RGBA sprite using the crisp bitmap font, then scale sprite if needed
    if not text:
        return
    bbox = draw.textbbox((0, 0), text, font=BASE_FONT)
    tw, th = (bbox[2] - bbox[0]), (bbox[3] - bbox[1])
    sprite = Image.new('RGBA', (max(1, tw), max(1, th)), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sprite)
    sd.text((0, 0), text, font=BASE_FONT, fill=COLORS[color])
    if size and size > 1:
        sprite = sprite.resize((sprite.width * size, sprite.height * size), Image.NEAREST)
    if bg is not None:
        draw.rectangle([x, y-2, x + sprite.width + 2, y + sprite.height], fill=COLORS[bg])
    img.paste(sprite, (x, y), sprite)


def measure_text(text: str, size: int = 1):
    """Return (width, height) for given text at scaling factor using the base bitmap font."""
    if not text:
        return (0, 0)
    tmp = Image.new('L', (1, 1))
    td = ImageDraw.Draw(tmp)
    bbox = td.textbbox((0, 0), text, font=BASE_FONT)
    w = max(1, bbox[2] - bbox[0]) * max(1, size)
    h = max(1, bbox[3] - bbox[1]) * max(1, size)
    return (w, h)


def save(img: Image.Image, name: str, scale: int = 1):
    p = out_dir / f"{name}.png"
    if scale and scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(p)
    return p


# ---------- Specific screens ----------

def screen_home(mode: str = 'HD', load_a: float = 0.00, active: str = 'RF', src_v: float = 12.56,
                sys12_enabled: bool = True, lvp_bypass: bool = False, lvp_latched: bool = False,
                fault: str | None = None, focus_mode: bool = False):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    # Layout mirrors DisplayUI::showStatus constants
    yMode, hMode = 6, 18
    yLoad, hLoad = yMode + hMode + 2, 18
    yActive, hActive = yLoad + hLoad + 2, 18
    ySrcV, hSrcV = yActive + hActive + 2, 12
    y12, h12 = ySrcV + hSrcV + 2, 12
    yLvp, hLvp = y12 + h12 + 2, 12
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

    # InputV
    draw_text(img, d, 4, ySrcV, f"InputV: {src_v:4.2f} V", size=1)

    # 12V sys
    draw_text(img, d, 4, y12, f"12V sys: {'ENABLED' if sys12_enabled else 'DISABLED'}", size=1)

    # LVP status
    if lvp_bypass:
        draw_text(img, d, 4, yLvp, "LVP : BYPASS", color='YELLOW', size=1)
    else:
        draw_text(img, d, 4, yLvp, f"LVP : {'ACTIVE' if lvp_latched else 'ok'}", color='WHITE', size=1)

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
        "Scan All Outputs",
        "Set LVP Cutoff",
        "LVP Bypass",
        "Set OCP Limit",
        "12V System",
        "Learn RF Button",
        "Clear RF Remotes",
        "Brightness",
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
    save(screen_home(mode='HD', load_a=0.00, active='RF', src_v=12.56, sys12_enabled=True, lvp_bypass=False, lvp_latched=False), 'home_hd_idle', scale)
    save(screen_home(mode='HD', load_a=3.42, active='LEFT', src_v=12.44, sys12_enabled=True, lvp_bypass=False, lvp_latched=False), 'home_hd_left', scale)
    save(screen_home(mode='HD', load_a=1.02, active='BRAKE', src_v=12.20, sys12_enabled=True, lvp_bypass=False, lvp_latched=True), 'home_hd_brake_lvp', scale)

    # Home (RV) with MODE focus
    save(screen_home(mode='RV', load_a=0.55, active='RF', src_v=12.70, sys12_enabled=True, lvp_bypass=False, lvp_latched=False, focus_mode=True), 'home_rv_idle_focus', scale)
    save(screen_home(mode='RV', load_a=2.10, active='BRAKE', src_v=12.38, sys12_enabled=True, lvp_bypass=False, lvp_latched=False), 'home_rv_brake', scale)

    # Menu
    save(screen_menu(selected=0), 'menu_top', scale)
    save(screen_menu(selected=4), 'menu_mid', scale)

    # System Info
    save(screen_system_info(fw='v1.0.2', wifi='OK 192.168.1.23', lvp_bypass=False,
                            faults=['INA226 load missing', 'Wi-Fi disconnected']), 'system_info', scale)

    # Adjusters and simple flows
    save(screen_simple_title_body('Brightness', ['128/255', '', 'OK=Save  BACK=Cancel']), 'brightness', scale)
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

    # OCP modal (approximate)
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    d.rectangle([8, 20, W-8, 92], outline=COLORS['YELLOW'], width=1)
    draw_text(img, d, 16, 28, 'Overcurrent', color='YELLOW', size=1)
    draw_text(img, d, 16, 44, 'OCP latched. Press OK', size=1)
    draw_text(img, d, 16, 56, 'to acknowledge.', size=1)
    draw_text(img, d, 16, 76, 'OK=Acknowledge  BACK=Cancel', color='WHITE', size=1)
    save(img, 'ocp_modal', scale)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate TLTB UI screenshots (PNG).')
    parser.add_argument('--scale', type=int, default=1, help='Integer scale factor for output images (e.g., 2 or 3).')
    args = parser.parse_args()
    p = generate_all(scale=max(1, int(args.scale)))
    print(f"Screens generated in {out_dir.resolve()} (scale={max(1, int(args.scale))}x)")
