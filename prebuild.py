#!python

# The prebuild script is intended to simplify life for developers and dev-ops.  It's repsonsible for acquiring 
# tools required by the build as well as dependencies on which we rely.  
# 
# By using this script, we can reduce the requirements for a developer getting started to:
#
# * A working C++ dev environment like visual studio, xcode, gcc, or clang
# * Qt 
# * CMake
# * Python 3.x
#
# The function of the build script is to acquire, if not already present, all the other build requirements
# The build script should be idempotent.  If you run it with the same arguments multiple times, that should 
# have no negative impact on the subsequent build times (i.e. re-running the prebuild script should not 
# trigger a header change that causes files to be rebuilt).  Subsequent runs after the first run should 
# execute quickly, determining that no work is to be done

import hifi_singleton
import hifi_utils
import hifi_android
import hifi_vcpkg

import argparse
import concurrent
import hashlib
import importlib
import json
import os
import platform
import shutil
import ssl
import sys
import re
import tempfile
import time
import functools

print = functools.partial(print, flush=True)

def parse_args():
    # our custom ports, relative to the script location
    defaultPortsPath = hifi_utils.scriptRelative('cmake', 'ports')
    from argparse import ArgumentParser
    parser = ArgumentParser(description='Prepare build dependencies.')
    parser.add_argument('--android', action='store_true')
    #parser.add_argument('--android', type=str)
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--force-bootstrap', action='store_true')
    parser.add_argument('--force-build', action='store_true')
    parser.add_argument('--vcpkg-root', type=str, help='The location of the vcpkg distribution')
    parser.add_argument('--build-root', required=True, type=str, help='The location of the cmake build')
    parser.add_argument('--ports-path', type=str, default=defaultPortsPath)
    if True:
        args = parser.parse_args()
    else:
        args = parser.parse_args(['--android', 'questInterface', '--build-root', 'C:/git/hifi/android/apps/questInterface/.externalNativeBuild/cmake/debug/arm64-v8a'])
    return args

def main():
    # Fixup env variables.  Leaving `USE_CCACHE` on will cause scribe to fail to build
    # VCPKG_ROOT seems to cause confusion on Windows systems that previously used it for 
    # building OpenSSL
    removeEnvVars = ['VCPKG_ROOT', 'USE_CCACHE']
    for var in removeEnvVars:
        if var in os.environ:
            del os.environ[var]

    args = parse_args()
    # Only allow one instance of the program to run at a time
    pm = hifi_vcpkg.VcpkgRepo(args)
    with hifi_singleton.Singleton(pm.lockFile) as lock:
        if not pm.upToDate():
            pm.bootstrap()

        # Always write the tag, even if we changed nothing.  This 
        # allows vcpkg to reclaim disk space by identifying directories with
        # tags that haven't been touched in a long time
        pm.writeTag()

        # Grab our required dependencies:
        #  * build host tools, like spirv-cross and scribe
        #  * build client dependencies like openssl and nvtt
        pm.setupDependencies()

        # wipe out the build directories (after writing the tag, since failure 
        # here shouldn't invalidte the vcpkg install)
        pm.cleanBuilds()

        # Write the vcpkg config to the build directory last
        pm.writeConfig()

print(sys.argv)
main()
