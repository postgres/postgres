#!/bin/sh

# Installs the passed in packages via macports. To make it fast enough
# for CI, cache the installation as a .dmg file.  To avoid
# unnecessarily updating the cache, the cached image is only modified
# when packages are installed or removed.  Any package this script is
# not instructed to install, will be removed again.
#
# This currently expects to be run in a macos cirrus-ci environment.

set -e
# set -x

packages="$@"

macos_major_version="` sw_vers -productVersion | sed 's/\..*//' `"
echo "macOS major version: $macos_major_version"

# Scan the available MacPorts releases to find one that matches the running
# macOS release.
macports_release_list_url="https://api.github.com/repos/macports/macports-base/releases"
macports_version_pattern="2\.10\.1"
macports_url="$( curl -s $macports_release_list_url | grep "\"https://github.com/macports/macports-base/releases/download/v$macports_version_pattern/MacPorts-$macports_version_pattern-$macos_major_version-[A-Za-z]*\.pkg\"" | sed 's/.*: "//;s/".*//' | head -1 )"
echo "MacPorts package URL: $macports_url"

cache_dmg="macports.hfs.dmg"

if [ "$CIRRUS_CI" != "true" ]; then
    echo "expect to be called within cirrus-ci" 1>2
    exit 1
fi

sudo mkdir -p /opt/local
mkdir -p ${MACPORTS_CACHE}/

# If we are starting from clean cache, perform a fresh macports
# install. Otherwise decompress the .dmg we created previously.
#
# After this we have a working macports installation, with an unknown set of
# packages installed.
new_install=0
update_cached_image=0
if [ -e ${MACPORTS_CACHE}/${cache_dmg}.zstd ]; then
    time zstd -T0 -d ${MACPORTS_CACHE}/${cache_dmg}.zstd -o ${cache_dmg}
    time sudo hdiutil attach -kernel ${cache_dmg} -owners on -shadow ${cache_dmg}.shadow -mountpoint /opt/local
else
    new_install=1
    curl -fsSL -o macports.pkg "$macports_url"
    time sudo installer -pkg macports.pkg -target /
    # this is a throwaway environment, and it'd be a few lines to gin
    # up a correct user / group when using the cache.
    echo macportsuser root | sudo tee -a /opt/local/etc/macports/macports.conf
fi
export PATH=/opt/local/sbin/:/opt/local/bin/:$PATH

# mark all installed packages unrequested, that allows us to detect
# packages that aren't needed anymore
if [ -n "$(port -q installed installed)" ] ; then
    sudo port unsetrequested installed
fi

# If setting all the required packages as requested fails, we need
# to install at least one of them. Need to do so one-by-one as
# port setrequested only reports failures for the first package.
echo "checking if all required packages are installed"
for package in $packages ; do
  if ! sudo port setrequested $package > /dev/null 2>&1 ; then
    update_cached_image=1
  fi
done
echo "done"
if [ "$update_cached_image" -eq 1 ]; then
    echo not all required packages installed, doing so now
    # to keep the image small, we deleted the ports tree from the image...
    sudo port selfupdate
    # XXX likely we'll need some other way to force an upgrade at some
    # point...
    sudo port upgrade outdated
    sudo port install -N $packages
    sudo port setrequested $packages
fi

# check if any ports should be uninstalled
if [ -n "$(port -q installed rleaves)" ] ; then
    echo superfluous packages installed
    update_cached_image=1
    sudo port uninstall --follow-dependencies rleaves

    # remove prior cache contents, don't want to increase size
    rm -f ${MACPORTS_CACHE}/*
fi

# Shrink installation if we created / modified it
if [ "$new_install" -eq 1 -o "$update_cached_image" -eq 1 ]; then
    sudo /opt/local/bin/port clean --all installed
    sudo rm -rf /opt/local/var/macports/{software,sources}/*
fi

# If we're starting from a clean cache, start a new image. If we have
# an image, but the contents changed, update the image in the cache
# location.
if [ "$new_install" -eq 1 ]; then
    # use a generous size, so additional software can be installed later
    time sudo hdiutil create -fs HFS+ -format UDRO -size 10g -layout NONE -srcfolder /opt/local/ ${cache_dmg}
    time zstd -T -10 -z ${cache_dmg} -o ${MACPORTS_CACHE}/${cache_dmg}.zstd
elif [ "$update_cached_image" -eq 1 ]; then
    sudo hdiutil detach /opt/local/
    time hdiutil convert -format UDRO ${cache_dmg} -shadow ${cache_dmg}.shadow -o updated.hfs.dmg
    rm ${cache_dmg}.shadow
    mv updated.hfs.dmg ${cache_dmg}
    time zstd --force -T -10 -z ${cache_dmg} -o ${MACPORTS_CACHE}/${cache_dmg}.zstd
    time sudo hdiutil attach -kernel ${cache_dmg} -owners on -shadow ${cache_dmg}.shadow -mountpoint /opt/local
fi
