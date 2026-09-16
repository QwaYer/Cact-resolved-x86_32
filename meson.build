# ==============================================================================
# Cact resolved — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dcactlib=../CactLibc-x86_32
#   ninja -C build-meson            # build-meson/resolved
#   ninja -C build-meson stage      # copy into -Dlr_sbin
#
# A PIE linked against CactLibc with /lib/ld.so as PT_INTERP, exactly as the
# Makefile built it.
# ==============================================================================

project('cact-resolved', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',        # -Wall -Wextra, as in the Makefile
    'buildtype=plain',
    'b_staticpic=false',
  ])

fs = import('fs')
bash = find_program('bash')
ld = find_program('ld')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_cactlib = get_option('cactlib')
cactlib = _cactlib.startswith('/') ? _cactlib : src_root / _cactlib
_lr_sbin = get_option('lr_sbin')
lr_sbin = _lr_sbin.startswith('/') ? _lr_sbin : src_root / _lr_sbin

start_o = cactlib / 'build-meson' / 'start.o'
clibc_so = cactlib / 'build-meson' / 'clibc.so'

if not fs.exists(start_o) or not fs.exists(clibc_so)
  error('CactLibc is not built: expected ' + start_o +
        '  —  build it first:  ninja -C ' + cactlib / 'build-meson')
endif

# Every object in this repo is position independent: the daemon is a PIE.
add_project_arguments('-fPIE', language: 'c')

c_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-nostdlib',
  '-ffunction-sections',
  '-fdata-sections',
  '-I' + (cactlib / 'include'),
]

resolved_lib = static_library('resolved', 'main.c', c_args: c_args)

resolved = custom_target('resolved',
  output: 'resolved',
  input: resolved_lib,
  command: [ld, '-m', 'elf_i386', '-pie', '--dynamic-linker=/lib/ld.so',
            '--hash-style=both', '-nostdlib', '--gc-sections',
            '-T', src_root / 'link.ld', '-o', '@OUTPUT@',
            start_o,
            '--whole-archive', resolved_lib, '--no-whole-archive',
            clibc_so],
  depend_files: files('link.ld'),
  build_by_default: true)

# Meson reserves the name "install" for itself, hence "stage".
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/resolved"',
            'bash', lr_sbin, resolved])
