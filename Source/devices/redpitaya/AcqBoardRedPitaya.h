/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI

    ------------------------------------------------------------------
*/

#ifndef __ACQBOARDREDPITAYA_H_2C4CBD67__
#define __ACQBOARDREDPITAYA_H_2C4CBD67__

#include "../AcquisitionBoard.h"

/**
    Interface for a network-based Red Pitaya device acting as an
    Open Ephys Acquisition Board compatible source.

    This implementation currently provides a basic skeleton:
    - It exposes only ADC channels (no headstages).
    - Streaming logic is implemented in run(), which can be adapted
      to receive samples over UDP or another transport.

    @see DataThread, SourceNode
*/

class AcqBoardRedPitaya : public AcquisitionBoard
{
public:
    /** Constructor */
    AcqBoardRedPitaya();

    /** Destructor */
    virtual ~AcqBoardRedPitaya();

    /** Detects whether a board is present */
    bool detectBoard() override;

    /** Initializes board after successful detection */
    bool initializeBoard() override;

    /** Returns true if the device is connected */
    bool foundInputSource() const override;

    /** Returns an array of connected headstages for this board (none for Red Pitaya) */
    Array<const Headstage*> getHeadstages() override;

    /** Returns available sample rates */
    Array<int> getAvailableSampleRates() override;

    /** Set sample rate (in Hz) */
    void setSampleRate (int sampleRateHz) override;

    /** Gets the current sample rate */
    float getSampleRate() const override;

    /** Checks for connected headstages (no-op for Red Pitaya) */
    void scanPorts() override;

    /** Enables AUX channel out (not used) */
    void enableAuxChannels (bool enabled) override;

    /** Checks whether AUX channels are enabled */
    bool areAuxChannelsEnabled() const override;

    /** Enables ADC channel out */
    void enableAdcChannels (bool enabled) override;

    /** Checks whether ADC channels are enabled */
    bool areAdcChannelsEnabled() const override;

    /** Returns bitVolts scaling value for each channel type */
    float getBitVolts (ContinuousChannel::Type) const override;

    /** Measures impedance of each channel (not supported) */
    void measureImpedances() override;

    /** Called when impedance measurement is complete */
    void impedanceMeasurementFinished() override;

    /** Save impedance measurements to XML (not supported) */
    void saveImpedances (File& file) override;

    /** Sets the method for determining channel names (no-op for now) */
    void setNamingScheme (ChannelNamingScheme scheme) override;

    /** Gets the method for determining channel names */
    ChannelNamingScheme getNamingScheme() override;

    bool isReady() override;

    /** Initializes data transfer */
    bool startAcquisition() override;

    /** Stops data transfer */
    bool stopAcquisition() override;

    /** Sets analog filter upper limit; returns actual value */
    double setUpperBandwidth (double upperBandwidth) override;

    /** Sets analog filter lower limit; returns actual value */
    double setLowerBandwidth (double lowerBandwidth) override;

    /** Sets DSP cutoff frequency; returns actual value */
    double setDspCutoffFreq (double freq) override;

    /** Gets the current DSP cutoff frequency */
    double getDspCutoffFreq() const override;

    /** Sets whether DSP offset is enabled */
    void setDspOffset (bool enabled) override;

    /** Sets whether TTL output mode is enabled */
    void setTTLOutputMode (bool enabled) override;

    /** Sets whether DAC highpass filter is enabled, and set the cutoff freq */
    void setDAChpf (float cutoff, bool enabled) override;

    /** Sets whether fast TTL settle is enabled, and set the trigger channel */
    void setFastTTLSettle (bool state, int channel) override;

    /** Sets level of noise slicer on DAC channels */
    int setNoiseSlicerLevel (int level) override;

    /** Turns LEDs on or off */
    void enableBoardLeds (bool enabled) override;

    /** Sets divider on clock output */
    int setClockDivider (int divide_ratio) override;

    /** Connects a headstage channel to a DAC (not used) */
    void connectHeadstageChannelToDAC (int headstageChannelIndex, int dacChannelIndex) override;

    /** Sets trigger threshold for DAC channel (if TTL output mode is enabled) */
    void setDACTriggerThreshold (int dacChannelIndex, float threshold) override;

    /** Returns true if a headstage is enabled (always false; no headstages) */
    bool isHeadstageEnabled (int hsNum) const override;

    /** Returns the active number of channels in a headstage (always 0) */
    int getActiveChannelsInHeadstage (int hsNum) const override;

    /** Returns the total number of channels in a headstage (always 0) */
    int getChannelsInHeadstage (int hsNum) const override;

    /** Returns total number of outputs per channel type */
    int getNumDataOutputs (ContinuousChannel::Type) override;

    /** Sets the number of channels to use in a headstage (no-op) */
    void setNumHeadstageChannels (int headstageIndex, int channelCount) override;

private:
    /** Fills data buffer */
    void run();

    /** Maximum samples per buffer for Red Pitaya ADC */
    static constexpr int MAX_SAMPLES_PER_BUFFER = 128;

    /** Data buffers */
    float samples[8 * MAX_SAMPLES_PER_BUFFER];
    int64 sampleNumbers[MAX_SAMPLES_PER_BUFFER];
    double timestamps[MAX_SAMPLES_PER_BUFFER];
    uint64 event_codes[MAX_SAMPLES_PER_BUFFER];

    /** Number of ADC channels we expose through this board */
    int numAdcChannels = 2;

    /** Whether we report that ADC channels are enabled */
    bool acquireAdc = true;

    /** Whether AUX channels are enabled (unused) */
    bool acquireAux = false;

    /** True if logical device is considered present */
    bool deviceFound = false;

    /** Latest DSP cutoff frequency */
    double dspCutoffFreqHz = 0.5;

    /** Analog filter band limits */
    double lowerBandwidthHz = 1.0;
    double upperBandwidthHz = 7500.0;

    /** Whether TTL output mode is enabled */
    bool ttlOutputMode = false;

    /** Desired clock divide ratio */
    int clockDivideRatio = 1;

    /** BitVolts scaling for ADC channels */
    float adcBitVolts = 1.0f;

    StreamingSocket* commandSocket = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AcqBoardRedPitaya);
};

#endif
