# The particlehold plugin wrapper: what Logic sees (an AU MIDI effect, type aumi), the controls and the panel.
# The sample and hold, clock, noise, slew and the CC stream live in ../engine.
#
# The identifiers below were chosen once, for the first build (0.1.0), and must never change (Logic finds saved
# projects' plugins by them). Plugin code Phld: not used by any other particledots plugin (Hlsd, Ogd1, Sdr1, Cmpl,
# Cnto, Pdwr, PdwS, Bead, Smru, Bryn). Declared as a MIDI effect the same way as Bryn, Hillside, Ogdoad and Strange Drift.

juce_add_plugin(particlehold
    COMPANY_NAME                "particledots"
    BUNDLE_ID                   com.particledots.particlehold
    PLUGIN_MANUFACTURER_CODE    Pdot
    PLUGIN_CODE                 Phld
    FORMATS                     AU VST3
    PRODUCT_NAME                "particlehold"
    PLUGIN_NAME                 "particlehold"
    DESCRIPTION                 "Sample and hold as MIDI CC (inspired by the Instruo tagh; not affiliated)"
    IS_SYNTH                    FALSE
    IS_MIDI_EFFECT              TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx
    AU_MAIN_TYPE                kAudioUnitType_MIDIProcessor
)

target_sources(particlehold PRIVATE
    Source/Parameters.h
    Source/Parameters.cpp
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/Style.h
)

target_compile_definitions(particlehold PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(particlehold
    PRIVATE
        particlehold_engine
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
