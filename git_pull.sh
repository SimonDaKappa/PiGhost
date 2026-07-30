#! /bin/sh
rm -rf /tmp/linux-kdoc-vendor   # clean slate if you already ran the broken version

git clone --filter=blob:none --no-checkout --depth 1 \
  https://github.com/torvalds/linux.git /tmp/linux-kdoc-vendor
cd /tmp/linux-kdoc-vendor

git sparse-checkout init --cone
git sparse-checkout set tools/docs tools/lib/python/kdoc Documentation/sphinx
git checkout master

# --- verify BEFORE copying anywhere ---
echo "tools/docs:"
ls tools/docs
echo "tools/lib/python/kdoc:"
ls tools/lib/python/kdoc
echo "Documentation/sphinx (extension files):"
ls Documentation/sphinx/kerneldoc.py Documentation/sphinx/automarkup.py

cd -   # back to your project root

mkdir -p project/docs/kerneldoc-src
cp -r /tmp/linux-kdoc-vendor/tools project/docs/kerneldoc-src/

mkdir -p project/docs/sphinx
cp /tmp/linux-kdoc-vendor/Documentation/sphinx/kerneldoc.py  project/docs/sphinx/
cp /tmp/linux-kdoc-vendor/Documentation/sphinx/automarkup.py project/docs/sphinx/

rm -rf /tmp/linux-kdoc-vendor