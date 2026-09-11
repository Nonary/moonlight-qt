#!/usr/bin/env python3
"""Run nested Gamescope with the real performance overlay at very low opacity.

This intentionally keeps mangoapp mapped and rendering; no_display would stop
that behavior. The configuration is process-local and never edits Steam's HUD
settings. Gamescope owns the overlay process and tears it down with the session.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--visible', action='store_true',
                        help='show a normal-opacity FPS counter for comparison')
    parser.add_argument('--refresh', type=int, default=120,
                        help='nested advertised refresh in Hz (default: 120)')
    parser.add_argument('--dry-run', action='store_true',
                        help='print the launch command without opening a window')
    parser.add_argument('command', nargs=argparse.REMAINDER,
                        help='optional command after --; defaults to moonlight-dev')
    args = parser.parse_args()
    if not 1 <= args.refresh <= 1000:
        parser.error('--refresh must be between 1 and 1000')
    if os.environ.get('GAMESCOPE_WAYLAND_DISPLAY'):
        parser.error('run this launcher from Desktop Mode, outside an existing Gamescope session')
    command = args.command
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        command = [str(Path.home() / '.local/bin/moonlight-dev')]
    for executable in ['gamescope', 'mangoapp', command[0]]:
        if not shutil.which(executable):
            parser.error(f'executable not found: {executable}')

    # no_display=0 matters: the real helper must keep its overlay window mapped.
    # Low but nonzero alpha keeps visible pixel data, avoiding a fully empty HUD.
    # No FPS limiter, Vulkan mode override, or injected MangoHud layer is added.
    config = ','.join([
        'fps_only', 'no_display=0', 'background_alpha=0',
        'alpha=' + ('1' if args.visible else '0.01'),
        'font_size=' + ('24' if args.visible else '8'),
        'position=top-left', 'text_outline=0', 'fps_limit=0',
    ])
    env = os.environ.copy()
    env['MANGOHUD_CONFIGFILE'] = '/dev/null'
    env['MANGOHUD_CONFIG'] = config
    # Only the child gets the wrapper's Gamescope-session marker.
    argv = ['gamescope', '-f', '-r', str(args.refresh), '--adaptive-sync',
            '--mangoapp', '--', 'env', 'XDG_SESSION_DESKTOP=gamescope', *command]
    print('Minimal Gamescope overlay: ' + ('visible comparison' if args.visible else '1% opacity, 8px font'), flush=True)
    print('Keep Moonlight composition/repaint experiments off for this comparison.', flush=True)
    if args.dry_run:
        print(shlex.join(['env', 'MANGOHUD_CONFIGFILE=/dev/null',
                          'MANGOHUD_CONFIG=' + config, *argv]))
        return 0
    os.execvpe(argv[0], argv, env)


if __name__ == '__main__':
    sys.exit(main())
