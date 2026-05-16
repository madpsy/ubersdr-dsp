#!/usr/bin/env python3
"""
mic_to_speaker.py — real-time noise reduction via ubersdr-dsp

Captures audio from a microphone, sends it through the ubersdr-dsp gRPC
server for noise reduction, and plays the processed audio on a speaker output.

The ubersdr-dsp server only accepts 12000 or 24000 Hz. If the audio device
does not support those rates natively (e.g. a USB interface that only supports
44100/48000 Hz), the script resamples transparently using scipy.signal.resample_poly.

Architecture:
  [mic hw]  →  raw_in_q  →  [resample thread]  →  send_q  →  [gRPC thread]
  [spk hw]  ←  raw_out_q ←  [resample thread]  ←  recv_q  ←  [gRPC thread]

The sounddevice callbacks are kept minimal (just queue.put/get) so they never
block the audio driver.

Requirements:
    pip install sounddevice grpcio grpcio-tools numpy scipy

Generate gRPC stubs (run once from the repo root):
    python -m grpc_tools.protoc \\
        -I proto \\
        --python_out=examples \\
        --grpc_python_out=examples \\
        proto/ubersdr_dsp.proto

Usage:
    python examples/mic_to_speaker.py [options]

    --filter    nr2|rn2|nr4|dfnr|bnr   (default: nr2)
    --server    host:port               (default: localhost:50051)
    --grpc-rate 12000|24000             (default: 24000)
    --device-rate Hz                    (default: auto-detect from device)
    --block     frames per chunk at grpc-rate (default: 960 = 40 ms at 24 kHz)
    --latency   sounddevice latency     (default: low)
    --list      list available audio devices and exit
    --input     input device index or name substring
    --output    output device index or name substring
    --param     KEY=VALUE filter parameter (repeatable)

Examples:
    # Default: NR2 filter, default mic/speaker
    python examples/mic_to_speaker.py

    # Steinberg UR24C (device 5) — auto-detects 48000 Hz device rate
    python examples/mic_to_speaker.py --filter nr2 --input 5 --output 5

    # DeepFilterNet3 neural denoiser
    python examples/mic_to_speaker.py --filter dfnr --input 5 --output 5

    # List devices
    python examples/mic_to_speaker.py --list
"""

import argparse
import queue
import sys
import threading
import time

import numpy as np

# ── gRPC imports ──────────────────────────────────────────────────────────────
try:
    import os as _os
    sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
    import ubersdr_dsp_pb2 as pb
    import ubersdr_dsp_pb2_grpc as rpc
except ImportError:
    print(
        "ERROR: gRPC stubs not found.\n"
        "Generate them with:\n"
        "  python -m grpc_tools.protoc \\\n"
        "      -I proto \\\n"
        "      --python_out=examples \\\n"
        "      --grpc_python_out=examples \\\n"
        "      proto/ubersdr_dsp.proto",
        file=sys.stderr,
    )
    sys.exit(1)

import grpc
import sounddevice as sd

try:
    from scipy.signal import resample_poly
    from math import gcd
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False


# ── Argument parsing ──────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Real-time mic → ubersdr-dsp noise reduction → speaker"
    )
    p.add_argument("--filter",      default="nr2",
                   choices=["nr2", "rn2", "nr4", "dfnr", "bnr"],
                   help="Noise reduction filter (default: nr2)")
    p.add_argument("--server",      default="localhost:50051",
                   help="ubersdr-dsp gRPC address (default: localhost:50051)")
    p.add_argument("--grpc-rate",   type=int, default=24000, choices=[12000, 24000],
                   dest="grpc_rate",
                   help="Sample rate sent to the gRPC server: 12000 or 24000 (default: 24000)")
    p.add_argument("--device-rate", type=int, default=None, dest="device_rate",
                   help="Force audio device sample rate (default: auto-detect)")
    p.add_argument("--block",       type=int, default=960,
                   help="Frames per gRPC chunk at grpc-rate (default: 960 = 40 ms at 24 kHz)")
    p.add_argument("--latency",     default="low",
                   help="sounddevice latency: 'low', 'high', or seconds (default: low)")
    p.add_argument("--input",       default=None,
                   help="Input device index or name substring (default: system default)")
    p.add_argument("--output",      default=None,
                   help="Output device index or name substring (default: system default)")
    p.add_argument("--list",        action="store_true",
                   help="List audio devices and exit")
    p.add_argument("--param",       action="append", default=[], metavar="KEY=VALUE",
                   help="Filter parameter (repeatable), e.g. --param gain-method=2")
    return p.parse_args()


def find_device(query, kind):
    if query is None:
        return None
    try:
        return int(query)
    except ValueError:
        pass
    query_lower = query.lower()
    for i, dev in enumerate(sd.query_devices()):
        if query_lower in dev["name"].lower():
            if kind == "input"  and dev["max_input_channels"]  > 0:
                return i
            if kind == "output" and dev["max_output_channels"] > 0:
                return i
    raise ValueError(f"No {kind} device matching '{query}' found")


def detect_device_rate(device_idx, kind, preferred=(48000, 44100, 96000, 192000, 24000, 12000)):
    """Find the first sample rate the device accepts, using its native channel count."""
    dev = sd.query_devices(device_idx)
    if kind == "input":
        channels = max(1, dev["max_input_channels"])
    else:
        channels = max(1, dev["max_output_channels"])

    for rate in preferred:
        try:
            if kind == "input":
                sd.check_input_settings(device=device_idx, channels=channels,
                                        dtype="float32", samplerate=rate)
            else:
                sd.check_output_settings(device=device_idx, channels=channels,
                                         dtype="float32", samplerate=rate)
            return rate
        except Exception:
            continue
    raise RuntimeError(f"No supported sample rate found for device {device_idx}")


# ── Rational resampler ────────────────────────────────────────────────────────

class Resampler:
    def __init__(self, in_rate, out_rate):
        if in_rate == out_rate:
            self._up = self._down = 1
        else:
            if not HAS_SCIPY:
                raise RuntimeError(
                    "scipy is required for resampling.\n"
                    "Install: pip install scipy"
                )
            g = gcd(in_rate, out_rate)
            self._up   = out_rate // g
            self._down = in_rate  // g

    def process(self, samples: np.ndarray) -> np.ndarray:
        if self._up == self._down:
            return samples
        return resample_poly(samples, self._up, self._down).astype(np.float32)

    @property
    def passthrough(self):
        return self._up == self._down


# ── gRPC request generator ────────────────────────────────────────────────────

def grpc_request_generator(send_queue, filter_name, block, grpc_rate, params):
    param_map = {}
    for kv in params:
        if "=" in kv:
            k, v = kv.split("=", 1)
            param_map[k.strip()] = v.strip()

    yield pb.AudioRequest(
        config=pb.SessionConfig(
            filter=filter_name,
            block=block,
            sample_rate=grpc_rate,
            channels=1,
            params=param_map,
        )
    )
    while True:
        chunk = send_queue.get()
        if chunk is None:
            return
        yield pb.AudioRequest(audio=pb.AudioChunk(pcm_data=chunk.tobytes()))


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    if args.list:
        print(sd.query_devices())
        return

    input_device  = find_device(args.input,  "input")
    output_device = find_device(args.output, "output")

    # Query device channel counts.
    # Cap at 2 (stereo) — virtual devices like PulseAudio/default report 32 channels
    # but opening that many is wasteful and dilutes the mic signal when averaging.
    in_dev_info  = sd.query_devices(input_device  if input_device  is not None else sd.default.device[0])
    out_dev_info = sd.query_devices(output_device if output_device is not None else sd.default.device[1])
    in_channels  = min(2, max(1, in_dev_info["max_input_channels"]))
    out_channels = min(2, max(1, out_dev_info["max_output_channels"]))

    # Determine device sample rate
    if args.device_rate:
        device_rate = args.device_rate
    else:
        probe = [args.grpc_rate, 48000, 44100, 96000, 192000]
        device_rate = detect_device_rate(input_device, "input", probe)

    grpc_rate  = args.grpc_rate
    block_grpc = args.block
    # Device-side block size: same wall-clock duration as block_grpc at grpc_rate
    block_dev  = int(round(block_grpc * device_rate / grpc_rate))

    needs_resample = (device_rate != grpc_rate)
    if needs_resample and not HAS_SCIPY:
        print(
            f"ERROR: device rate {device_rate} Hz ≠ gRPC rate {grpc_rate} Hz.\n"
            "Install scipy:  pip install scipy",
            file=sys.stderr,
        )
        sys.exit(1)

    up_resampler   = Resampler(device_rate, grpc_rate)
    down_resampler = Resampler(grpc_rate, device_rate)

    print(f"Audio device rate : {device_rate} Hz" +
          (f"  →  resampled to {grpc_rate} Hz for gRPC" if needs_resample else ""))
    print(f"Input device      : {in_dev_info['name']}  ({in_channels} ch)")
    print(f"Output device     : {out_dev_info['name']}  ({out_channels} ch)")
    print(f"Starting          : filter={args.filter}  server={args.server}  "
          f"grpc-rate={grpc_rate} Hz  block={block_grpc} frames")
    print("Press Ctrl+C to stop.\n")

    # ── Queues ────────────────────────────────────────────────────────────────
    # raw_in_q  : device-rate mono float32 blocks from mic callback
    # send_q    : grpc-rate mono float32 blocks to gRPC sender
    # recv_q    : grpc-rate mono float32 blocks from gRPC receiver
    # raw_out_q : device-rate mono float32 blocks to speaker callback
    raw_in_q  = queue.Queue(maxsize=32)
    send_q    = queue.Queue(maxsize=16)
    recv_q    = queue.Queue(maxsize=16)
    raw_out_q = queue.Queue(maxsize=32)

    stop_event = threading.Event()

    # Diagnostic counters (updated from callbacks, read from stats thread)
    stats = {
        "in_blocks":  0,
        "out_blocks": 0,
        "underruns":  0,
        "grpc_recv":  0,
    }

    # ── sounddevice callbacks (kept minimal — no heavy work here) ─────────────

    def input_callback(indata, frames, time_info, status):
        # indata shape: (frames, in_channels) — average first 2 channels to mono
        mono = indata[:, :2].mean(axis=1).astype(np.float32) if indata.shape[1] >= 2 \
               else indata[:, 0].astype(np.float32)
        stats["in_blocks"] += 1
        try:
            raw_in_q.put_nowait(mono)
        except queue.Full:
            pass

    playback_buf = np.array([], dtype=np.float32)

    def output_callback(outdata, frames, time_info, status):
        nonlocal playback_buf
        # Drain raw_out_q into playback_buf
        while len(playback_buf) < frames:
            try:
                playback_buf = np.concatenate([playback_buf, raw_out_q.get_nowait()])
            except queue.Empty:
                break

        if len(playback_buf) >= frames:
            mono_out     = playback_buf[:frames]
            playback_buf = playback_buf[frames:]
            stats["out_blocks"] += 1
        else:
            # Underrun — pad with silence
            mono_out = np.zeros(frames, dtype=np.float32)
            mono_out[:len(playback_buf)] = playback_buf
            playback_buf = np.array([], dtype=np.float32)
            stats["underruns"] += 1

        # Write mono to first 2 channels (L+R); zero any remaining channels
        outdata[:] = 0
        outdata[:, 0] = mono_out
        if outdata.shape[1] > 1:
            outdata[:, 1] = mono_out

    # ── Resample-in thread: device-rate → grpc-rate, packetise ───────────────

    def resample_in_thread():
        accum = np.array([], dtype=np.float32)
        while not stop_event.is_set():
            try:
                block = raw_in_q.get(timeout=0.05)
            except queue.Empty:
                continue
            resampled = up_resampler.process(block)
            accum = np.concatenate([accum, resampled])
            while len(accum) >= block_grpc:
                send_q.put(accum[:block_grpc].copy())
                accum = accum[block_grpc:]
        send_q.put(None)  # signal gRPC generator to stop

    # ── Resample-out thread: grpc-rate → device-rate ──────────────────────────

    def resample_out_thread():
        while not stop_event.is_set():
            try:
                pcm = recv_q.get(timeout=0.05)
            except queue.Empty:
                continue
            resampled = down_resampler.process(pcm)
            try:
                raw_out_q.put_nowait(resampled)
            except queue.Full:
                pass

    # ── gRPC thread ───────────────────────────────────────────────────────────

    def grpc_thread():
        try:
            channel = grpc.insecure_channel(args.server)
            stub    = rpc.DspServiceStub(channel)
            requests = grpc_request_generator(
                send_q, args.filter, block_grpc, grpc_rate, args.param
            )
            configured = False
            for response in stub.ProcessAudio(requests):
                if response.HasField("error"):
                    print(f"[gRPC] ERROR {response.error.code}: {response.error.message}",
                          file=sys.stderr)
                    stop_event.set()
                    return
                if response.HasField("ack"):
                    if not configured:
                        print(f"[gRPC] session {response.session_id} configured "
                              f"(filter={args.filter}, grpc-rate={grpc_rate}, channels=1)")
                        if response.ack.applied:
                            print(f"       params applied:  {dict(response.ack.applied)}")
                        if response.ack.rejected:
                            print(f"       params rejected: {dict(response.ack.rejected)}")
                        configured = True
                    continue
                if response.HasField("audio"):
                    pcm = np.frombuffer(response.audio.pcm_data, dtype=np.float32).copy()
                    stats["grpc_recv"] += 1
                    try:
                        recv_q.put_nowait(pcm)
                    except queue.Full:
                        pass
        except grpc.RpcError as e:
            if not stop_event.is_set():
                print(f"[gRPC] RPC error: {e.code()} — {e.details()}", file=sys.stderr)
                stop_event.set()

    # ── Stats thread: print pipeline counters every 5 s ──────────────────────

    def stats_thread():
        while not stop_event.is_set():
            stop_event.wait(5.0)
            if stop_event.is_set():
                break
            print(
                f"[stats] mic_blocks={stats['in_blocks']}  "
                f"grpc_recv={stats['grpc_recv']}  "
                f"spk_blocks={stats['out_blocks']}  "
                f"underruns={stats['underruns']}  "
                f"raw_in_q={raw_in_q.qsize()}  "
                f"send_q={send_q.qsize()}  "
                f"recv_q={recv_q.qsize()}  "
                f"raw_out_q={raw_out_q.qsize()}"
            )

    # ── Start worker threads ──────────────────────────────────────────────────

    threads = [
        threading.Thread(target=resample_in_thread,  daemon=True, name="resample-in"),
        threading.Thread(target=resample_out_thread, daemon=True, name="resample-out"),
        threading.Thread(target=grpc_thread,         daemon=True, name="grpc"),
        threading.Thread(target=stats_thread,        daemon=True, name="stats"),
    ]
    for t in threads:
        t.start()

    # ── Open audio streams ────────────────────────────────────────────────────

    latency = args.latency
    try:
        latency = float(latency)
    except ValueError:
        pass

    try:
        with sd.InputStream(
            device=input_device,
            channels=in_channels,
            samplerate=device_rate,
            blocksize=block_dev,
            dtype="float32",
            latency=latency,
            callback=input_callback,
        ), sd.OutputStream(
            device=output_device,
            channels=out_channels,
            samplerate=device_rate,
            blocksize=block_dev,
            dtype="float32",
            latency=latency,
            callback=output_callback,
        ):
            while not stop_event.is_set():
                time.sleep(0.1)

    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        stop_event.set()
        for t in threads:
            t.join(timeout=2.0)


if __name__ == "__main__":
    main()
