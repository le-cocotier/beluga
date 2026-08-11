#!/bin/bash


echo "--- Welcome to Beluga installer ---"
read -p "Do you want to start the installation ? (Y/n)" confirm && [[ $confirm == [yY] || $confirm == [yY][eE][sS] ]] || exit 1

# Check dependencies

if ! command -v "meson" &>/dev/null; then
    echo "❌ Error: meson not found. Please install it first."
    exit 1
fi

if ! command -v "git" &>/dev/null; then
    echo "❌ Error: git not found. Please install it first."
    exit 1
fi


# Create config files

echo "Create config files ..."
mkdir -pv ~/.beluga/
mkdir -pv ~/.beluga/assets/
cp -rv ./assets/beluga.txt ~/.beluga/assets/
mkdir -pv ~/.beluga/packages/
mkdir -pv ~/.beluga/config/

read -p "Do you want to replace your config file or keep it (init.lisp.bak) / (init.lisp.new) ? (Y/n)" confirm
if [[ "$confirm" =~ ^[Yy]$ ]]; then
    mv ~/.beluga/config/init.lisp  ~/.beluga/config/init.lisp.bak
    cp -rv ./config/init.lisp ~/.beluga/config/
else
    cp -rv ./config/init.lisp ~/.beluga/config/init.lisp.new
fi

# Install packages enabled by default in config/init.lisp (currently just
# "outline"). Packages live in their own git repos — same convention as
# the smart_delimiters example ("First git clone it") — so this clones
# rather than copying anything out of this checkout. Already-installed
# packages are left alone (git pull them yourself to update) rather than
# clobbered, since ~/.beluga/packages/ is meant to be user-editable.
#
# One "name url" pair per line — kept as a plain list rather than an
# associative array, since macOS ships bash 3.2 as /bin/bash (no
# declare -A support; that needs bash 4+) and this script's shebang
# targets /bin/bash.
BELUGA_PACKAGES="
outline https://git.giorgio-nas.fr/beluga/outine.git
smart_delimiters https://git.giorgio-nas.fr/beluga/smart_delimiters.git
c https://git.giorgio-nas.fr/beluga/c.git
explorer https://git.giorgio-nas.fr/beluga/explorer.git
"

echo "Installing packages ..."
echo "$BELUGA_PACKAGES" | while read -r pkg_name pkg_url; do
    [ -z "$pkg_name" ] && continue
    dest=~/.beluga/packages/"$pkg_name"
    if [ -d "$dest" ]; then
        echo "Package '$pkg_name' is already installed at $dest, skipping"
    else
        git clone "$pkg_url" "$dest" ||
            echo "⚠️  Warning: couldn't clone '$pkg_name' — (add-package \"$pkg_name\") in init.lisp will silently no-op until it's installed"
    fi
done


# Compile the project

echo "Start compilation ..."
meson setup build/
meson compile -C build/

# Add to path
echo "Adding beluga to the path"
sudo cp -f ./build/beluga /usr/local/bin/

echo "Installation finish"
echo "Check ~/.beluga/config/init.lisp for customization"
