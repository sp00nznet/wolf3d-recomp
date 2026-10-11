#!/usr/bin/env python3
"""
get_dlss.py - the optional NVIDIA DLSS SDK, downloaded into external/dlss-sdk/.

    py tools/get_dlss.py                     # shows the licence, asks before downloading
    py tools/get_dlss.py --accept-license    # for scripts: you have read and accept it

DLSS is an optional part of the build. The SDK is NVIDIA's, under the NVIDIA
RTX SDKs licence (not MIT), so it is never in this repository: this script
fetches the pieces the build uses -- the NGX headers, the static NGX library
and the DLSS runtime DLL -- from NVIDIA's GitHub release into a gitignored
folder. With it present, CMake builds the DLSS path (src/dlss.c) and copies
nvngx_dlss.dll next to the exe; without it, the build is the same minus DLSS.
See docs/dlss.md.
"""
import os
import sys
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, 'external', 'dlss-sdk')
TAG = 'v310.9.1'                 # the release the DLSS path was written and tested against
RAW = f'https://raw.githubusercontent.com/NVIDIA/DLSS/{TAG}/'
LICENSE_URL = f'https://github.com/NVIDIA/DLSS/blob/{TAG}/LICENSE.txt'

HEADERS = ('nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_defs_dlssd.h nvsdk_ngx_defs_dlssg.h nvsdk_ngx_defs_vk.h '
           'nvsdk_ngx_helpers.h nvsdk_ngx_helpers_cuda.h nvsdk_ngx_helpers_d3d.h nvsdk_ngx_helpers_dlssd.h '
           'nvsdk_ngx_helpers_dlssd_cuda.h nvsdk_ngx_helpers_dlssd_d3d.h nvsdk_ngx_helpers_dlssd_vk.h '
           'nvsdk_ngx_helpers_dlssg.h nvsdk_ngx_helpers_dlssg_d3d.h nvsdk_ngx_helpers_dlssg_vk.h '
           'nvsdk_ngx_helpers_vk.h nvsdk_ngx_loader.h nvsdk_ngx_params.h nvsdk_ngx_params_dlssd.h '
           'nvsdk_ngx_params_dlssg.h nvsdk_ngx_standalone_common.h nvsdk_ngx_standalone_cuda.h nvsdk_ngx_vk.h').split()
FILES = [('LICENSE.txt', 'LICENSE.txt')] + [(f'include/{h}', f'include/{h}') for h in HEADERS] + [
    ('lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', 'lib/nvsdk_ngx_s.lib'),       # static CRT, as the build
    ('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'bin/nvngx_dlss.dll'),        # ~59 MB
]


def main():
    if os.path.exists(os.path.join(DEST, 'bin', 'nvngx_dlss.dll')) and '--force' not in sys.argv:
        print(f'DLSS SDK {TAG}: already in {DEST}')
        return 0
    print(f'The NVIDIA DLSS SDK {TAG} is licensed by NVIDIA under the NVIDIA RTX SDKs licence:')
    print(f'  {LICENSE_URL}')
    print('It is downloaded for your own build only (about 65 MB) and is never part of this repository.')
    if '--accept-license' not in sys.argv:
        if input('Have you read it, and do you accept it? [y/N] ').strip().lower() not in ('y', 'yes'):
            print('Not downloaded. The build works without DLSS.')
            return 1
    for src, dst in FILES:
        path = os.path.join(DEST, dst)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.exists(path):
            continue
        print(f'  {dst}')
        tmp = path + '.part'
        # curl (in Windows 10 and later), not urllib: urllib stalled for good
        # 15 KB short of the end of the 59 MB DLL on every try, curl did not.
        # -C - resumes a partial file; --retry covers a dropped connection.
        r = subprocess.run(['curl', '-sSfL', '--retry', '5', '-C', '-', '-o', tmp, RAW + src])
        if r.returncode:
            sys.exit(f'could not download {src}; run this again to resume')
        os.replace(tmp, path)
    open(os.path.join(DEST, 'VERSION'), 'w').write(TAG + '\n')
    print(f'DLSS SDK {TAG} -> {DEST}. Re-run cmake (or Setup) to build with DLSS.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
