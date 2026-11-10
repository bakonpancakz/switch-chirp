# Sourced by the build scripts (inside devkitPro's msys2 bash). build.ps1 passes
# the toolchain's real location in DEVKITPRO_DIR: a devkitPro that was moved still
# has its msys2 mounting /opt/devkitpro at the old place.
export DEVKITPRO=${DEVKITPRO_DIR:-/opt/devkitpro}
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
# the portlibs' .pc files say prefix=/opt/devkitpro/portlibs/switch
export PKGCONF_FLAGS="--define-variable=prefix=$DEVKITPRO/portlibs/switch"
