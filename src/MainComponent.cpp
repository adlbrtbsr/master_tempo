// Slimmed implementation delegating to split source files
#include "MainComponent.h"

MainComponent::MainComponent()
{
	setAudioChannels (0, 0);
   #if JUCE_WINDOWS
	startLoopbackCaptureForEndpoint (preferredOutputName);
   #endif
	setupLabelsAndStatus();
	setupLoopbackUI();
	setupMidiUI();
	setupPrefilterControls();
	setupOSC();
	startTimersAndThreads();
	setSize (900, 600);
}

MainComponent::~MainComponent()
{
	deviceManager.removeAudioCallback (this);
	shutdownAudio();
	stopDspThread();
}

void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sr)
{
	prepareProcessing (sr, samplesPerBlockExpected);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
	if (bufferToFill.buffer != nullptr)
		bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
}
