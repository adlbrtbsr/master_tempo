#include "MainComponent.h"

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    statusLabel.setBounds (r.removeFromTop (24));
    bpmLabel.setBounds (r.removeFromTop (24));
    beatLabel.setBounds (r.removeFromTop (24));
    confLabel.setBounds (r.removeFromTop (24));

    if (loopbackHint.isVisible() || loopbackBox.isVisible() || refreshLoopbackButton.isVisible() || applyLoopbackButton.isVisible())
    {
        auto row = r.removeFromTop (28);
        loopbackHint.setBounds (row.removeFromLeft (170));
        loopbackBox.setBounds (row.removeFromLeft (jmax (200, row.getWidth() - 320)));
        refreshLoopbackButton.setBounds (row.removeFromLeft (120));
        applyLoopbackButton.setBounds (row.removeFromLeft (120));
    }

    if (changeLoopbackButton.isVisible())
    {
        auto row2 = r.removeFromTop (28);
        changeLoopbackButton.setBounds (row2.removeFromLeft (160));
    }

    {
        auto row3 = r.removeFromTop (28);
        midiHint.setBounds (row3.removeFromLeft (100));
        midiOutBox.setBounds (row3.removeFromLeft (jmax (200, row3.getWidth() - 240)));
        refreshMidiButton.setBounds (row3.removeFromLeft (120));
        connectMidiButton.setBounds (row3.removeFromLeft (120));
    }

    {
        auto row4 = r.removeFromTop (28);
        hpfHint.setBounds (row4.removeFromLeft (40));
        hpfSlider.setBounds (row4.removeFromLeft (160));
        lpfHint.setBounds (row4.removeFromLeft (40));
        lpfSlider.setBounds (row4.removeFromLeft (160));
        showCandToggle.setBounds (row4.removeFromLeft (140));
    }
}


