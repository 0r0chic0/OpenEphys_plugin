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

bool AcqBoardRedPitaya::detectBoard()
{
    std::cout << "detectBoard called" << std::endl;

    deviceFound = false;
    StreamingSocket socket;

    if (! socket.connect ("rp-f0cd35.local", 5000, 500))
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

    char buffer[16] = { 0 };
    int n = socket.read (buffer, sizeof (buffer) - 1, false);

    std::cout << "read bytes: " << n << std::endl;

    if (n > 0 && String (buffer).trim() == "OK")
        deviceFound = true;

    socket.close();
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

    if (commandSocket == nullptr)
        commandSocket = new StreamingSocket();

    if (! commandSocket->connect ("rp-f0cd35.local", 5000, 1000))
        return false;

    const char* msg = "START\n";
    commandSocket->write (msg, (int) strlen (msg));

    startThread();
    return true;
}

bool AcqBoardRedPitaya::stopAcquisition()
{
    if (commandSocket != nullptr)
    {
        const char* msg = "STOP\n";
        commandSocket->write (msg, (int) strlen (msg));
        commandSocket->close();
        delete commandSocket;
        commandSocket = nullptr;
    }

    if (isThreadRunning())
        signalThreadShouldExit();

    buffer->clear();
    return true;
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
            return 6; // The DeviceThread assumes up to 6 ADC channels.

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

    const int numHeadstageChannels = getNumDataOutputs (ContinuousChannel::ELECTRODE);
    const int numAuxChannels = getNumDataOutputs (ContinuousChannel::AUX);
    const int numAdcChannelsLocal = getNumDataOutputs (ContinuousChannel::ADC);

    const int totalChannels = numHeadstageChannels + numAuxChannels + numAdcChannelsLocal;

    if (totalChannels <= 0 || samplesPerBuffer <= 0)
        return;

    constexpr int headerSize = 22;
    constexpr int payloadSize = 6 * 2; // 6 int16 channels
    constexpr int packetSize = headerSize + payloadSize;

    uint8_t packet[packetSize];

    while (! threadShouldExit())
    {
        for (int sampleIndex = 0; sampleIndex < samplesPerBuffer; ++sampleIndex)
        {
            int bytesRead = 0;

            while (bytesRead < packetSize && ! threadShouldExit())
            {
                const int n = commandSocket->read (packet + bytesRead, packetSize - bytesRead, true);

                if (n <= 0)
                    return;

                bytesRead += n;
            }

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
