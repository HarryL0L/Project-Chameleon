#!/data/data/com.termux/files/usr/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the probe inside Termux: pkg install clang
# It links nothing GPU-related; system libraries are dlopen()ed at runtime.
set -e
cd "$(dirname "$0")"
clang -O2 -Wall -Wextra -o ahb_probe ahb_probe.c -ldl
echo "built ./ahb_probe - run: ./ahb_probe   (or ./ahb_probe --try-vendor)"
