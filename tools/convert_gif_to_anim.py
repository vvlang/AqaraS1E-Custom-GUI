#!/usr/bin/env python3
"""
Convert GIF / WebP / Image Sequence / Video to Aqara S1E 480x480 Streaming Animation (.anim)

Supported input formats:
  - Animated GIF / WebP  (via Pillow, no extra deps)
  - Video files          (MP4, MOV, AVI, MKV, WebM, TS, ...) via ffmpeg
  - Image sequence       (001.png, 002.png, ...) via Pillow
  - Existing Aqara .anim (retime and compact static lead-in)

Requires: Pillow >= 9.1 (for Image.Resampling.LANCZOS). Optional: ffmpeg.

Usage:
  python3 tools/convert_gif_to_anim.py input.gif   [output.anim] [--fps 60] [--max-frames 120]
  python3 tools/convert_gif_to_anim.py input.mp4   [output.anim] [--fps 30] [--start 5] [--duration 10]
  python3 tools/convert_gif_to_anim.py input.mov   [output.anim] [--fps 60] [--max-frames 180]
  python3 tools/convert_gif_to_anim.py input.anim output.anim --fps 6 --static-seconds 5 --motion-seconds 3

Tips:
  - For videos, specify --start (seconds) and --duration (seconds) to clip a segment.
  - For loop animations, keep total frames under 180 for reasonable file size.
  - The tool auto-crops & scales to 480x480 with center-crop + LANCZOS resampling.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageSequence

# Import shared anim writer. `__file__` may be None when frozen (PyInstaller etc.)
# so guard the path lookup.
try:
    _HERE = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _HERE = os.getcwd()
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from anim_generator import encode_qoi, write_anim  # noqa: E402

# ── Supported format detection ─────────────────────────────────────────────────
VIDEO_EXTS = {'.mp4', '.mov', '.avi', '.mkv', '.webm', '.ts', '.m2ts',
              '.flv', '.wmv', '.m4v', '.3gp', '.rm', '.rmvb', '.vob'}
IMAGE_EXTS = {'.gif', '.webp', '.apng', '.png', '.jpg', '.jpeg', '.bmp'}
ANIM_EXTS = {'.anim'}

# Pillow >= 9.1 has Image.Resampling; older versions have Image.LANCZOS directly.
# Use whichever is available so the script works on a wider range of environments.
try:
    _LANCZOS = Image.Resampling.LANCZOS
except AttributeError:
    _LANCZOS = Image.LANCZOS  # type: ignore[attr-defined]


def find_ffmpeg():
    """Locate ffmpeg on PATH or at the common Homebrew prefix."""
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg:
        return ffmpeg
    homebrew = '/opt/homebrew/bin/ffmpeg'
    if os.path.exists(homebrew):
        return homebrew
    return None


def resize_center_crop(img_rgba: Image.Image, size: int = 480) -> bytes:
    """Center-crop to square then LANCZOS-resize to size×size, return raw RGBA bytes."""
    src_w, src_h = img_rgba.size
    min_dim = min(src_w, src_h)
    crop_left = (src_w - min_dim) // 2
    crop_top = (src_h - min_dim) // 2
    cropped = img_rgba.crop((crop_left, crop_top, crop_left + min_dim, crop_top + min_dim))
    resized = cropped.resize((size, size), _LANCZOS)
    return resized.tobytes()


# ── GIF / WebP path ────────────────────────────────────────────────────────────
def convert_gif_webp(input_path, output_path, fps, max_frames):
    im = Image.open(input_path)
    total_in_src = getattr(im, 'n_frames', 1)
    print(f"  Source frames : {total_in_src}")
    print(f"  Target        : 480×480 @ {fps} FPS, max {max_frames} frames")

    frames_rgba = []
    for idx, frame in enumerate(ImageSequence.Iterator(im)):
        if len(frames_rgba) >= max_frames:
            print(f"  Reached max-frames limit ({max_frames}), stopping.")
            break
        frames_rgba.append(resize_center_crop(frame.convert('RGBA')))
        if (idx + 1) % 30 == 0 or idx == total_in_src - 1:
            print(f"  Processed {idx+1}/{total_in_src} frames ...")

    if not frames_rgba:
        raise ValueError(f"No decodable frames found in {input_path}")

    print(f"\nWriting {len(frames_rgba)} frames → {output_path}")
    write_anim(output_path, 480, 480, fps, frames_rgba)
    return output_path


# ── Video path (via ffmpeg) ────────────────────────────────────────────────────
def convert_video(input_path, output_path, fps, max_frames, start_sec=None, duration_sec=None):
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found. Install with: brew install ffmpeg")

    print(f"  Video info detected, extracting frames via ffmpeg ...")

    tmpdir = tempfile.mkdtemp(prefix='s1e_anim_')
    try:
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'warning']
        if start_sec is not None:
            cmd += ['-ss', str(start_sec)]
        cmd += ['-i', input_path]
        if duration_sec is not None:
            cmd += ['-t', str(duration_sec)]

        cmd += [
            '-vf', f'fps={fps},scale=960:960:force_original_aspect_ratio=increase,crop=960:960,scale=480:480',
            '-frames:v', str(max_frames),
            '-q:v', '1',                   # Highest quality for PNG
            '-pix_fmt', 'rgba',
            os.path.join(tmpdir, 'frame_%05d.png'),
        ]

        print(f"  Running ffmpeg: fps={fps}, max_frames={max_frames}" +
              (f", start={start_sec}s" if start_sec is not None else "") +
              (f", duration={duration_sec}s" if duration_sec is not None else ""))

        try:
            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError as e:
            raise RuntimeError(f"ffmpeg failed with exit code {e.returncode}") from e

        frame_files = sorted(f for f in os.listdir(tmpdir) if f.endswith('.png'))
        print(f"  Extracted {len(frame_files)} frames from video.")
        if not frame_files:
            raise RuntimeError("No frames extracted. Check input file and options.")

        frames_rgba = []
        for i, fname in enumerate(frame_files):
            img = Image.open(os.path.join(tmpdir, fname)).convert('RGBA')
            frames_rgba.append(img.tobytes())
            if (i + 1) % 30 == 0 or i == len(frame_files) - 1:
                print(f"  Encoded {i+1}/{len(frame_files)} frames ...")

        if not frames_rgba:
            raise RuntimeError("No decodable frames after ffmpeg extraction")

        print(f"\nWriting {len(frames_rgba)} frames → {output_path}")
        write_anim(output_path, 480, 480, fps, frames_rgba)
        return output_path

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ── Existing .anim path ────────────────────────────────────────────────────────
def read_anim_chunks(input_path):
    """Read an ANIM container without decoding its QOI frames."""
    with open(input_path, 'rb') as f:
        data = f.read()
    if len(data) < 14 or data[:4] != b'ANIM':
        raise ValueError(f"Not an ANIM file: {input_path}")
    width, height, fps, total = struct.unpack_from('<HHHI', data, 4)
    if width != 480 or height != 480:
        raise ValueError(f"Only 480x480 ANIM is supported, got {width}x{height}")
    chunks = []
    offset = 14
    for idx in range(total):
        if offset + 4 > len(data):
            raise ValueError(f"Truncated ANIM frame table at frame {idx}")
        chunk_len = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        end = offset + chunk_len
        if end > len(data):
            raise ValueError(f"Truncated ANIM frame {idx}")
        chunks.append(data[offset:end])
        offset = end
    return width, height, fps, chunks


def write_compact_timeline(output_path, width, height, fps, frames):
    """Write QOI chunks; None means hold the previously rendered frame."""
    with open(output_path, 'wb') as f:
        f.write(b'ANIM')
        f.write(struct.pack('<HHHI', width, height, fps, len(frames)))
        for frame in frames:
            payload = b'' if frame is None else frame
            f.write(struct.pack('<I', len(payload)))
            f.write(payload)


def convert_anim(input_path, output_path, fps, max_frames, static_seconds, motion_seconds):
    width, height, source_fps, source_frames = read_anim_chunks(input_path)
    if not source_frames:
        raise ValueError("ANIM contains no frames")
    if fps <= 0:
        raise ValueError("FPS must be positive")

    static_count = round(static_seconds * fps)
    motion_count = round(motion_seconds * fps)
    if static_count < 1 or motion_count < 1:
        raise ValueError("static/motion duration must each produce at least one frame")
    if static_count + motion_count > max_frames:
        raise ValueError("Requested timeline exceeds --max-frames")

    # The first frame is written once; empty chunks make the player hold it.
    timeline = [source_frames[0]] + [None] * (static_count - 1)

    # Resample the source motion and end exactly on frame 0 for a clean loop.
    if motion_count == 1:
        motion = [source_frames[0]]
    elif motion_count == 2:
        # Avoid ZeroDivisionError from `last_source / (motion_count - 2)` below.
        motion = [source_frames[0], source_frames[0]]
    else:
        last_source = max(0, len(source_frames) - 2)
        motion = [source_frames[round(i * last_source / (motion_count - 2))]
                  for i in range(motion_count - 1)] + [source_frames[0]]
    timeline.extend(motion)

    print(f"  Source ANIM     : {len(source_frames)} frames @ {source_fps} FPS")
    print(f"  Output timeline : {static_count} static + {motion_count} motion frames @ {fps} FPS")
    print(f"  Empty hold slots: {static_count - 1}")
    write_compact_timeline(output_path, 480, 480, fps, timeline)
    return output_path


# ── Dispatch ──────────────────────────────────────────────────────────────────
def convert(input_path, output_path=None, fps=60, max_frames=180,
            start_sec=None, duration_sec=None, static_seconds=5.0,
            motion_seconds=3.0):
    if not os.path.exists(input_path):
        raise FileNotFoundError(f"Input file not found: {input_path}")

    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = f"{base}_480.anim"

    ext = os.path.splitext(input_path)[1].lower()
    print(f"\n{'='*60}")
    print(f"  Input  : {input_path}")
    print(f"  Output : {output_path}")
    if ext in VIDEO_EXTS:
        fmt = 'Video'
    elif ext in ANIM_EXTS:
        fmt = 'ANIM'
    elif ext in IMAGE_EXTS:
        fmt = 'GIF/WebP/Image'
    else:
        raise ValueError(
            f"Unrecognized input extension {ext!r}. "
            f"Supported: {sorted(VIDEO_EXTS | IMAGE_EXTS | ANIM_EXTS)}"
        )
    print(f"  Format : {fmt}")
    print(f"{'='*60}")

    if ext in VIDEO_EXTS:
        convert_video(input_path, output_path, fps, max_frames, start_sec, duration_sec)
    elif ext in ANIM_EXTS:
        convert_anim(input_path, output_path, fps, max_frames, static_seconds, motion_seconds)
    else:
        convert_gif_webp(input_path, output_path, fps, max_frames)

    size_mb = os.path.getsize(output_path) / 1048576
    print(f"\nDone! Output: {output_path} ({size_mb:.2f} MB)")
    print("   Upload via Web Console -> Lockscreen / Streaming Animation -> Upload")
    return output_path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Convert GIF / WebP / Video to Aqara S1E 480x480 .anim',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # GIF to anim (full animation)
  python3 tools/convert_gif_to_anim.py input.gif output.anim

  # MP4 to anim (first 8 seconds, 30fps)
  python3 tools/convert_gif_to_anim.py input.mp4 output.anim --fps 30 --duration 8

  # MP4 to anim (clip from 12s to 20s, 60fps, max 120 frames)
  python3 tools/convert_gif_to_anim.py input.mp4 output.anim --fps 60 --start 12 --duration 8 --max-frames 120

  # MOV to anim (auto-crop center, 60fps)
  python3 tools/convert_gif_to_anim.py timelapse.mov output.anim --fps 60 --max-frames 180
        """,
    )
    parser.add_argument('input', help='Input file path (GIF/WebP/MP4/MOV/AVI/MKV/...)')
    parser.add_argument('output', nargs='?', default=None,
                        help='Output .anim file path (optional, defaults to <input>_480.anim)')
    parser.add_argument('--fps', type=int, default=60,
                        help='Target FPS (default: 60). Omit --start/--duration to use the full video.')
    parser.add_argument('--max-frames', type=int, default=180,
                        help='Max frames to encode (default: 180)')
    parser.add_argument('--start', type=float, default=None,
                        help='Video: start time in seconds (e.g. 5.0). Default: from beginning.')
    parser.add_argument('--duration', type=float, default=None,
                        help='Video: duration in seconds (e.g. 8.0). Default: full video.')
    parser.add_argument('--static-seconds', type=float, default=5.0,
                        help='ANIM: static lead-in duration in seconds (default: 5.0)')
    parser.add_argument('--motion-seconds', type=float, default=3.0,
                        help='ANIM: motion duration in seconds (default: 3.0)')
    args = parser.parse_args()

    convert(
        args.input, args.output,
        fps=args.fps,
        max_frames=args.max_frames,
        start_sec=args.start,
        duration_sec=args.duration,
        static_seconds=args.static_seconds,
        motion_seconds=args.motion_seconds,
    )
