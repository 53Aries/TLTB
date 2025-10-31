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
- The rendering matches layout/wording/colors from `DisplayUI.cpp` but uses system fonts (e.g., DejaVuSansMono or Consolas) instead of Adafruit GFX’s 5x7 font.
