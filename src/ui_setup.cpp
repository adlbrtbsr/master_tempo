#include "MainComponent.h"
#include <algorithm>
#include <cmath>

void MainComponent::setupLabelsAndStatus()
{
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(bpmLabel);
    addAndMakeVisible(beatLabel);
    addAndMakeVisible(confLabel);

    for (auto* lbl : { &statusLabel, &bpmLabel, &beatLabel })
    {
        lbl->setColour (juce::Label::textColourId, juce::Colours::white);
        lbl->setInterceptsMouseClicks (false, false);
    }
    statusLabel.setFont (juce::Font (18.0f));
    bpmLabel.setFont (juce::Font (18.0f));
    beatLabel.setFont (juce::Font (18.0f));

    statusLabel.setText("Initializing...", juce::dontSendNotification);
    bpmLabel.setText("BPM: --", juce::dontSendNotification);
    beatLabel.setText("Beat: --", juce::dontSendNotification);
    confLabel.setText("Conf: --", juce::dontSendNotification);

    deviceSelector.reset (new juce::AudioDeviceSelectorComponent (deviceManager,
                                                                  0, 0,
                                                                  0, 0,
                                                                  false, false, false, false));
}

void MainComponent::setupLoopbackUI()
{
    addAndMakeVisible (loopbackHint);
    loopbackHint.setText ("System output (loopback):", juce::dontSendNotification);
    loopbackHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (loopbackBox);
    addAndMakeVisible (refreshLoopbackButton);
    addAndMakeVisible (applyLoopbackButton);
    addAndMakeVisible (changeLoopbackButton);

    refreshLoopbackButton.onClick = [this]{ refreshLoopbackList(); };
    changeLoopbackButton.onClick = [this]
    {
        usingLoopback = false;
        loopbackHint.setVisible (true);
        loopbackBox.setVisible (true);
        refreshLoopbackButton.setVisible (true);
        applyLoopbackButton.setVisible (true);
        changeLoopbackButton.setVisible (false);
        resized();
    };
    applyLoopbackButton.onClick = [this]
    {
        const int idx = loopbackBox.getSelectedItemIndex();
        if (idx < 0) return;
       #if JUCE_WINDOWS
        juce::StringArray endpoints = WASAPILoopbackCapture::listRenderEndpoints();
        if (idx < endpoints.size())
        {
            const juce::String chosen = endpoints[idx];
            if (startLoopbackCaptureForEndpoint (chosen))
            {
                statusLabel.setText (juce::String ("Loopback started: ") + chosen, juce::dontSendNotification);
                loopbackHint.setVisible (false);
                loopbackBox.setVisible (false);
                refreshLoopbackButton.setVisible (false);
                applyLoopbackButton.setVisible (false);
                changeLoopbackButton.setVisible (true);
                resized();
            }
            else
            {
                juce::String reason;
                if (loopbackCapture)
                    reason = loopbackCapture->getLastError();
                statusLabel.setText (juce::String ("Failed to start loopback: ") + chosen +
                                     (reason.isNotEmpty() ? (" (" + reason + ")") : juce::String()),
                                     juce::dontSendNotification);
            }
        }
       #endif
    };

    refreshLoopbackList();
}

void MainComponent::setupMidiUI()
{
    addAndMakeVisible (midiHint);
    midiHint.setText ("MIDI out:", juce::dontSendNotification);
    midiHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (midiOutBox);
    addAndMakeVisible (refreshMidiButton);
    addAndMakeVisible (connectMidiButton);
    refreshMidiButton.onClick = [this]
    {
        midiOutBox.clear();
        auto devices = juce::MidiOutput::getAvailableDevices();
        int idx = 0;
        for (auto& d : devices)
            midiOutBox.addItem (d.name, ++idx);
        statusLabel.setText ("MIDI outs: " + juce::String (devices.size()), juce::dontSendNotification);
    };
    connectMidiButton.onClick = [this]
    {
        const int idx = midiOutBox.getSelectedItemIndex();
        auto devices = juce::MidiOutput::getAvailableDevices();
        if (idx >= 0 && idx < devices.size())
        {
            midiOut.reset();
            midiOut = juce::MidiOutput::openDevice (devices[(int) idx].identifier);
            if (midiOut)
                statusLabel.setText ("MIDI connected: " + devices[(int) idx].name, juce::dontSendNotification);
            else
                statusLabel.setText ("Failed to open MIDI: " + devices[(int) idx].name, juce::dontSendNotification);
        }
    };
    refreshMidiButton.onClick();
}

void MainComponent::setupPrefilterControls()
{
    addAndMakeVisible (hpfHint);
    hpfHint.setText ("HPF:", juce::dontSendNotification);
    hpfHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (hpfSlider);
    hpfSlider.setRange (10.0, 200.0, 1.0);
    hpfSlider.setValue (20.0, juce::dontSendNotification);
    hpfSlider.onValueChange = [this]
    {
        if (currentSampleRate.load() > 0.0)
            bandFilter.get<0>().coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass ((double) currentSampleRate.load(), (float) hpfSlider.getValue());
    };
    addAndMakeVisible (lpfHint);
    lpfHint.setText ("LPF:", juce::dontSendNotification);
    lpfHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (lpfSlider);
    lpfSlider.setRange (1000.0, 6000.0, 10.0);
    lpfSlider.setValue (6000.0, juce::dontSendNotification);
    lpfSlider.onValueChange = [this]
    {
        if (currentSampleRate.load() > 0.0)
            bandFilter.get<1>().coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass ((double) currentSampleRate.load(), (float) lpfSlider.getValue());
    };

    addAndMakeVisible (showCandToggle);
    showCandToggle.setToggleState (false, juce::dontSendNotification);
    showCandToggle.onClick = [this]
    {
        sendTempoCandidates = showCandToggle.getToggleState();
    };
}

void MainComponent::setupOSC()
{
    oscConnected = osc.connect ("127.0.0.1", 9000);
}

void MainComponent::startTimersAndThreads()
{
    startTimerHz (30);
    startDspThread();
}


