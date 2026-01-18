# Generates 160x128 PNG images of the TLTB UI screens for the manual.
# This approximates the Adafruit GFX default font; layout, sizes, and colors match your code.
# Output: docs/screens/*.png

from PIL import Image, ImageDraw, ImageFont
import argparse
from pathlib import Path

# Generate at 3x native resolution for smoother output
SCALE_FACTOR = 3
W, H = 160 * SCALE_FACTOR, 128 * SCALE_FACTOR

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
# Using larger font sizes for better quality when scaled
def load_fonts():
    try:
        mono = ImageFont.truetype("DejaVuSansMono.ttf", 24)
        mono2 = ImageFont.truetype("DejaVuSansMono.ttf", 48)
        return mono, mono2
    except Exception:
        try:
            cons = ImageFont.truetype("consola.ttf", 27)
            cons2 = ImageFont.truetype("consola.ttf", 54)
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
    # Scale coordinates by SCALE_FACTOR
    x, y = x * SCALE_FACTOR, y * SCALE_FACTOR
    f = FONT2 if size == 2 else FONT1
    if bg is not None:
        tw = int(draw.textlength(text, font=f))
        th = getattr(f, 'size', 24) + 6
        draw.rectangle([x, y-6, x + tw + 6, y + th], fill=COLORS[bg])
    draw.text((x, y), text, font=f, fill=COLORS[color])


def measure_text(text: str, size: int = 1):
    """Return (width, height) using the chosen FONT1/FONT2 (no sprite)."""
    if not text:
        return (0, 0)
    tmp = Image.new('L', (1, 1))
    td = ImageDraw.Draw(tmp)
    f = FONT2 if size == 2 else FONT1
    w = int(td.textlength(text, font=f))
    h = getattr(f, 'size', 24) + 6
    return (max(1, w), max(1, h))


def save(img: Image.Image, name: str, scale: int = 1):
    p = out_dir / f"{name}.png"
    if scale and scale > 1:
        # Use LANCZOS for high-quality resampling
        img = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    img.save(p, optimize=True)
    return p


# ---------- Specific screens ----------

def screen_home(mode: str = 'HD', load_a: float = 0.00, active: str = 'RF',
                sys12_enabled: bool = True, 
                batt_volt_status: str = 'ok', batt_volt_v: float = 13.2,
                system_volt_status: str = 'ok', system_volt_v: float = 12.8,
                cooldown_status: str = 'ok', cooldown_secs: int = 0,
                fault: str | None = None, focus_mode: bool = False):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    # Layout mirrors DisplayUI::showStatus constants
    GAP = 1
    yMode, hMode = 4, 16
    yLoad, hLoad = yMode + hMode + GAP, 16
    yActive, hActive = yLoad + hLoad + GAP, 16
    y12, h12 = yActive + hActive + GAP, 12
    yBattVolt, hBattVolt = y12 + h12 + GAP, 12
    ySystemVolt, hSystemVolt = yBattVolt + hBattVolt + GAP, 12
    yCooldown, hCooldown = ySystemVolt + hSystemVolt + GAP, 12
    yHint, hHint = 114, 12

    # MODE line (size=2) with optional focus highlight
    bg = 'BLUE' if focus_mode else 'BLACK'
    draw_text(img, d, 4, yMode, f"MODE: {mode}", color='WHITE', size=2, bg=bg)

    # Load (size=2) with color coding
    draw_text(img, d, 4, yLoad, "Load: ", size=2, color='WHITE')
    # Determine color based on amperage
    load_color = 'GREEN'  # <15A
    if load_a >= 20.0:
        load_color = 'RED'  # >=20A
    elif load_a >= 15.0:
        load_color = 'YELLOW'  # 15-<20A
    load_val = f"{load_a:4.2f} A"
    lv_w, _ = measure_text("Load: ", size=2)
    draw_text(img, d, 4 + lv_w//SCALE_FACTOR, yLoad, load_val, size=2, color=load_color)

    # Active
    draw_text(img, d, 4, yActive, "Active:", size=2)
    label = active
    av_w, _ = measure_text("Active:", size=2)
    draw_text(img, d, 4 + av_w//SCALE_FACTOR + 6, yActive, label, size=2)

    # 12V sys
    draw_text(img, d, 4, y12, f"12V sys: {'ENABLED' if sys12_enabled else 'DISABLED'}", size=1)

    # Batt Volt (was LVP) status with voltage
    if batt_volt_status == 'BYPASS':
        color = 'YELLOW'
        status_text = "Batt Volt: BYPASS"
    elif batt_volt_status == 'ACTIVE':
        color = 'RED'
        status_text = "Batt Volt: ACTIVE"
    else:
        color = 'GREEN'
        status_text = "Batt Volt: ok"
    volt_text = f"  {batt_volt_v:4.1f}V" if batt_volt_v is not None else "  N/A"
    draw_text(img, d, 4, yBattVolt, status_text + volt_text, color=color, size=1)

    # System Volt (was OUTV) status with voltage
    if system_volt_status == 'BYPASS':
        color = 'YELLOW'
        status_text = "System Volt: BYPASS"
    elif system_volt_status == 'ACTIVE':
        color = 'RED'
        status_text = "System Volt: ACTIVE"
    else:
        color = 'GREEN'
        status_text = "System Volt: ok"
    volt_text = f"  {system_volt_v:4.1f}V" if system_volt_v is not None else "  N/A"
    draw_text(img, d, 4, ySystemVolt, status_text + volt_text, color=color, size=1)

    # Cooldown timer
    if cooldown_status == 'ACTIVE':
        draw_text(img, d, 4, yCooldown, f"Cooldown: {cooldown_secs:3d}s", color='RED', size=1)
    elif cooldown_secs > 0:
        draw_text(img, d, 4, yCooldown, f"Hi-Amps Time: {cooldown_secs:3d}s", color='YELLOW', size=1)
    else:
        draw_text(img, d, 4, yCooldown, "Cooldown: ok", color='GREEN', size=1)

    # Footer
    draw_text(img, d, 4, yHint, "OK=Switch Mode", color='YELLOW', size=1)

    # Fault ticker stub (if provided, show along bottom edge)
    if fault:
        d.rectangle([0, H-10*SCALE_FACTOR, W, H], fill=COLORS['RED'])
        draw_text(img, d, 2, 128-10, fault, color='WHITE', size=1)

    return img


def screen_menu(selected: int = 0):
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)

    items = [
        "Set LVP Cutoff",
        "LVP Bypass",
        "Set OCP Limit",
        "Set Output V Cutoff",
        "OutV Bypass",
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
            d.rectangle([0, (y-2)*SCALE_FACTOR, W, (y-2+rowH)*SCALE_FACTOR], fill=COLORS['BLUE'])
            draw_text(img, d, 6, y, items[i], color='WHITE', size=1)
        else:
            draw_text(img, d, 6, y, items[i], color='WHITE', size=1)

    return img


def screen_system_info(fw: str = "v1.0.2", wifi: str = "OK 192.168.1.23", 
                       batt_bypass: bool = False, sys_bypass: bool = False,
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
    line("Batt Bypass", "ON" if batt_bypass else "OFF")
    line("Sys Bypass", "ON" if sys_bypass else "OFF")

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
    # Home (HD) - various states
    save(screen_home(mode='HD', load_a=0.00, active='RF', sys12_enabled=True, 
                     batt_volt_status='ok', batt_volt_v=18.5,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_idle', scale)
    
    save(screen_home(mode='HD', load_a=3.42, active='LEFT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.3,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_left', scale)
    
    save(screen_home(mode='HD', load_a=5.8, active='RIGHT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.1,
                     system_volt_status='ok', system_volt_v=13.3,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_right', scale)
    
    save(screen_home(mode='HD', load_a=16.5, active='BRAKE', sys12_enabled=True,
                     batt_volt_status='ACTIVE', batt_volt_v=14.2,
                     system_volt_status='ok', system_volt_v=13.1,
                     cooldown_status='ok', cooldown_secs=45), 'home_hd_brake_lvp', scale)
    
    save(screen_home(mode='HD', load_a=8.2, active='TAIL', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.4,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_tail', scale)
    
    save(screen_home(mode='HD', load_a=12.3, active='MARK', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.2,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_mark', scale)
    
    save(screen_home(mode='HD', load_a=22.3, active='AUX', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=17.8,
                     system_volt_status='ok', system_volt_v=13.2,
                     cooldown_status='ACTIVE', cooldown_secs=120), 'home_hd_aux_cooldown', scale)
    
    save(screen_home(mode='HD', load_a=0.0, active='OFF', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.6,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_off_disabled', scale)
    
    save(screen_home(mode='HD', load_a=19.5, active='LEFT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.0,
                     system_volt_status='ACTIVE', system_volt_v=10.8,
                     cooldown_status='ok', cooldown_secs=75), 'home_hd_system_volt_active', scale)
    
    save(screen_home(mode='HD', load_a=4.5, active='RIGHT', sys12_enabled=True,
                     batt_volt_status='BYPASS', batt_volt_v=17.2,
                     system_volt_status='BYPASS', system_volt_v=12.8,
                     cooldown_status='ok', cooldown_secs=0), 'home_hd_both_bypass', scale)

    # Home (RV) - various states
    save(screen_home(mode='RV', load_a=0.55, active='RF', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.5,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0,
                     focus_mode=True), 'home_rv_idle_focus', scale)
    
    save(screen_home(mode='RV', load_a=6.2, active='LEFT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.3,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ok', cooldown_secs=0), 'home_rv_left', scale)
    
    save(screen_home(mode='RV', load_a=18.2, active='REV', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.0,
                     system_volt_status='ok', system_volt_v=13.3,
                     cooldown_status='ok', cooldown_secs=20), 'home_rv_rev', scale)
    
    save(screen_home(mode='RV', load_a=2.10, active='Ele Brakes', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.4,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0), 'home_rv_ele_brakes', scale)
    
    save(screen_home(mode='RV', load_a=14.8, active='BRAKE', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.1,
                     system_volt_status='ok', system_volt_v=13.2,
                     cooldown_status='ok', cooldown_secs=35), 'home_rv_brake', scale)
    
    save(screen_home(mode='RV', load_a=21.5, active='RIGHT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=17.6,
                     system_volt_status='ok', system_volt_v=13.0,
                     cooldown_status='ACTIVE', cooldown_secs=95), 'home_rv_high_load', scale)
    
    save(screen_home(mode='RV', load_a=7.3, active='TAIL', sys12_enabled=True,
                     batt_volt_status='BYPASS', batt_volt_v=16.8,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ok', cooldown_secs=0), 'home_rv_tail_batt_bypass', scale)
    
    save(screen_home(mode='RV', load_a=0.0, active='OFF', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.5,
                     system_volt_status='ok', system_volt_v=13.5,
                     cooldown_status='ok', cooldown_secs=0), 'home_rv_off', scale)

    # Cooldown states (outputs disabled, 12V system OFF)
    save(screen_home(mode='HD', load_a=0.0, active='OFF', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.2,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ACTIVE', cooldown_secs=180), 'home_hd_cooldown_180s', scale)
    
    save(screen_home(mode='HD', load_a=0.0, active='None', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.0,
                     system_volt_status='ok', system_volt_v=13.3,
                     cooldown_status='ACTIVE', cooldown_secs=45), 'home_hd_cooldown_45s', scale)
    
    save(screen_home(mode='RV', load_a=0.0, active='OFF', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.3,
                     system_volt_status='ok', system_volt_v=13.4,
                     cooldown_status='ACTIVE', cooldown_secs=150), 'home_rv_cooldown_150s', scale)
    
    save(screen_home(mode='RV', load_a=0.0, active='None', sys12_enabled=False,
                     batt_volt_status='ok', batt_volt_v=18.1,
                     system_volt_status='ok', system_volt_v=13.2,
                     cooldown_status='ACTIVE', cooldown_secs=30), 'home_rv_cooldown_30s', scale)
    
    # High current monitoring (above threshold, countdown to cooldown)
    save(screen_home(mode='HD', load_a=23.5, active='AUX', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=18.0,
                     system_volt_status='ok', system_volt_v=13.2,
                     cooldown_status='ok', cooldown_secs=85), 'home_hd_high_current_warning', scale)
    
    save(screen_home(mode='RV', load_a=22.8, active='RIGHT', sys12_enabled=True,
                     batt_volt_status='ok', batt_volt_v=17.9,
                     system_volt_status='ok', system_volt_v=13.1,
                     cooldown_status='ok', cooldown_secs=42), 'home_rv_high_current_warning', scale)

    # Menu
    save(screen_menu(selected=0), 'menu_top', scale)
    save(screen_menu(selected=4), 'menu_mid', scale)
    save(screen_menu(selected=10), 'menu_bottom', scale)

    # System Info
    save(screen_system_info(fw='v1.0.2', wifi='OK 192.168.1.23', batt_bypass=False, sys_bypass=False,
                            faults=['INA226 load missing', 'Wi-Fi disconnected']), 'system_info', scale)

    # Adjusters and simple flows
    save(screen_simple_title_body('Set LVP Cutoff (V)', ['15.50', '', 'OK=Save  BACK=Cancel']), 'lvp_cutoff', scale)
    save(screen_simple_title_body('Set OCP (A)', ['20.0', '', 'OK=Save  BACK=Cancel']), 'ocp_limit', scale)
    save(screen_simple_title_body('Set Output V Cutoff', ['10.5V', '', 'OK=Save  BACK=Cancel']), 'outv_cutoff', scale)

    save(screen_simple_title_body('LVP Bypass', ['State: ON', '', 'OK=Toggle  BACK=Exit']), 'lvp_bypass_on', scale)
    save(screen_simple_title_body('LVP Bypass', ['State: OFF', '', 'OK=Toggle  BACK=Exit']), 'lvp_bypass_off', scale)
    
    save(screen_simple_title_body('OutV Bypass', ['State: ON', '', 'OK=Toggle  BACK=Exit']), 'outv_bypass_on', scale)
    save(screen_simple_title_body('OutV Bypass', ['State: OFF', '', 'OK=Toggle  BACK=Exit']), 'outv_bypass_off', scale)

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

    # OCP modal (matches main.cpp implementation)
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    # Red background
    d.rectangle([0, 0, W, H], fill=COLORS['RED'])
    
    draw_text(img, d, 6, 6, 'Overcurrent', color='WHITE', size=2, bg='RED')
    draw_text(img, d, 6, 34, 'Overcurrent condition.', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 46, 'System disabled.', color='WHITE', size=1, bg='RED')
    
    # Optional: show suspected relay (example with LEFT)
    draw_text(img, d, 6, 58, 'Check: LEFT', color='WHITE', size=1, bg='RED')
    
    # Footer instruction (black bar)
    d.rectangle([0, 108*SCALE_FACTOR, W, 128*SCALE_FACTOR], fill=COLORS['BLACK'])
    draw_text(img, d, 6, 112, 'Rotate to OFF to restart', color='YELLOW', size=1)
    save(img, 'ocp_modal', scale)

    # LVP modal (uses protectionAlarm)
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, H], fill=COLORS['RED'])
    
    draw_text(img, d, 6, 6, 'LVP TRIPPED', color='WHITE', size=2, bg='RED')
    draw_text(img, d, 6, 28, 'Input battery voltage low.', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 40, 'System disabled to prevent', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 52, 'battery damage.', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 64, 'Charge or replace battery.', color='WHITE', size=1, bg='RED')
    
    # Footer instruction (black bar)
    d.rectangle([0, 108*SCALE_FACTOR, W, 128*SCALE_FACTOR], fill=COLORS['BLACK'])
    draw_text(img, d, 6, 112, 'OK=Clear latch', color='YELLOW', size=1)
    save(img, 'lvp_modal', scale)

    # OUTV modal (uses protectionAlarm)
    img = Image.new('RGB', (W, H), COLORS['BLACK'])
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, H], fill=COLORS['RED'])
    
    draw_text(img, d, 6, 6, 'OUTV LOW', color='WHITE', size=2, bg='RED')
    draw_text(img, d, 6, 28, 'Output voltage low.', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 40, 'Possible internal fault or', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 52, 'battery voltage low.', color='WHITE', size=1, bg='RED')
    draw_text(img, d, 6, 64, 'Charge or replace battery.', color='WHITE', size=1, bg='RED')
    
    # Footer instruction (black bar)
    d.rectangle([0, 108*SCALE_FACTOR, W, 128*SCALE_FACTOR], fill=COLORS['BLACK'])
    draw_text(img, d, 6, 112, 'OK=Clear latch', color='YELLOW', size=1)
    save(img, 'outv_modal', scale)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate TLTB UI screenshots (PNG).')
    parser.add_argument('--scale', type=int, default=1, help='Integer scale factor for output images (e.g., 2 or 3).')
    args = parser.parse_args()
    p = generate_all(scale=max(1, int(args.scale)))
    print(f"Screens generated in {out_dir.resolve()} (scale={max(1, int(args.scale))}x)")
