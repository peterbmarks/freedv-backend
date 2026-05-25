//==========================================================================
// Name:            MinimalTxRxThreadTest.cpp
//
// Purpose:         End-to-end test of MinimalTxRxThread TX and RX paths.
//                  Pipes rade_src/wav/all.wav through a TX instance of
//                  MinimalTxRxThread, then pipes the encoded modem audio
//                  through an RX instance.  Feature files are captured via
//                  utTxFeatureFile / utRxFeatureFile and evaluated by
//                  rade_src/loss.py.  The test passes when loss.py prints
//                  "PASS".
//
// Created:         May 2026
// Authors:         Mooneer Salem
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//==========================================================================

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>

#include <unistd.h>

extern "C"
{
#include "fargan.h"
#include "lpcnet.h"
#include "fargan_config.h"
}
#include "rade_api.h"

#include "MinimalTxRxThread.h"
#include "MinimalRealTimeHelper.h"
#include "paCallbackData.h"
#include "pipeline_defines.h"
#include "rade_text.h"
#include "../util/GenericFIFO.h"
#include "../util/logging/ulog.h"

// ---------------------------------------------------------------------------
// Globals required by MinimalTxRxThread
// ---------------------------------------------------------------------------
std::atomic<bool> g_tx(false);
bool endingTx = false;
extern bool g_eoo_enqueued; // defined in MinimalTxRxThread.cpp

// ---------------------------------------------------------------------------
// Globals required by RADETransmitStep and RADEReceiveStep (unit-test hooks)
// ---------------------------------------------------------------------------
std::string utTxFeatureFile;
std::string utRxFeatureFile;

// ---------------------------------------------------------------------------
// Minimal WAV header parser – skips chunks until the "data" chunk is reached.
// Returns true on success; the file position is left at the first audio sample.
// ---------------------------------------------------------------------------
static bool skipWavHeader(FILE* f)
{
    auto read32 = [&](uint32_t& v) -> bool {
        return fread(&v, 4, 1, f) == 1;
    };

    char id[4];
    uint32_t size = 0;

    // Expect "RIFF"
    if (fread(id, 4, 1, f) != 1 || memcmp(id, "RIFF", 4) != 0) return false;
    if (!read32(size)) return false;  // overall file size
    if (fread(id, 4, 1, f) != 1 || memcmp(id, "WAVE", 4) != 0) return false;

    // Walk chunks until we find "data"
    while (true)
    {
        if (fread(id, 4, 1, f) != 1) return false;
        if (!read32(size))           return false;
        if (memcmp(id, "data", 4) == 0)
            return true;  // positioned at first sample
        if (fseek(f, (long)size, SEEK_CUR) != 0)
            return false;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // -----------------------------------------------------------------------
    // 1.  Create temporary files for TX and RX feature captures
    // -----------------------------------------------------------------------
    char txFeaturePath[] = "/tmp/MinimalTxRxTest_tx_XXXXXX";
    char rxFeaturePath[] = "/tmp/MinimalTxRxTest_rx_XXXXXX";

    int txFd = mkstemp(txFeaturePath);
    int rxFd = mkstemp(rxFeaturePath);
    assert(txFd >= 0 && rxFd >= 0);
    close(txFd);
    close(rxFd);

    utTxFeatureFile = txFeaturePath;
    utRxFeatureFile = rxFeaturePath;

    log_info("TX feature file: %s", txFeaturePath);
    log_info("RX feature file: %s", rxFeaturePath);

    // -----------------------------------------------------------------------
    // 2.  Initialise FARGAN / LPCNet / RADE
    // -----------------------------------------------------------------------
    FARGANState fargan;
    {
        float zeros[320]               = {0};
        float in_features[5 * NB_TOTAL_FEATURES] = {0};
        fargan_init(&fargan);
        fargan_cont(&fargan, zeros, in_features);
    }

    LPCNetEncState* encState = lpcnet_encoder_create();
    assert(encState != nullptr);

    char modelFile[1] = {0};
    rade_initialize();
    struct rade* rade = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER);
    assert(rade != nullptr);

    rade_text_t radeText = rade_text_create();
    assert(radeText != nullptr);

    // -----------------------------------------------------------------------
    // 3.  Build paCallBackData structs (TX and RX use separate structs to
    //     keep infifo/outfifo1 (TX) and infifo/outfifo2 (RX) independent)
    // -----------------------------------------------------------------------
    const int SPEECH_FIFO  = RADE_SPEECH_SAMPLE_RATE * 2; // 2 s at 16 kHz
    const int MODEM_FIFO   = RADE_MODEM_SAMPLE_RATE  * 4; // 4 s at  8 kHz
    const int DUMMY_FIFO   = 1024;

    // TX thread accesses infifo1 (input) and outfifo1 (output)
    paCallBackData txCbData;
    txCbData.infifo1  = new GenericFIFO<short>(SPEECH_FIFO);
    txCbData.outfifo1 = new GenericFIFO<short>(MODEM_FIFO);
    txCbData.infifo2  = new GenericFIFO<short>(DUMMY_FIFO);
    txCbData.outfifo2 = new GenericFIFO<short>(DUMMY_FIFO);

    // RX thread accesses infifo2 (input) and outfifo2 (output)
    paCallBackData rxCbData;
    rxCbData.infifo1  = new GenericFIFO<short>(DUMMY_FIFO);
    rxCbData.outfifo1 = new GenericFIFO<short>(DUMMY_FIFO);
    rxCbData.infifo2  = new GenericFIFO<short>(MODEM_FIFO);
    rxCbData.outfifo2 = new GenericFIFO<short>(SPEECH_FIFO);

    // -----------------------------------------------------------------------
    // 4.  Create TX and RX thread objects
    // -----------------------------------------------------------------------
    auto txHelper = std::make_shared<MinimalRealtimeHelper>();
    auto txThread = std::make_unique<MinimalTxRxThread>(
        true,                    // TX mode
        RADE_SPEECH_SAMPLE_RATE, // input  : 16 kHz speech from the WAV file
        RADE_MODEM_SAMPLE_RATE,  // output : 8 kHz modem waveform
        txHelper, rade, encState, &fargan, radeText, &txCbData);

    auto rxHelper = std::make_shared<MinimalRealtimeHelper>();
    auto rxThread = std::make_unique<MinimalTxRxThread>(
        false,                   // RX mode
        RADE_MODEM_SAMPLE_RATE,  // input  : 8 kHz modem waveform
        RADE_SPEECH_SAMPLE_RATE, // output : 16 kHz decoded speech
        rxHelper, rade, encState, &fargan, radeText, &rxCbData);

    // -----------------------------------------------------------------------
    // 5.  TX phase: g_tx = true
    //
    //     Both threads start; the TX thread processes speech→modem while the
    //     RX thread simply clears its FIFOs (because g_tx is true).
    // -----------------------------------------------------------------------
    g_tx.store(true, std::memory_order_release);
    endingTx       = false;
    g_eoo_enqueued = false;

    txThread->start();
    rxThread->start();
    txThread->waitForReady();
    rxThread->waitForReady();
    txThread->signalToStart();
    rxThread->signalToStart();

    // Give both threads time to finish their initial clearFifos_() before we
    // write audio data so it is not immediately wiped.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Open the WAV file supplied with rade_src
    const char* wavPath = RADE_SRC_DIR "/wav/all.wav";
    FILE* wavFile = fopen(wavPath, "rb");
    assert(wavFile != nullptr && "Could not open rade_src/wav/all.wav");
    assert(skipWavHeader(wavFile) && "Failed to parse WAV header");

    // Buffer that accumulates all modem samples produced by the TX thread
    std::vector<short> modemBuffer;
    modemBuffer.reserve(static_cast<size_t>(RADE_MODEM_SAMPLE_RATE) * 60);

    // Feed WAV samples to txCbData.infifo1 and drain txCbData.outfifo1
    const int SPEECH_CHUNK = RADE_SPEECH_SAMPLE_RATE * FRAME_DURATION_MS / MS_TO_SEC; // 320 samples = 20 ms
    std::vector<short> chunk(SPEECH_CHUNK);
    size_t samplesRead;

    while ((samplesRead = fread(chunk.data(), sizeof(short),
                                static_cast<size_t>(SPEECH_CHUNK), wavFile)) > 0)
    {
        size_t written = 0;
        while (written < samplesRead)
        {
            int space = txCbData.infifo1->numFree();
            if (space > 0)
            {
                int n = std::min(static_cast<int>(samplesRead - written), space);
                txCbData.infifo1->write(chunk.data() + written, n);
                written += static_cast<size_t>(n);
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Always drain the TX modem-output FIFO to prevent backpressure
            int avail = txCbData.outfifo1->numUsed();
            if (avail > 0)
            {
                size_t prev = modemBuffer.size();
                modemBuffer.resize(prev + static_cast<size_t>(avail));
                txCbData.outfifo1->read(modemBuffer.data() + prev, avail);
            }
        }
    }
    fclose(wavFile);
    wavFile = nullptr;

    // Signal end of TX so the thread sends the EOO burst
    endingTx = true;

    // Wait until the TX thread has enqueued the EOO marker, draining modem
    // output continuously to avoid FIFO back-pressure
    while (!g_eoo_enqueued)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int avail = txCbData.outfifo1->numUsed();
        if (avail > 0)
        {
            size_t prev = modemBuffer.size();
            modemBuffer.resize(prev + static_cast<size_t>(avail));
            txCbData.outfifo1->read(modemBuffer.data() + prev, avail);
        }
    }

    // Final drain: allow a short window for any last modem samples to land
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        int avail = txCbData.outfifo1->numUsed();
        if (avail > 0)
        {
            size_t prev = modemBuffer.size();
            modemBuffer.resize(prev + static_cast<size_t>(avail));
            txCbData.outfifo1->read(modemBuffer.data() + prev, avail);
        }
    }

    log_info("TX done: %zu modem samples collected", modemBuffer.size());

    // Switch to RX phase and stop the TX thread.  Stopping TX first ensures
    // the TX feature file is fully flushed before loss.py runs.
    g_tx.store(false, std::memory_order_release);
    txThread->stop();

    // -----------------------------------------------------------------------
    // 6.  RX phase: g_tx = false
    //
    //     Feed the collected modem samples into rxCbData.infifo2 in chunks
    //     while draining rxCbData.outfifo2 to prevent back-pressure.
    // -----------------------------------------------------------------------
    const int MODEM_CHUNK = RADE_MODEM_SAMPLE_RATE * FRAME_DURATION_MS / MS_TO_SEC; // 160 samples = 20 ms
    const size_t totalModem = modemBuffer.size();
    size_t pos = 0;

    while (pos < totalModem)
    {
        int space = rxCbData.infifo2->numFree();
        if (space > 0)
        {
            int n = std::min({static_cast<int>(totalModem - pos), space, MODEM_CHUNK});
            rxCbData.infifo2->write(modemBuffer.data() + pos, n);
            pos += static_cast<size_t>(n);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Drain decoded speech output so the RX thread is never blocked
        int avail = rxCbData.outfifo2->numUsed();
        if (avail > 0)
        {
            std::vector<short> discard(static_cast<size_t>(avail));
            rxCbData.outfifo2->read(discard.data(), avail);
        }
    }

    log_info("All modem samples fed to RX");

    // Wait for the RX thread to drain infifo2
    auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (rxCbData.infifo2->numUsed() > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Feed silence to ensure we finish processing all encoded output
        short temp[MODEM_CHUNK];
        memset(temp, 0, sizeof(short) * MODEM_CHUNK);
        rxCbData.infifo2->write(temp, MODEM_CHUNK);
        
        // Keep draining outfifo2 so the RX thread never stalls
        int avail = rxCbData.outfifo2->numUsed();
        if (avail > 0)
        {
            std::vector<short> discard(static_cast<size_t>(avail));
            rxCbData.outfifo2->read(discard.data(), avail);
        }
        else
        {
            break;
        }

        if (std::chrono::steady_clock::now() > drainDeadline)
        {
            log_error("Timeout waiting for RX to drain infifo2");
            break;
        }
    }

    // Give the RX pipeline a moment to finish its last frame
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop the RX thread; this also flushes and closes the RX feature file
    rxThread->stop();

    // -----------------------------------------------------------------------
    // 7.  Cleanup shared resources
    // -----------------------------------------------------------------------
    delete txCbData.infifo1;  delete txCbData.outfifo1;
    delete txCbData.infifo2;  delete txCbData.outfifo2;
    delete rxCbData.infifo1;  delete rxCbData.outfifo1;
    delete rxCbData.infifo2;  delete rxCbData.outfifo2;

    rade_text_destroy(radeText);
    rade_close(rade);
    rade_finalize();
    lpcnet_encoder_destroy(encState);

    // -----------------------------------------------------------------------
    // 8.  Run loss.py and determine pass / fail
    // -----------------------------------------------------------------------
    // Build command: PYTHONPATH includes rade_src so the 'radae' package is
    // found, then invoke the interpreter on loss.py.
    std::string lossCmd =
        std::string("PYTHONPATH=") + RADE_SRC_DIR + " " +
        PYTHON_EXECUTABLE + " " +
        RADE_SRC_DIR + "/loss.py " +
        txFeaturePath + " " +
        rxFeaturePath +
        " --loss_test 0.15 2>&1";

    log_info("Running: %s", lossCmd.c_str());

    FILE* pipe = popen(lossCmd.c_str(), "r");
    if (pipe == nullptr)
    {
        log_error("popen() failed – cannot run loss.py");
        unlink(txFeaturePath);
        unlink(rxFeaturePath);
        return 1;
    }

    std::string lossOutput;
    {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe) != nullptr)
        {
            lossOutput += buf;
            // Echo to stdout so CTest captures it
            fputs(buf, stdout);
        }
    }
    pclose(pipe);

    // Cleanup temp files
    unlink(txFeaturePath);
    unlink(rxFeaturePath);

    // loss.py always exits with code 0; parse its stdout for PASS / FAIL
    bool passed = lossOutput.find("PASS") != std::string::npos &&
                  lossOutput.find("FAIL") == std::string::npos;

    if (passed)
    {
        log_info("MinimalTxRxThreadTest: PASS");
        return 0;
    }
    else
    {
        log_error("MinimalTxRxThreadTest: FAIL");
        return 1;
    }
}
