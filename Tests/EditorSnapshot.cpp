/*
    Renders the plugin editor offscreen at several window sizes and beat counts, so the
    layout can be checked without loading it into a DAW.

    Build and run:
        cmake --build build --target EditorSnapshot
        ./build/EditorSnapshot_artefacts/Release/EditorSnapshot <output-dir>
*/

#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>

static void shoot (PolyrhythmProcessor& proc, const juce::File& dir,
                   const juce::String& name, int width, int height)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());

    if (editor == nullptr)
    {
        std::printf ("  !! createEditor() returned null\n");
        return;
    }

    editor->setSize (width, height);

    // The constrainer keeps the aspect ratio, so the size actually applied may differ
    const auto actual = editor->getLocalBounds();
    const auto image  = editor->createComponentSnapshot (actual, true);

    const auto file = dir.getChildFile (name + ".png");
    file.deleteFile();

    juce::PNGImageFormat png;
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());

    if (out != nullptr && png.writeImageToStream (image, *out))
        std::printf ("  wrote %-28s %4d x %-4d\n",
                     file.getFileName().toRawUTF8(), actual.getWidth(), actual.getHeight());
    else
        std::printf ("  !! failed to write %s\n", file.getFullPathName().toRawUTF8());
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File dir = argc > 1 ? juce::File (juce::String (argv[1]))
                                    : juce::File::getCurrentWorkingDirectory();
    dir.createDirectory();

    PolyrhythmProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    std::printf ("\nDefault beat counts (4 vs 3)\n");
    shoot (proc, dir, "01-default-1024x580",  EDITOR_BASE_W,     EDITOR_BASE_H);
    shoot (proc, dir, "02-small-512x290",     EDITOR_BASE_W / 2, EDITOR_BASE_H / 2);
    shoot (proc, dir, "03-large-1536x870",    (int) (EDITOR_BASE_W * 1.5), (int) (EDITOR_BASE_H * 1.5));

    // Worst case for pad legibility: the maximum number of beats on both tracks
    if (auto* a = dynamic_cast<juce::AudioParameterInt*> (proc.apvts.getParameter ("trackA_beats")))
        *a = 16;
    if (auto* b = dynamic_cast<juce::AudioParameterInt*> (proc.apvts.getParameter ("trackB_beats")))
        *b = 16;

    std::printf ("\nMaximum beat counts (16 vs 16)\n");
    shoot (proc, dir, "04-16beats-1024x580", EDITOR_BASE_W,     EDITOR_BASE_H);
    shoot (proc, dir, "05-16beats-512x290",  EDITOR_BASE_W / 2, EDITOR_BASE_H / 2);
    shoot (proc, dir, "06-16beats-2048x1160", EDITOR_BASE_W * 2, EDITOR_BASE_H * 2);

    // Scale lock on (F Phrygian), so the header shows the active state
    proc.scaleRoot.store (5);
    proc.scaleType.store (3);
    proc.scaleEnabled.store (true);
    proc.snapAllNotesToScale();

    std::printf ("\nScale lock on, F Phrygian\n");
    shoot (proc, dir, "07-scale-on-1024x580", EDITOR_BASE_W, EDITOR_BASE_H);

    std::printf ("\nSnapshots in %s\n\n", dir.getFullPathName().toRawUTF8());
    return 0;
}
