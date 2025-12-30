from SCons.Script import Import

env = Import("env")

if env.get("PIOENV") == "esp32s3-devkitc1-recovery":
    env.Replace(
        UPLOADCMD=(
            '"$PYTHONEXE" "$UPLOADER" --chip esp32s3 '
            '--port $UPLOAD_PORT --baud $UPLOAD_SPEED '
            '--before default_reset --after hard_reset '
            'write_flash 0x10000 $SOURCE'
        )
    )
