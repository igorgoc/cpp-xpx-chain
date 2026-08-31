#!/usr/bin/env python3
import os, subprocess, shutil, sys

bin_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'bin')
print(f"Bundling dependencies for {bin_dir}...")

def get_deps(path):
    try:
        out = subprocess.check_output(['otool', '-L', path]).decode('utf-8')
        deps = []
        for line in out.split('\n'):
            line = line.strip()
            if line.startswith('/opt/homebrew') or line.startswith('/usr/local'):
                lib = line.split()[0]
                deps.append(lib)
        return deps
    except Exception:
        return []

# Collect all dependencies recursively
all_copied = {}
to_process = []

for root, _, files in os.walk(bin_dir):
    for f in files:
        if f.endswith('.dylib') or f in ('sirius.bc', 'catapult.recovery'):
            p = os.path.join(root, f)
            to_process.append(p)

while to_process:
    curr = to_process.pop(0)
    deps = get_deps(curr)
    for d in deps:
        d_real = os.path.realpath(d)
        if not os.path.exists(d_real):
            continue
        dest_name = os.path.basename(d_real)
        dest_path = os.path.join(bin_dir, dest_name)
        
        # Link alias as well (e.g. librocksdb.11.dylib -> librocksdb.11.8.1.dylib)
        alias_name = os.path.basename(d)
        alias_path = os.path.join(bin_dir, alias_name)

        if not os.path.exists(dest_path):
            shutil.copy2(d_real, dest_path)
            os.chmod(dest_path, 0o755)
            print(f"Copied {d_real} -> {dest_name}")
            to_process.append(dest_path)
        
        if alias_name != dest_name and not os.path.exists(alias_path):
            try:
                os.symlink(dest_name, alias_path)
            except Exception:
                shutil.copy2(dest_path, alias_path)

print("Fixing install names and rpaths...")
for root, _, files in os.walk(bin_dir):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p):
            continue
        if f.endswith('.dylib') or f in ('sirius.bc', 'catapult.recovery'):
            # Set ID
            if f.endswith('.dylib'):
                subprocess.call(['install_name_tool', '-id', f'@rpath/{f}', p], stderr=subprocess.DEVNULL)
            
            # Ensure rpath is present
            subprocess.call(['install_name_tool', '-add_rpath', '@loader_path', p], stderr=subprocess.DEVNULL)
            
            # Change deps to @rpath
            deps = get_deps(p)
            for d in deps:
                base = os.path.basename(d)
                subprocess.call(['install_name_tool', '-change', d, f'@rpath/{base}', p], stderr=subprocess.DEVNULL)
            
            # Resign binary
            subprocess.call(['codesign', '--force', '--sign', '-', p], stderr=subprocess.DEVNULL)

print("All dependencies bundled and relocatable!")
