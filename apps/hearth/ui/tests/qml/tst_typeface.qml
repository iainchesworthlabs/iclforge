import QtQuick
import QtTest

import Hearth

// The harness itself, not a page: qml_test_main.cpp registers the family's faces and makes
// Archivo the application font, as src/main.cpp does, so that every other suite measures the
// glyphs a person sees. Without them a machine with no QT_QPA_FONTDIR has no font at all -
// Qt.fontFamilies() is empty and every glyph is a square as wide as its pixel size, about twice
// an Archivo one - and a window that "fits" there is one nobody has. A layout suite cannot tell
// that from its own failure, so this one says it plainly.
TestCase {
    id: testCase
    name: "Typeface"

    function test_theFamilysFacesAreRegistered() {
        const families = Qt.fontFamilies();
        const wanted = ["Archivo", "Material Symbols Sharp", "Noto Sans Arabic", "Noto Sans Hebrew"];
        for (let i = 0; i < wanted.length; ++i) {
            verify(families.indexOf(wanted[i]) >= 0,
                   wanted[i] + " is not registered: the suites would measure another typeface");
        }
    }

    function test_archivoIsTheDefaultFaceAndTheThemeFindsIt() {
        compare(Application.font.family, "Archivo");
        compare(Theme.headingFamily, "Archivo");
        compare(Theme.iconFamily, "Material Symbols Sharp");
    }

    // The thing the registration is for. A missing glyph is drawn as a box one pixel size wide, so
    // four narrow letters come to four pixel sizes there and about one in Archivo.
    TextMetrics {
        id: probe
        font.pixelSize: 40
        text: "iiii"
    }

    function test_aNarrowLetterIsNarrow() {
        verify(probe.width < 2 * probe.font.pixelSize,
               "\"iiii\" at " + probe.font.pixelSize + " px is " + probe.width
               + " px wide: every glyph is a box, so no layout suite is measuring a real typeface");
    }
}
