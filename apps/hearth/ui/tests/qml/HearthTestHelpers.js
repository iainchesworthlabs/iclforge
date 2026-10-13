// Deliberately NOT `.pragma library`: a library script has no imports of its
// own, so `item.Accessible` (an attached property) reads as undefined inside
// it. Imported plainly, the script shares each test file's own imports.
//
// Item-tree lookups the Hearth suites share. TestCase.findChild() only finds
// by objectName, and most of Hearth's controls carry none - a shared
// SegmentedControl's cells are all "seg-<value>" (so two controls with a
// "system" cell collide), and AppCheckBox/AppSlider/AppTextField rows are
// named by the label beside them. These walk the visual tree (children, and
// a Popup's contentItem where one is met) and match on what a person would
// read off the screen instead: an accessible name, a label.

// Depth-first over `item` and every visual descendant; the first for which
// `predicate` holds, or null.
function find(item, predicate) {
    if (item === null || item === undefined) {
        return null;
    }
    if (predicate(item)) {
        return item;
    }
    const kids = item.children;
    if (kids !== undefined) {
        for (let i = 0; i < kids.length; ++i) {
            const found = find(kids[i], predicate);
            if (found !== null) {
                return found;
            }
        }
    }
    return null;
}

// Every match, in tree order.
function findAll(item, predicate, out) {
    const results = out === undefined ? [] : out;
    if (item === null || item === undefined) {
        return results;
    }
    if (predicate(item)) {
        results.push(item);
    }
    const kids = item.children;
    if (kids !== undefined) {
        for (let i = 0; i < kids.length; ++i) {
            findAll(kids[i], predicate, results);
        }
    }
    return results;
}

// A visible SegmentedControl by its accessibleName, then its "seg-<value>"
// cell. Visible, because a page can hold two controls with the same name on
// sub-pages only one of which is showing (DecoderPage.qml's E-AC-3 and AC-4
// "Mode").
function segment(root, accessibleName, value) {
    const control = find(root, function(item) {
        return item.accessibleName === accessibleName && item.model !== undefined && item.selected !== undefined
               && item.visible;
    });
    if (control === null) {
        return null;
    }
    return find(control, function(item) { return item.objectName === "seg-" + value; });
}

// An AppCheckBox by its label text.
function checkBox(root, text) {
    return find(root, function(item) {
        return item.text === text && item.toggled !== undefined && item.note !== undefined && item.visible;
    });
}

// A control whose Accessible.name is `name` (a slider, a text field, a
// button) - Accessible.name is readable from QML as item.Accessible.name.
function byAccessibleName(root, name) {
    return find(root, function(item) {
        return item.Accessible !== undefined && item.Accessible.name === name && item.visible;
    });
}

// A visible Text whose text is exactly `text`.
function textItem(root, text) {
    return find(root, function(item) {
        return item.text === text && item.font !== undefined && item.visible;
    });
}

// A Text whose text contains `fragment`.
function textContaining(root, fragment) {
    return find(root, function(item) {
        return typeof item.text === "string" && item.font !== undefined && item.visible
               && item.text.indexOf(fragment) >= 0;
    });
}

// The non-visual QtQuick.Dialogs FileDialog/FolderDialog `owner` declares,
// by its title (PlayPage.qml has two). A dialog is not a visual child, and a
// ScrollView-rooted page lists it nowhere, so this asks the QObject tree
// (TestServices.findByProperty(), qml_test_main.cpp) - which is why it needs
// the TestServices singleton handed in.
function dialog(owner, titleText, services) {
    const found = services.findByProperty(owner, "title", titleText);
    return found !== null && found.accept !== undefined && found.currentFolder !== undefined ? found : null;
}

// file:///a/b/c.ec3 for /a/b/c.ec3.
function fileUrl(path) {
    return "file://" + (path.charAt(0) === "/" ? "" : "/") + path;
}

function folderOf(path) {
    return path.substring(0, path.lastIndexOf("/"));
}

// The control sitting in the same row as the label `labelText` - for a
// slider or field whose only name is the Text beside it (DecoderEac3.qml's
// "Cut"/"Boost"). `predicate` picks which sibling.
function besideLabel(root, labelText, predicate) {
    const label = textItem(root, labelText);
    if (label === null || label.parent === null) {
        return null;
    }
    const siblings = label.parent.children;
    for (let i = 0; i < siblings.length; ++i) {
        if (siblings[i] !== label && predicate(siblings[i])) {
            return siblings[i];
        }
    }
    return null;
}

// The visible Texts that show a negative number with a decimal point ("-3.0 dB", "-0.50"): a minus
// sign (hyphen-minus or U+2212) and digits, with no letter or digit before the sign, so "AC-3" is not
// one. A sentence that names a level in whole numbers ("-31 dBFS") is written, not worked out, and is
// not looked for here.
function negativeDecimals(root) {
    return findAll(root, function(item) {
        return typeof item.text === "string" && item.font !== undefined && item.visible
               && /(^|[^A-Za-z0-9])[-\u2212]\d+\.\d/.test(item.text);
    });
}

// Of those, the ones with a minus sign that is not inside a left-to-right isolate (U+2066 to
// U+2069): next to Arabic or Hebrew words, a sign outside one lands after its digits.
function bareNegativeDecimals(root) {
    return findAll(root, function(item) {
        return typeof item.text === "string" && item.font !== undefined && item.visible
               && /(^|[^\u2066A-Za-z0-9])[-\u2212]\d+\.\d/.test(item.text);
    });
}

// Every visible Text that holds a bidi isolate control (U+2066 to U+2069).
function isolated(root) {
    return findAll(root, function(item) {
        return typeof item.text === "string" && item.font !== undefined && item.visible
               && /[\u2066-\u2069]/.test(item.text);
    });
}

function isSlider(item) {
    return item.from !== undefined && item.to !== undefined && item.visualPosition !== undefined;
}
