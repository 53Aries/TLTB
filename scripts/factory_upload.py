Import("env")

# Override upload to write ONLY firmware.bin to factory partition
# Do not write bootloader or partition table

def custom_upload(target, source, env):
    from os.path import join
    import sys
    
    firmware_path = join(env.subst("$BUILD_DIR"), "firmware.bin")
    upload_port = env.subst("$UPLOAD_PORT")
    python_exe = env.subst("$PYTHONEXE")
    uploader = env.subst("$UPLOADER")
    
    print("\n" + "="*70)
    print("FACTORY PARTITION UPLOAD")
    print("="*70)
    print(f"Target address: 0x3D0000 (factory partition)")
    print(f"Firmware: {firmware_path}")
    print(f"Port: {upload_port}")
    print("="*70 + "\n")
    
    # Build esptool command
    cmd = [
        python_exe,
        uploader,
        "--chip", "esp32s3",
        "--port", upload_port,
        "--baud", "921600",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "8MB",
        "0x3D0000",  # Factory partition address
        firmware_path
    ]
    
    print("Running: " + " ".join(cmd))
    print("")
    
    result = env.Execute(" ".join(['"%s"' % s if ' ' in s else s for s in cmd]))
    
    if result != 0:
        sys.exit(1)
    
    print("\n" + "="*70)
    print("Factory firmware uploaded successfully to 0x3D0000")
    print("="*70 + "\n")

# Replace the upload command entirely
env.Replace(UPLOADCMD=custom_upload)
