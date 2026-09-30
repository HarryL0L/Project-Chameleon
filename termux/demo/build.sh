#!/data/data/com.termux/files/usr/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the demo producer inside Termux: pkg install clang
set -e
cd "$(dirname "$0")"
clang -O2 -Wall -Wextra -I../../common -o chameleon_demo chameleon_demo.c -ldl -lm
echo "built ./chameleon_demo - open the Chameleon app first, then run it"
