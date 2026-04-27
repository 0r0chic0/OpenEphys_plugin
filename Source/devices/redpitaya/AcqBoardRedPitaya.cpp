/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI

    ------------------------------------------------------------------
*/

#include "AcqBoardRedPitaya.h"
#include <cstring>

AcqBoardRedPitaya::AcqBoardRedPitaya()
    : AcquisitionBoard()
{
    boardType = BoardType::RedPitaya;

    // Default sample rate; can be overridden with setSampleRate().
    settings.boardSampleRate = 1000.0f;

    // By default, expose ADC channels only.
    acquireAdc = true;
    acquireAux = false;
}

AcqBoardRedPitaya::~AcqBoardRedPitaya()
{
    stopAcquisition();
}
/*
bool AcqBoardRedPitaya::detectBoard()
{
    std::cout << "detectBoard called" << std::endl;

    deviceFound = false;
    StreamingSocket socket;

    // Use a slightly larger timeout for discovery since the RP might be scanning AXI
    if (! socket.connect ("rp-f0cd35.local", 5000, 1000))
    {
        std::cout << "connect failed" << std::endl;
        return false;
    }

    std::cout << "connected" << std::endl;

    const char* msg = "REDPITAYA\n";
    socket.write (msg, (int) strlen (msg));

    if (! socket.waitUntilReady (true, 500))
    {
        std::cout << "no reply" << std::endl;
        socket.close();
        return false;
    }

    // Increased buffer size to catch "OK CHANNELS:XX"
    char buffer[64] = { 0 };
    int n = socket.read (buffer, sizeof (buffer) - 1, false);

    if (n > 0)
    {
        buffer[n] = '\0';
        String response (buffer);
        std::cout << "Received response: " << response << std::endl;

        // Check for the "OK" flag
        if (response.contains ("OK"))
        {
            // Parse the channel count
            if (response.contains ("CHANNELS:"))
            {
                int startIdx = response.indexOf ("CHANNELS:") + 9;

                // substring(startIdx) gets everything after the colon
                // getIntValue() stops parsing at the newline/space
                this->numAdcChannels = response.substring (startIdx).getIntValue();

                std::cout << "Detected Red Pitaya with " << numAdcChannels << " channels." << std::endl;
                deviceFound = true;
            }
            else
            {
                // Fallback for legacy code if needed
                this->numAdcChannels = 6;
                deviceFound = true;
            }
        }
    }

    socket.close();
    return deviceFound;
} */

bool AcqBoardRedPitaya::detectBoard()
{
    std::cout << "detectBoard called" << std::endl;

    deviceFound = false;

    if (commandSocket != nullptr && ! commandSocket->isConnected())
    {
        delete commandSocket;
        commandSocket = nullptr;
    }

    if (commandSocket == nullptr)
        commandSocket = new StreamingSocket();

    if (! commandSocket->connect ("rp-f0cd35.local", 5000, 1000))
    {
        std::cout << "connect failed" << std::endl;
        delete commandSocket;
        commandSocket = nullptr;
        return false;
    }

    std::cout << "connected" << std::endl;

    const char* msg = "REDPITAYA\n";
    commandSocket->write (msg, (int) strlen (msg));

    if (! commandSocket->waitUntilReady (true, 500))
    {
        std::cout << "no reply" << std::endl;
        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
        return false;
    }

    // Increased buffer size to catch "OK CHANNELS:XX"
    char buffer[64] = { 0 };
    int n = commandSocket->read (buffer, sizeof (buffer) - 1, false);

    if (n > 0)
    {
        buffer[n] = '\0';
        String response (buffer);
        std::cout << "Received response: " << response << std::endl;

        // Check for the "OK" flag
        if (response.contains ("OK"))
        {
            // Parse the channel count
            if (response.contains ("CHANNELS:"))
            {
                int startIdx = response.indexOf ("CHANNELS:") + 9;

                this->numAdcChannels = response.substring (startIdx).getIntValue();

                std::cout << "Detected Red Pitaya with " << numAdcChannels << " channels." << std::endl;
                deviceFound = true;
            }
            else
            {
                // Fallback for legacy code if needed
                this->numAdcChannels = 6;
                deviceFound = true;
            }
        }
    }
    if (! deviceFound)
    {
        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
    }

    return deviceFound;
}

bool AcqBoardRedPitaya::initializeBoard()
{
    if (! deviceFound)
        return false;

    // Any Red Pitaya-specific configuration (e.g. network setup) can be
    // performed here. At this stage we simply consider the device ready.

    return true;
}

bool AcqBoardRedPitaya::foundInputSource() const
{
    return deviceFound;
}

Array<const Headstage*> AcqBoardRedPitaya::getHeadstages()
{
    // Red Pitaya is modeled as an ADC-only device with no headstages.
    Array<const Headstage*> none;
    return none;
}

Array<int> AcqBoardRedPitaya::getAvailableSampleRates()
{
    Array<int> sampleRates;

    // Populate with a simple set of options; these can be adapted
    // to match the Red Pitaya configuration.
    sampleRates.add (1000);
    sampleRates.add (2000);
    sampleRates.add (5000);
    sampleRates.add (10000);

    return sampleRates;
}

void AcqBoardRedPitaya::setSampleRate (int sampleRateHz)
{
    settings.boardSampleRate = static_cast<float> (sampleRateHz);
}

float AcqBoardRedPitaya::getSampleRate() const
{
    return settings.boardSampleRate;
}

void AcqBoardRedPitaya::scanPorts()
{
    // No-op: there are no headstages to scan for Red Pitaya.
}

void AcqBoardRedPitaya::enableAuxChannels (bool enabled)
{
    acquireAux = enabled;
}

bool AcqBoardRedPitaya::areAuxChannelsEnabled() const
{
    return acquireAux;
}

void AcqBoardRedPitaya::enableAdcChannels (bool enabled)
{
    acquireAdc = enabled;
}

bool AcqBoardRedPitaya::areAdcChannelsEnabled() const
{
    return acquireAdc;
}

float AcqBoardRedPitaya::getBitVolts (ContinuousChannel::Type channelType) const
{
    if (channelType == ContinuousChannel::ADC)
        return adcBitVolts;

    return 1.0f;
}

void AcqBoardRedPitaya::measureImpedances()
{
}

void AcqBoardRedPitaya::impedanceMeasurementFinished()
{
}

void AcqBoardRedPitaya::saveImpedances (File& /*file*/)
{
}

void AcqBoardRedPitaya::setNamingScheme (ChannelNamingScheme scheme)
{
    channelNamingScheme = scheme;
}

ChannelNamingScheme AcqBoardRedPitaya::getNamingScheme()
{
    return channelNamingScheme;
}

bool AcqBoardRedPitaya::isReady()
{
    return deviceFound;
}

bool AcqBoardRedPitaya::startAcquisition()
{
    if (! deviceFound)
        return false;

    if (commandSocket != nullptr && ! commandSocket->isConnected())
    {
        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
    }

    if (commandSocket == nullptr)
    {
        commandSocket = new StreamingSocket();
        if (! commandSocket->connect ("rp-f0cd35.local", 5000, 1000))
        {
            std::cout << "Red Pitaya ERROR: Could not connect to board." << std::endl;
            delete commandSocket;
            commandSocket = nullptr;
            return false;
        }
    }

    const char* msg = "START\n";
    commandSocket->write (msg, (int) strlen (msg));

    startThread();
    return true;
}

bool AcqBoardRedPitaya::stopAcquisition()
{
    /*
    if (commandSocket != nullptr)
    {
        const char* msg = "STOP\n";
        commandSocket->write (msg, (int) strlen (msg));

        commandSocket->waitUntilReady (true, 100);

        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
    }

    if (isThreadRunning())
        signalThreadShouldExit();

    if (buffer != nullptr)
        buffer->clear();

    return true; */
        if (commandSocket != nullptr && commandSocket->isConnected())
        {
            // 1. Send the STOP command to the Red Pitaya
            const char* msg = "STOP\n";
            commandSocket->write (msg, (int) strlen (msg));

            // 2. Wait 50ms for the board to exit run_stream and the final packets to arrive
            juce::Thread::sleep (50);

            char trash[1024];
            int flushCount = 0;
            while (commandSocket->waitUntilReady (true, 0) == 1 && flushCount < 100)
            {
                commandSocket->read (trash, sizeof (trash), false);
                flushCount++;
            }

            if (flushCount > 0)
            {
                std::cout << "Cleared " << flushCount << " leftover packets during stop." << std::endl;
            }
        }

        if (isThreadRunning())
            signalThreadShouldExit();

        if (buffer != nullptr)
            buffer->clear();

        return true;
}
bool AcqBoardRedPitaya::sendRecordOnCommand()
{
    if (commandSocket == nullptr)
        return false;

    const char* msg = "RECORD ON\n";

    int written = commandSocket->write (msg, (int) strlen (msg));

    if (written <= 0)
        return false;

    return true;
}

bool AcqBoardRedPitaya::sendRecordOffCommand()
{
    if (commandSocket == nullptr)
        return false;

    const char* msg = "RECORD OFF\n";

    int written = commandSocket->write (msg, (int) strlen (msg));

    if (written <= 0)
        return false;

    return true;
}

void AcqBoardRedPitaya::updateSampleFrequency (int newFreq)
{
    if (! deviceFound)
        return;

    // 1. SELF-HEALING: Trash dead sockets
    if (commandSocket != nullptr && ! commandSocket->isConnected())
    {
        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
    }

    // 2. SELF-HEALING: Build a new connection if needed
    if (commandSocket == nullptr)
    {
        commandSocket = new StreamingSocket();

        // Connect (consider swapping to direct IP like "192.168.x.x" if mDNS lags)
        if (! commandSocket->connect ("rp-f0cd35.local", 5000, 1000))
        {
            std::cout << "Red Pitaya ERROR: Could not connect to board." << std::endl;
            delete commandSocket;
            commandSocket = nullptr;
            return;
        }
    }

    // 3. FIRE AND FORGET
    char msg[32];
    snprintf (msg, sizeof (msg), "FREQ:%d\n", newFreq);

    int written = commandSocket->write (msg, (int) strlen (msg));

    if (written > 0)
    {
        std::cout << "Red Pitaya: Sent command -> " << msg;
    }
    else
    {
        std::cout << "Red Pitaya Backend ERROR: Socket write failed." << std::endl;
        commandSocket->close();
    }
}

double AcqBoardRedPitaya::setUpperBandwidth (double upperBandwidth)
{
    upperBandwidthHz = upperBandwidth;
    return upperBandwidthHz;
}

double AcqBoardRedPitaya::setLowerBandwidth (double lowerBandwidth)
{
    lowerBandwidthHz = lowerBandwidth;
    return lowerBandwidthHz;
}

double AcqBoardRedPitaya::setDspCutoffFreq (double freq)
{
    dspCutoffFreqHz = freq;
    return dspCutoffFreqHz;
}

double AcqBoardRedPitaya::getDspCutoffFreq() const
{
    return dspCutoffFreqHz;
}

void AcqBoardRedPitaya::setDspOffset (bool /*enabled*/)
{
}

void AcqBoardRedPitaya::setTTLOutputMode (bool enabled)
{
    ttlOutputMode = enabled;
}

void AcqBoardRedPitaya::setDAChpf (float /*cutoff*/, bool /*enabled*/)
{
}

void AcqBoardRedPitaya::setFastTTLSettle (bool /*state*/, int /*channel*/)
{
}

int AcqBoardRedPitaya::setNoiseSlicerLevel (int level)
{
    return level;
}

void AcqBoardRedPitaya::enableBoardLeds (bool /*enabled*/)
{
}

int AcqBoardRedPitaya::setClockDivider (int divide_ratio)
{
    clockDivideRatio = divide_ratio;
    return clockDivideRatio;
}

void AcqBoardRedPitaya::connectHeadstageChannelToDAC (int /*headstageChannelIndex*/, int /*dacChannelIndex*/)
{
}

void AcqBoardRedPitaya::setDACTriggerThreshold (int /*dacChannelIndex*/, float /*threshold*/)
{
}

bool AcqBoardRedPitaya::isHeadstageEnabled (int /*hsNum*/) const
{
    return false;
}

int AcqBoardRedPitaya::getActiveChannelsInHeadstage (int /*hsNum*/) const
{
    return 0;
}

int AcqBoardRedPitaya::getChannelsInHeadstage (int /*hsNum*/) const
{
    return 0;
}

int AcqBoardRedPitaya::getNumDataOutputs (ContinuousChannel::Type channelType)
{
    if (channelType == ContinuousChannel::ELECTRODE)
        return 0;

    if (channelType == ContinuousChannel::AUX)
    {
        if (acquireAux)
            return 0;

        return 0;
    }

    if (channelType == ContinuousChannel::ADC)
    {
        if (acquireAdc)
            return numAdcChannels; // The DeviceThread assumes up to 6 ADC channels.

        return 0;
    }

    return 0;
}

void AcqBoardRedPitaya::setNumHeadstageChannels (int /*headstageIndex*/, int /*channelCount*/)
{
}

void AcqBoardRedPitaya::run()
{
    if (commandSocket == nullptr)
        return;

    int64 sampleNumber = 0;
    const int64 samplesPerBuffer = int64 (settings.boardSampleRate / 1000.0);
    uint64 eventCode = 0;

    const int numAdcChannelsLocal = getNumDataOutputs (ContinuousChannel::ADC);

    if (numAdcChannelsLocal <= 0 || samplesPerBuffer <= 0)
        return;

    const int headerSize = 22;
    const int payloadSize = numAdcChannelsLocal * 2;
    const int packetSize = headerSize + payloadSize;

    constexpr int MAX_PACKET_SIZE = 1024;
    uint8_t packet[MAX_PACKET_SIZE];

    bool synchronized = false;
    uint8_t syncBuffer[headerSize];
    int bytesSearched = 0;

    while (! synchronized && ! threadShouldExit())
    {
        int n = commandSocket->read (syncBuffer + bytesSearched, 1, true);
        if (n <= 0)
            return;
        bytesSearched++;

        if (bytesSearched == headerSize)
        {
            if (syncBuffer[8] == 0x03 && syncBuffer[9] == 0x00)
            {
                memcpy (packet, syncBuffer, headerSize);

                int pRead = 0;
                while (pRead < payloadSize && ! threadShouldExit())
                {
                    int pn = commandSocket->read (packet + headerSize + pRead, payloadSize - pRead, true);
                    if (pn <= 0)
                        return;
                    pRead += pn;
                }
                synchronized = true;
            }
            else
            {
                memmove (syncBuffer, syncBuffer + 1, headerSize - 1);
                bytesSearched--;
            }
        }
    }

    while (! threadShouldExit())
    {
        for (int sampleIndex = 0; sampleIndex < samplesPerBuffer; ++sampleIndex)
        {
            if (sampleIndex > 0 || ! synchronized)
            {
                int bytesRead = 0;
                while (bytesRead < packetSize && ! threadShouldExit())
                {
                    const int n = commandSocket->read (packet + bytesRead, packetSize - bytesRead, true);
                    if (n <= 0)
                        return;
                    bytesRead += n;
                }
            }

            synchronized = false;

            const int16_t* channels = reinterpret_cast<const int16_t*> (packet + headerSize);

            int ch = 0;
            for (int adc = 0; adc < numAdcChannelsLocal; ++adc)
            {
                samples[(ch * samplesPerBuffer) + sampleIndex] = float (channels[adc]);
                ++ch;
            }

            const double timeSeconds = double (sampleNumber) / double (settings.boardSampleRate);
            sampleNumbers[sampleIndex] = sampleNumber;
            timestamps[sampleIndex] = timeSeconds;
            event_codes[sampleIndex] = eventCode;

            ++sampleNumber;
        }

        buffer->addToBuffer (samples,
                             sampleNumbers,
                             timestamps,
                             event_codes,
                             (int) samplesPerBuffer);
    }
}
