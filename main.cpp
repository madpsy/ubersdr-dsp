/*  ubersdr-dsp — standalone noise reduction pipeline
 *
 *  Reads raw 24 kHz stereo float32 PCM from stdin,
 *  applies the selected noise reduction filter,
 *  writes raw 24 kHz stereo float32 PCM to stdout.
 *
 *  Usage:
 *    ubersdr-dsp --filter <nr2|rn2|nr4|dfnr|bnr> [options]
 *
 *  Common options:
 *    --filter  <name>     Noise reduction algorithm (required)
 *    --block   <samples>  Stereo frames per read (default: 960 = 40 ms at 24 kHz)
 *    --wisdom  <path>     FFTW3 wisdom file or directory (NR2 only)
 *
 *  NR2 (SpectralNR) options:
 *    --gain-method  <0-3>   0=Linear 1=Log 2=Gamma(default) 3=Trained
 *    --npe-method   <0-2>   0=OSMS(default) 1=MMSE 2=NSTAT
 *    --gain-max     <float> Max gain cap (default: 1.0)
 *    --gain-smooth  <float> Temporal smoothing (default: 0.85)
 *    --qspp         <float> Speech presence prior (default: 0.2)
 *    --no-ae                Disable artifact elimination
 *
 *  NR4 (SpecbleachFilter) options:
 *    --reduction   <dB>    Reduction amount 0-40 dB (default: 10)
 *    --smoothing   <pct>   Smoothing factor 0-100% (default: 0)
 *    --whitening   <pct>   Whitening factor 0-100% (default: 0)
 *    --no-adaptive         Disable adaptive noise tracking
 *    --noise-method <0-2>  0=SPP-MMSE 1=Brandt 2=Martin (default: 0)
 *
 *  DFNR (DeepFilterFilter) options:
 *    --model       <path>  Path to DeepFilterNet3_onnx.tar.gz
 *    --atten-limit <dB>    Attenuation limit 0-100 dB (default: 100)
 *    --pf-beta     <float> Post-filter beta 0-0.3 (default: 0)
 *
 *  BNR (NvidiaBnrFilter) options:
 *    --bnr-address <host:port>  gRPC server address (default: localhost:8001)
 *    --intensity   <float>      Intensity ratio 0-1 (default: 1.0)
 *
 *  RN2 (RNNoiseFilter) has no tunable parameters.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "SpectralNR.h"
#include "RNNoiseFilter.h"

#ifdef HAVE_SPECBLEACH
#  include "SpecbleachFilter.h"
#endif

#ifdef HAVE_DFNR
#  include "DeepFilterFilter.h"
#endif

#include "NvidiaBnrFilter.h"

// ── Argument helpers ──────────────────────────────────────────────────────────

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s --filter <nr2|rn2|nr4|dfnr|bnr> [options]\n"
        "\n"
        "Input/output: raw 24 kHz stereo float32 PCM on stdin/stdout\n"
        "\n"
        "Common:\n"
        "  --filter  <name>     nr2 | rn2 | nr4 | dfnr | bnr\n"
        "  --block   <samples>  Stereo frames per read (default: 960)\n"
        "  --wisdom  <path>     FFTW3 wisdom file/dir (NR2 only)\n"
        "\n"
        "NR2:\n"
        "  --gain-method <0-3>  0=Linear 1=Log 2=Gamma 3=Trained (default: 2)\n"
        "  --npe-method  <0-2>  0=OSMS 1=MMSE 2=NSTAT (default: 0)\n"
        "  --gain-max    <f>    Max gain cap (default: 1.0)\n"
        "  --gain-smooth <f>    Temporal smoothing (default: 0.85)\n"
        "  --qspp        <f>    Speech presence prior (default: 0.2)\n"
        "  --no-ae              Disable artifact elimination\n"
        "\n"
        "NR4:\n"
        "  --reduction   <dB>   0-40 dB (default: 10)\n"
        "  --smoothing   <pct>  0-100%% (default: 0)\n"
        "  --whitening   <pct>  0-100%% (default: 0)\n"
        "  --no-adaptive        Disable adaptive noise tracking\n"
        "  --noise-method <0-2> 0=SPP-MMSE 1=Brandt 2=Martin (default: 0)\n"
        "\n"
        "DFNR:\n"
        "  --model       <path> Path to DeepFilterNet3_onnx.tar.gz\n"
        "  --atten-limit <dB>   0-100 dB (default: 100)\n"
        "  --pf-beta     <f>    Post-filter beta (default: 0)\n"
        "\n"
        "BNR:\n"
        "  --bnr-address <h:p>  gRPC server (default: localhost:8001)\n"
        "  --intensity   <f>    0-1 (default: 1.0)\n",
        prog);
}

static const char* getArg(int argc, char** argv, const char* flag, const char* def = nullptr)
{
    for (int i = 1; i < argc - 1; ++i)
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return def;
}

static bool hasFlag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

static float getFloat(int argc, char** argv, const char* flag, float def)
{
    const char* v = getArg(argc, argv, flag);
    return v ? static_cast<float>(atof(v)) : def;
}

static int getInt(int argc, char** argv, const char* flag, int def)
{
    const char* v = getArg(argc, argv, flag);
    return v ? atoi(v) : def;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    if (argc < 2 || hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        usage(argv[0]);
        return 1;
    }

    const char* filterName = getArg(argc, argv, "--filter");
    if (!filterName) {
        fprintf(stderr, "Error: --filter is required\n\n");
        usage(argv[0]);
        return 1;
    }

    const int blockFrames = getInt(argc, argv, "--block", 960); // stereo frames
    if (blockFrames <= 0) {
        fprintf(stderr, "Error: --block must be > 0\n");
        return 1;
    }

    // Input/output buffers: stereo float32
    const int stereoSamples = blockFrames * 2;
    std::vector<float> inBuf(stereoSamples);
    std::vector<float> outBuf;

    // ── Set stdin/stdout to binary mode (important on Windows) ───────────────
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // ── NR2 ──────────────────────────────────────────────────────────────────
    if (strcmp(filterName, "nr2") == 0) {
        const char* wisdomPath = getArg(argc, argv, "--wisdom");
        if (wisdomPath) {
            if (!AetherSDR::SpectralNR::loadWisdom(wisdomPath))
                fprintf(stderr, "NR2: wisdom not loaded from '%s' — using FFTW_MEASURE\n", wisdomPath);
            else
                fprintf(stderr, "NR2: wisdom loaded from '%s'\n", wisdomPath);
        }

        AetherSDR::SpectralNR filter;
        filter.setGainMethod(getInt(argc, argv, "--gain-method", 2));
        filter.setNpeMethod (getInt(argc, argv, "--npe-method",  0));
        filter.setGainMax   (getFloat(argc, argv, "--gain-max",    1.0f));
        filter.setGainSmooth(getFloat(argc, argv, "--gain-smooth", 0.85f));
        filter.setQspp      (getFloat(argc, argv, "--qspp",        0.2f));
        filter.setAeFilter  (!hasFlag(argc, argv, "--no-ae"));

        fprintf(stderr, "NR2: gain-method=%d npe-method=%d gain-max=%.2f "
                "gain-smooth=%.2f qspp=%.2f ae=%s\n",
                filter.gainMethod(), filter.npeMethod(),
                (float)filter.gainMax(), (float)filter.gainSmooth(),
                (float)filter.qspp(),
                filter.aeFilter() ? "on" : "off");

        // NR2 processes mono float; we average stereo→mono in, duplicate mono→stereo out
        std::vector<float> monoIn(blockFrames), monoOut(blockFrames);

        while (fread(inBuf.data(), sizeof(float), stereoSamples, stdin) == (size_t)stereoSamples) {
            // Stereo → mono
            for (int i = 0; i < blockFrames; ++i)
                monoIn[i] = (inBuf[i * 2] + inBuf[i * 2 + 1]) * 0.5f;

            filter.process(monoIn.data(), monoOut.data(), blockFrames);

            // Mono → stereo
            for (int i = 0; i < blockFrames; ++i) {
                inBuf[i * 2]     = monoOut[i];
                inBuf[i * 2 + 1] = monoOut[i];
            }

            if (fwrite(inBuf.data(), sizeof(float), stereoSamples, stdout) != (size_t)stereoSamples)
                break;
        }
        return 0;
    }

    // ── RN2 ──────────────────────────────────────────────────────────────────
    if (strcmp(filterName, "rn2") == 0) {
        AetherSDR::RNNoiseFilter filter;
        if (!filter.isValid()) {
            fprintf(stderr, "RN2: rnnoise_create() failed\n");
            return 1;
        }
        fprintf(stderr, "RN2: ready\n");

        while (fread(inBuf.data(), sizeof(float), stereoSamples, stdin) == (size_t)stereoSamples) {
            outBuf = filter.process(inBuf.data(), blockFrames);
            if (fwrite(outBuf.data(), sizeof(float), outBuf.size(), stdout) != outBuf.size())
                break;
        }
        return 0;
    }

    // ── NR4 ──────────────────────────────────────────────────────────────────
    if (strcmp(filterName, "nr4") == 0) {
#ifndef HAVE_SPECBLEACH
        fprintf(stderr, "NR4: not compiled in (rebuild with -DHAVE_SPECBLEACH)\n");
        return 1;
#else
        AetherSDR::SpecbleachFilter filter;
        if (!filter.isValid()) {
            fprintf(stderr, "NR4: specbleach_initialize() failed\n");
            return 1;
        }
        filter.setReductionAmount    (getFloat(argc, argv, "--reduction",    10.0f));
        filter.setSmoothingFactor    (getFloat(argc, argv, "--smoothing",     0.0f));
        filter.setWhiteningFactor    (getFloat(argc, argv, "--whitening",     0.0f));
        filter.setAdaptiveNoise      (!hasFlag(argc, argv, "--no-adaptive"));
        filter.setNoiseEstimationMethod(getInt(argc, argv, "--noise-method",  0));

        fprintf(stderr, "NR4: reduction=%.1f dB smoothing=%.1f%% whitening=%.1f%% "
                "adaptive=%s noise-method=%d\n",
                filter.reductionAmount(), filter.smoothingFactor(),
                filter.whiteningFactor(),
                !hasFlag(argc, argv, "--no-adaptive") ? "on" : "off",
                getInt(argc, argv, "--noise-method", 0));

        while (fread(inBuf.data(), sizeof(float), stereoSamples, stdin) == (size_t)stereoSamples) {
            outBuf = filter.process(inBuf.data(), blockFrames);
            if (fwrite(outBuf.data(), sizeof(float), outBuf.size(), stdout) != outBuf.size())
                break;
        }
        return 0;
#endif
    }

    // ── DFNR ─────────────────────────────────────────────────────────────────
    if (strcmp(filterName, "dfnr") == 0) {
#ifndef HAVE_DFNR
        fprintf(stderr, "DFNR: not compiled in (rebuild with -DHAVE_DFNR)\n");
        return 1;
#else
        const char* modelPath = getArg(argc, argv, "--model", "");
        AetherSDR::DeepFilterFilter filter(modelPath ? modelPath : "");
        if (!filter.isValid()) {
            fprintf(stderr, "DFNR: failed to load model\n");
            return 1;
        }
        filter.setAttenLimit    (getFloat(argc, argv, "--atten-limit", 100.0f));
        filter.setPostFilterBeta(getFloat(argc, argv, "--pf-beta",       0.0f));

        fprintf(stderr, "DFNR: atten-limit=%.1f dB pf-beta=%.3f\n",
                filter.attenLimit(), filter.postFilterBeta());

        while (fread(inBuf.data(), sizeof(float), stereoSamples, stdin) == (size_t)stereoSamples) {
            outBuf = filter.process(inBuf.data(), blockFrames);
            if (fwrite(outBuf.data(), sizeof(float), outBuf.size(), stdout) != outBuf.size())
                break;
        }
        return 0;
#endif
    }

    // ── BNR ──────────────────────────────────────────────────────────────────
    if (strcmp(filterName, "bnr") == 0) {
#ifndef HAVE_BNR
        fprintf(stderr, "BNR: not compiled in (rebuild with -DHAVE_BNR)\n");
        return 1;
#else
        const char* address = getArg(argc, argv, "--bnr-address", "localhost:8001");
        float intensity = getFloat(argc, argv, "--intensity", 1.0f);

        AetherSDR::NvidiaBnrFilter filter;
        filter.setIntensityRatio(intensity);
        filter.onError = [](const std::string& msg) {
            fprintf(stderr, "BNR error: %s\n", msg.c_str());
        };
        filter.onConnectionChanged = [](bool connected) {
            fprintf(stderr, "BNR: %s\n", connected ? "connected" : "disconnected");
        };

        if (!filter.connectToServer(address)) {
            fprintf(stderr, "BNR: failed to connect to %s\n", address);
            return 1;
        }

        // BNR operates at 48 kHz mono internally; the filter handles its own
        // buffering. We feed 24 kHz stereo float32 and get back whatever is
        // available (non-blocking). Accumulate output to match input size.
        std::vector<float> accumulated;

        while (fread(inBuf.data(), sizeof(float), stereoSamples, stdin) == (size_t)stereoSamples) {
            // Downmix stereo → mono for BNR (it expects 48kHz mono, but we
            // pass 24kHz stereo mono-mixed; the NIM server handles sample rate)
            std::vector<float> mono(blockFrames);
            for (int i = 0; i < blockFrames; ++i)
                mono[i] = (inBuf[i * 2] + inBuf[i * 2 + 1]) * 0.5f;

            auto chunk = filter.process(mono.data(), blockFrames);
            accumulated.insert(accumulated.end(), chunk.begin(), chunk.end());

            // Emit output when we have enough (duplicate mono → stereo)
            while (static_cast<int>(accumulated.size()) >= blockFrames) {
                std::vector<float> stereoOut(stereoSamples);
                for (int i = 0; i < blockFrames; ++i) {
                    stereoOut[i * 2]     = accumulated[i];
                    stereoOut[i * 2 + 1] = accumulated[i];
                }
                accumulated.erase(accumulated.begin(), accumulated.begin() + blockFrames);
                if (fwrite(stereoOut.data(), sizeof(float), stereoSamples, stdout) != (size_t)stereoSamples)
                    goto done;
            }
        }
        done:
        filter.disconnect();
        return 0;
#endif
    }

    fprintf(stderr, "Error: unknown filter '%s'\n\n", filterName);
    usage(argv[0]);
    return 1;
}
