# TLTB Manual Assets

This folder will hold rendered images of the device UI to use in your manual.

- Generated screenshots: `docs/screens/*.png`
- Generator script: `scripts/generate_screens.py` (uses Python + Pillow)

## Generate screenshots

1) Make sure Python 3.9+ is installed.
2) Install Pillow:

```
pip install Pillow
```

3) Run the generator from the workspace root:

```
python scripts/generate_screens.py
```

Images will be written into `docs/screens`.

## Notes
- The rendering matches layout/wording/colors from `DisplayUI.cpp` but uses system fonts (e.g., DejaVuSansMono or Consolas) instead of Adafruit GFX’s 5x7 font. If you need exact pixel-perfect font, see Next Steps below.

## Next Steps (optional)
If you want 1:1 pixel output from the actual firmware font and drawing:

- Add a firmware-only screenshot mode that renders into an in-RAM `GFXcanvas16` and saves to BMP on SPIFFS/SD or streams via Serial/Wi-Fi.
- We can wire a hidden key combo (e.g., long-press BACK) to dump the current screen.
- I can add a minimal BMP/PPM writer and a `ui->renderTo(canvas)` hook so screenshots match the device exactly.
